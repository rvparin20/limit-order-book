#pragma once

#include <algorithm>
#include <cstddef>
#include <optional>
#include <vector>

#include "lob/object_pool.hpp"
#include "lob/types.hpp"

namespace lob {

// Price-time priority limit order book for a single instrument.
//
// Design:
//  * Prices live in a fixed tick range [min_price, max_price]. Each side is a
//    flat array of price levels indexed by (price - min_price), so finding a
//    level is one subtraction: no tree walk, no hashing.
//  * Each level is an intrusive doubly linked FIFO of orders. Arrival order
//    gives time priority; the prev/next pointers live inside the order, so
//    cancel is O(1) unlinking with no search.
//  * Orders come from a pre-allocated ObjectPool: no heap allocation after
//    construction.
//  * Order lookup by id is a flat vector (ids must be < max_order_id).
//  * Best bid/ask are cached as indices. When the best level empties, we scan
//    outward to the next non-empty level. That costs O(gap) in ticks, which
//    is small for a liquid book.
class OrderBook {
public:
    struct Order {
        OrderId id = 0;
        Price price = 0;
        Qty qty = 0;
        Side side = Side::Buy;
        Order* prev = nullptr;
        Order* next = nullptr;
    };

    struct Level {
        Order* head = nullptr;  // oldest order: matched first
        Order* tail = nullptr;  // newest order
        std::uint64_t total_qty = 0;
        std::uint32_t count = 0;
    };

    OrderBook(Price min_price, Price max_price, std::size_t max_live_orders, OrderId max_order_id)
        : min_price_(min_price),
          num_levels_(static_cast<std::int64_t>(max_price - min_price + 1)),
          bids_(static_cast<std::size_t>(num_levels_)),
          asks_(static_cast<std::size_t>(num_levels_)),
          pool_(max_live_orders),
          by_id_(static_cast<std::size_t>(max_order_id), nullptr),
          best_bid_(-1),
          best_ask_(num_levels_) {}

    // Limit order: match against the opposite side up to `price`, then rest the remainder.
    template <typename OnTrade>
    AddResult add_limit(OrderId id, Side side, Price price, Qty qty, OnTrade&& on_trade) {
        if (qty == 0 || id >= by_id_.size() || price < min_price_ ||
            price >= min_price_ + num_levels_) {
            return {Status::RejectedInvalid, 0, 0};
        }
        if (by_id_[id] != nullptr) return {Status::RejectedDuplicate, 0, 0};

        const std::int64_t idx = price - min_price_;
        const Qty remaining = match(id, side, idx, qty, on_trade);
        const Qty filled = qty - remaining;
        if (remaining == 0) return {Status::Accepted, filled, 0};

        Order* o = pool_.acquire();
        if (o == nullptr) return {Status::RejectedNoCapacity, filled, 0};

        o->id = id;
        o->price = price;
        o->qty = remaining;
        o->side = side;
        o->next = nullptr;

        Level& lvl = (side == Side::Buy ? bids_ : asks_)[static_cast<std::size_t>(idx)];
        o->prev = lvl.tail;
        if (lvl.tail) lvl.tail->next = o; else lvl.head = o;
        lvl.tail = o;
        lvl.total_qty += remaining;
        ++lvl.count;
        by_id_[id] = o;

        if (side == Side::Buy) best_bid_ = std::max(best_bid_, idx);
        else                   best_ask_ = std::min(best_ask_, idx);

        return {Status::Accepted, filled, remaining};
    }

    // Market order: take whatever liquidity is available; any remainder is dropped.
    template <typename OnTrade>
    AddResult add_market(OrderId id, Side side, Qty qty, OnTrade&& on_trade) {
        if (qty == 0) return {Status::RejectedInvalid, 0, 0};
        const std::int64_t limit = (side == Side::Buy) ? num_levels_ - 1 : 0;
        const Qty remaining = match(id, side, limit, qty, on_trade);
        return {Status::Accepted, qty - remaining, 0};
    }

