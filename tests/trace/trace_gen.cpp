//
// eigenbook_trace_gen (Phase B2.2): runs seeded random request streams on every book type and
// writes one NDJSON trace per (book, seed) for trace validation against the TLA+ specification.
//
// The streams stay inside the model's vocabulary: unique ids, prices 1..P, quantities 1..Q, limit
// orders with DAY / GTC / IOC / FOK (post-only only with DAY / GTC), market orders with IOC / FOK,
// cancels and modifies of resting orders. No limits are set, so the only possible reject is
// POST_ONLY_WOULD_TRADE; any other reject means the stream left the model and is a hard error.
//
// Usage: eigenbook_trace_gen --out DIR [--steps N] [--seeds K] [--prices P] [--max-qty Q]
//                            [--books map,map-m1,map-m2,dense,hybrid,dense-hardened,dense-direct]
//

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <random>
#include <sstream>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

#include <eigenbook/book/order_book.hpp>

#include "trace_recorder.hpp"

namespace eigenbook::trace {

namespace {

struct Options {
    std::filesystem::path out;
    int steps{300};
    std::uint64_t seeds{3};
    std::int64_t prices{5};
    std::uint64_t maxQty{3};
    std::vector<std::string>
        books{"map", "map-m1", "map-m2", "dense", "hybrid", "dense-hardened", "dense-direct"};
};

struct FillLog {
    std::vector<Fill> fills;

    void operator()(const Fill& f) { fills.push_back(f); }
};

// Model ids <-> book ids. Client-id books use the model id itself; the direct book translates
// its engine-issued handles.
struct IdMap {
    std::vector<OrderId> toBook{OrderId{0}}; // index = model id
    std::unordered_map<std::uint64_t, std::uint64_t> toModel;

