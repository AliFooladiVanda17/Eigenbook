#pragma once

// W2 level churn: a wide price band, few orders per level and short lifetimes, so price levels
// are created and destroyed constantly. Target churn (levels created + destroyed per op) >= 0.5.
//
// Drift variant: the mid price moves up one tick every `driftEvery` ops (a trending market), and
// a small fraction of orders never expire, so stale orders are left far behind the touch and the
// dense ladder's best-level search has to cross long empty stretches.

#include <cstddef>
#include <cstdint>
#include <functional>
#include <queue>
#include <utility>
#include <vector>

#include <workloads/workload.hpp>

namespace eigenbook::bench {

struct LevelChurnConfig {
    std::uint64_t seed{1};
    WorkloadSize size{};
    std::int64_t mid{100'000};
    std::int64_t halfWidth{1'000}; // passive prices within [mid - H, mid - 1] / [mid + 1, mid + H]
    double aggressiveFraction{0.10}; // adds priced through the touch
    std::int64_t aggressiveDepth{5}; // ticks through the opposite best
    double meanLifetimeOps{64.0};    // resting orders are cancelled after ~ this many ops
    std::uint64_t maxQuantity{10};
    std::size_t driftEvery{0};     // 0: no drift
    double longLivedFraction{0.0}; // resting orders that are never cancelled
};

[[nodiscard]] inline LevelChurnConfig levelChurnDrift(const std::uint64_t seed) {
    LevelChurnConfig c;
    c.seed = seed;
    c.halfWidth = 4'000;
    c.driftEvery = 100;
    c.longLivedFraction = 0.02;
    return c;
}

[[nodiscard]] inline Workload makeLevelChurn(const LevelChurnConfig& cfg) {
    Workload w;
    w.name = cfg.driftEvery == 0 ? "w2-churn" : "w2-drift";
    w.seed = cfg.seed;
    w.warmupOps = cfg.size.warmupOps;
    w.description = cfg.driftEvery == 0 ? "wide band, short lifetimes"
                                        : "wide band, short lifetimes, trending mid, 2% stale";
    const std::size_t total = cfg.size.warmupOps + cfg.size.measuredOps;
    w.ops.reserve(total);

    Rng rng(cfg.seed);
    ShadowBook shadow(1 << 16);
    // Min-heap of (due op index, order id).
    using Due = std::pair<std::size_t, std::uint64_t>;
    std::priority_queue<Due, std::vector<Due>, std::greater<>> expiries;
    std::int64_t mid = cfg.mid;
    std::uint64_t nextId = 1;

    for (std::size_t i = 0; i < total; ++i) {
        if (cfg.driftEvery != 0 && i % cfg.driftEvery == 0 && i != 0) {
            ++mid;
        }
        Op op;
        op.timed = i >= cfg.size.warmupOps;

        // Expired orders first; skip the ones that have traded away in the meantime.
        while (!expiries.empty() && expiries.top().first <= i &&
               !shadow.isLive(expiries.top().second)) {
            expiries.pop();
        }
        if (!expiries.empty() && expiries.top().first <= i) {
            op.kind = OpKind::CANCEL;
            op.id = {expiries.top().second};
            expiries.pop();
            shadow.apply(op, w.ops);
            continue;
        }

        op.kind = OpKind::ADD;
        op.side = bernoulli(rng, 0.5) ? OrderSide::BUY : OrderSide::SELL;
        op.quantity = {static_cast<std::uint64_t>(
            uniformInt(rng, 1, static_cast<std::int64_t>(cfg.maxQuantity)))};
        op.id = {nextId};
        op.timestamp = {nextId};
        ++nextId;
        if (bernoulli(rng, cfg.aggressiveFraction)) {
            const auto& book = shadow.view();
            const auto opposite = op.side == OrderSide::BUY ? book.bestAsk() : book.bestBid();
            const std::int64_t through = uniformInt(rng, 0, cfg.aggressiveDepth);
            const std::int64_t base = opposite ? opposite->value : mid;
            op.price = {op.side == OrderSide::BUY ? base + through : base - through};
        } else {
            const std::int64_t off = uniformInt(rng, 1, cfg.halfWidth);
            op.price = {op.side == OrderSide::BUY ? mid - off : mid + off};
        }
        shadow.apply(op, w.ops);
        if (shadow.isLive(op.id.value) && !bernoulli(rng, cfg.longLivedFraction)) {
            expiries.emplace(i + geometric(rng, cfg.meanLifetimeOps), op.id.value);
        }
    }
    shadow.finish(w);
    return w;
}

} // namespace eigenbook::bench
