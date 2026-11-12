#pragma once

// Pre-generated, seeded operation streams for the latency harness and the zero-allocation test.
//
// A generator decides each operation using a "shadow" copy of the book (a map-ladder OrderBook)
// that it updates as it goes. That keeps the live-order set correct on fills (a cancel targets an
// order that is really resting, unless it is meant to miss) and records, for every operation, the
// outcome the real book must produce: the op class and the number of fills. Replaying a stream on
// either book type therefore has a known expected result, which the harness and tests check.
//
// Randomness: std::mt19937_64 (fully specified by the standard) plus the small helpers below, so
// a seed gives the same stream with every standard library. (geometric() uses std::log, whose last
// bit may differ between C libraries.)

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <random>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

#include <harness/op_class.hpp>

#include <eigenbook/book/order_book.hpp>

namespace eigenbook::bench {

enum class OpKind : std::uint8_t {
    ADD,
    CANCEL,
    MODIFY
};

enum class QueuePosition : std::uint8_t {
    ANY,
    FRONT,
    MIDDLE,
    BACK
};

// One operation. Flat and trivially copyable so a stream is one contiguous array.
struct Op {
    OpKind kind{OpKind::ADD};
    OrderSide side{OrderSide::BUY};
    OrderType type{OrderType::LIMIT};
    bool timed{false}; // recorded in a histogram (false: warm-up / compensation)
    QueuePosition position{QueuePosition::ANY}; // cancels: where the target sat in its queue
    OpClass expected{OpClass::ADD_PASSIVE};     // outcome on the shadow book
    std::uint32_t expectedFills{0};
    OrderId id{0};
    Price price{0};       // add / modify
    Quantity quantity{0}; // add / modify
    Timestamp timestamp{0};
    std::uint64_t scheduledNs{0}; // paced workloads: intended start, ns from the first timed op
};

struct Workload {
    std::string name;
    std::uint64_t seed{0};
    std::vector<Op> ops;
    std::size_t warmupOps{0};      // ops[0, warmupOps) are warm-up; none of them is timed
    std::size_t timedOps{0};       // number of ops with timed == true
    std::size_t expectedOrders{0}; // book sizing (pool + index), >= peak live orders
    std::size_t peakLive{0};
    Price minPrice{0};
    Price maxPrice{0};
    bool paced{false}; // honour Op::scheduledNs (open loop)
    // Level churn over the timed ops, measured on the shadow book.
    std::uint64_t levelsCreated{0};
    std::uint64_t levelsDestroyed{0};
    std::string description;

    [[nodiscard]] double churnPerOp() const noexcept {
        return timedOps == 0 ? 0.0
                             : static_cast<double>(levelsCreated + levelsDestroyed) /
                                   static_cast<double>(timedOps);
    }

    // Tick range a dense ladder needs to accept every price in the stream.
    [[nodiscard]] std::size_t tickCount() const noexcept {
        return static_cast<std::size_t>(maxPrice - minPrice) + 1;
    }
};

// --- Random helpers ------------------------------------------------------------------------------

using Rng = std::mt19937_64;

// Uniform integer in [lo, hi] (modulo; the bias is irrelevant at these ranges).
[[nodiscard]] inline std::int64_t
uniformInt(Rng& rng, const std::int64_t lo, const std::int64_t hi) {
    const auto span = static_cast<std::uint64_t>(hi - lo) + 1;
    return lo + static_cast<std::int64_t>(rng() % span);
}

[[nodiscard]] inline std::size_t uniformIndex(Rng& rng, const std::size_t n) {
    return static_cast<std::size_t>(rng() % n);
}

// Uniform double in [0, 1).
[[nodiscard]] inline double uniform01(Rng& rng) {
    return static_cast<double>(rng() >> 11) * 0x1.0p-53;
}

[[nodiscard]] inline bool bernoulli(Rng& rng, const double p) {
    return uniform01(rng) < p;
}

// Number of trials until the first success, success probability 1/mean; >= 1.
[[nodiscard]] inline std::uint64_t geometric(Rng& rng, const double mean) {
    if (mean <= 1.0) {
        return 1;
    }
    const double p = 1.0 / mean;
    const double u = uniform01(rng);
    return 1 + static_cast<std::uint64_t>(std::floor(std::log1p(-u) / std::log1p(-p)));
}

// Exponential with the given rate (events per second); returns seconds.
[[nodiscard]] inline double exponential(Rng& rng, const double rate) {
    return -std::log1p(-uniform01(rng)) / rate;
}

// --- Shadow book ---------------------------------------------------------------------------------

// Applies each generated op to a map-ladder book, tags the op with its outcome and keeps the set
// of live (resting) order ids in O(1) per update.
class ShadowBook {
public:
    explicit ShadowBook(const std::size_t capacityHint = 1 << 16)
        : book(InstrumentId{0}, {}, capacityHint) {
        livePos.reserve(capacityHint);
        live.reserve(capacityHint);
    }

