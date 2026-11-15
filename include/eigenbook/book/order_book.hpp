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
    ParticipantId participant{0}; // owner of the order; checked by cancel and modify

    // Qualifiers. A MARKET order never rests, whatever its time in force (DAY / GTC act as IOC).
    TimeInForce timeInForce{TimeInForce::DAY};
    // Rejected with POST_ONLY_WOULD_TRADE instead of trading; LIMIT with DAY / GTC only.
    bool postOnly{false};
    // At least this much must trade on arrival, or the whole order is cancelled without touching
    // the book. 0: no minimum. FOK is the case minQuantity == quantity, plus IOC.
    Quantity minQuantity{0};
};

// New price and/or quantity for a resting order. Reducing the quantity at the same price keeps
// time priority; any other change re-enters the order (it may trade) at the back of its level.
struct ModifyRequest {
    OrderId id;
    Price price;
    Quantity quantity;
    Timestamp timestamp{0};       // caller time (see OrderRequest::timestamp)
    ParticipantId participant{0}; // must own the order (NOT_OWNER)
};

// One execution between the incoming (taker) and a resting (maker) order, at the maker's price.
struct Fill {
    OrderId makerId;
    OrderId takerId;
    Price price;
    Quantity quantity;
    OrderSide takerSide;
    bool makerFilled;                  // the maker left the book
    TradeId tradeId{0};                // per book, monotonic from 1
    ParticipantId makerParticipant{0}; // owner of the maker order (its execution report)
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

// Why a book is halted.
enum class HaltReason : std::uint8_t {
    NONE,
    MANUAL,       // halt(); resume() lifts it
    AUDIT_FAILED, // an invariant check failed; resume() refuses, the book must be rebuilt
};

// The first broken invariant checkInvariants() found.
enum class InvariantViolation : std::uint8_t {
    LADDER,      // a ladder's own structure or a queue in it (links, counts, aggregates)
    ORDER_SIDE,  // an order rests in the other side's ladder
    LOCATOR,     // the locator does not map a resting order's id to that order
    PRIORITY,    // priority stamps not strictly increasing in a queue, or ahead of the book
    LIMITS,      // a resting order or level outside BookLimits
    ORDER_COUNT, // orders in the ladders, the locator and the pool differ, or exceed maxOrders
    CROSSED,     // best bid >= best ask
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

    // Participant cancel: removes a resting order of `participant` in O(1) and returns the quantity
    // that was still open. NOT_OWNER if another participant owns it (the order is untouched).
    std::expected<Quantity, RejectReason> cancelOrder(OrderId id, ParticipantId participant);

    // Administrative cancel (engine, risk, kill switch): no ownership check. Not for requests that
    // arrive from a participant session.
    std::expected<Quantity, RejectReason> cancelOrder(OrderId id);

    // Only the owner (request.participant) may modify; NOT_OWNER otherwise.
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

    // Id of the most recent execution; TradeId{0} before the first one.
    [[nodiscard]] TradeId lastTradeId() const noexcept { return {tradeSequence}; }

    // --- Halt and audit ---------------------------------------------------------------------
    // While halted, addOrder and modifyOrder are rejected with INSTRUMENT_HALTED before any other
    // check; cancels (participant and administrative) still work, so members can pull orders.

    void halt() noexcept {
        if (haltState == HaltReason::NONE) {
            haltState = HaltReason::MANUAL;
        }
    }

    // Lifts a manual halt. Returns false, and stays halted, after a failed audit.
    bool resume() noexcept {
        if (haltState == HaltReason::AUDIT_FAILED) {
            return false;
        }
        haltState = HaltReason::NONE;
        return true;
    }

    [[nodiscard]] bool isHalted() const noexcept { return haltState != HaltReason::NONE; }

    [[nodiscard]] HaltReason haltReason() const noexcept { return haltState; }

    // Full consistency check (cold path, O(orders); dense ladder O(ticks + orders)). Read-only.
    // nullopt when every invariant holds, otherwise the first violation found.
    [[nodiscard]] std::optional<InvariantViolation> checkInvariants() const noexcept;

    // checkInvariants(), and on a violation halt with AUDIT_FAILED and remember it.
    std::optional<InvariantViolation> audit() noexcept;

    // The violation of the last failed audit, if any.
    [[nodiscard]] std::optional<InvariantViolation> lastViolation() const noexcept {
        return violation;
    }

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

    // Time in force, post-only and minimum quantity fit together (INVALID_QUALIFIERS otherwise).
    [[nodiscard]] static bool validQualifiers(const OrderRequest& request) noexcept;

