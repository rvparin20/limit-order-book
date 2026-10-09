#pragma once

// Synthetic order flow used by the benchmark and the differential test.
// The mid price follows a random walk. Passive limit orders cluster within a
// few ticks of mid, some limits cross the spread, and cancels target
// previously submitted orders (some of which will already have filled,
// as in a real feed).

#include <algorithm>
#include <cstdint>
#include <random>
#include <vector>

#include "lob/types.hpp"

namespace lob {

enum class OpType : std::uint8_t { Limit, Market, Cancel };

struct Op {
    OpType type;
    Side side;
    OrderId id;
    Price price;
    Qty qty;
};

struct FlowParams {
    std::size_t num_ops = 1'000'000;
    Price min_price = 0;
    Price max_price = 100'000;
    double p_limit = 0.55;
    double p_cancel = 0.38;   // remainder is market orders
    double p_cross = 0.10;    // fraction of limits priced through the spread
    std::uint64_t seed = 42;
};

inline std::vector<Op> generate_flow(const FlowParams& p) {
    std::mt19937_64 rng(p.seed);
    std::uniform_real_distribution<double> u(0.0, 1.0);
    std::geometric_distribution<int> offset(0.25);  // mean ~3 ticks from mid
    std::uniform_int_distribution<Qty> qty(1, 100);
    std::uniform_int_distribution<int> step(-1, 1);

    const Price lo = p.min_price + 1000, hi = p.max_price - 1000;
    Price mid = (p.min_price + p.max_price) / 2;
    OrderId next_id = 1;
    std::vector<OrderId> submitted;  // candidates for cancellation
    std::vector<Op> ops;
    ops.reserve(p.num_ops);

    while (ops.size() < p.num_ops) {
        mid = std::clamp<Price>(mid + step(rng), lo, hi);
        const double r = u(rng);
        const Side side = (u(rng) < 0.5) ? Side::Buy : Side::Sell;

        if (r < p.p_limit) {
            Price px;
            const int off = offset(rng);
            if (u(rng) < p.p_cross) px = (side == Side::Buy) ? mid + 1 + off : mid - 1 - off;
            else                    px = (side == Side::Buy) ? mid - 1 - off : mid + 1 + off;
            px = std::clamp(px, p.min_price, p.max_price);
            ops.push_back({OpType::Limit, side, next_id, px, qty(rng)});
            submitted.push_back(next_id++);
        } else if (r < p.p_limit + p.p_cancel && !submitted.empty()) {
            std::uniform_int_distribution<std::size_t> pick(0, submitted.size() - 1);
            const std::size_t i = pick(rng);
            ops.push_back({OpType::Cancel, side, submitted[i], 0, 0});
            submitted[i] = submitted.back();
            submitted.pop_back();
        } else {
            ops.push_back({OpType::Market, side, next_id++, 0, qty(rng)});
        }
    }
    return ops;
}

}  // namespace lob
