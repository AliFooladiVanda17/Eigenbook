#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>

#include <eigenbook/book/ladder/dense_ladder.hpp>
#include <eigenbook/book/ladder/hybrid_ladder.hpp>
#include <eigenbook/book/ladder/map_ladder.hpp>
#include <eigenbook/book/locator/direct_order_table.hpp>
#include <eigenbook/book/locator/hardened_hash_index.hpp>
#include <eigenbook/book/locator/hash_order_index.hpp>
#include <eigenbook/book/locator/order_locator.hpp>
#include <eigenbook/book/price_ladder.hpp>
#include <eigenbook/book/price_level.hpp>
#include <eigenbook/core/book_limits.hpp>
#include <eigenbook/core/checked_math.hpp>
#include <eigenbook/core/order.hpp>
#include <eigenbook/core/reject_reason.hpp>
#include <eigenbook/core/types.hpp>
#include <eigenbook/util/object_pool.hpp>

namespace eigenbook {

struct OrderRequest {
    OrderId id;
    OrderSide side;
    OrderType type;
    Price price{0}; // ignored for MARKET orders
    Quantity quantity;
    // Caller time, kept for reporting. Not used for priority: a resting order gets the book's own
    // priority sequence (Order::getTimestamp()); input time comes from the sequencer (Phase B3).
    Timestamp timestamp{0};
};

// New price and/or quantity for a resting order. Reducing the quantity at the same price keeps
// time priority; any other change re-enters the order (it may trade) at the back of its level.
struct ModifyRequest {
    OrderId id;
    Price price;
    Quantity quantity;
    Timestamp timestamp{0}; // caller time (see OrderRequest::timestamp)
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
// Ladder selects the price-level strategy (map M0 / M1 / M2, dense array, hybrid). Locator selects
// how cancel / modify find an order: OrderIndex (client ids, default), HardenedOrderIndex (client
// ids, seeded hash, bounded probes) or DirectOrderTable (handles issued by the book through
// reserveOrderId(); O(1) worst case).
template <template <OrderSide> class Ladder, typename Locator = OrderIndex>
    requires PriceLadder<Ladder<OrderSide::BUY>> && PriceLadder<Ladder<OrderSide::SELL>> &&
             OrderLocator<Locator>
class BasicOrderBook {
public:
    using BidLadder = Ladder<OrderSide::BUY>;
    using AskLadder = Ladder<OrderSide::SELL>;
    using LadderConfig = typename BidLadder::Config;
    using Result = std::expected<ExecutionResult, RejectReason>;

    // limits: hard per-instrument limits and the order storage to preallocate (BookLimits).
    explicit BasicOrderBook(InstrumentId iInstrument,
                            LadderConfig config = {},
                            BookLimits limits = {});

    // Shorthand for BookLimits{.reserveOrders = expectedOrders}: storage preallocated for
    // expectedOrders and allowed to grow beyond it.
    BasicOrderBook(InstrumentId iInstrument, LadderConfig config, std::size_t expectedOrders)
        : BasicOrderBook(iInstrument, config, BookLimits{.reserveOrders = expectedOrders}) {}

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

    [[nodiscard]] const Order* findOrder(const OrderId id) const noexcept { return index.find(id); }

    [[nodiscard]] const BidLadder& bids() const noexcept { return bidLadder; }

    [[nodiscard]] const AskLadder& asks() const noexcept { return askLadder; }

    [[nodiscard]] InstrumentId instrument() const noexcept { return instrumentId; }

    [[nodiscard]] const BookLimits& bookLimits() const noexcept { return limits; }

    // Number of resting orders on both sides.
    [[nodiscard]] std::size_t orderCount() const noexcept { return index.size(); }

    [[nodiscard]] bool empty() const noexcept { return index.empty(); }

    // Issuing locators only: reserves the id for the next addOrder (engine-assigned order
    // reference). CAPACITY_EXCEEDED when every slot is in use. An id that is reserved but never
    // added must be given back with releaseOrderId().
    [[nodiscard]] std::expected<OrderId, RejectReason> reserveOrderId() noexcept
        requires IssuingOrderLocator<Locator>
    {
        const std::optional<OrderId> id = index.issue();
        if (!id) [[unlikely]] {
            return std::unexpected(RejectReason::CAPACITY_EXCEEDED);
        }
        return *id;
    }

