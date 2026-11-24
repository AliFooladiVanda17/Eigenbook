#pragma once

// The deterministic engine core (Phase B3.1): books per instrument, driven only by sequenced
// commands, reporting everything through a gap-free output stream.
//
// Determinism: apply() is a function of the engine state and the input. It reads no clock (the
// input carries the sequencer's time), iterates no hash table, and produces events in a fixed
// order (the fills of a request, then its summary event), so the same input log gives the same
// output stream on every run and every book type.

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <vector>

#include <eigenbook/book/order_book.hpp>
#include <eigenbook/core/book_limits.hpp>
#include <eigenbook/core/reject_reason.hpp>
#include <eigenbook/engine/command.hpp>
#include <eigenbook/engine/output_event.hpp>

namespace eigenbook {

// Why apply() refused an input. These are faults of the input stream, not business rejects:
// nothing is emitted and nothing changes.
enum class ApplyError : std::uint8_t {
    SEQUENCE_GAP,       // the input's sequence is beyond the next one: an input is missing
    DUPLICATE_SEQUENCE, // the input's sequence was applied already
};

template <typename S>
concept OutputSink = requires(S sink, const OutputEvent& e) { sink(e); };

template <typename Book>
class Engine {
public:
    using LadderConfig = typename Book::LadderConfig;

    // Instrument ids index a flat table, so they must stay below this bound.
    static constexpr std::uint32_t kMaxInstruments = 1u << 16;

    explicit Engine(const std::uint64_t nextInputSequence = 1,
                    const std::uint64_t nextOutputSequence = 1)
        : nextInput(nextInputSequence), nextOutput(nextOutputSequence) {}

    Engine(const Engine&) = delete;
    Engine& operator=(const Engine&) = delete;

    // Cold path: registers a book. False if the id is taken or not below kMaxInstruments.
    bool addInstrument(const InstrumentId id,
                       const LadderConfig config = {},
                       const BookLimits limits = {}) {
        if (id.value >= kMaxInstruments) {
            return false;
        }
        if (books.size() <= id.value) {
            books.resize(id.value + 1);
        }
        if (books[id.value]) {
            return false;
        }
        books[id.value] = std::make_unique<Book>(id, config, limits);
        return true;
    }

    [[nodiscard]] const Book* book(const InstrumentId id) const noexcept {
        return id.value < books.size() ? books[id.value].get() : nullptr;
    }

    [[nodiscard]] std::uint64_t nextInputSequence() const noexcept { return nextInput; }

    [[nodiscard]] std::uint64_t nextOutputSequence() const noexcept { return nextOutput; }

    // Hot path. Applies exactly the next input, emits its events to sink(const OutputEvent&).
    template <OutputSink Sink>
    std::expected<void, ApplyError> apply(const SequencedCommand& in, Sink& sink) {
        if (in.sequence != nextInput) [[unlikely]] {
            return std::unexpected(in.sequence > nextInput ? ApplyError::SEQUENCE_GAP
                                                           : ApplyError::DUPLICATE_SEQUENCE);
        }
        ++nextInput;
        const Command& c = in.command;
        Book* b = c.instrument.value < books.size() ? books[c.instrument.value].get() : nullptr;
        if (b == nullptr) [[unlikely]] {
            reject(in, RejectReason::UNKNOWN_INSTRUMENT, sink);
            return {};
        }
        switch (c.type) {
            case CommandType::NEW_ORDER:
                onNewOrder(in, *b, sink);
                break;
            case CommandType::CANCEL:
                onCancel(in, *b, sink);
                break;
            case CommandType::MODIFY:
                onModify(in, *b, sink);
                break;
            case CommandType::HALT:
                b->halt();
                emit(in, EventType::HALTED, sink);
                break;
            case CommandType::RESUME:
                if (b->resume()) {
                    emit(in, EventType::RESUMED, sink);
                } else {
                    reject(in, RejectReason::INSTRUMENT_HALTED, sink); // halted by a failed audit
                }
                break;
            case CommandType::NONE:
            default:
                reject(in, RejectReason::INVALID_COMMAND, sink);
                break;
        }
        return {};
    }

private:
    // An event for input `in` with the next output sequence; the other fields are the caller's.
    [[nodiscard]] OutputEvent event(const SequencedCommand& in, const EventType type) noexcept {
        OutputEvent e;
        e.sequence = nextOutput++;
        e.inputSequence = in.sequence;
        e.time = in.time;
        e.type = type;
        e.side = in.command.side;
        e.instrument = in.command.instrument;
        e.orderId = in.command.id;
        return e;
    }

    template <typename Sink>
    void emit(const SequencedCommand& in, const EventType type, Sink& sink) {
        sink(static_cast<const OutputEvent&>(event(in, type)));
    }

    template <typename Sink>
    void reject(const SequencedCommand& in, const RejectReason reason, Sink& sink) {
        OutputEvent e = event(in, EventType::REJECTED);
        e.reason = reason;
        sink(static_cast<const OutputEvent&>(e));
    }

    // Fills are reported as they happen, then the request's summary.
    template <typename Sink>
    auto fillReporter(const SequencedCommand& in, Sink& sink) {
        return [this, &in, &sink](const Fill& f) {
            OutputEvent e = event(in, EventType::FILL);
            e.side = f.takerSide;
            e.orderId = f.takerId;
            e.makerId = f.makerId;
            e.price = f.price;
            e.quantity = f.quantity;
            e.tradeId = f.tradeId;
            e.makerParticipant = f.makerParticipant;
            sink(static_cast<const OutputEvent&>(e));
        };
    }

    template <typename Sink>
    void summarise(const SequencedCommand& in,
                   const EventType type,
                   const typename Book::Result& r,
                   Sink& sink) {
        if (!r) {
            reject(in, r.error(), sink);
            return;
        }
        OutputEvent e = event(in, type);
        e.quantity = r->filled;
        e.resting = r->resting;
        e.cancelled = r->cancelled;
        sink(static_cast<const OutputEvent&>(e));
    }

    template <typename Sink>
    void onNewOrder(const SequencedCommand& in, Book& b, Sink& sink) {
        const auto r = b.addOrder(in.command.orderRequest(), fillReporter(in, sink));
        summarise(in, EventType::ACCEPTED, r, sink);
    }

    template <typename Sink>
    void onModify(const SequencedCommand& in, Book& b, Sink& sink) {
        const auto r = b.modifyOrder(in.command.modifyRequest(), fillReporter(in, sink));
        summarise(in, EventType::MODIFIED, r, sink);
    }

    template <typename Sink>
    void onCancel(const SequencedCommand& in, Book& b, Sink& sink) {
        const auto r = b.cancelOrder(in.command.id, in.command.participant);
        if (!r) {
            reject(in, r.error(), sink);
            return;
        }
        OutputEvent e = event(in, EventType::CANCELLED);
        e.quantity = *r;
        sink(static_cast<const OutputEvent&>(e));
    }

    std::vector<std::unique_ptr<Book>> books; // index = InstrumentId::value
    std::uint64_t nextInput;
    std::uint64_t nextOutput;
};

} // namespace eigenbook
