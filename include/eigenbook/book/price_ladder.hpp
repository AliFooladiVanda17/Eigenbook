#pragma once

#include <concepts>
#include <cstddef>
#include <functional>
#include <optional>
#include <eigenbook/book/price_level.hpp>
#include <eigenbook/core/order.hpp>
#include <eigenbook/core/types.hpp>

namespace eigenbook {

// Price ordering of one side of the book: "better" is higher for bids, lower for asks.
template <OrderSide Side>
struct SideTraits;

template <>
struct SideTraits<OrderSide::BUY> {
    using Compare = std::greater<Price>;

    [[nodiscard]] static constexpr bool isBetter(const Price a, const Price b) noexcept {
        return a > b;
    }
};

template <>
struct SideTraits<OrderSide::SELL> {
    using Compare = std::less<Price>;

    [[nodiscard]] static constexpr bool isBetter(const Price a, const Price b) noexcept {
        return a < b;
    }
};

// What OrderBook needs from one side of the book. Both strategies from the README satisfy it.
template <typename L>
concept PriceLadder = requires(L ladder, const L cladder, Order& order, PriceLevel& level, Price p) {
    typename L::Config;
    { L::side } -> std::convertible_to<OrderSide>;
    { cladder.accepts(p) } noexcept -> std::same_as<bool>;
    ladder.addOrder(order);
    { ladder.removeOrder(order) } noexcept;
    { ladder.removeLevel(level) } noexcept;
    { ladder.bestLevel() } noexcept -> std::same_as<PriceLevel*>;
    { cladder.bestLevel() } noexcept -> std::same_as<const PriceLevel*>;
    { cladder.bestPrice() } noexcept -> std::same_as<std::optional<Price>>;
    { cladder.findLevel(p) } noexcept -> std::same_as<const PriceLevel*>;
    { cladder.levelCount() } noexcept -> std::same_as<std::size_t>;
    { cladder.empty() } noexcept -> std::same_as<bool>;
};

} // namespace eigenbook
