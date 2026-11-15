#pragma once

#include <cstdint>

namespace eigenbook {

// Why the book refused a request. Rejections are returned as values, never thrown.
enum class RejectReason : std::uint8_t {
    INVALID_QUANTITY,   // zero quantity
    DUPLICATE_ORDER_ID, // id already resting in this book
    PRICE_OUT_OF_RANGE, // limit price outside the ladder's tick range (dense ladder only)
    UNKNOWN_ORDER_ID,   // cancel / modify of an order that is not resting
    CAPACITY_EXCEEDED, // the order would need a new price level (or locator slot) that is not there
    INVALID_ORDER_ID,  // the book issues ids and did not issue this one, or it is stale
    ORDER_ID_REJECTED, // the hardened index refuses an id it could only store too far from home
    ORDER_QUANTITY_LIMIT, // quantity above BookLimits::maxOrderQuantity
    LEVEL_QUANTITY_LIMIT, // would take the quantity at its price above BookLimits::maxLevelQuantity
    PRICE_OUT_OF_BAND,    // limit price outside the instrument band [minPrice, maxPrice]
    NOTIONAL_LIMIT,       // |price| x quantity above BookLimits::maxNotional
    NOT_OWNER,            // cancel / modify by a participant that does not own the order
    POST_ONLY_WOULD_TRADE, // a post-only order (or its re-entry) would take liquidity
    INVALID_QUALIFIERS,    // time in force, post-only and minimum quantity do not fit together
    INSTRUMENT_HALTED,     // the book is halted: no new orders or modifies (cancels still work)
};

} // namespace eigenbook
