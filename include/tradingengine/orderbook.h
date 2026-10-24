//
// Created by afouladi on 5/12/2026.
//

#ifndef HPC_QUEUE_SRC_BOOK_ORDERBOOK_H
#define HPC_QUEUE_SRC_BOOK_ORDERBOOK_H

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <tradingengine/OrderIndex.h>
#include <tradingengine/PriceLadder.h>
#include <tradingengine/PriceLevel.h>
#include <tradingengine/order.h>
#include <tradingengine/types.h>
#include <tradingengine/util/ObjectPool.h>

enum class RejectReason : std::uint8_t {
    INVALID_QUANTITY,   // zero quantity
    DUPLICATE_ORDER_ID, // id already resting in this book
    PRICE_OUT_OF_RANGE, // limit price outside the ladder's tick range (dense ladder only)
    UNKNOWN_ORDER_ID,   // cancel / modify of an order that is not resting
};

struct OrderRequest {
    OrderId id;
    OrderSide side;
    OrderType type;
    Price price{0}; // ignored for MARKET orders
    Quantity quantity;
    Timestamp timestamp{0};
};

// New price and/or quantity for a resting order. Reducing the quantity at the same price keeps
// time priority; any other change re-enters the order (it may trade) at the back of its level.
struct ModifyRequest {
    OrderId id;
    Price price;
    Quantity quantity;
    Timestamp timestamp{0}; // arrival time used if the order loses priority
};

// One execution between the incoming (taker) and a resting (maker) order, at the maker's price.
struct Fill {
    OrderId makerId;
    OrderId takerId;
    Price price;
    Quantity quantity;
    OrderSide takerSide;
    bool makerFilled; // the maker left the book
};

// How the quantity of an accepted request was disposed of: filled + resting + cancelled equals
// the requested quantity.
struct ExecutionResult {
    Quantity filled{0};
    Quantity resting{0};   // limit remainder now in the book
    Quantity cancelled{0}; // market remainder with no more liquidity

    [[nodiscard]] bool fullyFilled() const noexcept {
        return resting.isZero() && cancelled.isZero();
    }
};

struct NoFillHandler {
    void operator()(const Fill&) const noexcept {}
};

// Limit order book for one instrument.
//
// Single owner: it is mutated by exactly one (shard) thread and has no internal locking. Fills are
// reported synchronously through a callback, so matching never allocates; resting orders live in
// a pool and are found through a flat order index.
//
// Ladder selects the price-level strategy: MapPriceLadder (sparse prices) or ArrayPriceLadder
// (dense, bounded tick range).
template <template <OrderSide> class Ladder>
    requires PriceLadder<Ladder<OrderSide::BUY>> && PriceLadder<Ladder<OrderSide::SELL>>
class BasicOrderBook {
public:
    using BidLadder = Ladder<OrderSide::BUY>;
    using AskLadder = Ladder<OrderSide::SELL>;
    using LadderConfig = typename BidLadder::Config;
    using Result = std::expected<ExecutionResult, RejectReason>;

    // expectedOrders sizes the order pool and index so steady-state trading does not allocate.
    explicit BasicOrderBook(InstrumentId iInstrument,
                            LadderConfig config = {},
                            std::size_t expectedOrders = 1024);

    // Orders, levels and the index point at each other: a book never changes address.
    BasicOrderBook(const BasicOrderBook&) = delete;
    BasicOrderBook& operator=(const BasicOrderBook&) = delete;
    BasicOrderBook(BasicOrderBook&&) = delete;
    BasicOrderBook& operator=(BasicOrderBook&&) = delete;

    ~BasicOrderBook() = default;

    // Matches against the opposite side by price-time priority. A limit remainder rests in the
    // book; a market remainder is cancelled. onFill(const Fill&) is called once per execution.
    template <typename OnFill>
    Result addOrder(const OrderRequest& request, OnFill&& onFill);

    Result addOrder(const OrderRequest& request) { return addOrder(request, NoFillHandler{}); }

    // Removes a resting order in O(1); returns the quantity that was still open.
    std::expected<Quantity, RejectReason> cancelOrder(OrderId id);

    template <typename OnFill>
    Result modifyOrder(const ModifyRequest& request, OnFill&& onFill);

    Result modifyOrder(const ModifyRequest& request) {
        return modifyOrder(request, NoFillHandler{});
    }

    [[nodiscard]] std::optional<Price> bestBid() const noexcept { return bidLadder.bestPrice(); }

    [[nodiscard]] std::optional<Price> bestAsk() const noexcept { return askLadder.bestPrice(); }

    [[nodiscard]] const Order* findOrder(const OrderId id) const noexcept {
        return index.find(id);
    }

