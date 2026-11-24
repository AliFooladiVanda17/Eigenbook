//
// Phase B3.1: sequencer, commands and the deterministic engine core. Every input is numbered
// once and gap-free, times never go back, the engine applies exactly the next input, reports
// every outcome through a gap-free output stream, and the same input log gives byte-identical
// output, on a fresh engine and on every book type.
//

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <random>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include <eigenbook/engine/command.hpp>
#include <eigenbook/engine/engine.hpp>
#include <eigenbook/engine/sequencer.hpp>

#include "test_support.hpp"

namespace eigenbook {

namespace {

constexpr InstrumentId kOne{1};
constexpr ParticipantId kAlice{11};
constexpr ParticipantId kBob{22};

struct EventLog {
    std::vector<OutputEvent> events;

    void operator()(const OutputEvent& e) { events.push_back(e); }
};

OrderRequest owned(OrderRequest r, const ParticipantId p) {
    r.participant = p;
    return r;
}

// An engine with one dense instrument and a sequencer on a manual clock.
struct Fixture {
    Engine<DenseOrderBook> engine;
    Sequencer<ManualClock> sequencer{ManualClock{Timestamp{1'000}}};
    EventLog log;

    Fixture() { EXPECT_TRUE(engine.addInstrument(kOne, {.minPrice = {0}, .tickCount = 1'000})); }

    // Sequences and applies one command; returns the events it produced.
    std::vector<OutputEvent> send(const Command& c) {
        const std::size_t before = log.events.size();
        sequencer.sourceClock().advance(10);
        EXPECT_TRUE(engine.apply(sequencer.sequence(c), log));
        return {log.events.begin() + static_cast<std::ptrdiff_t>(before), log.events.end()};
    }
};

} // namespace

TEST(Sequencer, NumbersAreGapFreeFromOne) {
    Sequencer<ManualClock> s;
    for (std::uint64_t n = 1; n <= 5; ++n) {
        EXPECT_EQ(s.sequence(Command::halt(kOne)).sequence, n);
    }
    EXPECT_EQ(s.nextSequence(), 6u);
}

TEST(Sequencer, TimeNeverGoesBackwards) {
    Sequencer<ManualClock> s{ManualClock{Timestamp{500}}};
    EXPECT_EQ(s.sequence(Command::halt(kOne)).time, Timestamp{500});
    s.sourceClock().set(Timestamp{300}); // the clock steps back
    EXPECT_EQ(s.sequence(Command::halt(kOne)).time, Timestamp{500});
    s.sourceClock().set(Timestamp{700});
    EXPECT_EQ(s.sequence(Command::halt(kOne)).time, Timestamp{700});
    EXPECT_EQ(s.lastTime(), Timestamp{700});
}

TEST(Sequencer, RestartContinuesAfterTheLastInput) {
    Sequencer<ManualClock> s{ManualClock{Timestamp{10}}, 101, Timestamp{900}};
    const SequencedCommand c = s.sequence(Command::resume(kOne));
    EXPECT_EQ(c.sequence, 101u);
    EXPECT_EQ(c.time, Timestamp{900}); // the clock is behind the journal: clamped
}

TEST(Sequencer, SteadyClockIsMonotonic) {
    Sequencer<SteadyClock> s;
    Timestamp previous{0};
    for (int i = 0; i < 1'000; ++i) {
        const Timestamp t = s.sequence(Command::halt(kOne)).time;
        EXPECT_GE(t, previous);
        previous = t;
    }
}

TEST(Command, RoundTripsOrderAndModifyRequests) {
    const OrderRequest r{.id = {7},
                         .side = OrderSide::SELL,
                         .type = OrderType::LIMIT,
                         .price = {101},
                         .quantity = {5},
                         .timestamp = {99},
                         .participant = kBob,
                         .timeInForce = TimeInForce::IOC,
                         .postOnly = false,
                         .minQuantity = {2}};
    const Command c = Command::newOrder(kOne, r);
    const OrderRequest back = c.orderRequest();
    EXPECT_EQ(back.id, r.id);
    EXPECT_EQ(back.side, r.side);
    EXPECT_EQ(back.price, r.price);
    EXPECT_EQ(back.quantity, r.quantity);
    EXPECT_EQ(back.participant, r.participant);
    EXPECT_EQ(back.timeInForce, r.timeInForce);
    EXPECT_EQ(back.minQuantity, r.minQuantity);
    EXPECT_EQ(back.timestamp, r.timestamp);
    const Command m = Command::modify(kOne, {.id = {7}, .price = {102}, .quantity = {3}});
    EXPECT_EQ(m.modifyRequest().price, Price{102});
    EXPECT_EQ(m.reserved1, 0u);
}

TEST(Engine, NewOrderReportsItsFillsThenItsSummary) {
    Fixture f;
    const auto resting =
        f.send(Command::newOrder(kOne, owned(limit(1, OrderSide::SELL, 100, 5), kAlice)));
    ASSERT_EQ(resting.size(), 1u);
    EXPECT_EQ(resting[0].type, EventType::ACCEPTED);
    EXPECT_EQ(resting[0].resting, Quantity{5});

    const auto taker =
        f.send(Command::newOrder(kOne, owned(limit(2, OrderSide::BUY, 100, 7), kBob)));
    ASSERT_EQ(taker.size(), 2u);
    EXPECT_EQ(taker[0].type, EventType::FILL);
    EXPECT_EQ(taker[0].orderId, OrderId{2});
    EXPECT_EQ(taker[0].makerId, OrderId{1});
    EXPECT_EQ(taker[0].price, Price{100});
    EXPECT_EQ(taker[0].quantity, Quantity{5});
    EXPECT_EQ(taker[0].tradeId, TradeId{1});
    EXPECT_EQ(taker[0].makerParticipant, kAlice);
    EXPECT_EQ(taker[0].side, OrderSide::BUY);
    EXPECT_EQ(taker[1].type, EventType::ACCEPTED);
    EXPECT_EQ(taker[1].quantity, Quantity{5}); // filled
    EXPECT_EQ(taker[1].resting, Quantity{2});
    EXPECT_EQ(taker[1].cancelled, Quantity{0});
    // Output and input sequences: gap-free, and each event names its input.
    EXPECT_EQ(resting[0].sequence, 1u);
    EXPECT_EQ(taker[0].sequence, 2u);
    EXPECT_EQ(taker[1].sequence, 3u);
    EXPECT_EQ(taker[0].inputSequence, 2u);
    EXPECT_EQ(taker[1].inputSequence, 2u);
    EXPECT_EQ(taker[1].time, Timestamp{1'020});
}

TEST(Engine, RejectsAreReportedAndChangeNothing) {
    Fixture f;
    (void)f.send(Command::newOrder(kOne, owned(limit(1, OrderSide::SELL, 100, 5), kAlice)));
    const auto unknown =
        f.send(Command::newOrder(InstrumentId{9}, limit(2, OrderSide::BUY, 100, 1)));
    ASSERT_EQ(unknown.size(), 1u);
    EXPECT_EQ(unknown[0].type, EventType::REJECTED);
    EXPECT_EQ(unknown[0].reason, RejectReason::UNKNOWN_INSTRUMENT);
    const auto zero = f.send(Command::newOrder(kOne, limit(3, OrderSide::BUY, 100, 0)));
    EXPECT_EQ(zero.at(0).reason, RejectReason::INVALID_QUANTITY);
    const auto notOwner = f.send(Command::cancel(kOne, OrderId{1}, kBob));
    EXPECT_EQ(notOwner.at(0).reason, RejectReason::NOT_OWNER);
    Command noType; // CommandType::NONE: never valid input
    noType.instrument = kOne;
    const auto invalid = f.send(noType);
    EXPECT_EQ(invalid.at(0).reason, RejectReason::INVALID_COMMAND);
    EXPECT_EQ(f.send(Command{}).at(0).reason, RejectReason::UNKNOWN_INSTRUMENT); // instrument 0
    EXPECT_EQ(f.engine.book(kOne)->findOrder({1})->getQuantity(), Quantity{5});
}

TEST(Engine, CancelAndModifyReportTheirOutcome) {
    Fixture f;
    (void)f.send(Command::newOrder(kOne, owned(limit(1, OrderSide::SELL, 100, 5), kAlice)));
    (void)f.send(Command::newOrder(kOne, owned(limit(2, OrderSide::BUY, 98, 4), kBob)));
    // Bob moves his bid up to the ask: it re-enters and trades.
    const auto modified = f.send(
        Command::modify(kOne, {.id = {2}, .price = {100}, .quantity = {4}, .participant = kBob}));
    ASSERT_EQ(modified.size(), 2u);
    EXPECT_EQ(modified[0].type, EventType::FILL);
    EXPECT_EQ(modified[1].type, EventType::MODIFIED);
    EXPECT_EQ(modified[1].quantity, Quantity{4});
    const auto cancelled = f.send(Command::cancel(kOne, OrderId{1}, kAlice));
    ASSERT_EQ(cancelled.size(), 1u);
    EXPECT_EQ(cancelled[0].type, EventType::CANCELLED);
    EXPECT_EQ(cancelled[0].quantity, Quantity{1});
    EXPECT_TRUE(f.engine.book(kOne)->empty());
}

TEST(Engine, HaltAndResumeAreSequencedInputs) {
    Fixture f;
    (void)f.send(Command::newOrder(kOne, limit(1, OrderSide::SELL, 100, 5)));
    EXPECT_EQ(f.send(Command::halt(kOne)).at(0).type, EventType::HALTED);
    EXPECT_EQ(f.send(Command::newOrder(kOne, limit(2, OrderSide::BUY, 100, 1))).at(0).reason,
              RejectReason::INSTRUMENT_HALTED);
    EXPECT_EQ(f.send(Command::cancel(kOne, OrderId{1}, ParticipantId{0})).at(0).type,
              EventType::CANCELLED);
    EXPECT_EQ(f.send(Command::resume(kOne)).at(0).type, EventType::RESUMED);
    EXPECT_EQ(f.send(Command::newOrder(kOne, limit(3, OrderSide::BUY, 100, 1))).at(0).type,
              EventType::ACCEPTED);
}

TEST(Engine, GapsAndDuplicatesAreRefusedWithoutOutput) {
    Engine<DenseOrderBook> engine;
    ASSERT_TRUE(engine.addInstrument(kOne, {.minPrice = {0}, .tickCount = 1'000}));
    EventLog log;
    const Command c = Command::newOrder(kOne, limit(1, OrderSide::BUY, 100, 1));
    ASSERT_TRUE(engine.apply({.sequence = 1, .time = {5}, .command = c}, log));
    EXPECT_EQ(engine.apply({.sequence = 3, .time = {6}, .command = c}, log).error(),
              ApplyError::SEQUENCE_GAP);
    EXPECT_EQ(engine.apply({.sequence = 1, .time = {6}, .command = c}, log).error(),
              ApplyError::DUPLICATE_SEQUENCE);
    EXPECT_EQ(log.events.size(), 1u);
    EXPECT_EQ(engine.nextInputSequence(), 2u);
    EXPECT_EQ(engine.book(kOne)->orderCount(), 1u);
}

TEST(Engine, InstrumentRegistrationIsChecked) {
    Engine<DenseOrderBook> engine;
    EXPECT_TRUE(engine.addInstrument(kOne, {.minPrice = {0}, .tickCount = 16}));
    EXPECT_FALSE(engine.addInstrument(kOne, {.minPrice = {0}, .tickCount = 16}));
    EXPECT_FALSE(engine.addInstrument(InstrumentId{Engine<DenseOrderBook>::kMaxInstruments},
                                      {.minPrice = {0}, .tickCount = 16}));
    EXPECT_EQ(engine.book(InstrumentId{2}), nullptr);
}

namespace {

// A seeded command log over three instruments: adds (some market, some IOC / FOK / post-only),
// participant cancels and modifies of ids that may or may not rest, and rare halts and resumes.
std::vector<SequencedCommand> randomLog(const std::uint64_t seed, const int count) {
    std::mt19937_64 rng(seed);
    Sequencer<ManualClock> sequencer;
    std::vector<SequencedCommand> log;
    log.reserve(static_cast<std::size_t>(count));
    std::uint64_t nextId = 1;
    // Instrument and owner of every id sent so far (index = id - 1), so that most cancels and
    // modifies address a real order as its owner; the rest stay random and are mostly rejected.
    std::vector<std::pair<InstrumentId, ParticipantId>> sent;
    for (int i = 0; i < count; ++i) {
        sequencer.sourceClock().advance(1 + rng() % 1'000);
        InstrumentId instrument{static_cast<std::uint32_t>(1 + rng() % 3)};
        ParticipantId participant{static_cast<std::uint32_t>(rng() % 3)};
        const auto op = rng() % 100;
        Command c;
        std::uint64_t target = 1 + rng() % nextId;
        if (op >= 60 && op < 98 && !sent.empty() && rng() % 5 != 0) {
            // Recent ids are more likely to still rest.
            const std::uint64_t window = std::min<std::uint64_t>(sent.size(), 50);
            target = sent.size() - rng() % window;
            instrument = sent[target - 1].first;
            participant = sent[target - 1].second;
        }
        if (op < 60) {
            sent.emplace_back(instrument, participant);
            OrderRequest r = limit(nextId++,
                                   rng() % 2 != 0 ? OrderSide::BUY : OrderSide::SELL,
                                   980 + static_cast<std::int64_t>(rng() % 41),
                                   1 + rng() % 20);
            r.participant = participant;
            if (rng() % 10 == 0) {
                r.type = OrderType::MARKET;
            }
            constexpr TimeInForce kTifs[] = {TimeInForce::DAY,
                                             TimeInForce::GTC,
                                             TimeInForce::IOC,
                                             TimeInForce::FOK};
            r.timeInForce = kTifs[rng() % 4];
            r.postOnly = r.type == OrderType::LIMIT && r.timeInForce <= TimeInForce::GTC &&
                         rng() % 5 == 0;
            c = Command::newOrder(instrument, r);
        } else if (op < 80) {
            c = Command::cancel(instrument, OrderId{target}, participant);
        } else if (op < 98) {
            c = Command::modify(instrument,
                                {.id = {target},
                                 .price = {980 + static_cast<std::int64_t>(rng() % 41)},
                                 .quantity = {1 + rng() % 20},
                                 .participant = participant});
        } else {
            c = rng() % 2 != 0 ? Command::halt(instrument) : Command::resume(instrument);
        }
        log.push_back(sequencer.sequence(c));
    }
    return log;
}

template <typename Book>
std::vector<OutputEvent> run(const std::vector<SequencedCommand>& log,
                             const typename Book::LadderConfig config) {
    Engine<Book> engine;
    for (std::uint32_t i = 1; i <= 3; ++i) {
        EXPECT_TRUE(engine.addInstrument(InstrumentId{i}, config));
    }
    EventLog out;
    for (const SequencedCommand& in : log) {
        EXPECT_TRUE(engine.apply(in, out));
    }
    return out.events;
}

bool sameBytes(const std::vector<OutputEvent>& a, const std::vector<OutputEvent>& b) {
    return a.size() == b.size() &&
           (a.empty() || std::memcmp(a.data(), b.data(), a.size() * sizeof(OutputEvent)) == 0);
}

} // namespace

TEST(Determinism, TheSameLogGivesByteIdenticalOutput) {
    const auto log = randomLog(42, 20'000);
    const ArrayLadderConfig dense{.minPrice = {900}, .tickCount = 200};
    const auto first = run<DenseOrderBook>(log, dense);
    const auto second = run<DenseOrderBook>(log, dense);
    EXPECT_TRUE(sameBytes(first, second));
    // The output stream is gap-free, and every input has exactly one summary event.
    std::size_t summaries = 0;
    for (std::size_t k = 0; k < first.size(); ++k) {
        ASSERT_EQ(first[k].sequence, k + 1);
        summaries += first[k].type != EventType::FILL ? 1 : 0;
    }
    EXPECT_EQ(summaries, log.size());
    // The log is not trivial: every kind of outcome occurs many times.
    std::size_t counts[8] = {};
    for (const OutputEvent& e : first) {
        ++counts[static_cast<std::size_t>(e.type)];
    }
    for (const EventType t : {EventType::FILL,
                              EventType::ACCEPTED,
                              EventType::MODIFIED,
                              EventType::CANCELLED,
                              EventType::REJECTED,
                              EventType::HALTED,
                              EventType::RESUMED}) {
        EXPECT_GT(counts[static_cast<std::size_t>(t)], 50u) << static_cast<int>(t);
    }
    std::cout << "[          ] 20,000 inputs -> " << first.size() << " events: fills " << counts[1]
              << ", accepted " << counts[2] << ", modified " << counts[3] << ", cancelled "
              << counts[4] << ", rejected " << counts[5] << ", halted " << counts[6] << ", resumed "
              << counts[7] << '\n';
}

TEST(Determinism, EveryBookTypeGivesTheSameOutput) {
    const auto log = randomLog(7, 20'000);
    const auto dense = run<DenseOrderBook>(log, {.minPrice = {900}, .tickCount = 200});
    EXPECT_TRUE(sameBytes(dense, run<OrderBook>(log, {})));
    EXPECT_TRUE(sameBytes(dense, run<OrderBookM2>(log, {.maxLevels = 256})));
    EXPECT_TRUE(sameBytes(dense, run<HybridOrderBook>(log, smallWindowHybrid())));
}

} // namespace eigenbook