    // The remainder of this request would rest (LIMIT, DAY or GTC).
    [[nodiscard]] static bool mayRest(const OrderRequest& request) noexcept {
        return request.type == OrderType::LIMIT &&
               (request.timeInForce == TimeInForce::DAY || request.timeInForce == TimeInForce::GTC);
    }

    // Checks on a limit price, whether or not the order can rest: band, ladder range, notional.
    [[nodiscard]] std::optional<RejectReason>
    validatePrice(OrderSide side, Price price, Quantity quantity) const noexcept;

    // Checks that `quantity` has room to rest at `price` on `side`: level quantity, ladder
    // capacity, order capacity. `replacing` is an order that is removed first (modify re-entry);
    // the level quantity, level and storage it frees are counted.
    [[nodiscard]] std::optional<RejectReason> validateRoom(OrderSide side,
                                                           Price price,
                                                           Quantity quantity,
                                                           const Order* replacing) const noexcept;

    // validatePrice and validateRoom.
    [[nodiscard]] std::optional<RejectReason>
    validateResting(OrderSide side,
                    Price price,
                    Quantity quantity,
                    const Order* replacing) const noexcept;

    // An order on `side` at limit `price` would trade with the best opposite level.
    [[nodiscard]] bool wouldTake(const OrderSide side, const Price price) const noexcept {
        if (side == OrderSide::BUY) {
            const std::optional<Price> ask = askLadder.bestPrice();
            return ask && *ask <= price;
        }
        const std::optional<Price> bid = bidLadder.bestPrice();
        return bid && *bid >= price;
    }

    // Read-only: the opposite side holds at least `needed` that a taker of `type` at `limit`
    // could trade with. Walks levels best to worst and stops as soon as the answer is known.
    template <OrderSide TakerSide>
    [[nodiscard]] bool canTake(OrderType type, Price limit, Quantity needed) const noexcept;

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
    // Last TradeId issued.
    std::uint64_t tradeSequence{0};
    HaltReason haltState{HaltReason::NONE};
    std::optional<InvariantViolation> violation;
    // Accepted mutations since the last periodic audit (BookLimits::auditEvery).
    std::uint32_t sinceAudit{0};

