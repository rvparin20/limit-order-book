// Unit tests + a differential test against the std::map baseline.
// No framework needed: build and run, non-zero exit code on failure.

#include <cstdio>
#include <cstdlib>
#include <vector>

#include "lob/baseline_book.hpp"
#include "lob/order_book.hpp"
#include "lob/order_flow.hpp"

using namespace lob;

static int failures = 0;
#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            std::fprintf(stderr, "FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); \
            ++failures;                                                      \
        }                                                                    \
    } while (0)

struct Recorder {
    std::vector<Trade> trades;
    void operator()(const Trade& t) { trades.push_back(t); }
};

static OrderBook make_book() { return OrderBook(0, 1000, 1024, 1024); }

static void test_rests_when_not_crossing() {
    OrderBook b = make_book();
    Recorder r;
    CHECK(b.add_limit(1, Side::Buy, 100, 10, r).resting == 10);
    CHECK(b.add_limit(2, Side::Sell, 101, 5, r).resting == 5);
    CHECK(r.trades.empty());
    CHECK(b.best_bid() == 100);
    CHECK(b.best_ask() == 101);
    CHECK(b.live_orders() == 2);
}

static void test_price_time_priority() {
    OrderBook b = make_book();
    Recorder r;
    b.add_limit(1, Side::Sell, 101, 5, r);  // worse price
    b.add_limit(2, Side::Sell, 100, 5, r);  // best price, first in time
    b.add_limit(3, Side::Sell, 100, 5, r);  // best price, second in time
    auto res = b.add_limit(4, Side::Buy, 101, 12, r);
    CHECK(res.filled == 12 && res.resting == 0);
    CHECK(r.trades.size() == 3);
    CHECK(r.trades[0].maker == 2 && r.trades[0].qty == 5 && r.trades[0].price == 100);
    CHECK(r.trades[1].maker == 3 && r.trades[1].qty == 5 && r.trades[1].price == 100);
    CHECK(r.trades[2].maker == 1 && r.trades[2].qty == 2 && r.trades[2].price == 101);
    CHECK(b.volume_at(Side::Sell, 101) == 3);
    CHECK(b.best_ask() == 101);
}

static void test_executes_at_maker_price() {
    OrderBook b = make_book();
    Recorder r;
    b.add_limit(1, Side::Buy, 100, 5, r);
    b.add_limit(2, Side::Sell, 90, 5, r);  // aggressive seller
    CHECK(r.trades.size() == 1 && r.trades[0].price == 100);
}

static void test_partial_fill_then_rest() {
    OrderBook b = make_book();
    Recorder r;
    b.add_limit(1, Side::Sell, 100, 4, r);
    auto res = b.add_limit(2, Side::Buy, 100, 10, r);
    CHECK(res.filled == 4 && res.resting == 6);
    CHECK(b.best_bid() == 100 && !b.best_ask().has_value());
    CHECK(b.volume_at(Side::Buy, 100) == 6);
}

static void test_market_order() {
    OrderBook b = make_book();
    Recorder r;
    b.add_limit(1, Side::Buy, 100, 5, r);
    b.add_limit(2, Side::Buy, 99, 5, r);
    auto res = b.add_market(3, Side::Sell, 7, r);
    CHECK(res.filled == 7 && res.resting == 0);
    CHECK(b.best_bid() == 99 && b.volume_at(Side::Buy, 99) == 3);
    auto res2 = b.add_market(4, Side::Sell, 100, r);  // more than available
    CHECK(res2.filled == 3);
    CHECK(!b.best_bid().has_value());
    CHECK(b.live_orders() == 0);
}

