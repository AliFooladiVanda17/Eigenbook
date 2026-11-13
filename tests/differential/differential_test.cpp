//
// Randomised differential test: every ladder (map M0 / M1 / M2, dense, hybrid) against a naive
// reference book. EIGENBOOK_LONG_TESTS raises the drift test to 1,000,000 steps x 5 seeds.
//

#include <algorithm>
#include <cstdint>
#include <map>
#include <random>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "test_support.hpp"

namespace eigenbook {

namespace {

// Deliberately simple: one vector per side, scanned linearly.
class ReferenceBook {
public:
    struct Resting {
        std::uint64_t id;
        OrderSide side;
        std::int64_t price;
        std::uint64_t qty;
        std::uint64_t seq;
    };

    std::vector<Fill>
    add(std::uint64_t id, OrderSide side, OrderType type, std::int64_t price, std::uint64_t qty) {
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
            fills.push_back({{best->id}, {id}, {best->price}, {traded}, side, best->qty == 0});
            if (best->qty == 0) {
                orders.erase(best);
            }
        }
        if (qty > 0 && type == OrderType::LIMIT) {
            orders.push_back({id, side, price, qty, seq++});
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
        if (it->price == price && qty <= it->qty) {
            it->qty = qty;
            return {};
        }
        const OrderSide side = it->side;
        orders.erase(it);
        return add(id, side, OrderType::LIMIT, price, qty);
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
};

bool sameFills(const std::vector<Fill>& a, const std::vector<Fill>& b) {
    return std::equal(a.begin(), a.end(), b.begin(), b.end(), [](const Fill& x, const Fill& y) {
        return x.makerId == y.makerId && x.takerId == y.takerId && x.price == y.price &&
               x.quantity == y.quantity && x.takerSide == y.takerSide &&
               x.makerFilled == y.makerFilled;
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

// One book of every ladder type, all fed the same stream.
struct Books {
    OrderBook m0;
    OrderBookM1 m1;
    OrderBookM2 m2;
    DenseOrderBook dense;
    HybridOrderBook hybrid;

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
                 16) {}

    template <typename F>
    void each(F&& f) {
        f("map-m0", m0);
        f("map-m1", m1);
        f("map-m2", m2);
        f("dense", dense);
        f("hybrid", hybrid);
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
                     const int depthEvery = 64) {
    ReferenceBook reference;
    std::mt19937_64 rng(seed);
    std::uint64_t nextId = 1;
    for (int step = 0; step < steps; ++step) {
        onStep(rng);
        const auto op = rng() % 10;
        std::vector<Fill> refFills;
        std::vector<FillLog> logs(5);
        std::size_t k = 0;

        if (op < 6 || reference.orders.empty()) {
            const std::uint64_t id = nextId++;
            const OrderSide side = rng() % 2 ? OrderSide::BUY : OrderSide::SELL;
            const OrderType type = rng() % 12 == 0 ? OrderType::MARKET : OrderType::LIMIT;
            const std::int64_t p = price(rng);
            const std::uint64_t qty = 1 + rng() % 50;
            const OrderRequest request{{id}, side, type, {p}, {qty}, {id}};
            refFills = reference.add(id, side, type, p, qty);
            books.each([&](const char* name, auto& book) {
                const auto r = book.addOrder(request, logs[k++]);
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
                    ASSERT_TRUE(book.modifyOrder(request, logs[k++])) << name << " step " << step;
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

} // namespace eigenbook
