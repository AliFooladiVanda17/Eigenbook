#pragma once

// Sequenced inputs (Phase B3.1).
//
// Everything the engine reacts to is a Command, given a sequence number and a timestamp by the
// Sequencer exactly once. A Command is a fixed-layout, trivially copyable value with no padding:
// the journal (B3.2) stores it byte for byte, and a replay must be able to reproduce the original
// run bit for bit. Every field offset is static-asserted; a layout change is a journal schema
// change.

#include <cstddef>
#include <cstdint>
#include <type_traits>

#include <eigenbook/book/order_book.hpp>
#include <eigenbook/core/types.hpp>

namespace eigenbook {

enum class CommandType : std::uint8_t {
    NONE = 0, // a value-initialised Command; never valid input
    NEW_ORDER = 1,
    CANCEL = 2, // participant cancel: only the owner may cancel
    MODIFY = 3,
    HALT = 4,
    RESUME = 5,
};

// One input. Fields a command type does not use are zero.
struct Command {
    CommandType type{CommandType::NONE};       // 0
    OrderSide side{OrderSide::BUY};            // 1   NEW_ORDER
    OrderType orderType{OrderType::LIMIT};     // 2   NEW_ORDER
    TimeInForce timeInForce{TimeInForce::DAY}; // 3  NEW_ORDER
    std::uint8_t postOnly{0};                  // 4   NEW_ORDER: 0 or 1
    std::uint8_t reserved0[3]{};               // 5   zero
    InstrumentId instrument{0};                // 8
    ParticipantId participant{0};              // 12  NEW_ORDER, CANCEL, MODIFY
    OrderId id{0};                             // 16  NEW_ORDER, CANCEL, MODIFY
    Price price{0};                            // 24  NEW_ORDER (limit), MODIFY
    Quantity quantity{0};                      // 32  NEW_ORDER, MODIFY
    Quantity minQuantity{0};                   // 40  NEW_ORDER
    Timestamp clientTime{0};                   // 48  caller time, for reporting only
    std::uint64_t reserved1{0};                // 56  zero

    [[nodiscard]] static Command newOrder(const InstrumentId instrument,
                                          const OrderRequest& r) noexcept {
        Command c;
        c.type = CommandType::NEW_ORDER;
        c.side = r.side;
        c.orderType = r.type;
        c.timeInForce = r.timeInForce;
        c.postOnly = r.postOnly ? 1 : 0;
        c.instrument = instrument;
        c.participant = r.participant;
        c.id = r.id;
        c.price = r.price;
        c.quantity = r.quantity;
        c.minQuantity = r.minQuantity;
        c.clientTime = r.timestamp;
        return c;
    }

    [[nodiscard]] static Command cancel(const InstrumentId instrument,
                                        const OrderId id,
                                        const ParticipantId participant) noexcept {
        Command c;
        c.type = CommandType::CANCEL;
        c.instrument = instrument;
        c.participant = participant;
        c.id = id;
        return c;
    }

    [[nodiscard]] static Command modify(const InstrumentId instrument,
                                        const ModifyRequest& r) noexcept {
        Command c;
        c.type = CommandType::MODIFY;
        c.instrument = instrument;
        c.participant = r.participant;
        c.id = r.id;
        c.price = r.price;
        c.quantity = r.quantity;
        c.clientTime = r.timestamp;
        return c;
    }

    [[nodiscard]] static Command halt(const InstrumentId instrument) noexcept {
        Command c;
        c.type = CommandType::HALT;
        c.instrument = instrument;
        return c;
    }

    [[nodiscard]] static Command resume(const InstrumentId instrument) noexcept {
        Command c;
        c.type = CommandType::RESUME;
        c.instrument = instrument;
        return c;
    }

    [[nodiscard]] OrderRequest orderRequest() const noexcept {
        return {.id = id,
                .side = side,
                .type = orderType,
                .price = price,
                .quantity = quantity,
                .timestamp = clientTime,
                .participant = participant,
                .timeInForce = timeInForce,
                .postOnly = postOnly != 0,
                .minQuantity = minQuantity};
    }

    [[nodiscard]] ModifyRequest modifyRequest() const noexcept {
        return {.id = id,
                .price = price,
                .quantity = quantity,
                .timestamp = clientTime,
                .participant = participant};
    }

    friend bool operator==(const Command&, const Command&) = default;
};

// A command with its place in the global order and the only time the engine sees.
struct SequencedCommand {
    std::uint64_t sequence{0}; // 0   gap-free, from 1
    Timestamp time{0};         // 8   sequencer time, non-decreasing
    Command command;           // 16

    friend bool operator==(const SequencedCommand&, const SequencedCommand&) = default;
};

static_assert(sizeof(Command) == 64);
static_assert(std::is_trivially_copyable_v<Command> && std::is_standard_layout_v<Command>);
static_assert(std::has_unique_object_representations_v<Command>, "no padding bytes");
static_assert(offsetof(Command, type) == 0 && offsetof(Command, side) == 1 &&
              offsetof(Command, orderType) == 2 && offsetof(Command, timeInForce) == 3 &&
              offsetof(Command, postOnly) == 4 && offsetof(Command, reserved0) == 5 &&
              offsetof(Command, instrument) == 8 && offsetof(Command, participant) == 12 &&
              offsetof(Command, id) == 16 && offsetof(Command, price) == 24 &&
              offsetof(Command, quantity) == 32 && offsetof(Command, minQuantity) == 40 &&
              offsetof(Command, clientTime) == 48 && offsetof(Command, reserved1) == 56);

static_assert(sizeof(SequencedCommand) == 80);
static_assert(std::is_trivially_copyable_v<SequencedCommand>);
static_assert(std::has_unique_object_representations_v<SequencedCommand>, "no padding bytes");
static_assert(offsetof(SequencedCommand, sequence) == 0 && offsetof(SequencedCommand, time) == 8 &&
              offsetof(SequencedCommand, command) == 16);

} // namespace eigenbook
