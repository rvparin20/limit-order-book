#pragma once

// Textbook implementation using standard containers. It is used for two things:
//   1. Benchmark baseline: how much do the flat arrays + object pool actually buy us?
//   2. Correctness oracle: both books must produce the identical trade stream.

#include <algorithm>
#include <functional>
#include <list>
#include <map>
#include <optional>
#include <unordered_map>

#include "lob/types.hpp"

namespace lob {

class BaselineBook {
    struct Order {
        OrderId id;
        Qty qty;
    };
    using Queue = std::list<Order>;
    struct Locator {
        Side side;
        Price price;
        Queue::iterator it;
    };

public:
    template <typename OnTrade>
    AddResult add_limit(OrderId id, Side side, Price price, Qty qty, OnTrade&& on_trade) {
        if (qty == 0) return {Status::RejectedInvalid, 0, 0};
        if (index_.count(id)) return {Status::RejectedDuplicate, 0, 0};
        Qty remaining = (side == Side::Buy) ? match(id, asks_, price, qty, on_trade, true)
                                            : match(id, bids_, price, qty, on_trade, false);
        Qty filled = qty - remaining;
        if (remaining == 0) return {Status::Accepted, filled, 0};
        Queue& q = (side == Side::Buy) ? bids_[price] : asks_[price];
        q.push_back(Order{id, remaining});
        index_.emplace(id, Locator{side, price, std::prev(q.end())});
        return {Status::Accepted, filled, remaining};
    }

    template <typename OnTrade>
    AddResult add_market(OrderId id, Side side, Qty qty, OnTrade&& on_trade) {
        if (qty == 0) return {Status::RejectedInvalid, 0, 0};
        Qty remaining = (side == Side::Buy) ? match_all(id, asks_, qty, on_trade)
                                            : match_all(id, bids_, qty, on_trade);
        return {Status::Accepted, qty - remaining, 0};
    }

    bool cancel(OrderId id) {
        auto f = index_.find(id);
        if (f == index_.end()) return false;
        const Locator& loc = f->second;
        if (loc.side == Side::Buy) erase_from(bids_, loc);
        else                       erase_from(asks_, loc);
        index_.erase(f);
        return true;
    }

    std::optional<Price> best_bid() const {
        if (bids_.empty()) return std::nullopt;
        return bids_.begin()->first;
    }
    std::optional<Price> best_ask() const {
        if (asks_.empty()) return std::nullopt;
        return asks_.begin()->first;
    }

private:
    template <typename Map>
    static void erase_from(Map& m, const Locator& loc) {
        auto lvl = m.find(loc.price);
        lvl->second.erase(loc.it);
        if (lvl->second.empty()) m.erase(lvl);
    }

    template <typename Map, typename OnTrade>
    Qty match(OrderId taker, Map& opp, Price limit, Qty qty, OnTrade& on_trade, bool buying) {
        while (qty > 0 && !opp.empty()) {
            auto lvl = opp.begin();
            if (buying ? lvl->first > limit : lvl->first < limit) break;
            qty = fill_level(taker, opp, lvl, qty, on_trade);
        }
        return qty;
    }

    template <typename Map, typename OnTrade>
    Qty match_all(OrderId taker, Map& opp, Qty qty, OnTrade& on_trade) {
        while (qty > 0 && !opp.empty()) qty = fill_level(taker, opp, opp.begin(), qty, on_trade);
        return qty;
    }

    template <typename Map, typename It, typename OnTrade>
    Qty fill_level(OrderId taker, Map& opp, It lvl, Qty qty, OnTrade& on_trade) {
        Queue& q = lvl->second;
        while (qty > 0 && !q.empty()) {
            Order& maker = q.front();
            Qty f = std::min(qty, maker.qty);
            maker.qty -= f;
            qty -= f;
            on_trade(Trade{taker, maker.id, lvl->first, f});
            if (maker.qty == 0) {
                index_.erase(maker.id);
                q.pop_front();
            }
        }
        if (q.empty()) opp.erase(lvl);
        return qty;
    }

    std::map<Price, Queue, std::greater<Price>> bids_;  // highest first
    std::map<Price, Queue, std::less<Price>> asks_;     // lowest first
    std::unordered_map<OrderId, Locator> index_;
};

}  // namespace lob
