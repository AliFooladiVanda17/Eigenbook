//
// Randomised differential test: every ladder (map M0 / M1 / M2, dense, hybrid) against a naive
// reference book. EIGENBOOK_LONG_TESTS raises the drift test to 1,000,000 steps x 5 seeds.
//

#include <algorithm>
#include <cstdint>
#include <map>
#include <optional>
#include <random>
#include <unordered_map>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "test_support.hpp"

namespace eigenbook {

namespace {

// Order qualifiers for the reference book, applied the naive way: post-only checks the best
// opposite price, FOK and the minimum quantity sum every opposite order that crosses.
struct RefQualifiers {
    TimeInForce tif{TimeInForce::DAY};
    bool postOnly{false};
    std::uint64_t minQty{0};
};

// Deliberately simple: one vector per side, scanned linearly.
class ReferenceBook {
public:
    struct Resting {
        std::uint64_t id;
        OrderSide side;
        std::int64_t price;
        std::uint64_t qty;
        std::uint64_t seq;
        bool postOnly{false};
    };

    // Set by add / modify: the request was rejected (post-only would trade); nothing changed.
    bool rejected{false};

    std::vector<Fill> add(std::uint64_t id,
                          OrderSide side,
                          OrderType type,
                          std::int64_t price,
                          std::uint64_t qty,
                          const RefQualifiers q = {}) {
        rejected = false;
        const auto crosses = [&](const Resting& r) {
            return r.side != side &&
                   (type == OrderType::MARKET ||
                    (side == OrderSide::BUY ? r.price <= price : r.price >= price));
        };
        if (q.postOnly) {
            const auto best = bestOpposite(side);
            if (best != orders.end() && crosses(*best)) {
                rejected = true;
                return {};
            }
        }
        const std::uint64_t needed = q.tif == TimeInForce::FOK ? qty : q.minQty;
        if (needed > 0) {
            std::uint64_t available = 0;
            for (const Resting& r : orders) {
                available += crosses(r) ? r.qty : 0;
            }
            if (available < needed) {
                return {};
            }
        }
        const bool mayRest = type == OrderType::LIMIT &&
                             (q.tif == TimeInForce::DAY || q.tif == TimeInForce::GTC);
        std::vector<Fill> fills;
        while (qty > 0) {
            auto best = bestOpposite(side);
            if (best == orders.end()) {
                break;
            }
            if (type == OrderType::LIMIT &&
                (side == OrderSide::BUY ? best->price > price : best->price < price)) {
                break;
            }
            const std::uint64_t traded = std::min(qty, best->qty);
            qty -= traded;
            best->qty -= traded;
            fills.push_back(
                {{best->id}, {id}, {best->price}, {traded}, side, best->qty == 0, {++trades}});
            if (best->qty == 0) {
                orders.erase(best);
            }
        }
        if (qty > 0 && mayRest) {
            orders.push_back({id, side, price, qty, seq++, q.postOnly});
        }
        return fills;
    }

    bool cancel(std::uint64_t id) {
        const auto it = find(id);
        if (it == orders.end()) {
            return false;
        }
        orders.erase(it);
        return true;
    }

    std::vector<Fill> modify(std::uint64_t id, std::int64_t price, std::uint64_t qty) {
        const auto it = find(id);
        rejected = false;
        if (it->price == price && qty <= it->qty) {
            it->qty = qty;
            return {};
        }
        const OrderSide side = it->side;
        const bool postOnly = it->postOnly;
        if (postOnly) {
            const auto best = bestOpposite(side);
            if (best != orders.end() &&
                (side == OrderSide::BUY ? best->price <= price : best->price >= price)) {
                rejected = true;
                return {};
            }
        }
        orders.erase(it);
        return add(id, side, OrderType::LIMIT, price, qty, {.postOnly = postOnly});
    }

    std::vector<Resting>::iterator find(std::uint64_t id) {
        return std::find_if(orders.begin(), orders.end(), [&](const Resting& r) {
            return r.id == id;
        });
    }

