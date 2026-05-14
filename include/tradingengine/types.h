//
// Created by afouladi on 5/12/2026.
//

#ifndef HPC_QUEUE_INCLUDE_TRADINGENGINE_TYPES_H
#define HPC_QUEUE_INCLUDE_TRADINGENGINE_TYPES_H

// Type safety for Price
struct Price {
    std::int64_t value;

    auto operator<=>(const Price &) const = default;

    Price operator+(const std::int64_t ticks) const {
        return {value + ticks};
    }

    Price operator-(const std::int64_t ticks) const {
        return {value - ticks};
    }

};

// Type safety for Quantity
struct Quantity {
    std::uint64_t value;

    auto operator<=>(const Quantity &) const = default;

    Quantity operator+(const Quantity other) const {
        return {value + other.value};
    }

    Quantity operator-(const Quantity other) const {
        return {value - other.value};
    }

    Quantity& operator+=(const Quantity other) {
        value += other.value;
        return *this;
    }

    Quantity& operator-=(const Quantity other) {
        value -= other.value;
        return *this;
    }
};

//Type safety for OrderId
struct OrderId {
    std::uint64_t value;

    bool operator==(const OrderId &) const = default;
};

template<>
struct std::hash<OrderId> {
    std::size_t operator()(const OrderId& id) const noexcept {
        return std::hash<std::uint64_t>{}(id.value);
    }
};

// Type safety for Timestamp
struct Timestamp {
    std::uint64_t value;

    auto operator<=>(const Timestamp &) const = default;
};

enum class OrderSide {
    BUY,
    SELL
};

enum class OrderType {
    LIMIT,
    MARKET
};
#endif // HPC_QUEUE_INCLUDE_TRADINGENGINE_TYPES_H
