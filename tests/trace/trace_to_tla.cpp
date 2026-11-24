//
// eigenbook_trace_to_tla (Phase B2.2): converts one NDJSON trace into the TLA+ inputs of the
// trace check (formal/tla/EigenbookTrace.tla):
//   OUTDIR/TraceData.tla   Trace == << event records >>  (JSON objects -> records, arrays ->
//   tuples) OUTDIR/Trace.cfg       the model constants for this trace, TraceSpec, the invariants
//
// --mutate KIND corrupts one aspect of the first event it applies to, for negative controls: a
// correct checker must reject the mutated trace. KIND is one of trade-price, drop-trade, stamp,
// queue-order, result-flip. The step that was changed is printed as "mutated step N".
//
// Usage: eigenbook_trace_to_tla INPUT.ndjson OUTDIR [--mutate KIND]
//

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <utility>
#include <vector>

#include "json_lite.hpp"

namespace {

using eigenbook::trace::JsonParser;
using eigenbook::trace::JsonValue;

[[noreturn]] void fail(const std::string& what) {
    std::cerr << "eigenbook_trace_to_tla: " << what << '\n';
    std::exit(1);
}

std::int64_t intField(const JsonValue& v, const char* key) {
    const JsonValue* f = v.find(key);
    if (f == nullptr || f->kind != JsonValue::Kind::INT) {
        fail(std::string("missing integer field ") + key);
    }
    return f->integer;
}

JsonValue& field(JsonValue& v, const char* key) {
    JsonValue* f = v.find(key);
    if (f == nullptr) {
        fail(std::string("missing field ") + key);
    }
    return *f;
}

// Applies the mutation to the first event it fits; returns that event's step, or 0.
std::int64_t mutate(std::vector<JsonValue>& events, const std::string& kind) {
    for (JsonValue& e : events) {
        const std::int64_t step = intField(e, "step");
        JsonValue& trades = field(e, "trades");
        if ((kind == "trade-price" || kind == "drop-trade") && !trades.array.empty()) {
            if (kind == "trade-price") {
                field(trades.array.front(), "price").integer += 1;
            } else {
                trades.array.pop_back();
            }
            return step;
        }
        JsonValue& resting = field(e, "resting");
        if (kind == "stamp" && !resting.array.empty()) {
            field(resting.array.front(), "stamp").integer += 1;
            return step;
        }
        if (kind == "queue-order") {
            for (auto& [side, levels] : field(e, "q").object) {
                for (JsonValue& queue : levels.array) {
                    if (queue.array.size() >= 2) {
                        std::swap(queue.array[0], queue.array[1]);
                        return step;
                    }
                }
            }
        }
        if (kind == "result-flip") {
            JsonValue& result = field(e, "result");
            result.string = result.string == "rejected" ? "accepted" : "rejected";
            return step;
        }
    }
    return 0;
}

} // namespace

int main(const int argc, char** argv) {
    if (argc != 3 && argc != 5) {
        fail("usage: eigenbook_trace_to_tla INPUT.ndjson OUTDIR [--mutate KIND]");
    }
    const std::filesystem::path input = argv[1];
    const std::filesystem::path outDir = argv[2];
    std::string mutation;
    if (argc == 5) {
        if (std::string(argv[3]) != "--mutate") {
            fail("unknown option " + std::string(argv[3]));
        }
        mutation = argv[4];
    }

    std::ifstream in(input, std::ios::binary);
    if (!in) {
        fail("cannot read " + input.string());
    }
    std::string line;
    if (!std::getline(in, line)) {
        fail("empty trace");
    }
    const auto header = JsonParser(line).parse();
    if (!header || header->find("format") == nullptr ||
        header->find("format")->string != "eigenbook-matching-trace" ||
        intField(*header, "version") != 1) {
        fail("not an eigenbook-matching-trace version 1 file");
    }
    std::vector<JsonValue> events;
    for (std::size_t n = 2; std::getline(in, line); ++n) {
        if (line.empty() || line == "\r") {
            continue;
        }
        auto e = JsonParser(line).parse();
        if (!e || e->kind != JsonValue::Kind::OBJECT) {
            fail("line " + std::to_string(n) + " is not a JSON object");
        }
        events.push_back(std::move(*e));
    }
    if (events.empty()) {
        fail("the trace has no events");
    }

    if (!mutation.empty()) {
        const std::int64_t step = mutate(events, mutation);
        if (step == 0) {
            fail("mutation " + mutation + " does not apply to this trace");
        }
        std::cout << "mutated step " << step << '\n';
    }

    // Model constants for this trace.
    const std::int64_t prices = intField(*header, "prices");
    std::int64_t maxId = 1;
    std::int64_t maxQty = std::max<std::int64_t>(1, intField(*header, "maxQty"));
    for (const JsonValue& e : events) {
        maxId = std::max(maxId, intField(e, "id"));
        maxQty = std::max(maxQty, intField(e, "qty"));
        for (const JsonValue& t : e.find("trades")->array) {
            maxQty = std::max(maxQty, intField(t, "qty"));
        }
    }

    std::filesystem::create_directories(outDir);
    {
        std::ofstream data(outDir / "TraceData.tla", std::ios::binary);
        data << "------------------------------ MODULE TraceData ------------------------------\n"
             << "\\* Generated by eigenbook_trace_to_tla from " << input.filename().string()
             << (mutation.empty() ? "" : " (mutated: " + mutation + ")")
             << ". Do not edit.\nEXTENDS Sequences\n\nTrace == <<\n";
        for (std::size_t k = 0; k < events.size(); ++k) {
            data << "    " << eigenbook::trace::toTla(events[k])
                 << (k + 1 < events.size() ? ",\n" : "\n");
        }
        data << ">>\n"
             << "============================================================================="
                "\n";
    }
    {
        const auto range = [](const std::int64_t n) {
            std::string s = "{";
            for (std::int64_t i = 1; i <= n; ++i) {
                s += (i == 1 ? "" : ", ") + std::to_string(i);
            }
            return s + "}";
        };
        std::ofstream cfg(outDir / "Trace.cfg", std::ios::binary);
        cfg << "\\* Generated by eigenbook_trace_to_tla. Do not edit.\n"
            << "SPECIFICATION TraceSpec\n\n"
            << "CONSTANTS\n"
            << "    Prices = " << range(prices) << "\n"
            << "    Ids = " << range(maxId) << "\n"
            << "    MaxQty = " << maxQty << "\n"
            << "    MaxSeq = " << events.size() + 1 << "\n"
            << "    Tifs = {\"GTC\", \"IOC\", \"FOK\"}\n\n"
            << "\\* Violated exactly when the whole trace was followed: the trace is accepted.\n"
            << "INVARIANT TraceIncomplete\n\n"
            << "\\* The specification's own invariants, on the states the C++ book reached.\n"
            << "INVARIANTS\n"
            << "    TypeOK\n    QueueConsistent\n    NoEmptyLevel\n    NoCross\n"
            << "    QuantityConservation\n    PriorityStampsIncrease\n    PriceTimePriority\n"
            << "    MakerPrice\n    NoTradeThroughLimit\n    MarketNeverRests\n"
            << "    ModifyPriorityRule\n    FokAllOrNothing\n    PostOnlyNeverTakes\n"
            << "    FilledMatchesTrades\n";
    }
    std::cout << "converted " << events.size() << " events (prices " << prices << ", ids " << maxId
              << ", max qty " << maxQty << ")\n";
    return 0;
}