    bool releaseOrderId(const OrderId id) noexcept
        requires IssuingOrderLocator<Locator>
    {
        return index.release(id);
    }

    // Read-only view of the order locator, for diagnostics and benchmarks (cluster length, probe
    // statistics in EIGENBOOK_INSTRUMENT_INDEX builds). Not part of the matching interface.
    [[nodiscard]] const Locator& orderIndex() const noexcept { return index; }

private:
    // Fills `quantity` of an incoming order against the side opposite to TakerSide; returns what
    // is left unfilled.
    template <OrderSide TakerSide, typename OnFill>
    Quantity sweep(OrderId takerId, OrderType type, Price limit, Quantity quantity, OnFill& onFill);

    void rest(const OrderRequest& request, Quantity quantity);

    // Every check a new order must pass, before any mutation.
    [[nodiscard]] std::optional<RejectReason>
    validateNew(const OrderRequest& request) const noexcept;

    // Checks that `quantity` could rest at `price` on `side`: band, ladder range, notional, level
    // quantity, ladder capacity, order capacity. `replacing` is an order that is removed first
    // (modify re-entry); the level quantity, level and storage it frees are counted.
    [[nodiscard]] std::optional<RejectReason>
    validateResting(OrderSide side,
                    Price price,
                    Quantity quantity,
                    const Order* replacing) const noexcept;

    // Matches and rests a request that has passed validation. Cannot reject; with map ladder M0 a
    // new tree node can still throw std::bad_alloc.
    template <typename OnFill>
    Result execute(const OrderRequest& request, OnFill& onFill);

    [[nodiscard]] const PriceLevel* levelAt(const OrderSide side,
                                            const Price price) const noexcept {
        return side == OrderSide::BUY ? bidLadder.findLevel(price) : askLadder.findLevel(price);
    }

    void removeResting(Order& order) noexcept;

    // Takes a resting order out of its ladder and the pool but keeps its id valid (modify
    // re-entry): Locator::unbind instead of erase.
    void detachForReentry(Order& order) noexcept;

    [[nodiscard]] static RejectReason rejectFor(const Admission a) noexcept {
        switch (a) {
            case Admission::DUPLICATE:
                return RejectReason::DUPLICATE_ORDER_ID;
            case Admission::NOT_ISSUED:
                return RejectReason::INVALID_ORDER_ID;
            case Admission::FULL:
                return RejectReason::CAPACITY_EXCEEDED;
            case Admission::PROBE_LIMIT:
                return RejectReason::ORDER_ID_REJECTED;
            case Admission::OK:
                break;
        }
        return RejectReason::INVALID_ORDER_ID;
    }

    [[nodiscard]] bool accepts(const OrderSide side, const Price price) const noexcept {
        return side == OrderSide::BUY ? bidLadder.accepts(price) : askLadder.accepts(price);
    }

    [[nodiscard]] bool canRestAt(const OrderSide side, const Price price) const noexcept {
        return side == OrderSide::BUY ? bidLadder.canRestAt(price) : askLadder.canRestAt(price);
    }

    // A taker at `limit` can trade with a maker level at `makerPrice`.
    template <OrderSide TakerSide>
    [[nodiscard]] static constexpr bool crosses(const Price limit,
                                                const Price makerPrice) noexcept {
        if constexpr (TakerSide == OrderSide::BUY) {
            return makerPrice <= limit;
        } else {
            return makerPrice >= limit;
        }
    }

