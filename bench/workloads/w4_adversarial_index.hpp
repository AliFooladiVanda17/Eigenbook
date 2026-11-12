#pragma once

// W4 adversarial index: stresses the order index (OrderId -> Order*) on the cancel path.
//
// COLLIDE: ids are built by inverting OrderIndex::hash so that all of them have the same home
//          slot. Linear probing then puts them in one cluster of length C, and every find, insert
//          and erase of such an id walks that cluster. This is what a client could do if it chose
//          its own order ids (HashDoS). A small stream of cancels for colliding ids that were never
//          added is included: each of those scans the whole cluster.
// LOAD_HALF: the index at its maximum load factor of 1/2 (capacity/2 - 1 live orders) with
//          sequential ids, the natural worst case without an adversary.
//
// Each timed op is paired with an untimed op that keeps the number of live orders constant.

#include <bit>
#include <cstddef>
#include <cstdint>
#include <vector>

#include <workloads/workload.hpp>

#include <eigenbook/book/locator/hash_order_index.hpp>

namespace eigenbook::bench {

// Inverse of OrderIndex::hash (the splitmix64 finalizer, a bijection on 64-bit integers).
[[nodiscard]] constexpr std::uint64_t unhash(std::uint64_t x) noexcept {
    // Inverse of an odd multiplier modulo 2^64 by Newton iteration (each step doubles the bits).
    constexpr auto inverse = [](const std::uint64_t a) {
        std::uint64_t inv = a;
        for (int i = 0; i < 5; ++i) {
            inv *= 2 - a * inv;
        }
        return inv;
    };
    x ^= (x >> 31) ^ (x >> 62);
    x *= inverse(0x94d049bb133111ebULL);
    x ^= (x >> 27) ^ (x >> 54);
    x *= inverse(0xbf58476d1ce4e5b9ULL);
    x ^= (x >> 30) ^ (x >> 60);
    return x;
}

enum class IndexAttack : std::uint8_t {
    COLLIDE,
    LOAD_HALF
};

struct AdversarialIndexConfig {
    std::uint64_t seed{1};
    WorkloadSize size{};
    IndexAttack mode{IndexAttack::COLLIDE};
    std::size_t expectedOrders{std::size_t{1} << 17}; // book sizing; fixes the index capacity
    std::size_t collidingIds{4'096};                  // COLLIDE: cluster length
    double missFraction{0.05};                        // COLLIDE: cancels of colliding, absent ids
    std::int64_t mid{50'000};
    std::int64_t levelsPerSide{50};
};

[[nodiscard]] inline Workload makeAdversarialIndex(const AdversarialIndexConfig& cfg) {
    Workload w;
    const bool collide = cfg.mode == IndexAttack::COLLIDE;
    w.name = collide ? "w4-collide" : "w4-load50";
    w.seed = cfg.seed;
    w.expectedOrders = cfg.expectedOrders;

    const std::size_t capacity = OrderIndex::capacityFor(cfg.expectedOrders);
    const auto bits = static_cast<unsigned>(std::countr_zero(capacity));
    Rng rng(cfg.seed);
    const std::uint64_t homeSlot = rng() & (capacity - 1);
    std::uint64_t nextColliding = 1;
    std::uint64_t nextSequential = 1;
    // j-th colliding id: the preimage of a hash whose low `bits` bits are homeSlot.
    auto collidingId = [&](const std::uint64_t j) {
        return unhash((j << bits) | homeSlot);
    };
    auto freshId = [&]() {
        return collide ? collidingId(nextColliding++) : nextSequential++;
    };

    const std::size_t liveTarget = collide ? cfg.collidingIds : capacity / 2 - 1;
    w.description = collide ? std::to_string(cfg.collidingIds) +
                                  " ids sharing one home slot, index capacity " +
                                  std::to_string(capacity)
                            : "index at load 1/2: " + std::to_string(liveTarget) +
                                  " live of capacity " + std::to_string(capacity);
    ShadowBook shadow(liveTarget + 1024);
    std::vector<std::uint64_t> liveIds;
    liveIds.reserve(liveTarget + 1);
    std::uint64_t ts = 1;

    auto add = [&](const bool timed) {
        Op op;
        op.kind = OpKind::ADD;
        op.side = bernoulli(rng, 0.5) ? OrderSide::BUY : OrderSide::SELL;
        const std::int64_t level = uniformInt(rng, 1, cfg.levelsPerSide);
        op.price = {op.side == OrderSide::BUY ? cfg.mid - level : cfg.mid + level};
        op.quantity = {1};
        op.id = {freshId()};
        op.timestamp = {ts++};
        op.timed = timed;
        shadow.apply(op, w.ops);
        liveIds.push_back(op.id.value);
    };
    auto cancelRandom = [&](const bool timed) {
        const std::size_t pick = uniformIndex(rng, liveIds.size());
        Op op;
        op.kind = OpKind::CANCEL;
        op.id = {liveIds[pick]};
        op.timed = timed;
        liveIds[pick] = liveIds.back();
        liveIds.pop_back();
        shadow.apply(op, w.ops);
    };

    for (std::size_t i = 0; i < liveTarget; ++i) {
        add(false);
    }
    std::size_t timed = 0;
    bool measuring = false;
    while (timed < cfg.size.measuredOps) {
        if (!measuring && w.ops.size() >= cfg.size.warmupOps) {
            measuring = true;
            w.warmupOps = w.ops.size();
        }
        if (collide && bernoulli(rng, cfg.missFraction)) {
            Op op;
            op.kind = OpKind::CANCEL;
            // A colliding id far beyond the ones handed out: never added.
            op.id = {collidingId((std::uint64_t{1} << 40) + nextColliding)};
            op.timed = measuring;
            shadow.apply(op, w.ops);
        } else if (bernoulli(rng, 0.5)) {
            cancelRandom(measuring);
            add(false);
        } else {
            add(measuring);
            cancelRandom(false);
        }
        timed += measuring ? 1 : 0;
    }
    shadow.finish(w);
    w.expectedOrders = cfg.expectedOrders; // exact: the attack depends on the index capacity
    return w;
}

} // namespace eigenbook::bench
