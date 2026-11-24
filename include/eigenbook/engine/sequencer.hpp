#pragma once

// The sequencer (Phase B3.1): gives every input exactly one place in a gap-free global order and
// the only timestamp the engine ever sees.

#include <chrono>
#include <concepts>
#include <cstdint>

#include <eigenbook/core/types.hpp>
#include <eigenbook/engine/command.hpp>

namespace eigenbook {

// A source of time for the sequencer, in nanoseconds on an arbitrary epoch.
template <typename C>
concept EngineClock = requires(C clock) {
    { clock.now() } noexcept -> std::same_as<Timestamp>;
};

// Time set by the caller: tests, simulations and replays.
class ManualClock {
public:
    explicit ManualClock(const Timestamp start = Timestamp{0}) noexcept : current(start) {}

    [[nodiscard]] Timestamp now() const noexcept { return current; }

    void set(const Timestamp t) noexcept { current = t; }

    void advance(const std::uint64_t nanoseconds) noexcept { current.value += nanoseconds; }

private:
    Timestamp current;
};

// std::chrono::steady_clock in nanoseconds since its (unspecified) epoch.
class SteadyClock {
public:
    [[nodiscard]] Timestamp now() const noexcept {
        const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch());
        return Timestamp{static_cast<std::uint64_t>(ns.count())};
    }
};

static_assert(EngineClock<ManualClock>);
static_assert(EngineClock<SteadyClock>);

template <EngineClock Clock>
class Sequencer {
public:
    // A fresh sequence starts at 1. A restart continues after the last journaled input
    // (B3.2): nextSequence = last + 1, lastTime = its timestamp.
    explicit Sequencer(Clock iClock = Clock(),
                       const std::uint64_t nextSequence = 1,
                       const Timestamp lastTime = Timestamp{0}) noexcept
        : clock(iClock), next(nextSequence), last(lastTime) {}

    // Numbers are consecutive; times never decrease (a clock that steps back is clamped to the
    // last time issued, so the sequence order and the time order agree).
    [[nodiscard]] SequencedCommand sequence(const Command& command) noexcept {
        Timestamp t = clock.now();
        if (t < last) {
            t = last;
        }
        last = t;
        return SequencedCommand{.sequence = next++, .time = t, .command = command};
    }

    [[nodiscard]] std::uint64_t nextSequence() const noexcept { return next; }

    [[nodiscard]] Timestamp lastTime() const noexcept { return last; }

    [[nodiscard]] Clock& sourceClock() noexcept { return clock; }

private:
    Clock clock;
    std::uint64_t next;
    Timestamp last;
};

} // namespace eigenbook