    [[nodiscard]] const BidLadder& bids() const noexcept { return bidLadder; }

    [[nodiscard]] const AskLadder& asks() const noexcept { return askLadder; }

    [[nodiscard]] InstrumentId instrument() const noexcept { return instrumentId; }

    // Number of resting orders on both sides.
    [[nodiscard]] std::size_t orderCount() const noexcept { return index.size(); }

    [[nodiscard]] bool empty() const noexcept { return index.empty(); }

private:
    // Fills `quantity` of an incoming order against the side opposite to TakerSide; returns what
    // is left unfilled.
    template <OrderSide TakerSide, typename OnFill>
    Quantity sweep(OrderId takerId, OrderType type, Price limit, Quantity quantity, OnFill& onFill);

    void rest(const OrderRequest& request, Quantity quantity);

    void removeResting(Order& order) noexcept;

    [[nodiscard]] bool accepts(const OrderSide side, const Price price) const noexcept {
        return side == OrderSide::BUY ? bidLadder.accepts(price) : askLadder.accepts(price);
    }

    // A taker at `limit` can trade with a maker level at `makerPrice`.
    template <OrderSide TakerSide>
    [[nodiscard]] static constexpr bool crosses(const Price limit, const Price makerPrice) noexcept {
        if constexpr (TakerSide == OrderSide::BUY) {
            return makerPrice <= limit;
        }
        else {
            return makerPrice >= limit;
        }
    }

    InstrumentId instrumentId;
    ObjectPool<Order> pool;
    OrderIndex index;
    BidLadder bidLadder;
    AskLadder askLadder;
};

// Sparse / unbounded prices.
using OrderBook = BasicOrderBook<MapPriceLadder>;

// Dense, bounded tick range.
using DenseOrderBook = BasicOrderBook<ArrayPriceLadder>;

// ---------------------------------------------------------------------------------------------
// Implementation
// ---------------------------------------------------------------------------------------------

template <template <OrderSide> class Ladder>
    requires PriceLadder<Ladder<OrderSide::BUY>> && PriceLadder<Ladder<OrderSide::SELL>>
BasicOrderBook<Ladder>::BasicOrderBook(const InstrumentId iInstrument,
                                       const LadderConfig config,
                                       const std::size_t expectedOrders)
    : instrumentId(iInstrument)
    , pool(std::clamp<std::size_t>(expectedOrders, 64, 4096))
    , index(expectedOrders)
    , bidLadder(config)
    , askLadder(config) {
    pool.reserve(expectedOrders);
}

template <template <OrderSide> class Ladder>
    requires PriceLadder<Ladder<OrderSide::BUY>> && PriceLadder<Ladder<OrderSide::SELL>>
template <typename OnFill>
auto BasicOrderBook<Ladder>::addOrder(const OrderRequest& request, OnFill&& onFill) -> Result {
    if (request.quantity.isZero()) [[unlikely]] {
        return std::unexpected(RejectReason::INVALID_QUANTITY);
    }
    if (index.contains(request.id)) [[unlikely]] {
        return std::unexpected(RejectReason::DUPLICATE_ORDER_ID);
    }
    if (request.type == OrderType::LIMIT && !accepts(request.side, request.price)) [[unlikely]] {
        return std::unexpected(RejectReason::PRICE_OUT_OF_RANGE);
    }

    const Quantity remaining =
        request.side == OrderSide::BUY
            ? sweep<OrderSide::BUY>(
                  request.id, request.type, request.price, request.quantity, onFill)
            : sweep<OrderSide::SELL>(
                  request.id, request.type, request.price, request.quantity, onFill);

    ExecutionResult result{.filled = request.quantity - remaining};
    if (remaining.isZero()) {
        return result;
    }
    if (request.type == OrderType::MARKET) {
        result.cancelled = remaining;
        return result;
    }
    rest(request, remaining);
    result.resting = remaining;
    return result;
}

template <template <OrderSide> class Ladder>
    requires PriceLadder<Ladder<OrderSide::BUY>> && PriceLadder<Ladder<OrderSide::SELL>>
auto BasicOrderBook<Ladder>::cancelOrder(const OrderId id) -> std::expected<Quantity, RejectReason> {
    Order* order = index.find(id);
    if (order == nullptr) [[unlikely]] {
        return std::unexpected(RejectReason::UNKNOWN_ORDER_ID);
    }
    const Quantity open = order->getQuantity();
    removeResting(*order);
    return open;
}

template <template <OrderSide> class Ladder>
    requires PriceLadder<Ladder<OrderSide::BUY>> && PriceLadder<Ladder<OrderSide::SELL>>
