#pragma once

// Replays one Op against a book and checks the outcome against what the generator predicted.
// Shared by the latency harness and the zero-allocation test so both run exactly the same code.

#include <cstdint>

#include <harness/op_class.hpp>
#include <workloads/workload.hpp>

#include <eigenbook/book/order_book.hpp>

namespace eigenbook::bench {

// Runs `op` on `book`. Returns false if the book rejected it. `fills` is incremented once per
// execution, inside the fill callback (that is the only work the callback does).
template <typename Book>
inline bool runOp(Book& book, const Op& op, std::uint32_t& fills) {
    auto onFill = [&fills](const Fill&) noexcept {
        ++fills;
    };
    switch (op.kind) {
        case OpKind::ADD:
            {
                const OrderRequest request{.id = op.id,
                                           .side = op.side,
                                           .type = op.type,
                                           .price = op.price,
                                           .quantity = op.quantity,
                                           .timestamp = op.timestamp};
                return book.addOrder(request, onFill).has_value();
            }
        case OpKind::CANCEL:
            return book.cancelOrder(op.id).has_value();
        case OpKind::MODIFY:
            {
                const ModifyRequest request{.id = op.id,
                                            .price = op.price,
                                            .quantity = op.quantity,
                                            .timestamp = op.timestamp};
                return book.modifyOrder(request, onFill).has_value();
            }
    }
    return false;
}

// True if what the book did matches the generator's prediction for `op`.
[[nodiscard]] inline bool
outcomeMatches(const Op& op, const bool accepted, const std::uint32_t fills) noexcept {
    if (fills != op.expectedFills) {
        return false;
    }
    switch (op.kind) {
        case OpKind::ADD:
            return accepted && addClassFor(fills) == op.expected;
        case OpKind::CANCEL:
            return accepted == (op.expected != OpClass::CANCEL_MISS);
        case OpKind::MODIFY:
            return accepted;
    }
    return false;
}

} // namespace eigenbook::bench
