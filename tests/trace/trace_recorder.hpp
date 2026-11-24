//
// Trace recorder (Phase B2.2): writes what a book did, one NDJSON line per request, in the
// abstract vocabulary of the TLA+ specification (formal/tla/EigenbookMatching.tla).
//
// The recorder sits outside the book: it reads the book's public state after each request, so the
// matching code has no tracing branch and no I/O. The abstraction function it applies
// (formal/README.md, "The model"):
//   - book order ids -> model ids 1, 2, ... (the caller supplies the mapping; with engine-issued
//     handles it is a real translation),
//   - DAY -> GTC (identical until sessions exist),
//   - per side and per model price 1..P, the queue of model ids in time priority,
//   - per resting order: open quantity, priority stamp, time in force, post-only.
//
// Line format (one JSON object per line; the first line is a header):
//   {"format":"eigenbook-matching-trace","version":1,"book":"dense","seed":1,"prices":5,...}
//   {"step":1,"kind":"addLimit","id":1,"side":"BUY","price":3,"qty":2,"tif":"GTC","po":false,
//    "result":"accepted","trades":[{"maker":2,"price":3,"qty":1}],
//    "q":{"BUY":[[],[],[1],[],[]],"SELL":[[],[],[],[],[]]},
//    "resting":[{"id":1,"qty":1,"stamp":1,"tif":"GTC","po":false}]}
//

#pragma once

#include <cstdint>
#include <ostream>
#include <string>
#include <vector>

#include <eigenbook/book/order_book.hpp>

namespace eigenbook::trace {

enum class TraceKind : std::uint8_t {
    ADD_LIMIT,
    ADD_MARKET,
    CANCEL,
    MODIFY
};

struct TraceTrade {
    std::uint64_t maker; // model id
    std::int64_t price;
    std::uint64_t qty;
};

// One request and its outcome, in model terms (the post-state is read from the book).
struct TraceEvent {
    TraceKind kind{TraceKind::ADD_LIMIT};
    std::uint64_t id{0}; // model id
    OrderSide side{OrderSide::BUY};
    std::int64_t price{0}; // 0 for market orders and cancels
    std::uint64_t qty{0};
    TimeInForce tif{TimeInForce::GTC};
    bool postOnly{false};
    bool rejected{false}; // only POST_ONLY_WOULD_TRADE is a modelled reject
    std::vector<TraceTrade> trades{};
};

[[nodiscard]] inline const char* kindName(const TraceKind k) {
    switch (k) {
        case TraceKind::ADD_LIMIT:
            return "addLimit";
        case TraceKind::ADD_MARKET:
            return "addMarket";
        case TraceKind::CANCEL:
            return "cancel";
        case TraceKind::MODIFY:
            return "modify";
    }
    return "?";
}

[[nodiscard]] inline const char* sideName(const OrderSide s) {
    return s == OrderSide::BUY ? "BUY" : "SELL";
}

// The abstraction of a time in force: DAY behaves as GTC until sessions exist.
[[nodiscard]] inline const char* tifName(const TimeInForce t) {
    switch (t) {
        case TimeInForce::DAY:
        case TimeInForce::GTC:
            return "GTC";
        case TimeInForce::IOC:
            return "IOC";
        case TimeInForce::FOK:
            return "FOK";
    }
    return "?";
}

inline void writeHeader(std::ostream& out,
                        const std::string& book,
                        const std::uint64_t seed,
                        const std::int64_t prices,
                        const std::uint64_t maxQty,
                        const int steps) {
    out << R"({"format":"eigenbook-matching-trace","version":1,"book":")" << book << R"(","seed":)"
        << seed << R"(,"prices":)" << prices << R"(,"maxQty":)" << maxQty << R"(,"steps":)" << steps
        << "}\n";
}

// Writes one event line. `toModel(OrderId) -> std::uint64_t` maps the book's ids to model ids;
// prices are the model prices 1..prices.
template <typename Book, typename ToModel>
void writeEvent(std::ostream& out,
                const int step,
                const TraceEvent& e,
                const Book& book,
                const std::int64_t prices,
                ToModel&& toModel) {
    out << R"({"step":)" << step << R"(,"kind":")" << kindName(e.kind) << R"(","id":)" << e.id
        << R"(,"side":")" << sideName(e.side) << R"(","price":)" << e.price << R"(,"qty":)" << e.qty
        << R"(,"tif":")" << tifName(e.tif) << R"(","po":)" << (e.postOnly ? "true" : "false")
        << R"(,"result":")" << (e.rejected ? "rejected" : "accepted") << R"(","trades":[)";
    for (std::size_t k = 0; k < e.trades.size(); ++k) {
        out << (k == 0 ? "" : ",") << R"({"maker":)" << e.trades[k].maker << R"(,"price":)"
            << e.trades[k].price << R"(,"qty":)" << e.trades[k].qty << "}";
    }
    out << R"(],"q":{)";
    std::vector<const Order*> resting;
    for (const OrderSide side : {OrderSide::BUY, OrderSide::SELL}) {
        out << (side == OrderSide::BUY ? "" : ",") << '"' << sideName(side) << R"(":[)";
        for (std::int64_t p = 1; p <= prices; ++p) {
            out << (p == 1 ? "" : ",") << '[';
            const PriceLevel* level = side == OrderSide::BUY ? book.bids().findLevel(Price{p})
                                                             : book.asks().findLevel(Price{p});
            bool first = true;
            if (level != nullptr) {
                level->forEachOrder([&](const Order& o) {
                    out << (first ? "" : ",") << toModel(o.getId());
                    first = false;
                    resting.push_back(&o);
                });
            }
            out << ']';
        }
        out << ']';
    }
    out << R"(},"resting":[)";
    for (std::size_t k = 0; k < resting.size(); ++k) {
        const Order& o = *resting[k];
        out << (k == 0 ? "" : ",") << R"({"id":)" << toModel(o.getId()) << R"(,"qty":)"
            << o.getQuantity().value << R"(,"stamp":)" << o.getTimestamp().value << R"(,"tif":")"
            << tifName(o.getTimeInForce()) << R"(","po":)" << (o.isPostOnly() ? "true" : "false")
            << "}";
    }
    out << "]}\n";
}

} // namespace eigenbook::trace
