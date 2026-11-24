//
// The trace format of Phase B2.2: the recorder writes the documented NDJSON line for a known
// scenario, the reader parses it back, and JSON maps to TLA+ the way the trace check expects
// (object -> record, array -> tuple). These tests need no Java; the TLC run itself is the CTest
// test formal_trace_validation (formal/README.md).
//

#include <cstdint>
#include <sstream>
#include <string>

#include <gtest/gtest.h>

#include "json_lite.hpp"
#include "test_support.hpp"
#include "trace_recorder.hpp"

namespace eigenbook::trace {

TEST(TraceFormat, JsonParsesTheTraceSubset) {
    const auto v = JsonParser(R"({"a":[1,-2,[]],"b":"x\"y","c":true,"d":false,"e":{}})").parse();
    ASSERT_TRUE(v);
    ASSERT_EQ(v->kind, JsonValue::Kind::OBJECT);
    EXPECT_EQ(v->find("a")->array.size(), 3u);
    EXPECT_EQ(v->find("a")->array[1].integer, -2);
    EXPECT_EQ(v->find("b")->string, "x\"y");
    EXPECT_TRUE(v->find("c")->boolean);
    EXPECT_FALSE(v->find("d")->boolean);
    EXPECT_EQ(v->find("missing"), nullptr);
}

TEST(TraceFormat, JsonRejectsMalformedInput) {
    for (const char* bad :
         {"", "{", "[1,]", "{\"a\" 1}", "{\"a\":1} x", "\"open", "tru", "{\"a\":\"\\n\"}"}) {
        EXPECT_FALSE(JsonParser(bad).parse()) << bad;
    }
}

TEST(TraceFormat, JsonMapsToTlaRecordsAndTuples) {
    const auto v =
        JsonParser(R"({"kind":"cancel","id":3,"po":false,"q":{"BUY":[[],[1,2]]}})").parse();
    ASSERT_TRUE(v);
    EXPECT_EQ(toTla(*v),
              R"([kind |-> "cancel", id |-> 3, po |-> FALSE, q |-> [BUY |-> <<<<>>, <<1, 2>>>>]])");
}

TEST(TraceFormat, RecorderWritesTheDocumentedLine) {
    DenseOrderBook book(kInstrument, {.minPrice = {1}, .tickCount = 3});
    ASSERT_TRUE(book.addOrder({.id = {1},
                               .side = OrderSide::SELL,
                               .type = OrderType::LIMIT,
                               .price = {2},
                               .quantity = {2},
                               .timeInForce = TimeInForce::DAY,
                               .postOnly = true}));
    FillLog log;
    ASSERT_TRUE(book.addOrder(limit(2, OrderSide::BUY, 2, 1), log));
    ASSERT_EQ(log.fills.size(), 1u);

    TraceEvent e{.kind = TraceKind::ADD_LIMIT,
                 .id = 2,
                 .side = OrderSide::BUY,
                 .price = 2,
                 .qty = 1,
                 .tif = TimeInForce::DAY};
    e.trades.push_back({.maker = 1, .price = 2, .qty = 1});
    std::ostringstream out;
    writeEvent(out, 2, e, book, 3, [](const OrderId id) { return id.value; });
    // DAY is written as GTC (abstraction function); the maker keeps its stamp and post-only flag.
    EXPECT_EQ(out.str(),
              R"({"step":2,"kind":"addLimit","id":2,"side":"BUY","price":2,"qty":1,"tif":"GTC",)"
              R"("po":false,"result":"accepted","trades":[{"maker":1,"price":2,"qty":1}],)"
              R"("q":{"BUY":[[],[],[]],"SELL":[[],[1],[]]},)"
              R"("resting":[{"id":1,"qty":1,"stamp":1,"tif":"GTC","po":true}]})"
              "\n");
    const auto parsed = JsonParser(out.str()).parse();
    ASSERT_TRUE(parsed);
    EXPECT_EQ(parsed->find("resting")->array[0].find("qty")->integer, 1);
}

TEST(TraceFormat, HeaderIdentifiesFormatAndVersion) {
    std::ostringstream out;
    writeHeader(out, "dense", 7, 5, 3, 300);
    const auto h = JsonParser(out.str()).parse();
    ASSERT_TRUE(h);
    EXPECT_EQ(h->find("format")->string, "eigenbook-matching-trace");
    EXPECT_EQ(h->find("version")->integer, 1);
    EXPECT_EQ(h->find("prices")->integer, 5);
}

} // namespace eigenbook::trace
