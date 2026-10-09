// Throughput and per-operation latency benchmark: OrderBook vs BaselineBook.
//
//   ./lob_bench [num_ops] [seed]
//
// Throughput run: replays the whole flow with no timing inside the loop.
// Latency run: times every operation with steady_clock (adds ~20 ns of
// clock overhead per sample, so treat the latency figures as upper bounds).

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <vector>

#include "lob/baseline_book.hpp"
#include "lob/order_book.hpp"
#include "lob/order_flow.hpp"

using namespace lob;
using Clock = std::chrono::steady_clock;

struct Sink {
    std::uint64_t trades = 0, volume = 0, checksum = 0;
    void operator()(const Trade& t) {
        ++trades;
        volume += t.qty;
        checksum = checksum * 31 + t.maker * 7 + static_cast<std::uint64_t>(t.price) + t.qty;
    }
};

template <typename Book>
inline void apply(Book& book, const Op& op, Sink& sink) {
    switch (op.type) {
        case OpType::Limit:  book.add_limit(op.id, op.side, op.price, op.qty, sink); break;
        case OpType::Market: book.add_market(op.id, op.side, op.qty, sink); break;
        case OpType::Cancel: book.cancel(op.id); break;
    }
}

struct Result {
    double mops;
    double p50, p99, p999, max;
    Sink sink;
};

template <typename MakeBook>
Result run(const std::vector<Op>& ops, MakeBook make) {
    Result r{};
    {   // throughput
        auto book = make();
        Sink sink;
        auto t0 = Clock::now();
        for (const Op& op : ops) apply(*book, op, sink);
        auto t1 = Clock::now();
        double secs = std::chrono::duration<double>(t1 - t0).count();
        r.mops = ops.size() / secs / 1e6;
        r.sink = sink;
    }
    {   // latency
        auto book = make();
        Sink sink;
        std::vector<std::uint32_t> ns;
        ns.reserve(ops.size());
        for (const Op& op : ops) {
            auto t0 = Clock::now();
            apply(*book, op, sink);
            auto t1 = Clock::now();
            ns.push_back(static_cast<std::uint32_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count()));
        }
        std::sort(ns.begin(), ns.end());
        auto pct = [&](double q) { return double(ns[std::min(ns.size() - 1, size_t(q * ns.size()))]); };
        r.p50 = pct(0.50);
        r.p99 = pct(0.99);
        r.p999 = pct(0.999);
        r.max = double(ns.back());
    }
    return r;
}

int main(int argc, char** argv) {
    FlowParams p;
    if (argc > 1) p.num_ops = std::strtoull(argv[1], nullptr, 10);
    if (argc > 2) p.seed = std::strtoull(argv[2], nullptr, 10);

    std::printf("Generating %zu ops (seed %llu)...\n", p.num_ops, (unsigned long long)p.seed);
    auto ops = generate_flow(p);

    auto fast = run(ops, [&] {
        return std::make_unique<OrderBook>(p.min_price, p.max_price, p.num_ops, p.num_ops + 1);
    });
    auto slow = run(ops, [] { return std::make_unique<BaselineBook>(); });

    if (fast.sink.checksum != slow.sink.checksum || fast.sink.trades != slow.sink.trades) {
        std::fprintf(stderr, "ERROR: books disagree on trades\n");
        return EXIT_FAILURE;
    }

    std::printf("Trades: %llu  Volume: %llu  (identical in both books)\n\n",
                (unsigned long long)fast.sink.trades, (unsigned long long)fast.sink.volume);
    std::printf("%-26s %12s %10s %10s %10s %10s\n", "", "M ops/sec", "p50 ns", "p99 ns", "p99.9 ns", "max ns");
    auto row = [](const char* name, const Result& r) {
        std::printf("%-26s %12.2f %10.0f %10.0f %10.0f %10.0f\n", name, r.mops, r.p50, r.p99, r.p999, r.max);
    };
    row("OrderBook (pool + arrays)", fast);
    row("Baseline (std::map/list)", slow);
    std::printf("\nSpeed-up: %.1fx throughput, %.1fx p99 latency\n",
                fast.mops / slow.mops, slow.p99 / fast.p99);
    return EXIT_SUCCESS;
}
