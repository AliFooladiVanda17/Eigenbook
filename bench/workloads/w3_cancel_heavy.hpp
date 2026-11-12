#pragma once

// W3 cancel-heavy: a stable book (L levels x Q orders per side, no crossing) and a timed stream
// that is mostly cancels of live orders at a controlled queue position (front / middle / back of
// their level), plus a small stream of cancels that miss.
//
// Every timed op is followed by an untimed compensating op that restores the level's size (a
// cancel is followed by an add at the same price, an add by a cancel of that level's front order),
// so the book stays the same size for the whole run and only the cancel path is exercised.

#include <cstddef>
#include <cstdint>
#include <string>

#include <workloads/workload.hpp>

namespace eigenbook::bench {

struct CancelHeavyConfig {
    std::uint64_t seed{1};
    WorkloadSize size{};
    double cancelFraction{0.70}; // of the timed ops that are not misses
    double missFraction{0.02};   // timed cancels of ids that were never added
    std::int64_t levelsPerSide{20};
    std::size_t ordersPerLevel{50};
    std::int64_t mid{50'000};
    std::uint64_t maxQuantity{10};
};

[[nodiscard]] inline Workload makeCancelHeavy(const CancelHeavyConfig& cfg) {
    Workload w;
    const int pct = static_cast<int>(cfg.cancelFraction * 100.0 + 0.5);
    w.name = "w3-cancel" + std::to_string(pct);
    w.seed = cfg.seed;
    w.description = std::to_string(pct) + "% cancels at front/middle/back, 2% misses";

    Rng rng(cfg.seed);
    const std::size_t perSide = static_cast<std::size_t>(cfg.levelsPerSide) * cfg.ordersPerLevel;
    ShadowBook shadow(perSide * 2 + 1024);
    std::uint64_t nextId = 1;
    std::uint64_t nextMissId = std::uint64_t{1} << 62; // never added

    auto priceOf = [&](const OrderSide side, const std::int64_t level) {
        return Price{side == OrderSide::BUY ? cfg.mid - level : cfg.mid + level};
    };
    auto add = [&](const OrderSide side, const Price price, const bool timed) {
        Op op;
        op.kind = OpKind::ADD;
        op.side = side;
        op.price = price;
        op.quantity = {static_cast<std::uint64_t>(
            uniformInt(rng, 1, static_cast<std::int64_t>(cfg.maxQuantity)))};
        op.id = {nextId};
        op.timestamp = {nextId};
        ++nextId;
        op.timed = timed;
        shadow.apply(op, w.ops);
    };
    auto cancel = [&](const std::uint64_t id, const QueuePosition pos, const bool timed) {
        Op op;
        op.kind = OpKind::CANCEL;
        op.id = {id};
        op.position = pos;
        op.timed = timed;
        shadow.apply(op, w.ops);
    };
    auto levelAt = [&](const OrderSide side, const Price price) {
        const auto& book = shadow.view();
        return side == OrderSide::BUY ? book.bids().findLevel(price) : book.asks().findLevel(price);
    };

    // Initial book (warm-up).
    for (const OrderSide side : {OrderSide::BUY, OrderSide::SELL}) {
        for (std::int64_t level = 1; level <= cfg.levelsPerSide; ++level) {
            for (std::size_t q = 0; q < cfg.ordersPerLevel; ++q) {
                add(side, priceOf(side, level), false);
            }
        }
    }

    // Warm-up steps (same mix, untimed) until warmupOps ops exist, then measured steps.
    std::size_t timed = 0;
    bool measuring = false;
    while (timed < cfg.size.measuredOps) {
        if (!measuring && w.ops.size() >= cfg.size.warmupOps) {
            measuring = true;
            w.warmupOps = w.ops.size();
        }
        if (bernoulli(rng, cfg.missFraction)) {
            cancel(nextMissId++, QueuePosition::ANY, measuring);
            timed += measuring ? 1 : 0;
            continue;
        }
        const OrderSide side = bernoulli(rng, 0.5) ? OrderSide::BUY : OrderSide::SELL;
        const Price price = priceOf(side, uniformInt(rng, 1, cfg.levelsPerSide));
        const PriceLevel* level = levelAt(side, price);
        if (bernoulli(rng, cfg.cancelFraction)) {
            const auto which = static_cast<QueuePosition>(1 + uniformIndex(rng, 3));
            const Order* target = level->front();
            if (which == QueuePosition::BACK) {
                target = level->back();
            } else if (which == QueuePosition::MIDDLE) {
                for (std::uint32_t k = level->orderCount() / 2; k > 0; --k) {
                    target = target->nextInLevel();
                }
            }
            cancel(target->getId().value, which, measuring);
            add(side, price, false); // compensation
        } else {
            add(side, price, measuring);
            cancel(levelAt(side, price)->front()->getId().value, QueuePosition::FRONT, false);
        }
        timed += measuring ? 1 : 0;
    }
    shadow.finish(w);
    return w;
}

} // namespace eigenbook::bench
