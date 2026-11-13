// eigenbook_latency: per-operation latency of the order book, by operation class.
//
//   eigenbook_latency [--workload all|w1|w2-churn|w2-drift|w3-cancel70|w3-cancel90|w4-collide|
//                                 w4-load50|w5-burst[,...]]
//                     [--book all|both|map,map-m1,map-m2,dense,hybrid,dense-hardened,dense-direct]
//                     [--runs N] [--seed S]
//                     [--measured N] [--warmup N]
//                     [--quick] [--cpu N | --no-pin] [--realtime] [--prefault]
//                     [--calibrate-ms N] [--hybrid-window TICKS] [--out DIR]
//
// Method (see docs/BENCHMARKS.md): pin to one P-core, raise priority, calibrate the invariant TSC
// against steady_clock, measure the empty-timer overhead, then for every workload and run:
// generate the seeded stream, warm up, and time each op with lfence;rdtsc ... rdtscp;lfence.
// Each op is recorded in the histogram of its class. Runs use seeds S, S+1, ...; tables show the
// median over runs of each statistic and the min-max spread.

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iomanip>
#include <iostream>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include <eigenbook_bench_build_info.hpp>
#include <harness/books.hpp>
#include <harness/environment.hpp>
#include <harness/executor.hpp>
#include <harness/histogram.hpp>
#include <harness/op_class.hpp>
#include <harness/upstream_counter.hpp>
#include <workloads/w1_baseline.hpp>
#include <workloads/w2_level_churn.hpp>
#include <workloads/w3_cancel_heavy.hpp>
#include <workloads/w4_adversarial_index.hpp>
#include <workloads/w5_microburst.hpp>
#include <workloads/workload.hpp>

#include <eigenbook/book/order_book.hpp>
#include <eigenbook/platform/affinity.hpp>
#include <eigenbook/platform/cpu_info.hpp>
#include <eigenbook/platform/system_info.hpp>
#include <eigenbook/platform/tsc.hpp>

