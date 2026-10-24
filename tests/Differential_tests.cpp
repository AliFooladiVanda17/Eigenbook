//
// Randomised differential test: map ladder vs dense ladder vs a naive reference book.
//

#include <algorithm>
#include <cstdint>
#include <gtest/gtest.h>
#include <random>
#include <utility>
#include <vector>

#include "TestSupport.h"

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

    std::vector<Fill> add(std::uint64_t id, OrderSide side, OrderType type, std::int64_t price,
                          std::uint64_t qty) {
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

template <typename Book>
std::vector<std::pair<std::int64_t, std::uint64_t>> depth(const Book& book, OrderSide side) {
    std::vector<std::pair<std::int64_t, std::uint64_t>> out;
    auto collect = [&](const PriceLevel& l) {
        out.emplace_back(l.price().value, l.quantity().value);
    };
    side == OrderSide::BUY ? book.bids().forEachLevel(collect) : book.asks().forEachLevel(collect);
    return out;
}

} // namespace

TEST(OrderBookDifferential, MapDenseAndReferenceAgree) {
    OrderBook mapBook(kInstrument, {}, 16);
    DenseOrderBook denseBook(kInstrument, {.minPrice = {900}, .tickCount = 200}, 16);
    ReferenceBook reference;

    std::mt19937_64 rng(3);
    std::uint64_t nextId = 1;
    for (int step = 0; step < 20'000; ++step) {
        const auto op = rng() % 10;
        FillLog mapFills, denseFills;
        std::vector<Fill> refFills;

        if (op < 6 || reference.orders.empty()) {
            const std::uint64_t id = nextId++;
            const OrderSide side = rng() % 2 ? OrderSide::BUY : OrderSide::SELL;
            const OrderType type = rng() % 12 == 0 ? OrderType::MARKET : OrderType::LIMIT;
            const std::int64_t price = 980 + static_cast<std::int64_t>(rng() % 41);
            const std::uint64_t qty = 1 + rng() % 50;
            const OrderRequest request{{id}, side, type, {price}, {qty}, {id}};
            const auto a = mapBook.addOrder(request, mapFills);
            const auto b = denseBook.addOrder(request, denseFills);
            ASSERT_TRUE(a && b);
            ASSERT_EQ(a->filled, b->filled);
            ASSERT_EQ(a->resting, b->resting);
            ASSERT_EQ(a->cancelled, b->cancelled);
            ASSERT_EQ((a->filled + a->resting + a->cancelled), Quantity{qty});
            refFills = reference.add(id, side, type, price, qty);
        }
        else {
            const auto& victim = reference.orders[rng() % reference.orders.size()];
            const std::uint64_t id = victim.id;
            if (op < 8) {
                ASSERT_TRUE(mapBook.cancelOrder({id}));
                ASSERT_TRUE(denseBook.cancelOrder({id}));
                ASSERT_TRUE(reference.cancel(id));
            }
            else {
                const std::int64_t price =
                    rng() % 2 ? victim.price : 980 + static_cast<std::int64_t>(rng() % 41);
                const std::uint64_t qty = 1 + rng() % 60;
                const ModifyRequest request{{id}, {price}, {qty}, {nextId}};
                ASSERT_TRUE(mapBook.modifyOrder(request, mapFills));
                ASSERT_TRUE(denseBook.modifyOrder(request, denseFills));
                refFills = reference.modify(id, price, qty);
            }
        }

        ASSERT_TRUE(sameFills(mapFills.fills, refFills)) << "step " << step;
        ASSERT_TRUE(sameFills(denseFills.fills, refFills)) << "step " << step;
        ASSERT_EQ(mapBook.orderCount(), reference.orders.size());
        ASSERT_EQ(denseBook.orderCount(), reference.orders.size());
        ASSERT_EQ(mapBook.bestBid(), denseBook.bestBid());
        ASSERT_EQ(mapBook.bestAsk(), denseBook.bestAsk());
        if (step % 64 == 0) {
            ASSERT_EQ(depth(mapBook, OrderSide::BUY), depth(denseBook, OrderSide::BUY));
            ASSERT_EQ(depth(mapBook, OrderSide::SELL), depth(denseBook, OrderSide::SELL));
        }
        if (mapBook.bestBid() && mapBook.bestAsk()) {
            ASSERT_LT(*mapBook.bestBid(), *mapBook.bestAsk()) << "book left crossed";
        }
    }
}
