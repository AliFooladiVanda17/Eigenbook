#pragma once

// W5 microbursts: arrivals from a self-exciting (Hawkes) process with exponential kernel,
//   intensity(t) = mu + sum over past events of alpha * exp(-beta * (t - t_i)),
// simulated with Ogata's thinning. Bursts form because every arrival raises the rate for a few
// tens of microseconds. Branching ratio alpha / beta < 1; long-run mean rate mu / (1 - alpha/beta).
//
// Ops are mixed: passive adds near the touch, aggressive adds, cancels of live orders. The
// harness replays the stream open-loop: it waits until each op's scheduled time, and reports
// service time (the op itself) and response time (completion minus scheduled time, which
// includes waiting behind earlier ops in a burst).

#include <cmath>
#include <cstddef>
#include <cstdint>

#include <workloads/workload.hpp>

namespace eigenbook::bench {

struct MicroburstConfig {
    std::uint64_t seed{1};
    WorkloadSize size{};
    double baseRatePerSecond{100'000.0}; // mu
    double excitation{40'000.0};         // alpha, per second
    double decayPerSecond{50'000.0};     // beta: excitation decays with time constant 20 us
    double passiveFraction{0.45};
    double aggressiveFraction{0.10}; // remainder: cancels of live orders
    std::int64_t mid{10'000};
    std::int64_t band{50};
    std::uint64_t maxQuantity{20};
};

[[nodiscard]] inline Workload makeMicroburst(const MicroburstConfig& cfg) {
    Workload w;
    w.name = "w5-burst";
    w.seed = cfg.seed;
    w.warmupOps = cfg.size.warmupOps;
    w.paced = true;
    const double branching = cfg.excitation / cfg.decayPerSecond;
    w.description =
        "Hawkes arrivals, branching " + std::to_string(branching).substr(0, 4) + ", mean rate " +
        std::to_string(static_cast<long long>(cfg.baseRatePerSecond / (1.0 - branching))) + "/s";
    const std::size_t total = cfg.size.warmupOps + cfg.size.measuredOps;
    w.ops.reserve(total);

    Rng rng(cfg.seed);
    ShadowBook shadow(1 << 16);
    std::uint64_t nextId = 1;

    // Ogata thinning. `excited` is the kernel sum at time t.
    double t = 0.0;
    double excited = 0.0;
    double firstTimed = -1.0;
    for (std::size_t i = 0; i < total; ++i) {
        for (;;) {
            // Valid upper bound: the intensity only decays until the next event.
            const double upper = cfg.baseRatePerSecond + excited;
            const double wait = exponential(rng, upper);
            t += wait;
            excited *= std::exp(-cfg.decayPerSecond * wait);
            if (uniform01(rng) * upper <= cfg.baseRatePerSecond + excited) {
                break;
            }
        }
        excited += cfg.excitation;

        Op op;
        op.timed = i >= cfg.size.warmupOps;
        if (op.timed && firstTimed < 0.0) {
            firstTimed = t;
        }
        op.scheduledNs = op.timed ? static_cast<std::uint64_t>((t - firstTimed) * 1e9) : 0;

        const double r = uniform01(rng);
        if (r >= cfg.passiveFraction + cfg.aggressiveFraction && shadow.liveCount() > 0) {
            op.kind = OpKind::CANCEL;
            op.id = {shadow.randomLive(rng)};
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
        if (r >= cfg.passiveFraction && r < cfg.passiveFraction + cfg.aggressiveFraction) {
            // Through the touch, up to the far edge of the band.
            const std::int64_t through = uniformInt(rng, 0, cfg.band);
            op.price = {op.side == OrderSide::BUY ? cfg.mid + through : cfg.mid - through};
        } else {
            const std::int64_t off = uniformInt(rng, 1, cfg.band);
            op.price = {op.side == OrderSide::BUY ? cfg.mid - off : cfg.mid + off};
        }
        shadow.apply(op, w.ops);
    }
    shadow.finish(w);
    return w;
}

} // namespace eigenbook::bench