    void bind(const std::uint64_t model, const OrderId book) {
        if (toBook.size() <= model) {
            toBook.resize(model + 1, OrderId{0});
        }
        toBook[model] = book;
        toModel[book.value] = model;
    }
};

template <typename Book>
std::unique_ptr<Book> makeBook(const Options& o) {
    if constexpr (std::is_same_v<Book, OrderBook>) {
        return std::make_unique<Book>(InstrumentId{1});
    } else if constexpr (std::is_same_v<Book, OrderBookM1> || std::is_same_v<Book, OrderBookM2>) {
        return std::make_unique<Book>(InstrumentId{1}, MapLadderConfig{.maxLevels = 64});
    } else if constexpr (std::is_same_v<Book, HybridOrderBook>) {
        // A window that does not cover the model prices: the stream also exercises the overflow
        // maps and recentering.
        return std::make_unique<Book>(InstrumentId{1},
                                      HybridLadderConfig{.windowTicks = 64,
                                                         .initialBase = {1'000},
                                                         .maxLevels = 64,
                                                         .maxShiftsPerOp = 64,
                                                         .maxMovesPerOp = 8});
    } else {
        return std::make_unique<Book>(
            InstrumentId{1},
            ArrayLadderConfig{.minPrice = {1}, .tickCount = static_cast<std::size_t>(o.prices)},
            BookLimits{.reserveOrders = 4096, .maxOrders = 4096});
    }
}

template <typename Book>
constexpr bool kIssuesIds =
    IssuingOrderLocator<std::remove_cvref_t<decltype(std::declval<Book&>().orderIndex())>>;

// Resting model ids, read from the book.
template <typename Book>
std::vector<std::uint64_t>
restingIds(const Book& book, const IdMap& ids, const std::int64_t prices) {
    std::vector<std::uint64_t> out;
    for (std::int64_t p = 1; p <= prices; ++p) {
        for (const PriceLevel* level :
             {book.bids().findLevel(Price{p}), book.asks().findLevel(Price{p})}) {
            if (level != nullptr) {
                level->forEachOrder(
                    [&](const Order& o) { out.push_back(ids.toModel.at(o.getId().value)); });
            }
        }
    }
    return out;
}

[[noreturn]] void fail(const std::string& what) {
    std::cerr << "eigenbook_trace_gen: " << what << '\n';
    std::exit(1);
}

// A limit price that builds a book: three in four orders are passive (bids in the lower half of
// the price range, asks in the upper half), so queues form and time priority is exercised; the
// rest are aggressive (priced in the other side's half) and sweep one or more levels.
std::int64_t limitPrice(std::mt19937_64& rng, const OrderSide side, const std::int64_t prices) {
    if (prices < 2) {
        return 1;
    }
    const std::int64_t half = prices / 2;
    const bool passive = rng() % 4 != 0;
    const bool low = (side == OrderSide::BUY) == passive;
    const std::int64_t first = low ? 1 : half + 1;
    const std::int64_t span = low ? half : prices - half;
    return first + static_cast<std::int64_t>(rng() % static_cast<std::uint64_t>(span));
}

// What one trace exercised, so that a passing check also says what it covered.
struct Coverage {
    int events{0};
    int trades{0};
    int sweeps{0}; // requests that traded with two or more makers
    std::size_t maxQueue{0};
    int postOnlyRejects{0};
    int fokNoFill{0}; // FOK orders that traded nothing
    int cancels{0};
    int reduces{0};
    int reentries{0};

    template <typename Book>
    void count(const TraceEvent& e, const Book& book, const std::int64_t prices) {
        ++events;
        trades += static_cast<int>(e.trades.size());
        sweeps += e.trades.size() >= 2 ? 1 : 0;
        postOnlyRejects += e.rejected ? 1 : 0;
        fokNoFill += (e.kind == TraceKind::ADD_LIMIT || e.kind == TraceKind::ADD_MARKET) &&
                             e.tif == TimeInForce::FOK && e.trades.empty()
                         ? 1
                         : 0;
        cancels += e.kind == TraceKind::CANCEL ? 1 : 0;
        for (std::int64_t p = 1; p <= prices; ++p) {
            for (const PriceLevel* level :
                 {book.bids().findLevel(Price{p}), book.asks().findLevel(Price{p})}) {
                if (level != nullptr) {
                    maxQueue = std::max<std::size_t>(maxQueue, level->orderCount());
                }
            }
        }
    }

    [[nodiscard]] std::string summary() const {
        std::ostringstream s;
        s << events << " events, " << trades << " trades, " << sweeps << " sweeps, max queue "
          << maxQueue << ", post-only rejects " << postOnlyRejects << ", FOK no-fill " << fokNoFill
          << ", cancels " << cancels << ", reduces " << reduces << ", re-entries " << reentries;
        return s.str();
    }
};

template <typename Book>
void run(const std::string& name, const std::uint64_t seed, const Options& o) {
    auto book = makeBook<Book>(o);
    IdMap ids;
    std::mt19937_64 rng(seed);
    const std::filesystem::path file = o.out / (name + "-seed" + std::to_string(seed) + ".ndjson");
    std::ofstream out(file, std::ios::binary);
    if (!out) {
        fail("cannot write " + file.string());
    }
    writeHeader(out, name, seed, o.prices, o.maxQty, o.steps);
    const auto toModel = [&](const OrderId id) {
        return ids.toModel.at(id.value);
    };
    std::uint64_t nextId = 1;
    Coverage cover;

    for (int step = 1; step <= o.steps; ++step) {
        const std::vector<std::uint64_t> live = restingIds(*book, ids, o.prices);
        const auto op = rng() % 10;
        TraceEvent e;
        FillLog log;

        if (op < 6 || live.empty()) {
            e.id = nextId++;
            e.side = rng() % 2 != 0 ? OrderSide::BUY : OrderSide::SELL;
            e.qty = 1 + rng() % o.maxQty;
            OrderRequest r{.id = OrderId{0},
                           .side = e.side,
                           .type = OrderType::LIMIT,
                           .quantity = {e.qty}};
            if (rng() % 8 == 0) {
                e.kind = TraceKind::ADD_MARKET;
                e.tif = rng() % 2 != 0 ? TimeInForce::IOC : TimeInForce::FOK;
                r.type = OrderType::MARKET;
            } else {
                e.kind = TraceKind::ADD_LIMIT;
                e.price = limitPrice(rng, e.side, o.prices);
                constexpr TimeInForce kTifs[] = {TimeInForce::DAY,
                                                 TimeInForce::GTC,
                                                 TimeInForce::GTC,
                                                 TimeInForce::IOC,
                                                 TimeInForce::FOK};
                e.tif = kTifs[rng() % 5];
                e.postOnly = (e.tif == TimeInForce::DAY || e.tif == TimeInForce::GTC) &&
                             rng() % 4 == 0;
                r.type = OrderType::LIMIT;
                r.price = {e.price};
            }
            r.timeInForce = e.tif;
            r.postOnly = e.postOnly;
            OrderId bookId{e.id};
            if constexpr (kIssuesIds<Book>) {
                bookId = book->reserveOrderId().value();
            }
            ids.bind(e.id, bookId);
            r.id = bookId;
            const auto result = book->addOrder(r, log);
            if (!result) {
                if (result.error() != RejectReason::POST_ONLY_WOULD_TRADE) {
                    fail(name + " step " + std::to_string(step) + ": unmodelled reject of an add");
                }
                e.rejected = true;
                if constexpr (kIssuesIds<Book>) {
                    (void)book->releaseOrderId(bookId);
                }
            }
        } else {
            const std::uint64_t victim = live[rng() % live.size()];
            const OrderId bookId = ids.toBook[victim];
            const Order& order = *book->findOrder(bookId);
            e.id = victim;
            e.side = order.getSide();
            if (op < 8) {
                e.kind = TraceKind::CANCEL;
                if (!book->cancelOrder(bookId)) {
                    fail(name + " step " + std::to_string(step) +
                         ": cancel of a resting order failed");
                }
            } else {
                e.kind = TraceKind::MODIFY;
                e.price = rng() % 2 != 0 ? order.getPrice().value
                                         : limitPrice(rng, e.side, o.prices);
                e.qty = 1 + rng() % o.maxQty;
                e.tif = order.getTimeInForce();
                e.postOnly = order.isPostOnly();
                const bool reduce = e.price == order.getPrice().value &&
                                    e.qty <= order.getQuantity().value;
                ++(reduce ? cover.reduces : cover.reentries);
                const auto result =
                    book->modifyOrder({.id = bookId, .price = {e.price}, .quantity = {e.qty}}, log);
                if (!result) {
                    if (result.error() != RejectReason::POST_ONLY_WOULD_TRADE) {
                        fail(name + " step " + std::to_string(step) +
                             ": unmodelled reject of a modify");
                    }
                    e.rejected = true;
                }
            }
        }
        for (const Fill& f : log.fills) {
            e.trades.push_back(
                {.maker = toModel(f.makerId), .price = f.price.value, .qty = f.quantity.value});
        }
        writeEvent(out, step, e, *book, o.prices, toModel);
        if (const auto violation = book->checkInvariants()) {
            fail(name + " step " + std::to_string(step) + ": book invariant violated");
        }
        cover.count(e, *book, o.prices);
    }
    std::cout << name << "-seed" << seed << ": " << cover.summary() << '\n';
}

Options parse(const int argc, char** argv) {
    Options o;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        const auto next = [&]() -> std::string {
            if (i + 1 >= argc) {
                fail("missing value for " + a);
            }
            return argv[++i];
        };
        if (a == "--out") {
            o.out = next();
        } else if (a == "--steps") {
            o.steps = std::stoi(next());
        } else if (a == "--seeds") {
            o.seeds = std::stoull(next());
        } else if (a == "--prices") {
            o.prices = std::stoll(next());
        } else if (a == "--max-qty") {
            o.maxQty = std::stoull(next());
        } else if (a == "--books") {
            o.books.clear();
            std::stringstream list(next());
            for (std::string b; std::getline(list, b, ',');) {
                o.books.push_back(b);
            }
        } else {
            fail("unknown option " + a);
        }
    }
    if (o.out.empty() || o.steps < 1 || o.seeds < 1 || o.prices < 1 || o.maxQty < 1) {
        fail("usage: eigenbook_trace_gen --out DIR [--steps N] [--seeds K] [--prices P] "
             "[--max-qty Q] [--books list]");
    }
    return o;
}

} // namespace

} // namespace eigenbook::trace

int main(const int argc, char** argv) {
    using namespace eigenbook;
    using namespace eigenbook::trace;
    const Options o = parse(argc, argv);
    std::filesystem::create_directories(o.out);
    for (const std::string& name : o.books) {
        for (std::uint64_t seed = 1; seed <= o.seeds; ++seed) {
            if (name == "map") {
                run<OrderBook>(name, seed, o);
            } else if (name == "map-m1") {
                run<OrderBookM1>(name, seed, o);
            } else if (name == "map-m2") {
                run<OrderBookM2>(name, seed, o);
            } else if (name == "dense") {
                run<DenseOrderBook>(name, seed, o);
            } else if (name == "hybrid") {
                run<HybridOrderBook>(name, seed, o);
            } else if (name == "dense-hardened") {
                run<DenseOrderBookHardened>(name, seed, o);
            } else if (name == "dense-direct") {
                run<DenseOrderBookDirect>(name, seed, o);
            } else {
                fail("unknown book " + name);
            }
        }
    }
    std::cout << "wrote " << o.books.size() * o.seeds << " traces of " << o.steps << " steps to "
              << o.out.string() << '\n';
    return 0;
}
