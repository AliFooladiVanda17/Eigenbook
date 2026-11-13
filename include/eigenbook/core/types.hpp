#pragma once

#include <compare>
#include <cstddef>
#include <cstdint>
#include <functional>

namespace eigenbook {

// Fixed instead of std::hardware_destructive_interference_size, which is not ABI-stable
// (GCC warns on it) and is 64 on every x86-64 / most ARM64 targets we care about.
inline constexpr std::size_t kCacheLineSize = 64;

// Type safety for Price. The value is expressed in ticks (integer), never in floating point.
struct Price {
    std::int64_t value;

    constexpr auto operator<=>(const Price&) const noexcept = default;

    constexpr Price operator+(const std::int64_t ticks) const noexcept { return {value + ticks}; }

    constexpr Price operator-(const std::int64_t ticks) const noexcept { return {value - ticks}; }

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

// The participant (trading account) that owns an order. Assigned by the gateway, which has already
// authenticated the session; the book only compares it. 0 is an ordinary value, the default for
// single-participant use.
struct ParticipantId {
    std::uint32_t value;

    constexpr auto operator<=>(const ParticipantId&) const noexcept = default;
};

// Identifies one execution. Per book, monotonic from 1; never reused.
struct TradeId {
    std::uint64_t value;

    constexpr auto operator<=>(const TradeId&) const noexcept = default;
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

enum class OrderSide : std::uint8_t {
    BUY,
    SELL
};

enum class OrderType : std::uint8_t {
    LIMIT,
    MARKET
};

// How long an order may stay in the book.
//   DAY  rests until the end of the trading day (expiry arrives with sessions, Phase B8).
//   GTC  rests until cancelled.
//   IOC  trades what it can on arrival; the rest is cancelled, never rests.
//   FOK  trades its whole quantity on arrival or nothing at all; never rests.
enum class TimeInForce : std::uint8_t {
    DAY,
    GTC,
    IOC,
    FOK
};

[[nodiscard]] constexpr OrderSide opposite(const OrderSide side) noexcept {
    return side == OrderSide::BUY ? OrderSide::SELL : OrderSide::BUY;
}

} // namespace eigenbook

// std::hash specialisations must be declared outside namespace eigenbook.
template <>
struct std::hash<eigenbook::OrderId> {
    std::size_t operator()(const eigenbook::OrderId& id) const noexcept {
        return std::hash<std::uint64_t>{}(id.value);
    }
};

template <>
struct std::hash<eigenbook::InstrumentId> {
    std::size_t operator()(const eigenbook::InstrumentId& id) const noexcept {
        return std::hash<std::uint32_t>{}(id.value);
    }
};

template <>
struct std::hash<eigenbook::ShardId> {
    std::size_t operator()(const eigenbook::ShardId& id) const noexcept {
        return std::hash<std::uint16_t>{}(id.value);
    }
};
