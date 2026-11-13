#pragma once

// Overflow-free arithmetic for boundary checks.

#include <cstdint>

#include <eigenbook/core/types.hpp>

namespace eigenbook {

// |p| as an unsigned value, defined for every int64 including INT64_MIN.
[[nodiscard]] constexpr std::uint64_t magnitude(const Price p) noexcept {
    return p.value < 0 ? ~static_cast<std::uint64_t>(p.value) + 1
                       : static_cast<std::uint64_t>(p.value);
}

// |price| x quantity <= limit, computed exactly without forming the product:
// a x b <= L  <=>  a == 0 || b <= floor(L / a)   for unsigned a, b, L.
[[nodiscard]] constexpr bool
notionalWithin(const Price price, const Quantity quantity, const std::uint64_t limit) noexcept {
    const std::uint64_t a = magnitude(price);
    return a == 0 || quantity.value <= limit / a;
}

// existing + added <= limit, without overflow.
[[nodiscard]] constexpr bool
sumWithin(const Quantity existing, const Quantity added, const Quantity limit) noexcept {
    return added.value <= limit.value && existing.value <= limit.value - added.value;
}

} // namespace eigenbook
