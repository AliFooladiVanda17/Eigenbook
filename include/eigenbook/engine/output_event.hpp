#pragma once

// Engine output (Phase B3.1): what the engine reports for each sequenced input, in a gap-free
// output sequence. Like Command, an OutputEvent is a fixed-layout value without padding, so an
// output stream can be compared byte for byte between a run and its replay.

#include <cstddef>
#include <cstdint>
#include <type_traits>

#include <eigenbook/core/reject_reason.hpp>
#include <eigenbook/core/types.hpp>

namespace eigenbook {

enum class EventType : std::uint8_t {
    NONE = 0,
    FILL = 1,      // one execution; quantity at price between makerId and orderId (the taker)
    ACCEPTED = 2,  // a new order completed: quantity filled, resting, cancelled
    MODIFIED = 3,  // a modify completed: quantity filled, resting, cancelled
    CANCELLED = 4, // a cancel: quantity = the open quantity removed
    REJECTED = 5,  // the input changed nothing; reason says why
    HALTED = 6,
    RESUMED = 7,
};

// Fields an event type does not use are zero.
//
// | type      | orderId | makerId | price | quantity | resting | cancelled | tradeId |
// makerParticipant |
// |-----------|---------|---------|-------|----------|---------|-----------|---------|------------------|
// | FILL      | taker   | maker   | trade | traded   |         |           | yes     | yes | |
// ACCEPTED  | order   |         |       | filled   | resting | cancelled |         | | | MODIFIED
// | order   |         |       | filled   | resting | cancelled |         |                  | |
// CANCELLED | order   |         |       | open qty |         |           |         | | | REJECTED
// | order   |         |       |          |         |           |         |                  |
struct OutputEvent {
    std::uint64_t sequence{0};         // 0   output sequence, gap-free from 1
    std::uint64_t inputSequence{0};    // 8   the input that caused it
    Timestamp time{0};                 // 16  that input's sequencer time
    EventType type{EventType::NONE};   // 24
    RejectReason reason{};             // 25  REJECTED only
    OrderSide side{OrderSide::BUY};    // 26  FILL: taker side; orders: order side
    std::uint8_t reserved0{0};         // 27
    InstrumentId instrument{0};        // 28
    OrderId orderId{0};                // 32
    OrderId makerId{0};                // 40
    Price price{0};                    // 48
    Quantity quantity{0};              // 56
    Quantity resting{0};               // 64
    Quantity cancelled{0};             // 72
    TradeId tradeId{0};                // 80
    ParticipantId makerParticipant{0}; // 88
    std::uint32_t reserved1{0};        // 92

    friend bool operator==(const OutputEvent&, const OutputEvent&) = default;
};

static_assert(sizeof(OutputEvent) == 96);
static_assert(std::is_trivially_copyable_v<OutputEvent> && std::is_standard_layout_v<OutputEvent>);
static_assert(std::has_unique_object_representations_v<OutputEvent>, "no padding bytes");
static_assert(offsetof(OutputEvent, type) == 24 && offsetof(OutputEvent, instrument) == 28 &&
              offsetof(OutputEvent, orderId) == 32 && offsetof(OutputEvent, tradeId) == 80 &&
              offsetof(OutputEvent, makerParticipant) == 88 &&
              offsetof(OutputEvent, reserved1) == 92);

} // namespace eigenbook
