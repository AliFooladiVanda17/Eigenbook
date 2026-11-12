//
// Created by afouladi on 5/12/2026.
//

#ifndef HPC_QUEUE_INCLUDE_TRADINGENGINE_TYPES_H
#define HPC_QUEUE_INCLUDE_TRADINGENGINE_TYPES_H

#include <compare>
#include <cstddef>
#include <cstdint>
#include <functional>

// Fixed instead of std::hardware_destructive_interference_size, which is not ABI-stable
// (GCC warns on it) and is 64 on every x86-64 / most ARM64 targets we care about.
inline constexpr std::size_t kCacheLineSize = 64;

// Type safety for Price. The value is expressed in ticks (integer), never in floating point.
struct Price {
    std::int64_t value;

    constexpr auto operator<=>(const Price&) const noexcept = default;

    constexpr Price operator+(const std::int64_t ticks) const noexcept {
        return {value + ticks};
    }

    constexpr Price operator-(const std::int64_t ticks) const noexcept {
        return {value - ticks};
    }

    // Distance between two prices in ticks.
    constexpr std::int64_t operator-(const Price other) const noexcept {
        return value - other.value;
    }
};

// Type safety for Quantity
struct Quantity {
    std::uint64_t value;

    constexpr auto operator<=>(const Quantity&) const noexcept = default;

    constexpr Quantity operator+(const Quantity other) const noexcept {
        return {value + other.value};
    }

    constexpr Quantity operator-(const Quantity other) const noexcept {
        return {value - other.value};
    }

    constexpr Quantity& operator+=(const Quantity other) noexcept {
        value += other.value;
        return *this;
    }

    constexpr Quantity& operator-=(const Quantity other) noexcept {
        value -= other.value;
        return *this;
    }

    [[nodiscard]] constexpr bool isZero() const noexcept { return value == 0; }
};

// Type safety for OrderId
struct OrderId {
    std::uint64_t value;

    constexpr bool operator==(const OrderId&) const noexcept = default;
};

// Type safety for Timestamp
struct Timestamp {
    std::uint64_t value;

    constexpr auto operator<=>(const Timestamp&) const noexcept = default;
};

// Stable, pre-resolved instrument handle (Symbol -> InstrumentId happens on the cold path).
struct InstrumentId {
    std::uint32_t value;

    constexpr auto operator<=>(const InstrumentId&) const noexcept = default;
};

// Index of the shard (thread) that owns a set of instruments.
struct ShardId {
    std::uint16_t value;

    constexpr auto operator<=>(const ShardId&) const noexcept = default;
};

template <>
struct std::hash<OrderId> {
    std::size_t operator()(const OrderId& id) const noexcept {
        return std::hash<std::uint64_t>{}(id.value);
    }
};

template <>
struct std::hash<InstrumentId> {
    std::size_t operator()(const InstrumentId& id) const noexcept {
        return std::hash<std::uint32_t>{}(id.value);
    }
};

template <>
struct std::hash<ShardId> {
    std::size_t operator()(const ShardId& id) const noexcept {
        return std::hash<std::uint16_t>{}(id.value);
    }
};

enum class OrderSide : std::uint8_t {
    BUY,
    SELL
};

enum class OrderType : std::uint8_t {
    LIMIT,
    MARKET
};

[[nodiscard]] constexpr OrderSide opposite(const OrderSide side) noexcept {
    return side == OrderSide::BUY ? OrderSide::SELL : OrderSide::BUY;
}

#endif // HPC_QUEUE_INCLUDE_TRADINGENGINE_TYPES_H