    // Cancel a resting order. Returns false if it doesn't exist (e.g. already filled).
    bool cancel(OrderId id) {
        if (id >= by_id_.size()) return false;
        Order* o = by_id_[id];
        if (o == nullptr) return false;

        const std::int64_t idx = o->price - min_price_;
        Level& lvl = (o->side == Side::Buy ? bids_ : asks_)[static_cast<std::size_t>(idx)];
        unlink(lvl, o);
        by_id_[id] = nullptr;

        if (lvl.head == nullptr) {
            if (o->side == Side::Buy && idx == best_bid_) advance_best_bid();
            if (o->side == Side::Sell && idx == best_ask_) advance_best_ask();
        }
        pool_.release(o);
        return true;
    }

    std::optional<Price> best_bid() const {
        if (best_bid_ < 0) return std::nullopt;
        return min_price_ + best_bid_;
    }
    std::optional<Price> best_ask() const {
        if (best_ask_ >= num_levels_) return std::nullopt;
        return min_price_ + best_ask_;
    }

    std::uint64_t volume_at(Side side, Price price) const {
        if (price < min_price_ || price >= min_price_ + num_levels_) return 0;
        const auto& book = (side == Side::Buy) ? bids_ : asks_;
        return book[static_cast<std::size_t>(price - min_price_)].total_qty;
    }

    std::size_t live_orders() const { return pool_.in_use(); }

private:
    // Fill `qty` against the opposite side, never crossing price index `limit`.
    // Returns the unfilled quantity.
    template <typename OnTrade>
    Qty match(OrderId taker, Side side, std::int64_t limit, Qty qty, OnTrade& on_trade) {
        if (side == Side::Buy) {
            while (qty > 0 && best_ask_ < num_levels_ && best_ask_ <= limit) {
                qty = fill_level(taker, asks_[static_cast<std::size_t>(best_ask_)], qty, on_trade);
                if (asks_[static_cast<std::size_t>(best_ask_)].head == nullptr) advance_best_ask();
            }
        } else {
            while (qty > 0 && best_bid_ >= 0 && best_bid_ >= limit) {
                qty = fill_level(taker, bids_[static_cast<std::size_t>(best_bid_)], qty, on_trade);
                if (bids_[static_cast<std::size_t>(best_bid_)].head == nullptr) advance_best_bid();
            }
        }
        return qty;
    }

    // Walk one price level oldest-first (time priority).
    template <typename OnTrade>
    Qty fill_level(OrderId taker, Level& lvl, Qty qty, OnTrade& on_trade) {
        while (qty > 0 && lvl.head != nullptr) {
            Order* maker = lvl.head;
            const Qty f = std::min(qty, maker->qty);
            maker->qty -= f;
            lvl.total_qty -= f;
            qty -= f;
            on_trade(Trade{taker, maker->id, maker->price, f});
            if (maker->qty == 0) {
                unlink(lvl, maker);
                by_id_[maker->id] = nullptr;
                pool_.release(maker);
            }
        }
        return qty;
    }

    static void unlink(Level& lvl, Order* o) {
        if (o->prev) o->prev->next = o->next; else lvl.head = o->next;
        if (o->next) o->next->prev = o->prev; else lvl.tail = o->prev;
        lvl.total_qty -= o->qty;
        --lvl.count;
    }

    void advance_best_bid() {
        while (best_bid_ >= 0 && bids_[static_cast<std::size_t>(best_bid_)].head == nullptr) --best_bid_;
    }
    void advance_best_ask() {
        while (best_ask_ < num_levels_ && asks_[static_cast<std::size_t>(best_ask_)].head == nullptr) ++best_ask_;
    }

    Price min_price_;
    std::int64_t num_levels_;
    std::vector<Level> bids_;
    std::vector<Level> asks_;
    ObjectPool<Order> pool_;
    std::vector<Order*> by_id_;
    std::int64_t best_bid_;  // index of best bid level, -1 if no bids
    std::int64_t best_ask_;  // index of best ask level, num_levels_ if no asks
};

}  // namespace lob
