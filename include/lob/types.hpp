#pragma once

#include <cstdint>

namespace lob {

using OrderId = std::uint64_t;
using Price = std::int64_t;  // integer ticks: never use floating point for prices
using Qty = std::uint32_t;

enum class Side : std::uint8_t { Buy, Sell };

// One fill between an incoming (taker) order and a resting (maker) order.
// Trades always execute at the maker's price.
struct Trade {
    OrderId taker;
    OrderId maker;
    Price price;
    Qty qty;
};

enum class Status : std::uint8_t {
    Accepted,          // order was processed (may be fully filled, partially filled, or resting)
    RejectedInvalid,   // zero quantity, price outside the book's range, or id out of range
    RejectedDuplicate, // an order with this id is already resting
    RejectedNoCapacity // book is full; any fills that already happened stand, remainder dropped
};

struct AddResult {
    Status status;
    Qty filled;   // quantity executed immediately
    Qty resting;  // quantity left on the book (always 0 for market orders)
};

}  // namespace lob
