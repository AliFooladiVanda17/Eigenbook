#pragma once

#include <cstdint>

namespace eigenbook {

// Why the book refused a request. Rejections are returned as values, never thrown.
enum class RejectReason : std::uint8_t {
    INVALID_QUANTITY,   // zero quantity
    DUPLICATE_ORDER_ID, // id already resting in this book
    PRICE_OUT_OF_RANGE, // limit price outside the ladder's tick range (dense ladder only)
    UNKNOWN_ORDER_ID,   // cancel / modify of an order that is not resting
    CAPACITY_EXCEEDED,  // the order would need a new price level and the ladder is full
};

} // namespace eigenbook
