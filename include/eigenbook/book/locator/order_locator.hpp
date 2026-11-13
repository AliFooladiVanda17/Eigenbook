#pragma once

// OrderId -> Order* for cancel and modify. Three implementations:
//
//   OrderIndex          client-chosen ids; open addressing with linear probing; grows (default)
//   HardenedOrderIndex  client-chosen ids; keyed hash with a per-instance seed, fixed capacity,
//                       bounded probe distance (an id that would land too far is refused)
//   DirectOrderTable    engine-issued handles (slot + generation); one index + one compare
//
// The book uses a locator in three steps for a new order: admit(id) before any matching (may
// reject), then insert(id, order) if the remainder rests (cannot fail after a successful admit),
// or abandon(id) if it does not.

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <optional>

#include <eigenbook/core/order.hpp>
#include <eigenbook/core/types.hpp>

namespace eigenbook {

// What admit() says about a new order's id.
enum class Admission : std::uint8_t {
    OK,          // insert(id, order) will succeed
    DUPLICATE,   // the id is resting already
    NOT_ISSUED,  // issuing locator: the id is not a reserved handle (never issued, or stale)
    FULL,        // no room for another order
    PROBE_LIMIT, // hardened index: the id would be stored further from its home than allowed
};

template <typename L>
concept OrderLocator = requires(L locator, const L clocator, OrderId id, Order* order) {
    { clocator.find(id) } noexcept -> std::same_as<Order*>;
    { clocator.admit(id) } noexcept -> std::same_as<Admission>;
    locator.insert(id, order);
    { locator.erase(id) } noexcept -> std::same_as<bool>;
    // The order leaves the book but its id stays valid for a re-entry (modify).
    { locator.unbind(id) } noexcept;
    // An admitted id whose order did not rest (filled or cancelled in full).
    { locator.abandon(id) } noexcept;
    { clocator.size() } noexcept -> std::same_as<std::size_t>;
    { clocator.empty() } noexcept -> std::same_as<bool>;
};

// A locator that hands out the ids itself (engine-assigned order references).
template <typename L>
concept IssuingOrderLocator = OrderLocator<L> && requires(L locator, OrderId id) {
    { locator.issue() } noexcept -> std::same_as<std::optional<OrderId>>;
    { locator.release(id) } noexcept -> std::same_as<bool>;
};

} // namespace eigenbook
