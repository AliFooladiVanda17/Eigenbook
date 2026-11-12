// Uses the installed headers the way a downstream project would. Exit code 0 = pass.

#include <cstdio>

#include <eigenbook/book/order_book.hpp>

#if defined(EIGENBOOK_CONSUMER_PLATFORM)
#include <eigenbook/platform/affinity.hpp>
#include <eigenbook/platform/cpu_info.hpp>
#endif

int main() {
#if defined(EIGENBOOK_CONSUMER_PLATFORM)
    // Linked through Eigenbook::eigenbook: the compiled platform library must resolve.
    if (eigenbook::platform::logicalCpuCount() == 0) {
        std::puts("package_consumer: platform library returned no CPUs");
        return 1;
    }
    (void)eigenbook::platform::cpuInfo();
#endif
    using namespace eigenbook;

    OrderBook book(InstrumentId{1});
    if (!book.addOrder({.id = {1},
                        .side = OrderSide::SELL,
                        .type = OrderType::LIMIT,
                        .price = {100},
                        .quantity = {10}})) {
        std::puts("package_consumer: resting order rejected");
        return 1;
    }

    int fills = 0;
    const auto result = book.addOrder({.id = {2},
                                       .side = OrderSide::BUY,
                                       .type = OrderType::LIMIT,
                                       .price = {100},
                                       .quantity = {4}},
                                      [&fills](const Fill&) { ++fills; });

    if (!result || result->filled != Quantity{4} || fills != 1 ||
        book.findOrder({1})->getQuantity() != Quantity{6}) {
        std::puts("package_consumer: unexpected matching result");
        return 1;
    }
    std::puts("package_consumer: OK");
    return 0;
}