    // Applies `op` (which must have kind, side, type, id, price, quantity, timestamp, timed and
    // position set), fills in op.expected / op.expectedFills, and appends it to `out`.
    void apply(Op op, std::vector<Op>& out) {
        const std::size_t levelsBefore = book.bids().levelCount() + book.asks().levelCount();
        std::uint32_t fills = 0;
        auto onFill = [&](const Fill& f) {
            ++fills;
            if (f.makerFilled) {
                removeLive(f.makerId.value);
            }
        };
        bool created = false;

        switch (op.kind) {
            case OpKind::ADD:
                {
                    const OrderRequest request{.id = op.id,
                                               .side = op.side,
                                               .type = op.type,
                                               .price = op.price,
                                               .quantity = op.quantity,
                                               .timestamp = op.timestamp};
                    const auto result = book.addOrder(request, onFill);
                    if (!result) {
                        throw std::logic_error("workload generator produced a rejected add");
                    }
                    if (!result->resting.isZero()) {
                        addLive(op.id.value);
                        created = levelOf(op.side, op.price)->orderCount() == 1;
                    }
                    op.expected = addClassFor(fills);
                    trackPrice(op.price);
                    break;
                }
            case OpKind::CANCEL:
                {
                    const bool hit = book.cancelOrder(op.id).has_value();
                    if (hit) {
                        removeLive(op.id.value);
                        switch (op.position) {
                            case QueuePosition::FRONT:
                                op.expected = OpClass::CANCEL_HIT_FRONT;
                                break;
                            case QueuePosition::MIDDLE:
                                op.expected = OpClass::CANCEL_HIT_MIDDLE;
                                break;
                            case QueuePosition::BACK:
                                op.expected = OpClass::CANCEL_HIT_BACK;
                                break;
                            case QueuePosition::ANY:
                                op.expected = OpClass::CANCEL_HIT;
                                break;
                        }
                    } else {
                        op.expected = OpClass::CANCEL_MISS;
                    }
                    break;
                }
            case OpKind::MODIFY:
                {
                    const Order* before = book.findOrder(op.id);
                    if (before == nullptr) {
                        throw std::logic_error("workload generator modified a non-resting order");
                    }
                    const OrderSide side = before->getSide();
                    const bool inPlace = op.price == before->getPrice() &&
                                         op.quantity <= before->getQuantity();
                    const ModifyRequest request{.id = op.id,
                                                .price = op.price,
                                                .quantity = op.quantity,
                                                .timestamp = op.timestamp};
                    const auto result = book.modifyOrder(request, onFill);
                    if (!result) {
                        throw std::logic_error("workload generator produced a rejected modify");
                    }
                    if (result->resting.isZero()) {
                        removeLive(op.id.value);
                    } else if (!inPlace) {
                        created = levelOf(side, op.price)->orderCount() == 1;
                    }
                    op.side = side;
                    op.expected = inPlace ? OpClass::MODIFY_IN_PLACE : OpClass::MODIFY_REENTER;
                    trackPrice(op.price);
                    break;
                }
        }

        op.expectedFills = fills;
        const std::size_t levelsAfter = book.bids().levelCount() + book.asks().levelCount();
        if (op.timed) {
            const std::size_t made = created ? 1 : 0;
            levelsCreated += made;
            levelsDestroyed += levelsBefore + made - levelsAfter;
        }
        peakLive = std::max(peakLive, book.orderCount());
        out.push_back(op);
    }

    [[nodiscard]] const OrderBook& view() const noexcept { return book; }

    [[nodiscard]] bool isLive(const std::uint64_t id) const { return livePos.contains(id); }

    [[nodiscard]] std::size_t liveCount() const noexcept { return live.size(); }

    [[nodiscard]] std::uint64_t randomLive(Rng& rng) const {
        return live[uniformIndex(rng, live.size())];
    }

    // Copies the generation statistics into the workload and finalises its sizing fields.
    void finish(Workload& w) const {
        w.peakLive = peakLive;
        // Slack so the measured book never grows its pool or index.
        w.expectedOrders = std::max<std::size_t>(w.expectedOrders, peakLive + peakLive / 8 + 64);
        w.levelsCreated = levelsCreated;
        w.levelsDestroyed = levelsDestroyed;
        w.minPrice = minSeen;
        w.maxPrice = maxSeen;
        w.timedOps = static_cast<std::size_t>(
            std::ranges::count_if(w.ops, [](const Op& o) { return o.timed; }));
    }

private:
    [[nodiscard]] const PriceLevel* levelOf(const OrderSide side, const Price price) const {
        return side == OrderSide::BUY ? book.bids().findLevel(price) : book.asks().findLevel(price);
    }

    void addLive(const std::uint64_t id) {
        livePos.emplace(id, live.size());
        live.push_back(id);
    }

    void removeLive(const std::uint64_t id) {
        const auto it = livePos.find(id);
        if (it == livePos.end()) {
            return;
        }
        const std::size_t pos = it->second;
        livePos.erase(it);
        if (pos + 1 != live.size()) {
            live[pos] = live.back();
            livePos[live[pos]] = pos;
        }
        live.pop_back();
    }

    void trackPrice(const Price p) {
        minSeen = std::min(minSeen, p);
        maxSeen = std::max(maxSeen, p);
    }

    OrderBook book;
    std::vector<std::uint64_t> live;
    std::unordered_map<std::uint64_t, std::size_t> livePos;
    std::size_t peakLive{0};
    std::uint64_t levelsCreated{0};
    std::uint64_t levelsDestroyed{0};
    Price minSeen{std::numeric_limits<std::int64_t>::max()};
    Price maxSeen{std::numeric_limits<std::int64_t>::min()};
};

// Common sizes for every workload.
struct WorkloadSize {
    std::size_t warmupOps{200'000};
    std::size_t measuredOps{1'000'000};
};

} // namespace eigenbook::bench