namespace {

using namespace eigenbook;
using namespace eigenbook::bench;
namespace fs = std::filesystem;

// Empty-timer p50 over 200,000 pairs: a quick core-clock probe (the TSC rate is fixed, so this
// value moves when the core clock does).
// Called outside the measured loop (it allocates its sample buffer).
std::uint64_t clockProbe() {
    constexpr std::size_t kPairs = 200'000;
    std::vector<std::uint64_t> samples(kPairs);
    for (std::uint64_t& s : samples) {
        const std::uint64_t t0 = platform::tscStart();
        const std::uint64_t t1 = platform::tscStop();
        s = t1 - t0;
    }
    const auto mid = samples.begin() + static_cast<std::ptrdiff_t>(kPairs / 2);
    std::ranges::nth_element(samples, mid);
    return *mid;
}

UpstreamCounter& upstreamCounter() {
    static UpstreamCounter counter;
    return counter;
}

#if !EIGENBOOK_PLATFORM_HAS_TSC
#error "eigenbook_latency needs an x86-64 time-stamp counter"
#endif

// --- Options -------------------------------------------------------------------------------------

struct Options {
    std::vector<std::string> workloads{"all"};
    std::vector<std::string> books{"all"};
    unsigned runs{5};
    std::uint64_t seed{1};
    std::optional<std::size_t> measured;
    std::size_t warmup{200'000};
    bool quick{false};
    std::optional<unsigned> cpu;
    bool pin{true};
    bool realtime{false};
    bool prefault{false};
    unsigned calibrateMs{1000};
    std::size_t hybridWindow{16384};
    fs::path out{EIGENBOOK_BENCH_RESULTS_DIR};
};

[[noreturn]] void usage(const std::string& error) {
    std::cerr << "eigenbook_latency: " << error << "\n"
              << "see the header of bench/latency/main.cpp or docs/BENCHMARKS.md for options\n";
    std::exit(2);
}

std::vector<std::string> split(const std::string& s) {
    std::vector<std::string> out;
    std::stringstream in(s);
    for (std::string item; std::getline(in, item, ',');) {
        if (!item.empty()) {
            out.push_back(item);
        }
    }
    return out;
}

Options parse(const int argc, char** argv) {
    Options o;
    for (int i = 1; i < argc; ++i) {
        const std::string_view a = argv[i];
        auto value = [&]() -> std::string {
            if (i + 1 >= argc) {
                usage("missing value for " + std::string(a));
            }
            return argv[++i];
        };
        if (a == "--workload") {
            o.workloads = split(value());
        } else if (a == "--book") {
            o.books = split(value());
            if (o.books == std::vector<std::string>{"both"}) {
                o.books = {"map", "dense"}; // the A2 pair
            }
        } else if (a == "--runs") {
            o.runs = static_cast<unsigned>(std::stoul(value()));
        } else if (a == "--seed") {
            o.seed = std::stoull(value());
        } else if (a == "--measured") {
            o.measured = std::stoull(value());
        } else if (a == "--warmup") {
            o.warmup = std::stoull(value());
        } else if (a == "--quick") {
            o.quick = true;
        } else if (a == "--cpu") {
            o.cpu = static_cast<unsigned>(std::stoul(value()));
        } else if (a == "--no-pin") {
            o.pin = false;
        } else if (a == "--realtime") {
            o.realtime = true;
        } else if (a == "--prefault") {
            o.prefault = true;
        } else if (a == "--hybrid-window") {
            o.hybridWindow = std::stoull(value());
        } else if (a == "--calibrate-ms") {
            o.calibrateMs = static_cast<unsigned>(std::stoul(value()));
        } else if (a == "--out") {
            o.out = value();
        } else {
            usage("unknown option " + std::string(a));
        }
    }
    if (o.quick) {
        o.runs = std::min(o.runs, 2U);
    }
    if (o.runs == 0) {
        usage("--runs must be at least 1");
    }
    return o;
}

// --- Workload registry
// ----------------------------------------------------------------------------

struct WorkloadSpec {
    std::string name;
    std::size_t defaultMeasured;
    std::function<Workload(std::uint64_t seed, WorkloadSize size, unsigned run)> make;
};

std::vector<WorkloadSpec> registry() {
    return {
        {"w1",
         1'800'000,
         [](std::uint64_t, WorkloadSize size, unsigned run) {
             // Run 0 is exactly the demo's stream (seed 42).
             return makeBaseline({.seed = 42 + run,
                                  .totalOps = size.warmupOps + size.measuredOps,
                                  .warmupOps = size.warmupOps});
         }},
        {"w2-churn",
         1'000'000,
         [](std::uint64_t seed, WorkloadSize size, unsigned) {
             LevelChurnConfig c;
             c.seed = seed;
             c.size = size;
             return makeLevelChurn(c);
         }},
        {"w2-drift",
         1'000'000,
         [](std::uint64_t seed, WorkloadSize size, unsigned) {
             LevelChurnConfig c = levelChurnDrift(seed);
             c.size = size;
             return makeLevelChurn(c);
         }},
        {"w3-cancel70",
         1'000'000,
         [](std::uint64_t seed, WorkloadSize size, unsigned) {
             return makeCancelHeavy({.seed = seed, .size = size, .cancelFraction = 0.70});
         }},
        {"w3-cancel90",
         1'000'000,
         [](std::uint64_t seed, WorkloadSize size, unsigned) {
             return makeCancelHeavy({.seed = seed, .size = size, .cancelFraction = 0.90});
         }},
        {"w4-collide",
         100'000, // every op walks a 4096-slot cluster
         [](std::uint64_t seed, WorkloadSize size, unsigned) {
             return makeAdversarialIndex(
                 {.seed = seed, .size = size, .mode = IndexAttack::COLLIDE});
         }},
        {"w4-load50",
         1'000'000,
         [](std::uint64_t seed, WorkloadSize size, unsigned) {
             return makeAdversarialIndex(
                 {.seed = seed, .size = size, .mode = IndexAttack::LOAD_HALF});
         }},
        {"w5-burst",
         1'000'000,
         [](std::uint64_t seed, WorkloadSize size, unsigned) {
             MicroburstConfig c;
             c.seed = seed;
             c.size = size;
             return makeMicroburst(c);
         }},
    };
}

// --- Measurement ---------------------------------------------------------------------------------

struct RunResult {
    std::array<LatencyHistogram, kOpClassCount> service;
    LatencyHistogram response; // paced workloads only: completion - scheduled start
    std::uint64_t mismatches{0};
    double legacyMeanNs{0.0}; // W1 only: demo-style whole-loop mean
    std::size_t maxCluster{0};
    std::uint64_t idRejects{0}; // hardened index: ids refused by the probe limit (whole run)
    std::uint64_t upstreamAllocations{0}; // pmr default-resource allocations, measured part
    std::uint64_t clockProbeP50{0};       // empty-timer p50 (ticks) just before the measured part
#if defined(EIGENBOOK_INSTRUMENT_INDEX)
    OrderIndexStats indexStats;
    bool hasIndexStats{false}; // the book's locator is an OrderIndex
#endif
};

template <typename Book>
std::unique_ptr<Book> makeBook(const Workload& w) {
    return BookTraits<Book>::make(w);
}

template <typename Book>
double legacyMean(const Workload& w) {
    // The demo's method: one steady_clock interval around the whole stream from an empty book.
    auto book = makeBook<Book>(w);
    Runner<Book> runner(*book, w);
    std::uint32_t fills = 0;
    const auto start = std::chrono::steady_clock::now();
    for (const Op& op : w.ops) {
        (void)runner.step(op, fills);
    }
    const auto elapsed = std::chrono::steady_clock::now() - start;
    return std::chrono::duration<double, std::nano>(elapsed).count() /
           static_cast<double>(w.ops.size());
}

template <typename Book>
RunResult measure(const Workload& w, const Environment& env, const bool prefault) {
    RunResult r;
    auto book = makeBook<Book>(w);
    if (prefault) {
        platform::prefault(const_cast<Op*>(w.ops.data()), w.ops.size() * sizeof(Op));
    }

    Runner<Book> runner(*book, w); // id translation for books that issue their own handles
    constexpr bool kHashIndex = requires { book->orderIndex().maxClusterLength(); };
    std::uint32_t fills = 0;
    for (std::size_t i = 0; i < w.warmupOps; ++i) {
        fills = 0;
        const bool ok = runner.step(w.ops[i], fills);
        r.mismatches += outcomeMatches(w.ops[i], ok, fills) ? 0 : 1;
    }
#if defined(EIGENBOOK_INSTRUMENT_INDEX)
    // Probe statistics of the measured part only (OrderIndex books).
    if constexpr (requires { book->orderIndex().resetStats(); }) {
        book->orderIndex().resetStats();
    }
#endif

    const double ticksPerNs = env.tsc.ticksPerNanosecond();
    r.clockProbeP50 = clockProbe();
    const std::uint64_t upstreamBefore = upstreamCounter().allocations();
    // Paced: start the schedule 1 ms from now.
    const std::uint64_t base = platform::tscNow() + static_cast<std::uint64_t>(1e6 * ticksPerNs);
    for (std::size_t i = w.warmupOps; i < w.ops.size(); ++i) {
        const Op& op = w.ops[i];
        fills = 0;
        if (!op.timed) {
            const bool ok = runner.step(op, fills);
            r.mismatches += outcomeMatches(op, ok, fills) ? 0 : 1;
            continue;
        }
        Op effective = runner.prepare(op); // untimed: id translation
        std::uint64_t target = 0;
        if (w.paced) {
            target = base +
                     static_cast<std::uint64_t>(static_cast<double>(op.scheduledNs) * ticksPerNs);
            while (platform::tscNow() < target) {
                platform::cpuRelax();
            }
        }
        const std::uint64_t t0 = platform::tscStart();
        const bool ok = runner.run(effective, fills); // issuing books: reserveOrderId + add
        const std::uint64_t t1 = platform::tscStop();
        runner.finish(op, effective);
        r.service[static_cast<std::size_t>(op.expected)].record(t1 - t0);
        if (w.paced) {
            r.response.record(t1 - std::min(target, t0));
        }
        r.mismatches += outcomeMatches(op, ok, fills) ? 0 : 1;
    }
    r.upstreamAllocations = upstreamCounter().allocations() - upstreamBefore;
    if constexpr (kHashIndex) {
        r.maxCluster = book->orderIndex().maxClusterLength();
    }
    if constexpr (requires { book->orderIndex().probeLimitRejects(); }) {
        r.idRejects = book->orderIndex().probeLimitRejects();
    }
#if defined(EIGENBOOK_INSTRUMENT_INDEX)
    if constexpr (requires { book->orderIndex().stats(); }) {
        r.indexStats = book->orderIndex().stats();
        r.hasIndexStats = true;
    }
#endif
    return r;
}

// --- Book registry
// ---------------------------------------------------------------------------------

struct BookRunner {
    std::string_view name;
    RunResult (*measure)(const Workload&, const Environment&, bool);
    double (*legacy)(const Workload&);
};

std::vector<BookRunner> selectedBooks(const Options& o) {
    std::vector<BookRunner> all;
    forEachBookType([&]<typename Book>() {
        all.push_back({BookTraits<Book>::name, &measure<Book>, &legacyMean<Book>});
    });
    if (std::ranges::find(o.books, "all") != o.books.end()) {
        return all;
    }
    std::vector<BookRunner> chosen;
    for (const std::string& name : o.books) {
        const auto it = std::ranges::find(all, name, &BookRunner::name);
        if (it == all.end()) {
            usage("unknown book '" + name +
                  "' (map, map-m1, map-m2, dense, hybrid, dense-hardened, dense-direct, all, "
                  "both)");
        }
        chosen.push_back(*it);
    }
    return chosen;
}

// --- Reporting -----------------------------------------------------------------------------------

constexpr std::array<double, 6> kPercentiles{50.0, 90.0, 99.0, 99.9, 99.99, 100.0};
constexpr std::array<std::string_view, 6> kPercentileNames{"p50",
                                                           "p90",
                                                           "p99",
                                                           "p99.9",
                                                           "p99.99",
                                                           "max"};

struct Stats {
    std::uint64_t count{0};
    double mean{0.0};
    std::array<double, kPercentiles.size()> p{};
};

Stats statsOf(const LatencyHistogram& h, const double scale) {
    Stats s;
    s.count = h.count();
    s.mean = h.mean() * scale;
    for (std::size_t i = 0; i < kPercentiles.size(); ++i) {
        const std::uint64_t v = kPercentiles[i] >= 100.0 ? h.max()
                                                         : h.valueAtPercentile(kPercentiles[i]);
        s.p[i] = static_cast<double>(v) * scale;
    }
    return s;
}

double median(std::vector<double> v) {
    std::ranges::sort(v);
    const std::size_t n = v.size();
    return n % 2 == 1 ? v[n / 2] : (v[n / 2 - 1] + v[n / 2]) / 2.0;
}

std::string fmt(const double v) {
    std::ostringstream s;
    if (v >= 100.0) {
        s << std::fixed << std::setprecision(0) << v;
    } else {
        s << std::fixed << std::setprecision(1) << v;
    }
    return s.str();
}

struct Collected {
    std::string workload;
    std::string book;
    std::vector<RunResult> runs;
    std::vector<std::uint64_t> seeds;
    double churn{0.0};
    std::string description;
};

#if defined(EIGENBOOK_INSTRUMENT_INDEX)
// Probe statistics of every run of one workload / book, added together.
OrderIndexStats mergedIndexStats(const Collected& c) {
    OrderIndexStats total;
    auto add = [](OrderIndexStats::Distribution& into, const OrderIndexStats::Distribution& d) {
        for (std::size_t b = 0; b < OrderIndexStats::kBuckets; ++b) {
            into.counts[b] += d.counts[b];
        }
        into.total += d.total;
        into.max = std::max(into.max, d.max);
    };
    for (const RunResult& r : c.runs) {
        add(total.findProbes, r.indexStats.findProbes);
        add(total.insertProbes, r.indexStats.insertProbes);
        add(total.eraseProbes, r.indexStats.eraseProbes);
        add(total.eraseScan, r.indexStats.eraseScan);
        add(total.eraseShifts, r.indexStats.eraseShifts);
    }
    return total;
}

// Lowest bucket floor with at least p % of the samples at or below its bucket.
std::uint64_t distributionPercentile(const OrderIndexStats::Distribution& d, const double p) {
    if (d.total == 0) {
        return 0;
    }
    const auto rank =
        static_cast<std::uint64_t>(std::ceil(p / 100.0 * static_cast<double>(d.total)));
    std::uint64_t seen = 0;
    for (std::size_t b = 0; b < OrderIndexStats::kBuckets; ++b) {
        seen += d.counts[b];
        if (seen >= rank) {
            return OrderIndexStats::bucketFloor(b);
        }
    }
    return d.max;
}
#endif

void printTable(std::ostream& out, const Collected& c, const Environment& env) {
    const bool ns = env.canReportNanoseconds();
    const double scale = ns ? 1.0 / env.tsc.ticksPerNanosecond() : 1.0;
    std::uint64_t mismatches = 0;
    std::string upstream;
    std::string probes;
    for (const RunResult& r : c.runs) {
        mismatches += r.mismatches;
        upstream += (upstream.empty() ? "" : "/") + std::to_string(r.upstreamAllocations);
        probes += (probes.empty() ? "" : "/") + std::to_string(r.clockProbeP50);
    }
    out << "\n"
        << c.workload << " / " << c.book << " (" << c.description << "; " << c.runs.size()
        << " runs; churn " << fmt(c.churn) << " levels/op; outcome mismatches " << mismatches
        << "; max index cluster " << c.runs.back().maxCluster << "; id rejects "
        << c.runs.back().idRejects << "; pmr upstream allocations per run " << upstream
        << "; clock probe (empty-timer p50, ticks) per run " << probes << ")\n";
    if (c.workload == "w1") {
        std::vector<double> legacy;
        for (const RunResult& r : c.runs) {
            legacy.push_back(r.legacyMeanNs);
        }
        out << "  legacy demo-style mean over the whole stream: " << fmt(median(legacy))
            << " ns/op [min " << fmt(*std::ranges::min_element(legacy)) << ", max "
            << fmt(*std::ranges::max_element(legacy))
            << "] (steady_clock, all ops mixed; for continuity only)\n";
    }
    out << "  " << std::left << std::setw(22)
        << (ns ? std::string("class (ns)") : std::string("class (ticks)")) << std::right
        << std::setw(10) << "count" << std::setw(9) << "mean";
    for (const std::string_view name : kPercentileNames) {
        out << std::setw(20) << name;
    }
    out << "\n";

    auto row = [&](const std::string_view name, auto getter) {
        std::vector<Stats> per;
        for (const RunResult& r : c.runs) {
            per.push_back(statsOf(getter(r), scale));
        }
        if (per.front().count == 0) {
            return;
        }
        std::uint64_t total = 0;
        std::vector<double> means;
        for (const Stats& s : per) {
            total += s.count;
            means.push_back(s.mean);
        }
        out << "  " << std::left << std::setw(22) << name << std::right << std::setw(10) << total
            << std::setw(9) << fmt(median(means));
        for (std::size_t i = 0; i < kPercentiles.size(); ++i) {
            std::vector<double> v;
            for (const Stats& s : per) {
                v.push_back(s.p[i]);
            }
            const auto [lo, hi] = std::ranges::minmax(v);
            std::ostringstream cell;
            cell << fmt(median(v)) << " [" << fmt(lo) << "-" << fmt(hi) << "]";
            out << std::setw(20) << cell.str();
        }
        out << "\n";
    };
    for (std::size_t k = 0; k < kOpClassCount; ++k) {
        row(toString(static_cast<OpClass>(k)),
            [k](const RunResult& r) -> const LatencyHistogram& { return r.service[k]; });
    }
    row("response (paced)",
        [](const RunResult& r) -> const LatencyHistogram& { return r.response; });
#if defined(EIGENBOOK_INSTRUMENT_INDEX)
    if (c.runs.empty() || !c.runs.front().hasIndexStats) {
        return;
    }
    const OrderIndexStats s = mergedIndexStats(c);
    auto dist = [&](const char* name, const OrderIndexStats::Distribution& d) {
        out << "    " << std::left << std::setw(14) << name << std::right << " n " << d.total;
        for (const double p : {50.0, 99.0, 99.9}) {
            out << ", p" << p << " >= " << distributionPercentile(d, p);
        }
        out << ", max " << d.max << "\n";
    };
    out << "  index probe lengths (all runs, measured part; bucket floors):\n";
    dist("find", s.findProbes);
    dist("insert", s.insertProbes);
    dist("erase-locate", s.eraseProbes);
    dist("erase-scan", s.eraseScan);
    dist("erase-shift", s.eraseShifts);
#endif
}

void writeCsv(const fs::path& dir, const std::vector<Collected>& all, const Environment& env) {
    const double scale = env.canReportNanoseconds() ? 1.0 / env.tsc.ticksPerNanosecond() : 0.0;
    std::ofstream summary(dir / "summary.csv");
    summary << "workload,book,class,run,seed,count,mean_ticks";
    for (const std::string_view n : kPercentileNames) {
        summary << "," << n << "_ticks";
    }
    summary << ",mean_ns";
    for (const std::string_view n : kPercentileNames) {
        summary << "," << n << "_ns";
    }
    summary << ",churn_levels_per_op,outcome_mismatches,upstream_allocations,clock_probe_p50_"
               "ticks\n";

    for (const Collected& c : all) {
        auto emit = [&](const std::string_view cls,
                        const LatencyHistogram& h,
                        const std::size_t run,
                        const RunResult& r) {
            if (h.count() == 0) {
                return;
            }
            const Stats cyc = statsOf(h, 1.0);
            summary << c.workload << "," << c.book << "," << cls << "," << run << ","
                    << c.seeds[run] << "," << cyc.count << "," << cyc.mean;
            for (const double v : cyc.p) {
                summary << "," << v;
            }
            summary << "," << (scale > 0 ? cyc.mean * scale : 0.0);
            for (const double v : cyc.p) {
                summary << "," << (scale > 0 ? v * scale : 0.0);
            }
            summary << "," << c.churn << "," << r.mismatches << "," << r.upstreamAllocations << ","
                    << r.clockProbeP50 << "\n";
        };
        for (std::size_t run = 0; run < c.runs.size(); ++run) {
            const RunResult& r = c.runs[run];
            for (std::size_t k = 0; k < kOpClassCount; ++k) {
                emit(toString(static_cast<OpClass>(k)), r.service[k], run, r);
            }
            emit("response", r.response, run, r);
        }

        // Full distribution, all runs merged.
        std::ofstream dist(dir / ("distribution_" + c.workload + "_" + c.book + ".csv"));
        dist << "class,low_ticks,high_ticks,count,cumulative_fraction,low_ns,high_ns\n";
        auto dump = [&](const std::string_view cls, auto getter) {
            LatencyHistogram merged;
            for (const RunResult& r : c.runs) {
                merged.merge(getter(r));
            }
            if (merged.count() == 0) {
                return;
            }
            std::uint64_t cumulative = 0;
            merged.forEachBucket(
                [&](const std::uint64_t lo, const std::uint64_t hi, const std::uint64_t n) {
                    cumulative += n;
                    dist << cls << "," << lo << "," << hi << "," << n << ","
                         << static_cast<double>(cumulative) / static_cast<double>(merged.count())
                         << "," << static_cast<double>(lo) * scale << ","
                         << static_cast<double>(hi) * scale << "\n";
                });
        };
        for (std::size_t k = 0; k < kOpClassCount; ++k) {
            dump(toString(static_cast<OpClass>(k)),
                 [k](const RunResult& r) -> const LatencyHistogram& { return r.service[k]; });
        }
        dump("response", [](const RunResult& r) -> const LatencyHistogram& { return r.response; });

#if defined(EIGENBOOK_INSTRUMENT_INDEX)
        // Full probe / shift length distributions, all runs merged (OrderIndex books only).
        if (!c.runs.empty() && c.runs.front().hasIndexStats) {
            const OrderIndexStats s = mergedIndexStats(c);
            std::ofstream probes(dir / ("index_probes_" + c.workload + "_" + c.book + ".csv"));
            probes << "distribution,length_from,count\n";
            auto emitDist = [&](const char* name, const OrderIndexStats::Distribution& d) {
                for (std::size_t b = 0; b < OrderIndexStats::kBuckets; ++b) {
                    if (d.counts[b] != 0) {
                        probes << name << "," << OrderIndexStats::bucketFloor(b) << ","
                               << d.counts[b] << "\n";
                    }
                }
            };
            emitDist("find", s.findProbes);
            emitDist("insert", s.insertProbes);
            emitDist("erase-locate", s.eraseProbes);
            emitDist("erase-scan", s.eraseScan);
            emitDist("erase-shift", s.eraseShifts);
        }
#endif
    }
}

// --- Environment ---------------------------------------------------------------------------------

// Empty tscStart/tscStop pairs. Measured at the start and at the end of a session: the TSC runs at
// a fixed rate, so a different result means the core clock changed in between.
LatencyHistogram timerOverhead() {
    LatencyHistogram overhead;
    for (int i = 0; i < 1'000'000; ++i) {
        const std::uint64_t t0 = platform::tscStart();
        const std::uint64_t t1 = platform::tscStop();
        overhead.record(t1 - t0);
    }
    return overhead;
}

Environment setUp(const Options& o) {
    Environment env;
    env.cpu = platform::cpuInfo();
    const std::vector<platform::LogicalCpu> cpus = platform::enumerateCpus();
    env.logicalCpus = static_cast<unsigned>(cpus.size());
    std::vector<unsigned> cores;
    for (const platform::LogicalCpu& c : cpus) {
        if (c.coreIndex && std::ranges::find(cores, *c.coreIndex) == cores.end()) {
            cores.push_back(*c.coreIndex);
        }
        env.performanceCpus += c.coreType == platform::CoreType::PERFORMANCE ? 1 : 0;
    }
    env.physicalCores = static_cast<unsigned>(cores.size());

    if (o.pin) {
        const std::optional<unsigned> target = o.cpu ? o.cpu : platform::pickBenchmarkCpu(cpus);
        if (!target || !platform::pinCurrentThread(*target)) {
            std::cerr << "warning: could not pin to a CPU; results are unpinned\n";
        } else {
            env.pinnedCpu = platform::currentCpu();
            env.pinnedCoreType = platform::currentCoreType();
        }
    }
    env.priority = platform::raiseThreadPriority(o.realtime);
    env.os = platform::osDescription();
    env.power = platform::powerPolicy();
    env.powerSource = platform::powerSource();
    env.largePages = platform::largePagesAvailable();
    env.build = {EIGENBOOK_BENCH_COMPILER,
                 EIGENBOOK_BENCH_BUILD_TYPE,
                 EIGENBOOK_BENCH_FLAGS,
                 EIGENBOOK_BENCH_GIT_SHA};

    // Calibrate three times; keep the median, report the spread.
    std::vector<platform::TscCalibration> cal;
    for (int i = 0; i < 3; ++i) {
        cal.push_back(platform::calibrateTsc(std::chrono::milliseconds(o.calibrateMs)));
    }
    std::ranges::sort(cal, {}, &platform::TscCalibration::ticksPerSecond);
    env.tsc = cal[1];
    env.tscSpreadPercent = (cal[2].ticksPerSecond / cal[0].ticksPerSecond - 1.0) * 100.0;

    const LatencyHistogram overhead = timerOverhead();
    env.timerOverheadMin = overhead.min();
    env.timerOverheadP50 = overhead.valueAtPercentile(50.0);
    env.timerOverheadP99 = overhead.valueAtPercentile(99.0);
    return env;
}

} // namespace

int main(int argc, char** argv) {
    const Options opts = parse(argc, argv);
    // Before any book exists, so every map ladder pool takes the counter as its upstream.
    std::pmr::set_default_resource(&upstreamCounter());
    bookOptions().hybridWindowTicks = opts.hybridWindow;
    Environment env = setUp(opts);
    print(std::cout, env);
    std::cout << "Hybrid window  : " << opts.hybridWindow << " ticks (--hybrid-window)\n";
    if (!env.canReportNanoseconds()) {
        std::cout << "\nNo invariant TSC: all values below are in TSC ticks.\n";
    }

    std::vector<WorkloadSpec> selected;
    for (const WorkloadSpec& spec : registry()) {
        const bool all = std::ranges::find(opts.workloads, "all") != opts.workloads.end();
        if (all || std::ranges::find(opts.workloads, spec.name) != opts.workloads.end()) {
            selected.push_back(spec);
        }
    }
    if (selected.empty()) {
        usage("no workload matches --workload");
    }

    std::vector<Collected> collected;
    for (const WorkloadSpec& spec : selected) {
        WorkloadSize size{.warmupOps = opts.warmup,
                          .measuredOps = opts.measured.value_or(
                              opts.quick ? std::min<std::size_t>(spec.defaultMeasured, 100'000)
                                         : spec.defaultMeasured)};
        const std::vector<BookRunner> books = selectedBooks(opts);
        std::vector<Collected> perBook(books.size());
        for (std::size_t b = 0; b < books.size(); ++b) {
            perBook[b].workload = spec.name;
            perBook[b].book = std::string(books[b].name);
        }
        for (unsigned run = 0; run < opts.runs; ++run) {
            const std::uint64_t seed = opts.seed + run;
            const Workload w = spec.make(seed, size, run);
            std::cerr << spec.name << " run " << run + 1 << "/" << opts.runs << ": " << w.ops.size()
                      << " ops (" << w.timedOps << " timed)\n";
            // Rotate which book goes first so none always runs on a warmer (or cooler) machine.
            for (std::size_t k = 0; k < books.size(); ++k) {
                const std::size_t b = (run + k) % books.size();
                RunResult r = books[b].measure(w, env, opts.prefault);
                if (spec.name == "w1") {
                    r.legacyMeanNs = books[b].legacy(w);
                }
                perBook[b].runs.push_back(std::move(r));
                perBook[b].seeds.push_back(w.seed);
            }
            for (Collected& c : perBook) {
                c.churn = w.churnPerOp();
                c.description = w.description;
            }
        }
        for (Collected& c : perBook) {
            printTable(std::cout, c, env);
            collected.push_back(std::move(c));
        }
        std::cout.flush();
    }

    env.timerOverheadEndP50 = timerOverhead().valueAtPercentile(50.0);
    std::cout << "\n";
    print(std::cout, env); // again, now with the end-of-session clock check

    std::error_code ec;
    fs::create_directories(opts.out, ec);
    writeCsv(opts.out, collected, env);
    std::ofstream envFile(opts.out / "environment.txt");
    print(envFile, env);
    std::cout << "\nCSV written to " << fs::absolute(opts.out).string() << "\n";

    std::uint64_t mismatches = 0;
    for (const Collected& c : collected) {
        for (const RunResult& r : c.runs) {
            mismatches += r.mismatches;
        }
    }
    if (mismatches != 0) {
        std::cerr << "ERROR: " << mismatches
                  << " operations did not match the generator's prediction\n";
        return 1;
    }
    return 0;
}