template <typename OnFill>
auto BasicOrderBook<Ladder>::modifyOrder(const ModifyRequest& request, OnFill&& onFill) -> Result {
    Order* order = index.find(request.id);
    if (order == nullptr) [[unlikely]] {
        return std::unexpected(RejectReason::UNKNOWN_ORDER_ID);
    }
    if (request.quantity.isZero()) [[unlikely]] {
        return std::unexpected(RejectReason::INVALID_QUANTITY);
    }

    if (request.price == order->getPrice()) {
        if (request.quantity <= order->getQuantity()) {
            // Same price, not larger: O(1) in place, time priority kept.
            PriceLevel::ownerOf(*order)->reduce(*order, request.quantity);
            return ExecutionResult{.resting = request.quantity};
        }
    }
    else if (!accepts(order->getSide(), request.price)) [[unlikely]] {
        return std::unexpected(RejectReason::PRICE_OUT_OF_RANGE);
    }

    // Price change or size increase: the order loses priority and re-enters (it may cross).
    const OrderRequest reentry{.id = request.id,
                               .side = order->getSide(),
                               .type = OrderType::LIMIT,
                               .price = request.price,
                               .quantity = request.quantity,
                               .timestamp = request.timestamp};
    removeResting(*order);
    return addOrder(reentry, std::forward<OnFill>(onFill));
}

template <template <OrderSide> class Ladder>
    requires PriceLadder<Ladder<OrderSide::BUY>> && PriceLadder<Ladder<OrderSide::SELL>>
template <OrderSide TakerSide, typename OnFill>
Quantity BasicOrderBook<Ladder>::sweep(const OrderId takerId,
                                       const OrderType type,
                                       const Price limit,
                                       Quantity quantity,
                                       OnFill& onFill) {
    auto& makers = [this]() -> auto& {
        if constexpr (TakerSide == OrderSide::BUY) {
            return askLadder;
        }
        else {
            return bidLadder;
        }
    }();

    while (!quantity.isZero()) {
        PriceLevel* level = makers.bestLevel();
        if (level == nullptr) {
            break;
        }
        const Price levelPrice = level->price();
        if (type == OrderType::LIMIT && !crosses<TakerSide>(limit, levelPrice)) {
            break;
        }

        // Consume the level in FIFO order.
        do {
            Order& maker = *level->front();
            const Quantity open = maker.getQuantity();
            const Quantity traded = std::min(quantity, open);
            const bool makerFilled = traded == open;
            quantity -= traded;

            onFill(Fill{.makerId = maker.getId(),
                        .takerId = takerId,
                        .price = levelPrice,
                        .quantity = traded,
                        .takerSide = TakerSide,
                        .makerFilled = makerFilled});

            if (makerFilled) {
                level->popFront();
                index.erase(maker.getId());
                pool.destroy(&maker);
            }
            else {
                level->fillFront(traded);
            }
        } while (!quantity.isZero() && !level->empty());

        // Empty levels are removed immediately.
        if (level->empty()) {
            makers.removeLevel(*level);
        }
    }
    return quantity;
}

template <template <OrderSide> class Ladder>
    requires PriceLadder<Ladder<OrderSide::BUY>> && PriceLadder<Ladder<OrderSide::SELL>>
void BasicOrderBook<Ladder>::rest(const OrderRequest& request, const Quantity quantity) {
    Order* order = pool.create(request.id,
                               request.side,
                               OrderType::LIMIT,
                               request.price,
                               quantity,
                               request.timestamp);
    // Index growth or a new tree level can allocate; never leave a half-linked node behind.
    try {
        index.insert(request.id, order);
    }
    catch (...) {
        pool.destroy(order);
        throw;
    }
    try {
        if (request.side == OrderSide::BUY) {
            bidLadder.addOrder(*order);
        }
        else {
            askLadder.addOrder(*order);
        }
    }
    catch (...) {
        index.erase(request.id);
        pool.destroy(order);
        throw;
    }
}

template <template <OrderSide> class Ladder>
    requires PriceLadder<Ladder<OrderSide::BUY>> && PriceLadder<Ladder<OrderSide::SELL>>
void BasicOrderBook<Ladder>::removeResting(Order& order) noexcept {
    if (order.getSide() == OrderSide::BUY) {
        bidLadder.removeOrder(order);
    }
    else {
        askLadder.removeOrder(order);
    }
    index.erase(order.getId());
    pool.destroy(&order);
}

// The two supported books are compiled once, in orderbook.cpp.
extern template class BasicOrderBook<MapPriceLadder>;
extern template class BasicOrderBook<ArrayPriceLadder>;

#endif // HPC_QUEUE_SRC_BOOK_ORDERBOOK_H