    std::vector<Resting> orders;

private:
    std::vector<Resting>::iterator bestOpposite(OrderSide side) {
        auto best = orders.end();
        for (auto it = orders.begin(); it != orders.end(); ++it) {
            if (it->side == side) {
                continue;
            }
            if (best == orders.end()) {
                best = it;
                continue;
            }
            const bool better = side == OrderSide::BUY ? it->price < best->price
                                                       : it->price > best->price;
            if (better || (it->price == best->price && it->seq < best->seq)) {
                best = it;
            }
        }
        return best;
    }

    std::uint64_t seq{0};
    std::uint64_t trades{0}; // last trade id
};

bool sameFills(const std::vector<Fill>& a, const std::vector<Fill>& b) {
    return std::equal(a.begin(), a.end(), b.begin(), b.end(), [](const Fill& x, const Fill& y) {
        return x.makerId == y.makerId && x.takerId == y.takerId && x.price == y.price &&
               x.quantity == y.quantity && x.takerSide == y.takerSide &&
               x.makerFilled == y.makerFilled && x.tradeId == y.tradeId &&
               x.makerParticipant == y.makerParticipant;
    });
}

using Depth = std::vector<std::pair<std::int64_t, std::uint64_t>>;

template <typename Book>
Depth depth(const Book& book, OrderSide side) {
    Depth out;
    auto collect = [&](const PriceLevel& l) {
        out.emplace_back(l.price().value, l.quantity().value);
    };
    side == OrderSide::BUY ? book.bids().forEachLevel(collect) : book.asks().forEachLevel(collect);
    return out;
}

// Aggregated levels of the reference book, best to worst.
Depth depth(const ReferenceBook& ref, OrderSide side) {
    std::map<std::int64_t, std::uint64_t> levels;
    for (const auto& r : ref.orders) {
        if (r.side == side) {
            levels[r.price] += r.qty;
        }
    }
    Depth out(levels.begin(), levels.end());
    if (side == OrderSide::BUY) {
        std::ranges::reverse(out);
    }
    return out;
}

// Steps between two full book audits (checkInvariants) in the differential runs.
constexpr int kFullAuditEvery = 4096;

// One book of every ladder type, all fed the same stream.
struct Books {
    OrderBook m0;
    OrderBookM1 m1;
    OrderBookM2 m2;
    DenseOrderBook dense;
    HybridOrderBook hybrid;
    DenseOrderBookHardened hardened; // fixed capacity: sized well above the live set

    Books(const Price denseMin, const std::size_t denseTicks, const Price hybridBase)
        : m0(kInstrument, {}, 16)
        , m1(kInstrument, {.maxLevels = 1 << 16}, 16)
        , m2(kInstrument, {.maxLevels = 1 << 16}, 16)
        , dense(kInstrument, {.minPrice = denseMin, .tickCount = denseTicks}, 16)
        // A small window, so the stream keeps moving levels between window and maps.
        , hybrid(kInstrument,
                 {.windowTicks = 64,
                  .initialBase = hybridBase,
                  .maxLevels = 1 << 16,
                  .maxShiftsPerOp = 16,
                  .maxMovesPerOp = 4},
                 16)
        , hardened(kInstrument, {.minPrice = denseMin, .tickCount = denseTicks}, 1 << 16) {}

