#pragma once

// Per-instrument hard limits, checked before any mutation of the book.
//
// The defaults reject nothing a real instrument would send, but keep every quantity sum the book
// maintains below 2^64 (an order quantity <= 2^62 added to a level aggregate <= 2^63 cannot wrap),
// so the unchecked Quantity arithmetic inside the book can never overflow.

#include <cstddef>
#include <cstdint>
#include <limits>

#include <eigenbook/core/types.hpp>

namespace eigenbook {

struct BookLimits {
    // Order storage (pool and locator) preallocated at construction.
    std::size_t reserveOrders{1024};
    // 0: storage may grow beyond reserveOrders (that allocates on the hot path).
    // > 0: hard limit; storage is preallocated for it and a new order beyond it is rejected with
    //      CAPACITY_EXCEEDED, so the pool and the locator never grow.
    std::size_t maxOrders{0};

    Quantity maxOrderQuantity{std::uint64_t{1} << 62}; // ORDER_QUANTITY_LIMIT
    // LEVEL_QUANTITY_LIMIT: the aggregate quantity at one price.
    Quantity maxLevelQuantity{std::uint64_t{1} << 63};

    // Instrument price band for limit prices (PRICE_OUT_OF_BAND).
    Price minPrice{std::numeric_limits<std::int64_t>::min()};
    Price maxPrice{std::numeric_limits<std::int64_t>::max()};

    // |price| x quantity of a limit order, in ticks x lots (NOTIONAL_LIMIT). Market orders carry no
    // price; their protection is Phase B6.
    std::uint64_t maxNotional{std::numeric_limits<std::uint64_t>::max()};

    // Run the full invariant check after every auditEvery-th accepted mutation (add, modify,
    // cancel) and halt the book (HaltReason::AUDIT_FAILED) if it fails. 0: never. The check is
    // O(orders) (dense ladder: O(ticks + orders)), so this is for soak tests and debugging, not
    // for a production hot path.
    std::uint32_t auditEvery{0};

    [[nodiscard]] constexpr std::size_t orderCapacity() const noexcept {
        return maxOrders != 0 ? maxOrders : reserveOrders;
    }
};

} // namespace eigenbook
