#pragma once

// W1 baseline: exactly the operation stream of apps/demo/main.cpp, for continuity with the
// numbers published before the harness existed.
//
// About a third of the ops are cancels drawn from a list of every id ever added; the list is not
// updated on fills, so part of the cancels miss. Here those are classified as cancel-miss
// instead of being averaged in.

#include <cstddef>
#include <cstdint>
#include <vector>

#include <workloads/workload.hpp>

namespace eigenbook::bench {

struct BaselineConfig {
    std::uint64_t seed{42}; // the demo's seed
    std::size_t totalOps{2'000'000};
    std::size_t warmupOps{200'000};
};

[[nodiscard]] inline Workload makeBaseline(const BaselineConfig& cfg) {
    Workload w;
    w.name = "w1-baseline";
    w.seed = cfg.seed;
    w.warmupOps = cfg.warmupOps;
    w.description = "demo stream: 2/3 limit adds around 10000, 1/3 cancels from a stale id list";
    w.ops.reserve(cfg.totalOps);

    Rng rng(cfg.seed);
    std::vector<std::uint64_t> stale; // the demo's list: never updated on fills
    ShadowBook shadow(cfg.totalOps);
    std::uint64_t nextId = 1;
    for (std::size_t i = 0; i < cfg.totalOps; ++i) {
        Op op;
        op.timed = i >= cfg.warmupOps;
        // Same RNG call order as the demo.
        if (!stale.empty() && rng() % 3 == 0) {
            const std::size_t pick = static_cast<std::size_t>(rng() % stale.size());
            op.kind = OpKind::CANCEL;
            op.id = {stale[pick]};
            stale[pick] = stale.back();
            stale.pop_back();
            shadow.apply(op, w.ops);
            continue;
        }
        op.kind = OpKind::ADD;
        op.side = rng() % 2 ? OrderSide::BUY : OrderSide::SELL;
        const std::int64_t offset = static_cast<std::int64_t>(rng() % 64) - 8;
        op.price = {op.side == OrderSide::BUY ? 10'000 - offset : 10'000 + offset};
        op.quantity = {1 + rng() % 100};
        op.id = {nextId};
        op.timestamp = {nextId};
        stale.push_back(nextId++);
        shadow.apply(op, w.ops);
    }
    shadow.finish(w);
    // The demo's dense book covers [9000, 11048); keep that so W1 matches it.
    w.minPrice = std::min(w.minPrice, Price{9'000});
    w.maxPrice = std::max(w.maxPrice, Price{9'000 + 2'047});
    return w;
}

} // namespace eigenbook::bench
