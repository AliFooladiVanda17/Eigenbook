#pragma once

// Replays one Op against a book and checks the outcome against what the generator predicted.
// Shared by the latency harness and the zero-allocation test so both run exactly the same code.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

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

// Maps workload ids to the handles an issuing book (DirectOrderTable) gave out. Built from the
// workload before any timing, so lookups and updates never allocate. An id with no live handle
// maps to OrderId{0}: generation 0 is never issued, so the book reports it as unknown (the same
// miss the generator predicted for a cancel of a filled or never-added order).
class HandleTable {
public:
    explicit HandleTable(const Workload& w) {
        for (const Op& op : w.ops) {
            if (op.kind == OpKind::ADD) {
                ids.push_back(op.id.value);
            }
        }
        std::ranges::sort(ids);
        const auto [first, last] = std::ranges::unique(ids);
        ids.erase(first, last);
        handles.assign(ids.size(), OrderId{0});
    }

    [[nodiscard]] OrderId lookup(const OrderId workloadId) const noexcept {
        const auto it = std::ranges::lower_bound(ids, workloadId.value);
        if (it == ids.end() || *it != workloadId.value) {
            return OrderId{0};
        }
        return handles[static_cast<std::size_t>(it - ids.begin())];
    }

    void set(const OrderId workloadId, const OrderId handle) noexcept {
        const auto it = std::ranges::lower_bound(ids, workloadId.value);
        if (it != ids.end() && *it == workloadId.value) {
            handles[static_cast<std::size_t>(it - ids.begin())] = handle;
        }
    }

private:
    std::vector<std::uint64_t> ids;
    std::vector<OrderId> handles;
};

// Runs a workload's ops on one book in three steps, so callers can time only the middle one:
//   prepare (untimed)  translate workload ids to the book's handles, if the book issues them
//   run     (timed)    reserveOrderId() for adds on issuing books, then the book call
//   finish  (untimed)  remember the handle of an add that rested
// For books with client ids, prepare and finish do nothing.
template <typename Book>
class Runner {
public:
    static constexpr bool kIssuing = requires(Book& b) { b.reserveOrderId(); };

    Runner(Book& iBook, const Workload& w) : book(iBook), table(makeTable(w)) {}

    [[nodiscard]] Op prepare(const Op& op) const noexcept {
        Op effective = op;
        if constexpr (kIssuing) {
            if (op.kind != OpKind::ADD) {
                effective.id = table->lookup(op.id);
            }
        }
        return effective;
    }

    bool run(Op& effective, std::uint32_t& fills) {
        if constexpr (kIssuing) {
            if (effective.kind == OpKind::ADD) {
                const auto handle = book.reserveOrderId();
                if (!handle) [[unlikely]] {
                    return false;
                }
                effective.id = *handle;
            }
        }
        return runOp(book, effective, fills);
    }

    void finish(const Op& op, const Op& effective) noexcept {
        if constexpr (kIssuing) {
            if (op.kind == OpKind::ADD && book.findOrder(effective.id) != nullptr) {
                table->set(op.id, effective.id);
            }
        }
    }

    // prepare + run + finish, for untimed ops.
    bool step(const Op& op, std::uint32_t& fills) {
        Op effective = prepare(op);
        const bool ok = run(effective, fills);
        finish(op, effective);
        return ok;
    }

private:
    static std::optional<HandleTable> makeTable(const Workload& w) {
        if constexpr (kIssuing) {
            return HandleTable(w);
        } else {
            (void)w;
            return std::nullopt;
        }
    }

    Book& book;
    std::optional<HandleTable> table;
};

} // namespace eigenbook::bench
