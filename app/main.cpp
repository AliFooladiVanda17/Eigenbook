#include <chrono>
#include <cstdint>
#include <iostream>
#include <random>
#include <vector>

#include <tradingengine/orderbook.h>

namespace {

struct Op {
    enum Kind : std::uint8_t { ADD, CANCEL };
    Kind kind;
    OrderRequest request;
};

// Random walk around a mid price: mostly passive limits with some aggressive ones and cancels.
std::vector<Op> makeWorkload(const std::size_t count) {
    std::mt19937_64 rng(42);
    std::vector<Op> ops;
    std::vector<std::uint64_t> live;
    ops.reserve(count);
    std::uint64_t nextId = 1;
    for (std::size_t i = 0; i < count; ++i) {
        if (!live.empty() && rng() % 3 == 0) {
            const std::size_t pick = rng() % live.size();
            Op cancel{Op::CANCEL, {}};
            cancel.request.id = {live[pick]};
            ops.push_back(cancel);
            live[pick] = live.back();
            live.pop_back();
            continue;
        }
        const OrderSide side = rng() % 2 ? OrderSide::BUY : OrderSide::SELL;
        const std::int64_t offset = static_cast<std::int64_t>(rng() % 64) - 8;
        const std::int64_t price = side == OrderSide::BUY ? 10'000 - offset : 10'000 + offset;
        const OrderRequest request{.id = {nextId},
                                   .side = side,
                                   .type = OrderType::LIMIT,
                                   .price = {price},
                                   .quantity = {1 + rng() % 100},
                                   .timestamp = {nextId}};
        live.push_back(nextId++);
        ops.push_back({Op::ADD, request});
    }
    return ops;
}

template <typename Book>
void run(const char* name, Book& book, const std::vector<Op>& ops) {
    std::uint64_t fills = 0;
    auto onFill = [&fills](const Fill&) { ++fills; };

    const auto start = std::chrono::steady_clock::now();
    for (const Op& op : ops) {
        if (op.kind == Op::ADD) {
            (void)book.addOrder(op.request, onFill);
        }
        else {
            (void)book.cancelOrder(op.request.id); // may already be filled
        }
    }
    const auto elapsed = std::chrono::steady_clock::now() - start;
    const double ns = std::chrono::duration<double, std::nano>(elapsed).count();

    std::cout << name << ": " << ops.size() << " ops, " << fills << " fills, "
              << ns / static_cast<double>(ops.size()) << " ns/op, resting " << book.orderCount()
              << ", best " << (book.bestBid() ? book.bestBid()->value : -1) << " / "
              << (book.bestAsk() ? book.bestAsk()->value : -1) << '\n';
}

} // namespace

int main() {
    constexpr std::size_t kOps = 2'000'000;
    const std::vector<Op> ops = makeWorkload(kOps);

    OrderBook mapBook(InstrumentId{1}, {}, kOps);
    run("std::map ladder    ", mapBook, ops);

    DenseOrderBook denseBook(InstrumentId{1}, {.minPrice = {9'000}, .tickCount = 2'048}, kOps);
    run("array+bitset ladder", denseBook, ops);
    return 0;
}