    template <typename F>
    void each(F&& f) {
        f("map-m0", m0);
        f("map-m1", m1);
        f("map-m2", m2);
        f("dense", dense);
        f("hybrid", hybrid);
        f("dense-hardened", hardened);
    }
};

// Random adds (1 in 12 market), cancels and modifies; `price()` draws a limit price. After every
// step every book must report the reference's fills, order count and best prices; every
// `depthEvery` steps also the full depth and the hybrid ladder's invariants.
template <typename PriceFn, typename StepFn>
void runDifferential(Books& books,
                     const std::uint64_t seed,
                     const int steps,
                     PriceFn&& price,
                     StepFn&& onStep,
                     const int depthEvery = 64,
                     const bool qualifiers = false) {
    ReferenceBook reference;
    std::mt19937_64 rng(seed);
    std::uint64_t nextId = 1;
    for (int step = 0; step < steps; ++step) {
        onStep(rng);
        const auto op = rng() % 10;
        std::vector<Fill> refFills;
        std::vector<FillLog> logs(6);
        std::size_t k = 0;

        if (op < 6 || reference.orders.empty()) {
            const std::uint64_t id = nextId++;
            const OrderSide side = rng() % 2 ? OrderSide::BUY : OrderSide::SELL;
            const OrderType type = rng() % 12 == 0 ? OrderType::MARKET : OrderType::LIMIT;
            const std::int64_t p = price(rng);
            const std::uint64_t qty = 1 + rng() % 50;
            OrderRequest request{{id}, side, type, {p}, {qty}, {id}};
            RefQualifiers q;
            if (qualifiers) {
                // Always a valid combination: rejects other than post-only are tested elsewhere.
                q.tif = static_cast<TimeInForce>(rng() % 4);
                const bool rests = type == OrderType::LIMIT &&
                                   (q.tif == TimeInForce::DAY || q.tif == TimeInForce::GTC);
                q.postOnly = rests && rng() % 4 == 0;
                q.minQty = !q.postOnly && rng() % 5 == 0 ? 1 + rng() % qty : 0;
                request.timeInForce = q.tif;
                request.postOnly = q.postOnly;
                request.minQuantity = {q.minQty};
            }
            refFills = reference.add(id, side, type, p, qty, q);
            books.each([&](const char* name, auto& book) {
                const auto r = book.addOrder(request, logs[k++]);
                if (reference.rejected) {
                    ASSERT_FALSE(r) << name << " step " << step;
                    ASSERT_EQ(r.error(), RejectReason::POST_ONLY_WOULD_TRADE) << name;
                    return;
                }
                ASSERT_TRUE(r) << name << " step " << step;
                ASSERT_EQ((r->filled + r->resting + r->cancelled), Quantity{qty}) << name;
            });
        } else {
            const auto victim = reference.orders[rng() % reference.orders.size()];
            if (op < 8) {
                ASSERT_TRUE(reference.cancel(victim.id));
                books.each([&](const char* name, auto& book) {
                    ASSERT_TRUE(book.cancelOrder({victim.id})) << name << " step " << step;
                });
            } else {
                const std::int64_t p = rng() % 2 ? victim.price : price(rng);
                const std::uint64_t qty = 1 + rng() % 60;
                const ModifyRequest request{{victim.id}, {p}, {qty}, {nextId}};
                refFills = reference.modify(victim.id, p, qty);
                books.each([&](const char* name, auto& book) {
                    const auto r = book.modifyOrder(request, logs[k++]);
                    if (reference.rejected) {
                        ASSERT_FALSE(r) << name << " step " << step;
                        ASSERT_EQ(r.error(), RejectReason::POST_ONLY_WOULD_TRADE) << name;
                        return;
                    }
                    ASSERT_TRUE(r) << name << " step " << step;
                });
            }
        }
        if (::testing::Test::HasFatalFailure()) {
            return;
        }

        k = 0;
        const bool checkDepth = step % depthEvery == 0;
        const Depth refBids = checkDepth ? depth(reference, OrderSide::BUY) : Depth{};
        const Depth refAsks = checkDepth ? depth(reference, OrderSide::SELL) : Depth{};
        books.each([&](const char* name, auto& book) {
            const FillLog& log = logs[k++];
            ASSERT_TRUE(sameFills(log.fills, refFills)) << name << " step " << step;
            ASSERT_EQ(book.orderCount(), reference.orders.size()) << name << " step " << step;
            if (checkDepth) {
                ASSERT_EQ(depth(book, OrderSide::BUY), refBids) << name << " step " << step;
                ASSERT_EQ(depth(book, OrderSide::SELL), refAsks) << name << " step " << step;
            }
            // The full book audit walks every tick of a dense ladder (2^20 here), so it runs far
            // less often than the depth comparison.
            if (step % kFullAuditEvery == 0) {
                ASSERT_EQ(book.checkInvariants(), std::nullopt) << name << " step " << step;
            }
            if (book.bestBid() && book.bestAsk()) {
                ASSERT_LT(*book.bestBid(), *book.bestAsk())
                    << name << " left crossed at step " << step;
            }
        });
        ASSERT_EQ(books.hybrid.bestBid(), books.m0.bestBid()) << "step " << step;
        ASSERT_EQ(books.hybrid.bestAsk(), books.m0.bestAsk()) << "step " << step;
        if (checkDepth) {
            ASSERT_TRUE(books.hybrid.bids().checkInvariants()) << "step " << step;
            ASSERT_TRUE(books.hybrid.asks().checkInvariants()) << "step " << step;
        }
        if (::testing::Test::HasFatalFailure()) {
            return;
        }
    }
}

#if defined(EIGENBOOK_LONG_TESTS)
constexpr int kLongSteps = 1'000'000;
constexpr std::uint64_t kLongSeeds = 5;
#else
constexpr int kLongSteps = 50'000;
constexpr std::uint64_t kLongSeeds = 1;
#endif

} // namespace

// Prices in a narrow static band. (Name kept for requirement traceability; it now runs every
// ladder.)
TEST(OrderBookDifferential, MapDenseAndReferenceAgree) {
    Books books({900}, 200, {0});
    runDifferential(
        books,
        3,
        20'000,
        [](std::mt19937_64& rng) { return 980 + static_cast<std::int64_t>(rng() % 41); },
        [](std::mt19937_64&) {});
}

// A mid that random-walks with occasional jumps of +-500 ticks: the hybrid window keeps
// recentering, evicting and importing levels; every ladder must still agree with the reference.
TEST(OrderBookDifferential, AllLaddersAgreeUnderADriftingMid) {
    for (std::uint64_t seed = 1; seed <= kLongSeeds; ++seed) {
        Books books({0}, 1 << 20, {500'000});
        std::int64_t mid = 500'000;
        runDifferential(
            books,
            seed,
            kLongSteps,
            [&mid](std::mt19937_64& rng) {
                return mid - 20 + static_cast<std::int64_t>(rng() % 41);
            },
            [&mid](std::mt19937_64& rng) {
                const auto r = rng() % 1000;
                if (r == 0) {
                    mid += rng() % 2 ? 500 : -500;
                } else if (r < 300) {
                    mid += rng() % 2 ? 1 : -1;
                }
                mid = std::clamp<std::int64_t>(mid, 1'000, (1 << 20) - 1'000);
            },
            256);
        ASSERT_FALSE(::testing::Test::HasFatalFailure()) << "seed " << seed;
        EXPECT_GT(books.hybrid.bids().levelsMoved() + books.hybrid.asks().levelsMoved(), 0u)
            << "seed " << seed;
    }
}

// Random time in force (DAY / GTC / IOC / FOK), post-only and minimum quantities in a narrow band,
// where most orders cross: FOK and minimum-quantity failures, post-only rejects (also on modify)
// and IOC remainders must match the naive reference exactly.
TEST(OrderBookDifferential, QualifiersAgreeWithTheReference) {
    for (std::uint64_t seed = 11; seed <= 13; ++seed) {
        Books books({900}, 200, {960});
        runDifferential(
            books,
            seed,
            20'000,
            [](std::mt19937_64& rng) { return 980 + static_cast<std::int64_t>(rng() % 41); },
            [](std::mt19937_64&) {},
            64,
            true);
        ASSERT_FALSE(::testing::Test::HasFatalFailure()) << "seed " << seed;
    }
}

// The book with engine-issued handles (DirectOrderTable) against the same reference. The reference
// keeps its own sequential ids; the test maps them to the book's handles (and back, to compare
// fills).
TEST(OrderBookDifferential, DirectHandlesAgreeWithTheReference) {
    for (std::uint64_t seed = 1; seed <= kLongSeeds; ++seed) {
        DenseOrderBookDirect book(kInstrument, {.minPrice = {0}, .tickCount = 1 << 20}, 1 << 16);
        ReferenceBook reference;
        std::unordered_map<std::uint64_t, OrderId> toHandle;
        std::unordered_map<std::uint64_t, std::uint64_t> toRef;
        std::mt19937_64 rng(seed);
        std::uint64_t nextId = 1;
        std::int64_t mid = 500'000;
        auto price = [&] {
            return mid - 20 + static_cast<std::int64_t>(rng() % 41);
        };
        auto forget = [&](const std::uint64_t refId) {
            toRef.erase(toHandle.at(refId).value);
            toHandle.erase(refId);
        };
        for (int step = 0; step < kLongSteps; ++step) {
            if (rng() % 1000 < 300) {
                mid += rng() % 2 ? 1 : -1;
            }
            const auto op = rng() % 10;
            FillLog log;
            std::vector<Fill> refFills;
            std::uint64_t subject = 0; // reference id of the order this step adds or modifies
            if (op < 6 || reference.orders.empty()) {
                subject = nextId++;
                const OrderSide side = rng() % 2 ? OrderSide::BUY : OrderSide::SELL;
                const OrderType type = rng() % 12 == 0 ? OrderType::MARKET : OrderType::LIMIT;
                const std::int64_t p = price();
                const std::uint64_t qty = 1 + rng() % 50;
                refFills = reference.add(subject, side, type, p, qty);
                const auto handle = book.reserveOrderId();
                ASSERT_TRUE(handle);
                toHandle.emplace(subject, *handle);
                toRef.emplace(handle->value, subject);
                ASSERT_TRUE(book.addOrder({*handle, side, type, {p}, {qty}, {subject}}, log))
                    << step;
            } else {
                const auto victim = reference.orders[rng() % reference.orders.size()];
                const OrderId handle = toHandle.at(victim.id);
                if (op < 8) {
                    ASSERT_TRUE(reference.cancel(victim.id));
                    ASSERT_TRUE(book.cancelOrder(handle)) << step;
                    forget(victim.id);
                } else {
                    subject = victim.id;
                    const std::int64_t p = rng() % 2 ? victim.price : price();
                    const std::uint64_t qty = 1 + rng() % 60;
                    refFills = reference.modify(victim.id, p, qty);
                    ASSERT_TRUE(book.modifyOrder({handle, {p}, {qty}, {nextId}}, log)) << step;
                }
            }
            // Translate the book's fills back to reference ids, then retire handles that left.
            ASSERT_EQ(log.fills.size(), refFills.size()) << step;
            for (std::size_t k = 0; k < refFills.size(); ++k) {
                Fill f = log.fills[k];
                f.makerId = {toRef.at(f.makerId.value)};
                f.takerId = {toRef.at(f.takerId.value)};
                ASSERT_TRUE(sameFills({f}, {refFills[k]})) << step;
            }
            for (const Fill& f : log.fills) {
                if (f.makerFilled) {
                    forget(toRef.at(f.makerId.value));
                }
            }
            if (subject != 0 && toHandle.contains(subject) &&
                book.findOrder(toHandle.at(subject)) == nullptr) {
                forget(subject); // filled in full or market remainder: its handle is gone
            }
            ASSERT_EQ(book.orderCount(), reference.orders.size()) << step;
            ASSERT_EQ(toHandle.size(), reference.orders.size()) << step;
            if (step % kFullAuditEvery == 0) {
                ASSERT_EQ(book.checkInvariants(), std::nullopt) << step;
            }
            if (step % 256 == 0) {
                ASSERT_EQ(depth(book, OrderSide::BUY), depth(reference, OrderSide::BUY)) << step;
                ASSERT_EQ(depth(book, OrderSide::SELL), depth(reference, OrderSide::SELL)) << step;
            }
        }
    }
}

} // namespace eigenbook