static void test_cancel() {
    OrderBook b = make_book();
    Recorder r;
    b.add_limit(1, Side::Buy, 100, 5, r);
    b.add_limit(2, Side::Buy, 100, 5, r);
    b.add_limit(3, Side::Buy, 98, 5, r);
    CHECK(b.cancel(1));
    CHECK(!b.cancel(1));                     // already gone
    CHECK(b.volume_at(Side::Buy, 100) == 5);
    CHECK(b.cancel(2));                      // empties best level
    CHECK(b.best_bid() == 98);               // best moves to next level
    b.add_market(4, Side::Sell, 5, r);       // fills order 3 completely
    CHECK(!b.cancel(3));                     // can't cancel a filled order
    CHECK(!b.cancel(999));                   // never existed
}

static void test_cancelled_order_loses_priority() {
    OrderBook b = make_book();
    Recorder r;
    b.add_limit(1, Side::Sell, 100, 5, r);
    b.add_limit(2, Side::Sell, 100, 5, r);
    b.cancel(1);
    b.add_limit(1, Side::Sell, 100, 5, r);   // id reused after cancel: back of the queue
    b.add_limit(3, Side::Buy, 100, 5, r);
    CHECK(r.trades.size() == 1 && r.trades[0].maker == 2);
}

static void test_rejections() {
    OrderBook b = make_book();
    Recorder r;
    CHECK(b.add_limit(1, Side::Buy, 100, 0, r).status == Status::RejectedInvalid);
    CHECK(b.add_limit(1, Side::Buy, 5000, 1, r).status == Status::RejectedInvalid);
    CHECK(b.add_limit(5000, Side::Buy, 100, 1, r).status == Status::RejectedInvalid);
    CHECK(b.add_limit(1, Side::Buy, 100, 1, r).status == Status::Accepted);
    CHECK(b.add_limit(1, Side::Buy, 100, 1, r).status == Status::RejectedDuplicate);

    OrderBook tiny(0, 1000, 1, 16);          // room for one resting order
    CHECK(tiny.add_limit(1, Side::Buy, 100, 1, r).status == Status::Accepted);
    CHECK(tiny.add_limit(2, Side::Buy, 99, 1, r).status == Status::RejectedNoCapacity);
}

// Run the same random flow through both books; every trade must match exactly.
static void test_matches_baseline() {
    FlowParams p;
    p.num_ops = 200'000;
    p.seed = 7;
    auto ops = generate_flow(p);

    OrderBook fast(p.min_price, p.max_price, p.num_ops, p.num_ops + 1);
    BaselineBook slow;
    Recorder rf, rs;

    for (const Op& op : ops) {
        switch (op.type) {
            case OpType::Limit:
                fast.add_limit(op.id, op.side, op.price, op.qty, rf);
                slow.add_limit(op.id, op.side, op.price, op.qty, rs);
                break;
            case OpType::Market:
                fast.add_market(op.id, op.side, op.qty, rf);
                slow.add_market(op.id, op.side, op.qty, rs);
                break;
            case OpType::Cancel:
                CHECK(fast.cancel(op.id) == slow.cancel(op.id));
                break;
        }
        if (fast.best_bid() != slow.best_bid() || fast.best_ask() != slow.best_ask()) {
            CHECK(false && "top of book diverged");
            return;
        }
    }
    CHECK(rf.trades.size() == rs.trades.size());
    bool same = rf.trades.size() == rs.trades.size();
    for (std::size_t i = 0; same && i < rf.trades.size(); ++i) {
        const Trade &a = rf.trades[i], &b = rs.trades[i];
        same = a.taker == b.taker && a.maker == b.maker && a.price == b.price && a.qty == b.qty;
    }
    CHECK(same);
    std::printf("  differential: %zu ops, %zu trades identical\n", ops.size(), rf.trades.size());
}

int main() {
    test_rests_when_not_crossing();
    test_price_time_priority();
    test_executes_at_maker_price();
    test_partial_fill_then_rest();
    test_market_order();
    test_cancel();
    test_cancelled_order_loses_priority();
    test_rejections();
    test_matches_baseline();
    if (failures) {
        std::printf("%d check(s) FAILED\n", failures);
        return EXIT_FAILURE;
    }
    std::printf("All tests passed\n");
    return EXIT_SUCCESS;
}