    InstrumentId instrumentId;
    BookLimits limits;
    ObjectPool<Order> pool;
    Locator index;
    BidLadder bidLadder;
    AskLadder askLadder;
    // Priority sequence of resting orders: stamped into Order::timestamp when an order rests or
    // re-enters. Monotonic per book; the engine-wide sequence arrives with the sequencer (B3).
    std::uint64_t prioritySequence{0};
};

// Sparse / unbounded prices (map ladder M0: nodes from a pool over the heap).
using OrderBook = BasicOrderBook<MapPriceLadder>;

// Sparse prices, bounded number of levels (MapLadderConfig::maxLevels), no allocation after
// construction: map ladder M1 (pool over a preallocated arena) and M2 (fixed-block free list).
using OrderBookM1 = BasicOrderBook<MapPriceLadderM1>;
using OrderBookM2 = BasicOrderBook<MapPriceLadderM2>;

// Dense, bounded tick range.
using DenseOrderBook = BasicOrderBook<ArrayPriceLadder>;

// Dense window around the touch, sparse overflow (HybridLadderConfig).
using HybridOrderBook = BasicOrderBook<HybridPriceLadder>;

// Other order locators (Phase A4). Hardened: client ids, keyed hash, bounded probes, fixed
// capacity. Direct: engine-issued handles (reserveOrderId), O(1) worst-case cancel.
using DenseOrderBookHardened = BasicOrderBook<ArrayPriceLadder, HardenedOrderIndex>;
using DenseOrderBookDirect = BasicOrderBook<ArrayPriceLadder, DirectOrderTable>;
using OrderBookM2Hardened = BasicOrderBook<MapPriceLadderM2, HardenedOrderIndex>;
using OrderBookM2Direct = BasicOrderBook<MapPriceLadderM2, DirectOrderTable>;

// ---------------------------------------------------------------------------------------------
// Implementation
// ---------------------------------------------------------------------------------------------

template <template <OrderSide> class Ladder, typename Locator>
    requires PriceLadder<Ladder<OrderSide::BUY>> && PriceLadder<Ladder<OrderSide::SELL>> &&
                 OrderLocator<Locator>
BasicOrderBook<Ladder, Locator>::BasicOrderBook(const InstrumentId iInstrument,
                                                const LadderConfig config,
                                                const BookLimits iLimits)
    : instrumentId(iInstrument)
    , limits(iLimits)
    , pool(std::clamp<std::size_t>(iLimits.orderCapacity(), 64, 4096))
    , index(iLimits.orderCapacity())
    , bidLadder(config)
    , askLadder(config) {
    pool.reserve(iLimits.orderCapacity());
}

template <template <OrderSide> class Ladder, typename Locator>
    requires PriceLadder<Ladder<OrderSide::BUY>> && PriceLadder<Ladder<OrderSide::SELL>> &&
             OrderLocator<Locator>
auto BasicOrderBook<Ladder, Locator>::validateNew(const OrderRequest& request) const noexcept
    -> std::optional<RejectReason> {
    if (request.quantity.isZero()) [[unlikely]] {
        return RejectReason::INVALID_QUANTITY;
    }
    if (request.quantity > limits.maxOrderQuantity) [[unlikely]] {
        return RejectReason::ORDER_QUANTITY_LIMIT;
    }
    // The id must be storable if the order ends up resting.
    if (const Admission admission = index.admit(request.id); admission != Admission::OK)
        [[unlikely]] {
        return rejectFor(admission);
    }
    if (request.type == OrderType::LIMIT) {
        return validateResting(request.side, request.price, request.quantity, nullptr);
    }
    return std::nullopt;
}

template <template <OrderSide> class Ladder, typename Locator>
    requires PriceLadder<Ladder<OrderSide::BUY>> && PriceLadder<Ladder<OrderSide::SELL>> &&
             OrderLocator<Locator>
auto BasicOrderBook<Ladder, Locator>::validateResting(const OrderSide side,
                                                      const Price price,
                                                      const Quantity quantity,
                                                      const Order* replacing) const noexcept
    -> std::optional<RejectReason> {
    if (price < limits.minPrice || price > limits.maxPrice) [[unlikely]] {
        return RejectReason::PRICE_OUT_OF_BAND;
    }
    if (!accepts(side, price)) [[unlikely]] {
        return RejectReason::PRICE_OUT_OF_RANGE;
    }
    if (!notionalWithin(price, quantity, limits.maxNotional)) [[unlikely]] {
        return RejectReason::NOTIONAL_LIMIT;
    }
    // Conservative (all checks below): they assume the whole quantity rests, so an order that
    // would trade in full can be rejected by them too.
    const PriceLevel* level = levelAt(side, price);
    const PriceLevel* ownLevel =
        replacing != nullptr ? PriceLevel::ownerOf(const_cast<Order&>(*replacing)) : nullptr;
    Quantity atLevel = level != nullptr ? level->quantity() : Quantity{0};
    if (level != nullptr && level == ownLevel) {
        atLevel -= replacing->getQuantity();
    }
    if (!sumWithin(atLevel, quantity, limits.maxLevelQuantity)) [[unlikely]] {
        return RejectReason::LEVEL_QUANTITY_LIMIT;
    }
    // A full ladder only takes the order if its level exists, or if the order being replaced frees
    // a level on this side.
    const bool freesOwnLevel = ownLevel != nullptr && ownLevel->orderCount() == 1;
    if (!canRestAt(side, price) && !freesOwnLevel) [[unlikely]] {
        return RejectReason::CAPACITY_EXCEEDED;
    }
    if (replacing == nullptr && limits.maxOrders != 0 && index.size() >= limits.maxOrders)
        [[unlikely]] {
        return RejectReason::CAPACITY_EXCEEDED;
    }
    return std::nullopt;
}

template <template <OrderSide> class Ladder, typename Locator>
    requires PriceLadder<Ladder<OrderSide::BUY>> && PriceLadder<Ladder<OrderSide::SELL>> &&
             OrderLocator<Locator>
template <typename OnFill>
auto BasicOrderBook<Ladder, Locator>::addOrder(const OrderRequest& request, OnFill&& onFill)
    -> Result {
    // Every check happens before any mutation: a rejected order leaves the book untouched.
    if (const std::optional<RejectReason> reject = validateNew(request)) [[unlikely]] {
        return std::unexpected(*reject);
    }
    return execute(request, onFill);
}

template <template <OrderSide> class Ladder, typename Locator>
    requires PriceLadder<Ladder<OrderSide::BUY>> && PriceLadder<Ladder<OrderSide::SELL>> &&
             OrderLocator<Locator>
template <typename OnFill>
auto BasicOrderBook<Ladder, Locator>::execute(const OrderRequest& request, OnFill& onFill)
    -> Result {
    const Quantity remaining = request.side == OrderSide::BUY
                                   ? sweep<OrderSide::BUY>(request.id,
                                                           request.type,
                                                           request.price,
                                                           request.quantity,
                                                           onFill)
                                   : sweep<OrderSide::SELL>(request.id,
                                                            request.type,
                                                            request.price,
                                                            request.quantity,
                                                            onFill);

    ExecutionResult result{.filled = request.quantity - remaining};
    if (remaining.isZero()) {
        index.abandon(request.id);
        return result;
    }
    if (request.type == OrderType::MARKET) {
        index.abandon(request.id);
        result.cancelled = remaining;
        return result;
    }
    rest(request, remaining);
    result.resting = remaining;
    return result;
}

template <template <OrderSide> class Ladder, typename Locator>
    requires PriceLadder<Ladder<OrderSide::BUY>> && PriceLadder<Ladder<OrderSide::SELL>> &&
             OrderLocator<Locator>
auto BasicOrderBook<Ladder, Locator>::cancelOrder(const OrderId id)
    -> std::expected<Quantity, RejectReason> {
    Order* order = index.find(id);
    if (order == nullptr) [[unlikely]] {
        return std::unexpected(RejectReason::UNKNOWN_ORDER_ID);
    }
    const Quantity open = order->getQuantity();
    removeResting(*order);
    return open;
}

template <template <OrderSide> class Ladder, typename Locator>
    requires PriceLadder<Ladder<OrderSide::BUY>> && PriceLadder<Ladder<OrderSide::SELL>> &&
             OrderLocator<Locator>
template <typename OnFill>
auto BasicOrderBook<Ladder, Locator>::modifyOrder(const ModifyRequest& request, OnFill&& onFill)
    -> Result {
    Order* order = index.find(request.id);
    if (order == nullptr) [[unlikely]] {
        return std::unexpected(RejectReason::UNKNOWN_ORDER_ID);
    }
    if (request.quantity.isZero()) [[unlikely]] {
        return std::unexpected(RejectReason::INVALID_QUANTITY);
    }
    if (request.quantity > limits.maxOrderQuantity) [[unlikely]] {
        return std::unexpected(RejectReason::ORDER_QUANTITY_LIMIT);
    }

    if (request.price == order->getPrice() && request.quantity <= order->getQuantity()) {
        // Same price, not larger: O(1) in place, time priority kept. Only lowers aggregates.
        PriceLevel::ownerOf(*order)->reduce(*order, request.quantity);
        return ExecutionResult{.resting = request.quantity};
    }

    // Price change or size increase: the order loses priority and re-enters (it may cross).
    // All-or-nothing: the re-entry is validated completely, counting what the order itself frees,
    // before the order is touched; afterwards nothing can reject it.
    if (const std::optional<RejectReason> reject =
            validateResting(order->getSide(), request.price, request.quantity, order))
        [[unlikely]] {
        return std::unexpected(*reject);
    }
    const OrderRequest reentry{.id = request.id,
                               .side = order->getSide(),
                               .type = OrderType::LIMIT,
                               .price = request.price,
                               .quantity = request.quantity,
                               .timestamp = request.timestamp};
    detachForReentry(*order); // the locator keeps the id valid (unbind), so no admission is needed
    return execute(reentry, onFill);
}

template <template <OrderSide> class Ladder, typename Locator>
    requires PriceLadder<Ladder<OrderSide::BUY>> && PriceLadder<Ladder<OrderSide::SELL>> &&
             OrderLocator<Locator>
template <OrderSide TakerSide, typename OnFill>
Quantity BasicOrderBook<Ladder, Locator>::sweep(const OrderId takerId,
                                                const OrderType type,
                                                const Price limit,
                                                Quantity quantity,
                                                OnFill& onFill) {
    auto& makers = [this]() -> auto& {
        if constexpr (TakerSide == OrderSide::BUY) {
            return askLadder;
        } else {
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
            } else {
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

template <template <OrderSide> class Ladder, typename Locator>
    requires PriceLadder<Ladder<OrderSide::BUY>> && PriceLadder<Ladder<OrderSide::SELL>> &&
             OrderLocator<Locator>
void BasicOrderBook<Ladder, Locator>::rest(const OrderRequest& request, const Quantity quantity) {
    Order* order = pool.create(request.id,
                               request.side,
                               OrderType::LIMIT,
                               request.price,
                               quantity,
                               Timestamp{++prioritySequence});
    // Index growth or a new tree level can allocate; never leave a half-linked node behind.
    try {
        index.insert(request.id, order);
    } catch (...) {
        pool.destroy(order);
        throw;
    }
    try {
        if (request.side == OrderSide::BUY) {
            bidLadder.addOrder(*order);
        } else {
            askLadder.addOrder(*order);
        }
    } catch (...) {
        index.erase(request.id);
        pool.destroy(order);
        throw;
    }
}

template <template <OrderSide> class Ladder, typename Locator>
    requires PriceLadder<Ladder<OrderSide::BUY>> && PriceLadder<Ladder<OrderSide::SELL>> &&
             OrderLocator<Locator>
void BasicOrderBook<Ladder, Locator>::removeResting(Order& order) noexcept {
    if (order.getSide() == OrderSide::BUY) {
        bidLadder.removeOrder(order);
    } else {
        askLadder.removeOrder(order);
    }
    index.erase(order.getId());
    pool.destroy(&order);
}

template <template <OrderSide> class Ladder, typename Locator>
    requires PriceLadder<Ladder<OrderSide::BUY>> && PriceLadder<Ladder<OrderSide::SELL>> &&
             OrderLocator<Locator>
void BasicOrderBook<Ladder, Locator>::detachForReentry(Order& order) noexcept {
    if (order.getSide() == OrderSide::BUY) {
        bidLadder.removeOrder(order);
    } else {
        askLadder.removeOrder(order);
    }
    index.unbind(order.getId());
    pool.destroy(&order);
}

} // namespace eigenbook