    // Called after every accepted mutation: one compare when auditing is off.
    void afterMutation() noexcept {
        if (limits.auditEvery != 0 && ++sinceAudit >= limits.auditEvery) [[unlikely]] {
            sinceAudit = 0;
            (void)audit();
        }
    }
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
auto BasicOrderBook<Ladder, Locator>::checkInvariants() const noexcept
    -> std::optional<InvariantViolation> {
    // Structure first: the order walk below relies on consistent queues.
    if (!bidLadder.checkInvariants() || !askLadder.checkInvariants()) {
        return InvariantViolation::LADDER;
    }
    std::size_t orders = 0;
    std::optional<InvariantViolation> found;
    const auto checkSide = [&](const auto& ladder, const OrderSide side) {
        ladder.forEachLevelWhile([&](const PriceLevel& level) {
            if (level.quantity() > limits.maxLevelQuantity) {
                found = InvariantViolation::LIMITS;
                return false;
            }
            std::uint64_t previousStamp = 0;
            for (const Order* order = level.front(); order != nullptr;
                 order = order->nextInLevel()) {
                ++orders;
                const std::uint64_t stamp = order->getTimestamp().value;
                if (order->getSide() != side) {
                    found = InvariantViolation::ORDER_SIDE;
                } else if (index.find(order->getId()) != order) {
                    found = InvariantViolation::LOCATOR;
                } else if (stamp <= previousStamp || stamp > prioritySequence) {
                    found = InvariantViolation::PRIORITY;
                } else if (order->getQuantity() > limits.maxOrderQuantity ||
                           order->getPrice() < limits.minPrice ||
                           order->getPrice() > limits.maxPrice ||
                           !notionalWithin(order->getPrice(),
                                           order->getQuantity(),
                                           limits.maxNotional)) {
                    found = InvariantViolation::LIMITS;
                }
                if (found) {
                    return false;
                }
                previousStamp = stamp;
            }
            return true;
        });
    };
    checkSide(bidLadder, OrderSide::BUY);
    if (!found) {
        checkSide(askLadder, OrderSide::SELL);
    }
    if (found) {
        return found;
    }
    // Every resting order is in exactly one queue, one locator slot and one pool node.
    if (orders != index.size() || orders != pool.size() ||
        (limits.maxOrders != 0 && orders > limits.maxOrders)) {
        return InvariantViolation::ORDER_COUNT;
    }
    const std::optional<Price> bid = bidLadder.bestPrice();
    const std::optional<Price> ask = askLadder.bestPrice();
    if (bid && ask && *bid >= *ask) {
        return InvariantViolation::CROSSED;
    }
    return std::nullopt;
}

template <template <OrderSide> class Ladder, typename Locator>
    requires PriceLadder<Ladder<OrderSide::BUY>> && PriceLadder<Ladder<OrderSide::SELL>> &&
             OrderLocator<Locator>
auto BasicOrderBook<Ladder, Locator>::audit() noexcept -> std::optional<InvariantViolation> {
    const std::optional<InvariantViolation> result = checkInvariants();
    if (result) [[unlikely]] {
        violation = result;
        haltState = HaltReason::AUDIT_FAILED;
    }
    return result;
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
    if (!validQualifiers(request)) [[unlikely]] {
        return RejectReason::INVALID_QUALIFIERS;
    }
    // The id must be storable if the order ends up resting.
    if (const Admission admission = index.admit(request.id); admission != Admission::OK)
        [[unlikely]] {
        return rejectFor(admission);
    }
    if (request.type == OrderType::LIMIT) {
        if (const std::optional<RejectReason> reject =
                validatePrice(request.side, request.price, request.quantity)) [[unlikely]] {
            return reject;
        }
        // IOC / FOK never rest, so they need no room.
        if (mayRest(request)) {
            if (const std::optional<RejectReason> reject =
                    validateRoom(request.side, request.price, request.quantity, nullptr))
                [[unlikely]] {
                return reject;
            }
        }
        if (request.postOnly && wouldTake(request.side, request.price)) [[unlikely]] {
            return RejectReason::POST_ONLY_WOULD_TRADE;
        }
    }
    return std::nullopt;
}

template <template <OrderSide> class Ladder, typename Locator>
    requires PriceLadder<Ladder<OrderSide::BUY>> && PriceLadder<Ladder<OrderSide::SELL>> &&
             OrderLocator<Locator>
bool BasicOrderBook<Ladder, Locator>::validQualifiers(const OrderRequest& request) noexcept {
    const TimeInForce tif = request.timeInForce;
    if (tif != TimeInForce::DAY && tif != TimeInForce::GTC && tif != TimeInForce::IOC &&
        tif != TimeInForce::FOK) {
        return false;
    }
    if (request.minQuantity > request.quantity) {
        return false;
    }
    // Post-only is a resting-only promise: it cannot be combined with anything that must trade.
    return !request.postOnly || (mayRest(request) && request.minQuantity.isZero());
}

template <template <OrderSide> class Ladder, typename Locator>
    requires PriceLadder<Ladder<OrderSide::BUY>> && PriceLadder<Ladder<OrderSide::SELL>> &&
             OrderLocator<Locator>
template <OrderSide TakerSide>
bool BasicOrderBook<Ladder, Locator>::canTake(const OrderType type,
                                              const Price limit,
                                              const Quantity needed) const noexcept {
    const auto& makers = [this]() -> const auto& {
        if constexpr (TakerSide == OrderSide::BUY) {
            return askLadder;
        } else {
            return bidLadder;
        }
    }();
    // Cannot wrap: the walk stops once the sum reaches `needed` (<= maxOrderQuantity <= 2^62),
    // and one level adds at most maxLevelQuantity (<= 2^63).
    Quantity available{0};
    makers.forEachLevelWhile([&](const PriceLevel& level) {
        if (type == OrderType::LIMIT && !crosses<TakerSide>(limit, level.price())) {
            return false;
        }
        available += level.quantity();
        return available < needed;
    });
    return available >= needed;
}

template <template <OrderSide> class Ladder, typename Locator>
    requires PriceLadder<Ladder<OrderSide::BUY>> && PriceLadder<Ladder<OrderSide::SELL>> &&
             OrderLocator<Locator>
auto BasicOrderBook<Ladder, Locator>::validateResting(const OrderSide side,
                                                      const Price price,
                                                      const Quantity quantity,
                                                      const Order* replacing) const noexcept
    -> std::optional<RejectReason> {
    if (const std::optional<RejectReason> reject = validatePrice(side, price, quantity)) {
        return reject;
    }
    return validateRoom(side, price, quantity, replacing);
}

template <template <OrderSide> class Ladder, typename Locator>
    requires PriceLadder<Ladder<OrderSide::BUY>> && PriceLadder<Ladder<OrderSide::SELL>> &&
             OrderLocator<Locator>
auto BasicOrderBook<Ladder, Locator>::validatePrice(const OrderSide side,
                                                    const Price price,
                                                    const Quantity quantity) const noexcept
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
    return std::nullopt;
}

template <template <OrderSide> class Ladder, typename Locator>
    requires PriceLadder<Ladder<OrderSide::BUY>> && PriceLadder<Ladder<OrderSide::SELL>> &&
             OrderLocator<Locator>
auto BasicOrderBook<Ladder, Locator>::validateRoom(const OrderSide side,
                                                   const Price price,
                                                   const Quantity quantity,
                                                   const Order* replacing) const noexcept
    -> std::optional<RejectReason> {
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
    if (isHalted()) [[unlikely]] {
        return std::unexpected(RejectReason::INSTRUMENT_HALTED);
    }
    // Every check happens before any mutation: a rejected order leaves the book untouched.
    if (const std::optional<RejectReason> reject = validateNew(request)) [[unlikely]] {
        return std::unexpected(*reject);
    }
    const Result result = execute(request, onFill);
    afterMutation();
    return result;
}

template <template <OrderSide> class Ladder, typename Locator>
    requires PriceLadder<Ladder<OrderSide::BUY>> && PriceLadder<Ladder<OrderSide::SELL>> &&
             OrderLocator<Locator>
template <typename OnFill>
auto BasicOrderBook<Ladder, Locator>::execute(const OrderRequest& request, OnFill& onFill)
    -> Result {
    // FOK and minimum quantity: decided by a read-only look at the opposite side, so a failed
    // condition cancels the whole order without a single fill.
    const Quantity needed = request.timeInForce == TimeInForce::FOK ? request.quantity
                                                                    : request.minQuantity;
    if (!needed.isZero()) {
        const bool enough = request.side == OrderSide::BUY
                                ? canTake<OrderSide::BUY>(request.type, request.price, needed)
                                : canTake<OrderSide::SELL>(request.type, request.price, needed);
        if (!enough) {
            index.abandon(request.id);
            return ExecutionResult{.cancelled = request.quantity};
        }
    }

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
    if (!mayRest(request)) {
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
    afterMutation();
    return open;
}

template <template <OrderSide> class Ladder, typename Locator>
    requires PriceLadder<Ladder<OrderSide::BUY>> && PriceLadder<Ladder<OrderSide::SELL>> &&
             OrderLocator<Locator>
auto BasicOrderBook<Ladder, Locator>::cancelOrder(const OrderId id, const ParticipantId participant)
    -> std::expected<Quantity, RejectReason> {
    Order* order = index.find(id);
    if (order == nullptr) [[unlikely]] {
        return std::unexpected(RejectReason::UNKNOWN_ORDER_ID);
    }
    if (order->getParticipant() != participant) [[unlikely]] {
        return std::unexpected(RejectReason::NOT_OWNER);
    }
    const Quantity open = order->getQuantity();
    removeResting(*order);
    afterMutation();
    return open;
}

template <template <OrderSide> class Ladder, typename Locator>
    requires PriceLadder<Ladder<OrderSide::BUY>> && PriceLadder<Ladder<OrderSide::SELL>> &&
             OrderLocator<Locator>
template <typename OnFill>
auto BasicOrderBook<Ladder, Locator>::modifyOrder(const ModifyRequest& request, OnFill&& onFill)
    -> Result {
    if (isHalted()) [[unlikely]] {
        return std::unexpected(RejectReason::INSTRUMENT_HALTED);
    }
    Order* order = index.find(request.id);
    if (order == nullptr) [[unlikely]] {
        return std::unexpected(RejectReason::UNKNOWN_ORDER_ID);
    }
    if (order->getParticipant() != request.participant) [[unlikely]] {
        return std::unexpected(RejectReason::NOT_OWNER);
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
        afterMutation();
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
    // The order's own side does not count: only the opposite best can be taken.
    if (order->isPostOnly() && wouldTake(order->getSide(), request.price)) [[unlikely]] {
        return std::unexpected(RejectReason::POST_ONLY_WOULD_TRADE);
    }
    const OrderRequest reentry{.id = request.id,
                               .side = order->getSide(),
                               .type = OrderType::LIMIT,
                               .price = request.price,
                               .quantity = request.quantity,
                               .timestamp = request.timestamp,
                               .participant = order->getParticipant(),
                               .timeInForce = order->getTimeInForce(),
                               .postOnly = order->isPostOnly()};
    detachForReentry(*order); // the locator keeps the id valid (unbind), so no admission is needed
    const Result result = execute(reentry, onFill);
    afterMutation();
    return result;
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
                        .makerFilled = makerFilled,
                        .tradeId = {++tradeSequence},
                        .makerParticipant = maker.getParticipant()});

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
                               Timestamp{++prioritySequence},
                               request.participant,
                               request.timeInForce,
                               request.postOnly);
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
