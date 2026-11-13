# Benchmarks

What is measured, how, on what, and the results. Every number here comes from
`bench/results/<session>/summary.csv`, written by `eigenbook_latency`; the tables are generated
from those files, not typed in.

## Method

**Timer.** Each operation is timed on its own: `lfence; rdtsc` before, `rdtscp; lfence` after
([include/eigenbook/platform/tsc.hpp](../include/eigenbook/platform/tsc.hpp)). The timed region
is exactly one book call (`addOrder`, `cancelOrder` or `modifyOrder`) including building its
request struct and the fill callback, which only counts fills.

- The harness refuses to print nanoseconds unless the CPU reports an **invariant TSC**
  (CPUID 0x80000007 EDX[8]).
- The TSC frequency is **calibrated** against `std::chrono::steady_clock` three times over 1 s
  each; the median is used and the spread is reported.
- The **empty-timer overhead** (a `tscStart()`/`tscStop()` pair with nothing between) is measured
  and reported. It is **not subtracted**: the numbers below include it.
- Raw values are TSC ticks, a fixed-rate clock, not core cycles. Because the overhead of an empty
  pair is a fixed amount of core work, the harness measures it again at the end of the session:
  a change means the core clock changed during the session (**clock check**).

**Histograms.** A log-linear histogram (HdrHistogram-style, 3 significant digits: exact below
2048 ticks, then 1024 sub-buckets per power of two), preallocated, no allocation while recording
(tested). Count, min, max and mean are exact. A percentile is the upper bound of the bucket that
contains it (conservative, at most 0.1 % above the exact value).

**One histogram per operation class**, never mixed:

| Class | Meaning |
|---|---|
| `add-passive` | add that does not trade |
| `add-aggr-1`, `add-aggr-2..4`, `add-aggr-5+` | add that trades, by number of fills |
| `cancel-hit` | cancel of a resting order (W3: split into `-front`, `-middle`, `-back` queue position) |
| `cancel-miss` | cancel of an id that is not resting |
| `modify-in-place`, `modify-reenter` | (no workload uses modify yet) |
| `response (paced)` | W5 only: completion time minus the op's scheduled arrival time |

**Environment control** (`Eigenbook::platform`): the thread is pinned to one logical CPU, chosen
as a P-core on hybrid Intel CPUs (core type from CPUID leaf 0x1A, read while pinned to each CPU in
turn; cross-checked against Windows' `EfficiencyClass`), away from CPU 0 and its hyper-thread
sibling. Priority is raised (Windows: `HIGH_PRIORITY_CLASS` + `THREAD_PRIORITY_TIME_CRITICAL`;
Linux: optional `SCHED_FIFO` with `--realtime`). The report prints CPU, core id and type, OS,
power plan, power source, large-page availability, compiler, flags, build type and git SHA.

**Warm-up and preallocation.** Every workload is generated completely before timing starts
(a flat array of 56-byte ops). Each run builds a fresh book sized for the workload's peak number
of live orders (+12.5 %), runs at least 200,000 untimed warm-up ops, then the measured ops.
`--prefault` touches every page of the op stream first; the book's own pool, index and dense
ladder are zero-initialised when constructed, so they are already resident. Large pages are not
used (on Windows they need `SeLockMemoryPrivilege`, not available here).

**Repeatability.** Five runs with seeds S … S+4 (W1: 42 … 46, so run 0 is the old demo stream).
Each run regenerates the workload and measures both books on it; the order of the two books
alternates between runs. Tables show the **median over the five runs** of each statistic, with
the **min–max over runs** in small print. `--quick` (2 runs, ≤ 100,000 measured ops) is for CI.

**Correctness during measurement.** The generator predicts every op's outcome on a shadow book
(class and number of fills). The harness compares each op's real outcome with the prediction and
reports mismatches; a run with mismatches exits with an error. All runs below: 0 mismatches.

**Allocation during measurement.** The harness installs a counting `std::pmr` default resource,
so the number of chunks the map ladder's node pool fetches from upstream during the measured part
is reported per run ("pmr upstream allocations"). Allocations through `operator new` (pool or
index growth) are checked separately by the zero-allocation test.

## Workloads

| Name | Shape | Timed ops / run |
|---|---|---:|
| `w1` | Exactly the old demo stream (2,000,000 ops; ~2/3 limit adds around 10,000, ~1/3 cancels drawn from a list of every id ever added, which is never updated on fills, so some cancels miss). Ops 0–199,999 are warm-up. | 1,800,000 |
| `w2-churn` | Passive limits uniform in [mid−1000, mid−1] / [mid+1, mid+1000], 10 % priced through the touch, each resting order cancelled after a geometric lifetime (mean 64 ops). Measured level churn ≈ 1.0 levels created + destroyed per op. | 1,000,000 |
| `w2-drift` | As `w2-churn` with band ±4000, the mid rising 1 tick per 100 ops, and 2 % of orders never cancelled (left behind the trend). Churn ≈ 0.9. | 1,000,000 |
| `w3-cancel70`, `w3-cancel90` | 20 levels × 50 orders per side, never crossing. 70 % / 90 % of timed ops cancel an order at the front, middle or back of its level (equal thirds), the rest are passive adds; each timed op is followed by an untimed op restoring the level's size. 2 % of timed ops cancel ids that never existed. | 1,000,000 |
| `w4-collide` | 4096 order ids constructed (by inverting `OrderIndex::hash`) to share one home slot in a 262,144-slot index: one probe cluster of length 4096. Timed cancels / adds of those ids, each paired with an untimed op keeping the count constant; 5 % cancels of colliding ids that do not exist. | 100,000 |
| `w4-load50` | The index at its maximum load factor: 131,071 live orders in a 262,144-slot index, sequential ids. Timed cancel / add, each paired with the opposite untimed op. | 1,000,000 |
| `w5-burst` | Hawkes (self-exciting) arrivals, branching ratio 0.8, excitation decay 20 µs, long-run mean rate 500,000 ops/s; 45 % passive adds, 10 % aggressive adds, 45 % cancels of live orders. Replayed open-loop: each op starts at its scheduled time (busy-wait on the TSC). | 1,000,000 |

All workload parameters are fields of config structs in [bench/workloads/](../bench/workloads/).

## How to reproduce

```bash
cmake --preset bench                 # or: cmake --preset msvc-release on Windows
cmake --build --preset bench
./build/bench/bench/latency/eigenbook_latency --out bench/results/mine
#   --workload w2-churn,w4-collide   --book map|dense|both   --runs 5   --seed 1
#   --measured N   --warmup N   --quick   --cpu N | --no-pin   --realtime (Linux)   --prefault
```

`eigenbook_latency_instrumented` is the same program built with `EIGENBOOK_INSTRUMENT_INDEX`: it
also prints probe-length statistics of the order index. Its latencies are not comparable with the
plain build.

## Phase A2: results

### Status: provisional (machine on battery, core clock not stable)

These runs were taken on a laptop **on battery with the Windows "Balanced" power plan**. The
per-run clock probe (empty-timer p50, same instructions every time) took three distinct values
during the sessions, **about 29, 55 and 97 TSC ticks**, i.e. the core clock moved by roughly 3×
between runs. The harness now reports the probe for every run, and the tables show it.

Rules for reading the tables:

- Compare two rows only if their runs have the **same clock probe**. Map vs dense within one
  workload mostly qualifies (both books run on the same stream, alternately); different
  workloads often do not.
- Absolute values are provisional. Before quoting them, re-run on AC power with the
  "High performance" (or "Ultimate performance") plan and check that every run's probe is the
  same. That is a change to the machine's settings and is the owner's decision; it has not been
  made here.

### Environment

| | |
|---|---|
| CPU | 13th Gen Intel Core i7-13650HX, 14 cores / 20 threads (hybrid: 12 logical CPUs on P-cores) |
| Pinned to | CPU 2 (P-core; CPU 0 and its sibling avoided) |
| Priority | `HIGH_PRIORITY_CLASS` + `THREAD_PRIORITY_TIME_CRITICAL` |
| OS | Windows 10.0.26200 (Windows 11 Enterprise) |
| Power | Balanced plan, **on battery** |
| Large pages | not available (no `SeLockMemoryPrivilege`) |
| TSC | invariant; 2.8032 GHz, calibration spread < 0.00002 % |
| Timer overhead | empty `tscStart`/`tscStop` pair: min 49, p50 55 ticks (≈ 20 ns) at the ~55-tick clock state; included in every number below |
| Main table | Clang 21.1.8 (MSVC STL), `-O3 -DNDEBUG`, Release |
| Second table | MSVC 19.44.35229, `/O2 /Ob2 /DNDEBUG`, Release |
| Source | git `74e5b83445ac` plus the uncommitted Phase A2 changes (`-dirty`) |
| Runs | 5 per workload and book; median [min–max over runs] |
| Files | `bench/results/clang-21.1.8/`, `bench/results/msvc-19.44/` (`summary.csv`, `distribution_*.csv`, `environment.txt`, `console.txt`) |

Every run of every workload: **0 outcome mismatches** (each op did what the generator predicted).

### What the data shows (comparisons at equal clock probe only)

1. **Level churn, W2-drift** (all ten runs at probe 54–56): the dense ladder is faster than the
   map ladder at every percentile. Passive add p50 63.5 vs 167 ns, p99 136 vs 267 ns, p99.99
   1.5 vs 6.3 µs; cancel p50 66 vs 204 ns, p99.99 1.5 vs 6.8 µs. The map ladder's pmr pool
   fetched memory from upstream 2–3 times in every measured run; the dense ladder never did.
   Which part of the map's tail comes from those fetches, from pointer chasing or from
   rebalancing is the question Phase A3 answers; this data does not separate them.
2. **Level churn, W2-churn** (map runs at 54–58; three of five dense runs at 54–57, the other two
   at 97, so the dense medians come from the ~55 runs): passive add p50 52.8 vs 110 ns, cancel p50
   63 vs 130 ns, dense vs map.
3. **Cancel by queue position, W3** (all runs at 54–57): a cancel costs about the same wherever the
   order sits, as an intrusive list should: p50 68–70 ns at the front, 62–67 ns in the middle,
   60–62 ns at the back; p99 153 / 144–152 / 118–123 ns. Front cancels are consistently the
   slowest. One plausible reason, not tested: the front order is the oldest, so its cache line is
   the least recently touched. Map and dense are within a few ns, because W3's levels never
   empty, so no ladder work happens.
4. **Cancel of an unknown id** (W3): p50 43–46 ns, p99 82–89 ns: the cost of an index miss.
5. **Order-index attack, W4** (all runs at probe 97, so do not compare these with W1–W3): with
   4096 ids built to share one home slot, a cancel takes p50 16.7–16.8 µs, against 340–355 ns with the
   index at its maximum natural load (W4-load50, longest cluster 42). A cancel of an absent
   colliding id takes 8.9 µs. Map and dense are the same: the index dominates.
6. **Microbursts, W5** (probe 97): service times stay below 0.35 µs at p99, but response time
   from the scheduled arrival reaches p99 45–49 µs and p99.9 0.38–0.40 ms. Open-loop replay counts
   every stall of the measuring thread against all the ops queued behind it; what stalled it
   (pre-emption, interrupts, clock changes) is not identified here.
7. **The far tail**: outside W4-collide and the W5 response times, p99.99 ranges from 0.2 to
   13 µs and most classes have maxima of tens of µs up to about 1 ms; even the cheapest op
   (cancel miss) reaches 5–18 µs in some runs. On a non-isolated Windows core this is consistent
   with interrupts and pre-emption, but it is not attributed. Do not read the far tail (p99.99,
   max) as a property of the book until it has been measured on an isolated core.
8. **W1 continuity**: the demo-style whole-loop mean, recomputed inside the harness, is 131 ns/op
   (map) and 104 ns/op (dense) in the Clang session (probe 57) and 190 / 171 ns/op in the MSVC
   session (mixed clock states). The README's former ~106 / ~90 ns/op came from the demo binary
   under unrecorded conditions; none of these are comparable with each other.

### Not measured here

- Linux, GCC, `SCHED_FIFO`, isolated cores (`isolcpus`/`nohz_full`), huge pages: no Linux machine
  in this environment. The Linux code paths have not been compiled.
- A stable core clock (see Status).
- **Modify:** no workload issues modify requests yet, so `modify-in-place` / `modify-reenter` have
  no data. The modify path is reworked in Phase B1 (all-or-nothing modify); workloads get a
  modify mix then.
- Google Benchmark micro-benchmarks (`bench/micro/`): the dependency could not be fetched here.
- Index probe statistics (`eigenbook_latency_instrumented`): built, tested, not yet run for
  publication; that is Phase A4.

### Results: Clang 21.1.8

Latencies in ns. Cells: median over 5 runs, min–max over runs in small print. `ops` is the total
over the 5 runs.

**w1 / map**: churn 0.08 levels/op; pmr upstream allocations per run: 0, 1, 1, 1, 0; clock probe per run (ticks): 57, 57, 57, 57, 57

| class | ops | p50 | p90 | p99 | p99.9 | p99.99 | max |
|---|---:|---:|---:|---:|---:|---:|---:|
| add-passive | 5300656 | 184 <sub>179–229</sub> | 280 <sub>275–339</sub> | 418 <sub>410–483</sub> | 568 <sub>514–3547</sub> | 12865 <sub>10336–12911</sub> | 103177 <sub>91823–552482</sub> |
| add-aggr-1 | 328066 | 182 <sub>173–227</sub> | 295 <sub>289–352</sub> | 445 <sub>441–511</sub> | 691 <sub>625–2260</sub> | 12762 <sub>10325–46370</sub> | 38399 <sub>27623–112393</sub> |
| add-aggr-2..4 | 363393 | 210 <sub>204–250</sub> | 375 <sub>368–436</sub> | 581 <sub>563–692</sub> | 857 <sub>804–3761</sub> | 12933 <sub>11883–26712</sub> | 68733 <sub>30420–102527</sub> |
| add-aggr-5+ | 9093 | 419 <sub>400–468</sub> | 728 <sub>675–864</sub> | 1005 <sub>989–1275</sub> | 3935 <sub>1257–4492</sub> | 9687 <sub>5669–68161</sub> | 9687 <sub>5669–68161</sub> |
| cancel-hit | 2358547 | 354 <sub>334–438</sub> | 482 <sub>465–579</sub> | 631 <sub>615–741</sub> | 3772 <sub>800–4280</sub> | 13402 <sub>12979–38105</sub> | 410796 <sub>74879–708643</sub> |
| cancel-miss | 640245 | 184 <sub>179–230</sub> | 249 <sub>230–306</sub> | 393 <sub>388–461</sub> | 528 <sub>455–3558</sub> | 12808 <sub>10051–13059</sub> | 91233 <sub>21733–216495</sub> |

**w1 / dense**: churn 0.08 levels/op; pmr upstream allocations per run: 0, 0, 0, 0, 0; clock probe per run (ticks): 59, 57, 58, 59, 59

| class | ops | p50 | p90 | p99 | p99.9 | p99.99 | max |
|---|---:|---:|---:|---:|---:|---:|---:|
| add-passive | 5300656 | 176 <sub>172–218</sub> | 248 <sub>234–301</sub> | 394 <sub>390–456</sub> | 540 <sub>508–2087</sub> | 12716 <sub>10622–12956</sub> | 504010 <sub>23961–564665</sub> |
| add-aggr-1 | 328066 | 176 <sub>170–217</sub> | 267 <sub>257–321</sub> | 405 <sub>401–465</sub> | 600 <sub>523–688</sub> | 12591 <sub>10097–12956</sub> | 55202 <sub>13060–534449</sub> |
| add-aggr-2..4 | 363393 | 194 <sub>188–233</sub> | 345 <sub>336–403</sub> | 541 <sub>516–638</sub> | 3427 <sub>849–3707</sub> | 12876 <sub>12214–12899</sub> | 34738 <sub>13281–89216</sub> |
| add-aggr-5+ | 9093 | 350 <sub>343–407</sub> | 677 <sub>623–776</sub> | 974 <sub>928–1222</sub> | 5690 <sub>4024–11113</sub> | 13202 <sub>12961–13469</sub> | 13202 <sub>12961–13469</sub> |
| cancel-hit | 2358547 | 349 <sub>330–426</sub> | 478 <sub>446–556</sub> | 626 <sub>616–722</sub> | 3972 <sub>3707–4135</sub> | 13013 <sub>12317–36552</sub> | 112656 <sub>48342–580150</sub> |
| cancel-miss | 640245 | 186 <sub>181–228</sub> | 246 <sub>225–291</sub> | 393 <sub>392–457</sub> | 551 <sub>490–1192</sub> | 12739 <sub>6855–32534</sub> | 21570 <sub>13031–598758</sub> |

**w2-churn / map**: churn 1.00 levels/op; pmr upstream allocations per run: 0, 0, 0, 0, 2; clock probe per run (ticks): 54, 58, 54, 54, 54

| class | ops | p50 | p90 | p99 | p99.9 | p99.99 | max |
|---|---:|---:|---:|---:|---:|---:|---:|
| add-passive | 2366359 | 110 <sub>108–155</sub> | 148 <sub>146–240</sub> | 192 <sub>187–320</sub> | 245 <sub>231–407</sub> | 565 <sub>478–12922</sub> | 82270 <sub>35619–103116</sub> |
| add-aggr-1 | 299095 | 131 <sub>127–153</sub> | 194 <sub>186–312</sub> | 252 <sub>236–424</sub> | 321 <sub>290–531</sub> | 4854 <sub>609–12705</sub> | 35370 <sub>18214–93513</sub> |
| add-aggr-2..4 | 7558 | 163 <sub>153–218</sub> | 258 <sub>248–422</sub> | 331 <sub>306–553</sub> | 422 <sub>360–697</sub> | 10455 <sub>426–33241</sub> | 10455 <sub>426–33241</sub> |
| cancel-hit | 2326988 | 130 <sub>127–174</sub> | 172 <sub>166–280</sub> | 216 <sub>206–365</sub> | 265 <sub>249–450</sub> | 6284 <sub>525–29315</sub> | 62324 <sub>41474–100092</sub> |

**w2-churn / dense**: churn 1.00 levels/op; pmr upstream allocations per run: 0, 0, 0, 0, 0; clock probe per run (ticks): 54, 97, 54, 57, 97

| class | ops | p50 | p90 | p99 | p99.9 | p99.99 | max |
|---|---:|---:|---:|---:|---:|---:|---:|
| add-passive | 2366359 | 52.8 <sub>52.1–94.2</sub> | 67.4 <sub>63.5–120</sub> | 87.4 <sub>80.3–160</sub> | 144 <sub>111–259</sub> | 1457 <sub>276–8972</sub> | 40991 <sub>39222–59339</sub> |
| add-aggr-1 | 299095 | 82.4 <sub>81.0–148</sub> | 113 <sub>103–205</sub> | 174 <sub>144–318</sub> | 281 <sub>229–555</sub> | 5411 <sub>446–13230</sub> | 36780 <sub>33558–74917</sub> |
| add-aggr-2..4 | 7558 | 125 <sub>119–224</sub> | 168 <sub>156–310</sub> | 223 <sub>197–432</sub> | 407 <sub>275–2996</sub> | 419 <sub>289–3359</sub> | 419 <sub>289–3359</sub> |
| cancel-hit | 2326988 | 63.1 <sub>62.1–113</sub> | 81.7 <sub>78.1–147</sub> | 113 <sub>101–205</sub> | 146 <sub>127–275</sub> | 1409 <sub>237–8949</sub> | 61556 <sub>44459–96580</sub> |

**w2-drift / map**: churn 0.89 levels/op; pmr upstream allocations per run: 2, 3, 2, 3, 2; clock probe per run (ticks): 54, 54, 54, 54, 54

| class | ops | p50 | p90 | p99 | p99.9 | p99.99 | max |
|---|---:|---:|---:|---:|---:|---:|---:|
| add-passive | 2381063 | 167 <sub>164–273</sub> | 210 <sub>207–369</sub> | 267 <sub>259–478</sub> | 403 <sub>340–1071</sub> | 6341 <sub>5159–58219</sub> | 75662 <sub>39036–768563</sub> |
| add-aggr-1 | 288861 | 168 <sub>167–203</sub> | 257 <sub>253–444</sub> | 339 <sub>334–615</sub> | 431 <sub>417–1431</sub> | 6038 <sub>2672–61278</sub> | 38102 <sub>8728–109647</sub> |
| add-aggr-2..4 | 12940 | 194 <sub>182–312</sub> | 353 <sub>333–615</sub> | 484 <sub>457–966</sub> | 796 <sub>630–3113</sub> | 5739 <sub>1644–7195</sub> | 5739 <sub>1644–7195</sub> |
| add-aggr-5+ | 3 | 377 <sub>342–475</sub> | 377 <sub>342–475</sub> | 377 <sub>342–475</sub> | 377 <sub>342–475</sub> | 377 <sub>342–475</sub> | 377 <sub>342–475</sub> |
| cancel-hit | 2317133 | 204 <sub>203–296</sub> | 273 <sub>270–474</sub> | 335 <sub>330–598</sub> | 412 <sub>385–835</sub> | 6849 <sub>6512–54703</sub> | 71791 <sub>68756–153542</sub> |

**w2-drift / dense**: churn 0.89 levels/op; pmr upstream allocations per run: 0, 0, 0, 0, 0; clock probe per run (ticks): 54, 56, 54, 56, 54

| class | ops | p50 | p90 | p99 | p99.9 | p99.99 | max |
|---|---:|---:|---:|---:|---:|---:|---:|
| add-passive | 2381063 | 63.5 <sub>62.4–64.2</sub> | 88.8 <sub>88.8–90.3</sub> | 136 <sub>132–148</sub> | 227 <sub>212–266</sub> | 1534 <sub>476–1561</sub> | 60118 <sub>38017–75971</sub> |
| add-aggr-1 | 288861 | 92.0 <sub>91.7–92.8</sub> | 142 <sub>140–150</sub> | 232 <sub>225–260</sub> | 388 <sub>375–419</sub> | 1746 <sub>1432–7226</sub> | 41534 <sub>2128–69792</sub> |
| add-aggr-2..4 | 12940 | 143 <sub>142–147</sub> | 228 <sub>222–241</sub> | 364 <sub>352–407</sub> | 497 <sub>444–527</sub> | 700 <sub>598–2200</sub> | 700 <sub>598–2200</sub> |
| add-aggr-5+ | 3 | 349 <sub>257–471</sub> | 349 <sub>257–471</sub> | 349 <sub>257–471</sub> | 349 <sub>257–471</sub> | 349 <sub>257–471</sub> | 349 <sub>257–471</sub> |
| cancel-hit | 2317133 | 66.0 <sub>65.6–66.0</sub> | 91.3 <sub>90.6–91.7</sub> | 129 <sub>127–132</sub> | 179 <sub>177–187</sub> | 1464 <sub>326–1631</sub> | 78987 <sub>61562–98761</sub> |

**w3-cancel70 / map**: churn 0.00 levels/op; pmr upstream allocations per run: 0, 0, 0, 0, 0; clock probe per run (ticks): 56, 54, 56, 54, 56

| class | ops | p50 | p90 | p99 | p99.9 | p99.99 | max |
|---|---:|---:|---:|---:|---:|---:|---:|
| add-passive | 1467934 | 94.5 <sub>94.2–94.9</sub> | 122 <sub>121–122</sub> | 158 <sub>156–160</sub> | 216 <sub>209–221</sub> | 462 <sub>342–1511</sub> | 53450 <sub>39887–72023</sub> |
| cancel-hit-front | 1143558 | 69.9 <sub>69.6–70.3</sub> | 105 <sub>105–105</sub> | 153 <sub>153–154</sub> | 207 <sub>205–208</sub> | 388 <sub>297–5174</sub> | 52912 <sub>34324–67646</sub> |
| cancel-hit-middle | 1143553 | 67.4 <sub>63.9–67.8</sub> | 103 <sub>98.8–104</sub> | 150 <sub>145–151</sub> | 200 <sub>197–202</sub> | 286 <sub>275–1478</sub> | 58975 <sub>50534–79838</sub> |
| cancel-hit-back | 1145020 | 62.1 <sub>61.7–62.1</sub> | 84.5 <sub>84.5–84.5</sub> | 120 <sub>120–120</sub> | 159 <sub>156–163</sub> | 268 <sub>214–1386</sub> | 47186 <sub>31717–83633</sub> |
| cancel-miss | 99935 | 44.9 <sub>44.2–49.2</sub> | 61.4 <sub>61.0–62.1</sub> | 88.5 <sub>88.1–89.2</sub> | 115 <sub>113–137</sub> | 219 <sub>159–7614</sub> | 5234 <sub>304–17925</sub> |

**w3-cancel70 / dense**: churn 0.00 levels/op; pmr upstream allocations per run: 0, 0, 0, 0, 0; clock probe per run (ticks): 54, 56, 54, 57, 54

| class | ops | p50 | p90 | p99 | p99.9 | p99.99 | max |
|---|---:|---:|---:|---:|---:|---:|---:|
| add-passive | 1467934 | 62.8 <sub>62.8–65.3</sub> | 86.0 <sub>85.3–117</sub> | 125 <sub>125–181</sub> | 184 <sub>182–266</sub> | 289 <sub>248–2293</sub> | 71447 <sub>19193–90908</sub> |
| cancel-hit-front | 1143558 | 68.1 <sub>67.8–78.1</sub> | 103 <sub>103–139</sub> | 153 <sub>152–223</sub> | 205 <sub>203–314</sub> | 308 <sub>267–2441</sub> | 46488 <sub>36693–77286</sub> |
| cancel-hit-middle | 1143553 | 66.7 <sub>63.1–78.1</sub> | 103 <sub>98.5–138</sub> | 152 <sub>147–222</sub> | 202 <sub>197–320</sub> | 329 <sub>265–6312</sub> | 27212 <sub>19285–65924</sub> |
| cancel-hit-back | 1145020 | 59.9 <sub>59.9–66.7</sub> | 83.8 <sub>83.5–117</sub> | 123 <sub>122–178</sub> | 168 <sub>167–255</sub> | 247 <sub>234–591</sub> | 52361 <sub>16757–67668</sub> |
| cancel-miss | 99935 | 43.5 <sub>43.2–44.6</sub> | 58.5 <sub>58.1–78.8</sub> | 83.1 <sub>82.4–122</sub> | 109 <sub>106–170</sub> | 176 <sub>128–271</sub> | 260 <sub>141–5819</sub> |

**w3-cancel90 / map**: churn 0.00 levels/op; pmr upstream allocations per run: 0, 0, 0, 0, 0; clock probe per run (ticks): 55, 54, 56, 54, 56

| class | ops | p50 | p90 | p99 | p99.9 | p99.99 | max |
|---|---:|---:|---:|---:|---:|---:|---:|
| add-passive | 489702 | 94.9 <sub>93.5–96.3</sub> | 121 <sub>120–124</sub> | 159 <sub>158–164</sub> | 223 <sub>209–225</sub> | 789 <sub>317–5693</sub> | 37372 <sub>7110–123123</sub> |
| cancel-hit-front | 1471573 | 69.9 <sub>69.6–70.3</sub> | 105 <sub>104–105</sub> | 153 <sub>153–154</sub> | 206 <sub>203–210</sub> | 304 <sub>278–5419</sub> | 35702 <sub>21736–89848</sub> |
| cancel-hit-middle | 1469834 | 62.8 <sub>62.8–63.1</sub> | 97.4 <sub>97.0–97.7</sub> | 144 <sub>143–144</sub> | 197 <sub>193–199</sub> | 344 <sub>291–369</sub> | 69120 <sub>21668–72641</sub> |
| cancel-hit-back | 1468771 | 61.4 <sub>61.4–61.7</sub> | 83.8 <sub>83.5–83.8</sub> | 118 <sub>117–119</sub> | 156 <sub>153–159</sub> | 229 <sub>215–341</sub> | 23399 <sub>17569–114149</sub> |
| cancel-miss | 100120 | 46.0 <sub>44.2–48.5</sub> | 61.4 <sub>59.9–62.8</sub> | 87.4 <sub>86.0–88.1</sub> | 117 <sub>111–121</sub> | 179 <sub>164–377</sub> | 417 <sub>213–10586</sub> |

**w3-cancel90 / dense**: churn 0.00 levels/op; pmr upstream allocations per run: 0, 0, 0, 0, 0; clock probe per run (ticks): 54, 57, 54, 57, 54

| class | ops | p50 | p90 | p99 | p99.9 | p99.99 | max |
|---|---:|---:|---:|---:|---:|---:|---:|
| add-passive | 489702 | 62.8 <sub>62.8–63.1</sub> | 86.0 <sub>86.0–86.7</sub> | 129 <sub>127–132</sub> | 192 <sub>187–200</sub> | 324 <sub>270–1926</sub> | 55184 <sub>20412–108973</sub> |
| cancel-hit-front | 1471573 | 68.1 <sub>68.1–68.1</sub> | 103 <sub>103–103</sub> | 153 <sub>152–153</sub> | 208 <sub>205–209</sub> | 412 <sub>318–6535</sub> | 62590 <sub>19071–69979</sub> |
| cancel-hit-middle | 1469834 | 62.4 <sub>62.1–62.4</sub> | 97.0 <sub>97.0–97.4</sub> | 144 <sub>144–145</sub> | 197 <sub>194–199</sub> | 354 <sub>279–360</sub> | 26636 <sub>18944–90289</sub> |
| cancel-hit-back | 1468771 | 59.9 <sub>59.6–59.9</sub> | 83.1 <sub>82.8–83.5</sub> | 119 <sub>119–120</sub> | 163 <sub>161–168</sub> | 269 <sub>229–1496</sub> | 59948 <sub>7431–74576</sub> |
| cancel-miss | 100120 | 43.2 <sub>43.2–43.2</sub> | 58.1 <sub>57.8–58.5</sub> | 81.7 <sub>81.0–84.5</sub> | 108 <sub>106–142</sub> | 182 <sub>145–325</sub> | 399 <sub>212–6719</sub> |

**w4-collide / map**: churn 0.00 levels/op; pmr upstream allocations per run: 0, 0, 0, 0, 0; clock probe per run (ticks): 97, 97, 97, 97, 97

| class | ops | p50 | p90 | p99 | p99.9 | p99.99 | max |
|---|---:|---:|---:|---:|---:|---:|---:|
| add-passive | 237751 | 14954 <sub>14943–14965</sub> | 15034 <sub>15000–15045</sub> | 23173 <sub>17705–26278</sub> | 78036 <sub>30890–86758</sub> | 123287 <sub>69908–163013</sub> | 201881 <sub>150478–336416</sub> |
| cancel-hit | 237239 | 16746 <sub>16712–16803</sub> | 18721 <sub>18618–18801</sub> | 26735 <sub>22203–28493</sub> | 83150 <sub>36438–95342</sub> | 124109 <sub>72831–168767</sub> | 227292 <sub>165241–344802</sub> |
| cancel-miss | 25010 | 8875 <sub>8875–8875</sub> | 8904 <sub>8898–8909</sub> | 9303 <sub>9012–11626</sub> | 55068 <sub>16997–69634</sub> | 151976 <sub>27675–173331</sub> | 151976 <sub>27675–173331</sub> |

**w4-collide / dense**: churn 0.00 levels/op; pmr upstream allocations per run: 0, 0, 0, 0, 0; clock probe per run (ticks): 97, 97, 97, 97, 97

| class | ops | p50 | p90 | p99 | p99.9 | p99.99 | max |
|---|---:|---:|---:|---:|---:|---:|---:|
| add-passive | 237751 | 14828 <sub>14828–14840</sub> | 14886 <sub>14863–14908</sub> | 22420 <sub>19406–23139</sub> | 59406 <sub>28379–64748</sub> | 96621 <sub>62831–102739</sub> | 158956 <sub>110962–217382</sub> |
| cancel-hit | 237239 | 16803 <sub>16609–16906</sub> | 18744 <sub>18698–18892</sub> | 24223 <sub>23538–26027</sub> | 62328 <sub>32580–71187</sub> | 110776 <sub>85525–481826</sub> | 417467 <sub>153937–751512</sub> |
| cancel-miss | 25010 | 8870 <sub>8864–8870</sub> | 8887 <sub>8887–8892</sub> | 10936 <sub>9018–11506</sub> | 25822 <sub>20445–45205</sub> | 58818 <sub>25288–79740</sub> | 58818 <sub>25288–79740</sub> |

**w4-load50 / map**: churn 0.00 levels/op; pmr upstream allocations per run: 0, 0, 0, 0, 0; clock probe per run (ticks): 97, 97, 97, 97, 97

| class | ops | p50 | p90 | p99 | p99.9 | p99.99 | max |
|---|---:|---:|---:|---:|---:|---:|---:|
| add-passive | 2498829 | 272 <sub>265–278</sub> | 372 <sub>347–387</sub> | 505 <sub>464–538</sub> | 2370 <sub>792–2913</sub> | 17260 <sub>16598–29520</sub> | 575369 <sub>27534–595528</sub> |
| cancel-hit | 2501171 | 355 <sub>320–376</sub> | 503 <sub>457–535</sub> | 684 <sub>638–735</sub> | 3969 <sub>3735–4183</sub> | 21073 <sub>17899–49132</sub> | 220205 <sub>55756–499523</sub> |

**w4-load50 / dense**: churn 0.00 levels/op; pmr upstream allocations per run: 0, 0, 0, 0, 0; clock probe per run (ticks): 97, 97, 97, 97, 97

| class | ops | p50 | p90 | p99 | p99.9 | p99.99 | max |
|---|---:|---:|---:|---:|---:|---:|---:|
| add-passive | 2498829 | 191 <sub>188–219</sub> | 283 <sub>268–335</sub> | 427 <sub>388–495</sub> | 608 <sub>540–2525</sub> | 16644 <sub>13790–21358</sub> | 469742 <sub>68373–549087</sub> |
| cancel-hit | 2501171 | 340 <sub>323–403</sub> | 494 <sub>460–570</sub> | 689 <sub>636–784</sub> | 3750 <sub>3573–4161</sub> | 21472 <sub>18732–46758</sub> | 535875 <sub>520803–634778</sub> |

**w5-burst / map**: churn 0.92 levels/op; pmr upstream allocations per run: 0, 0, 0, 0, 0; clock probe per run (ticks): 97, 97, 97, 97, 97

| class | ops | p50 | p90 | p99 | p99.9 | p99.99 | max |
|---|---:|---:|---:|---:|---:|---:|---:|
| add-passive | 2388416 | 174 <sub>173–176</sub> | 235 <sub>232–240</sub> | 312 <sub>300–321</sub> | 508 <sub>371–625</sub> | 12203 <sub>7939–17739</sub> | 139995 <sub>24044–1020540</sub> |
| add-aggr-1 | 291615 | 132 <sub>130–133</sub> | 314 <sub>308–324</sub> | 432 <sub>420–446</sub> | 586 <sub>519–661</sub> | 11484 <sub>8373–16347</sub> | 47514 <sub>17959–81547</sub> |
| add-aggr-2..4 | 132640 | 227 <sub>222–234</sub> | 405 <sub>393–415</sub> | 570 <sub>555–592</sub> | 791 <sub>686–2012</sub> | 32100 <sub>3176–42808</sub> | 57400 <sub>8858–145005</sub> |
| add-aggr-5+ | 734 | 535 <sub>503–541</sub> | 674 <sub>645–725</sub> | 858 <sub>838–969</sub> | 962 <sub>859–1047</sub> | 962 <sub>859–1047</sub> | 962 <sub>859–1047</sub> |
| cancel-hit | 2186595 | 178 <sub>178–181</sub> | 253 <sub>248–258</sub> | 347 <sub>332–354</sub> | 564 <sub>417–705</sub> | 13253 <sub>8219–33721</sub> | 86921 <sub>61934–164745</sub> |
| response | 5000000 | 324 <sub>320–327</sub> | 563 <sub>504–568</sub> | 48630 <sub>6712–199452</sub> | 396347 <sub>276529–684566</sub> | 543196 <sub>458812–1993060</sub> | 618788 <sub>532775–2198690</sub> |

**w5-burst / dense**: churn 0.92 levels/op; pmr upstream allocations per run: 0, 0, 0, 0, 0; clock probe per run (ticks): 97, 97, 97, 97, 97

| class | ops | p50 | p90 | p99 | p99.9 | p99.99 | max |
|---|---:|---:|---:|---:|---:|---:|---:|
| add-passive | 2388416 | 97.0 <sub>97.0–98.5</sub> | 121 <sub>120–122</sub> | 152 <sub>144–159</sub> | 262 <sub>211–407</sub> | 2542 <sub>515–11929</sub> | 108547 <sub>22906–148601</sub> |
| add-aggr-1 | 291615 | 129 <sub>127–129</sub> | 204 <sub>202–205</sub> | 260 <sub>250–269</sub> | 412 <sub>324–471</sub> | 8401 <sub>972–36872</sub> | 48787 <sub>27608–143306</sub> |
| add-aggr-2..4 | 132640 | 179 <sub>177–180</sub> | 277 <sub>272–280</sub> | 377 <sub>358–382</sub> | 535 <sub>457–674</sub> | 8487 <sub>2751–48812</sub> | 37487 <sub>8383–85856</sub> |
| add-aggr-5+ | 734 | 365 <sub>356–374</sub> | 460 <sub>446–495</sub> | 570 <sub>541–727</sub> | 640 <sub>564–799</sub> | 640 <sub>564–799</sub> | 640 <sub>564–799</sub> |
| cancel-hit | 2186595 | 110 <sub>110–111</sub> | 143 <sub>142–143</sub> | 184 <sub>177–185</sub> | 259 <sub>223–407</sub> | 7802 <sub>602–8253</sub> | 87029 <sub>68468–192308</sub> |
| response | 5000000 | 246 <sub>244–248</sub> | 376 <sub>355–415</sub> | 45502 <sub>15947–159452</sub> | 383561 <sub>280730–416803</sub> | 509589 <sub>455525–586666</sub> | 593394 <sub>523159–697464</sub> |

### Results: MSVC 19.44

Same workloads and seeds. Most workloads in this session ran in more than one clock state (see the probes); treat this table as weaker evidence than the Clang one.

<details>
<summary>MSVC 19.44 tables</summary>

**w1 / map**: churn 0.08 levels/op; pmr upstream allocations per run: 0, 1, 1, 1, 0; clock probe per run (ticks): 57, 57, 59, 97, 97

| class | ops | p50 | p90 | p99 | p99.9 | p99.99 | max |
|---|---:|---:|---:|---:|---:|---:|---:|
| add-passive | 5300656 | 196 <sub>189–265</sub> | 336 <sub>300–397</sub> | 508 <sub>431–529</sub> | 1108 <sub>585–1818</sub> | 16883 <sub>14931–25137</sub> | 523278 <sub>131461–619962</sub> |
| add-aggr-1 | 328066 | 197 <sub>190–262</sub> | 339 <sub>316–403</sub> | 577 <sub>471–635</sub> | 1215 <sub>679–1852</sub> | 16153 <sub>12739–43402</sub> | 73011 <sub>21361–82469</sub> |
| add-aggr-2..4 | 363393 | 237 <sub>226–312</sub> | 439 <sub>405–540</sub> | 797 <sub>637–843</sub> | 1939 <sub>911–3884</sub> | 21130 <sub>9429–38972</sub> | 70631 <sub>25256–121495</sub> |
| add-aggr-5+ | 9093 | 486 <sub>461–654</sub> | 886 <sub>797–1068</sub> | 1372 <sub>1113–1462</sub> | 4928 <sub>2363–66758</sub> | 7347 <sub>2543–133159</sub> | 7347 <sub>2543–133159</sub> |
| cancel-hit | 2358547 | 376 <sub>367–519</sub> | 535 <sub>500–655</sub> | 739 <sub>650–816</sub> | 2219 <sub>1054–4289</sub> | 43904 <sub>21518–55525</sub> | 178351 <sub>95192–537257</sub> |
| cancel-miss | 640245 | 193 <sub>189–270</sub> | 289 <sub>263–341</sub> | 417 <sub>398–483</sub> | 1671 <sub>513–3633</sub> | 16849 <sub>9343–23949</sub> | 64186 <sub>26047–95090</sub> |

**w1 / dense**: churn 0.08 levels/op; pmr upstream allocations per run: 0, 0, 0, 0, 0; clock probe per run (ticks): 61, 59, 59, 97, 97

| class | ops | p50 | p90 | p99 | p99.9 | p99.99 | max |
|---|---:|---:|---:|---:|---:|---:|---:|
| add-passive | 5300656 | 256 <sub>181–259</sub> | 347 <sub>258–354</sub> | 482 <sub>394–491</sub> | 1160 <sub>505–3804</sub> | 35456 <sub>13847–55981</sub> | 514552 <sub>93598–559930</sub> |
| add-aggr-1 | 328066 | 255 <sub>181–258</sub> | 373 <sub>277–379</sub> | 518 <sub>407–534</sub> | 978 <sub>545–3701</sub> | 40799 <sub>17614–59269</sub> | 94153 <sub>22436–169959</sub> |
| add-aggr-2..4 | 363393 | 291 <sub>206–300</sub> | 492 <sub>365–511</sub> | 781 <sub>574–821</sub> | 2631 <sub>832–3798</sub> | 46096 <sub>13333–57351</sub> | 105165 <sub>73210–197216</sub> |
| add-aggr-5+ | 9093 | 564 <sub>397–584</sub> | 1015 <sub>732–1061</sub> | 1350 <sub>1073–1525</sub> | 10976 <sub>3770–63013</sub> | 38748 <sub>4330–91439</sub> | 38748 <sub>4330–91439</sub> |
| cancel-hit | 2358547 | 510 <sub>357–519</sub> | 648 <sub>487–663</sub> | 811 <sub>630–847</sub> | 4329 <sub>889–7711</sub> | 47488 <sub>21495–69269</sub> | 168537 <sub>105876–592851</sub> |
| cancel-miss | 640245 | 265 <sub>188–269</sub> | 339 <sub>255–350</sub> | 479 <sub>393–487</sub> | 2779 <sub>484–3847</sub> | 19395 <sub>12237–48082</sub> | 91444 <sub>24314–112575</sub> |

**w2-churn / map**: churn 1.00 levels/op; pmr upstream allocations per run: 0, 0, 0, 0, 2; clock probe per run (ticks): 97, 97, 97, 97, 29

| class | ops | p50 | p90 | p99 | p99.9 | p99.99 | max |
|---|---:|---:|---:|---:|---:|---:|---:|
| add-passive | 2366359 | 204 <sub>61.4–205</sub> | 275 <sub>82.8–278</sub> | 360 <sub>112–363</sub> | 446 <sub>222–454</sub> | 9263 <sub>9029–10964</sub> | 23176 <sub>22397–28445</sub> |
| add-aggr-1 | 299095 | 236 <sub>70.3–237</sub> | 346 <sub>103–347</sub> | 449 <sub>138–451</sub> | 563 <sub>222–565</sub> | 7374 <sub>2766–12796</sub> | 13639 <sub>9349–28328</sub> |
| add-aggr-2..4 | 7558 | 322 <sub>92.4–327</sub> | 469 <sub>141–476</sub> | 589 <sub>182–615</sub> | 715 <sub>240–4871</sub> | 767 <sub>245–9433</sub> | 767 <sub>245–9433</sub> |
| cancel-hit | 2326988 | 242 <sub>72.1–242</sub> | 312 <sub>93.1–312</sub> | 392 <sub>119–396</sub> | 484 <sub>172–498</sub> | 9263 <sub>7374–10068</sub> | 26871 <sub>15344–88178</sub> |

**w2-churn / dense**: churn 1.00 levels/op; pmr upstream allocations per run: 0, 0, 0, 0, 0; clock probe per run (ticks): 97, 97, 97, 97, 29

| class | ops | p50 | p90 | p99 | p99.9 | p99.99 | max |
|---|---:|---:|---:|---:|---:|---:|---:|
| add-passive | 2366359 | 108 <sub>32.5–108</sub> | 136 <sub>40.7–137</sub> | 168 <sub>49.9–170</sub> | 248 <sub>97.7–256</sub> | 4289 <sub>2834–5533</sub> | 32572 <sub>22263–457031</sub> |
| add-aggr-1 | 299095 | 163 <sub>49.2–165</sub> | 226 <sub>66.7–228</sub> | 339 <sub>125–350</sub> | 507 <sub>298–569</sub> | 8784 <sub>4329–9726</sub> | 13018 <sub>11539–22576</sub> |
| add-aggr-2..4 | 7558 | 244 <sub>71.7–248</sub> | 338 <sub>98.5–340</sub> | 451 <sub>127–462</sub> | 550 <sub>187–677</sub> | 624 <sub>226–7384</sub> | 624 <sub>226–7384</sub> |
| cancel-hit | 2326988 | 119 <sub>35.7–120</sub> | 153 <sub>46.0–155</sub> | 212 <sub>62.8–214</sub> | 274 <sub>81.7–274</sub> | 1472 <sub>282–2287</sub> | 24595 <sub>12228–28821</sub> |

**w2-drift / map**: churn 0.89 levels/op; pmr upstream allocations per run: 2, 3, 2, 3, 2; clock probe per run (ticks): 29, 29, 29, 29, 29

| class | ops | p50 | p90 | p99 | p99.9 | p99.99 | max |
|---|---:|---:|---:|---:|---:|---:|---:|
| add-passive | 2381063 | 88.5 <sub>87.8–88.8</sub> | 116 <sub>115–116</sub> | 150 <sub>149–152</sub> | 352 <sub>320–898</sub> | 4937 <sub>3907–6170</sub> | 20710 <sub>13819–24954</sub> |
| add-aggr-1 | 288861 | 81.7 <sub>81.0–82.0</sub> | 127 <sub>125–127</sub> | 177 <sub>175–179</sub> | 361 <sub>332–1195</sub> | 5051 <sub>2585–8122</sub> | 12084 <sub>9171–17214</sub> |
| add-aggr-2..4 | 12940 | 114 <sub>111–115</sub> | 198 <sub>196–205</sub> | 356 <sub>338–373</sub> | 2714 <sub>2159–11661</sub> | 11505 <sub>4622–12884</sub> | 11505 <sub>4622–12884</sub> |
| add-aggr-5+ | 3 | 221 <sub>190–244</sub> | 221 <sub>190–244</sub> | 221 <sub>190–244</sub> | 221 <sub>190–244</sub> | 221 <sub>190–244</sub> | 221 <sub>190–244</sub> |
| cancel-hit | 2317133 | 113 <sub>113–114</sub> | 156 <sub>155–156</sub> | 191 <sub>189–192</sub> | 242 <sub>237–250</sub> | 2531 <sub>2415–4534</sub> | 13099 <sub>10721–15342</sub> |

**w2-drift / dense**: churn 0.89 levels/op; pmr upstream allocations per run: 0, 0, 0, 0, 0; clock probe per run (ticks): 29, 29, 29, 29, 29

| class | ops | p50 | p90 | p99 | p99.9 | p99.99 | max |
|---|---:|---:|---:|---:|---:|---:|---:|
| add-passive | 2381063 | 36.4 <sub>35.7–36.7</sub> | 50.7 <sub>50.7–51.4</sub> | 92.0 <sub>81.0–104</sub> | 258 <sub>231–300</sub> | 2450 <sub>2358–3596</sub> | 12903 <sub>12744–30892</sub> |
| add-aggr-1 | 288861 | 54.2 <sub>53.9–54.2</sub> | 85.6 <sub>84.9–89.2</sub> | 197 <sub>184–210</sub> | 365 <sub>360–2029</sub> | 7129 <sub>4061–12477</sub> | 12493 <sub>11823–12966</sub> |
| add-aggr-2..4 | 12940 | 83.8 <sub>82.8–84.9</sub> | 143 <sub>139–154</sub> | 281 <sub>263–308</sub> | 469 <sub>421–652</sub> | 4150 <sub>3855–13283</sub> | 4150 <sub>3855–13283</sub> |
| add-aggr-5+ | 3 | 162 <sub>156–446</sub> | 162 <sub>156–446</sub> | 162 <sub>156–446</sub> | 162 <sub>156–446</sub> | 162 <sub>156–446</sub> | 162 <sub>156–446</sub> |
| cancel-hit | 2317133 | 38.2 <sub>37.8–38.2</sub> | 51.4 <sub>51.4–52.1</sub> | 71.7 <sub>70.6–73.5</sub> | 108 <sub>104–112</sub> | 433 <sub>323–1681</sub> | 11966 <sub>11513–18860</sub> |

**w3-cancel70 / map**: churn 0.00 levels/op; pmr upstream allocations per run: 0, 0, 0, 0, 0; clock probe per run (ticks): 29, 30, 59, 57, 61

| class | ops | p50 | p90 | p99 | p99.9 | p99.99 | max |
|---|---:|---:|---:|---:|---:|---:|---:|
| add-passive | 1467934 | 119 <sub>50.7–169</sub> | 160 <sub>64.6–214</sub> | 229 <sub>87.4–284</sub> | 386 <sub>134–538</sub> | 2822 <sub>1139–4015</sub> | 26943 <sub>12162–34440</sub> |
| cancel-hit-front | 1143558 | 89.2 <sub>38.9–129</sub> | 133 <sub>57.8–193</sub> | 200 <sub>86.3–286</sub> | 339 <sub>127–400</sub> | 2181 <sub>565–2956</sub> | 21442 <sub>16290–23727</sub> |
| cancel-hit-middle | 1143553 | 85.3 <sub>36.7–122</sub> | 132 <sub>55.7–184</sub> | 201 <sub>83.8–274</sub> | 360 <sub>129–409</sub> | 2032 <sub>615–2189</sub> | 20600 <sub>13743–553382</sub> |
| cancel-hit-back | 1145020 | 80.3 <sub>35.0–117</sub> | 113 <sub>47.8–159</sub> | 173 <sub>70.6–230</sub> | 306 <sub>103–323</sub> | 1922 <sub>500–2298</sub> | 15979 <sub>11227–22287</sub> |
| cancel-miss | 99935 | 57.4 <sub>23.9–81.0</sub> | 81.7 <sub>32.5–110</sub> | 115 <sub>47.1–155</sub> | 223 <sub>142–228</sub> | 1949 <sub>378–6849</sub> | 2230 <sub>430–10660</sub> |

**w3-cancel70 / dense**: churn 0.00 levels/op; pmr upstream allocations per run: 0, 0, 0, 0, 0; clock probe per run (ticks): 29, 29, 56, 58, 61

| class | ops | p50 | p90 | p99 | p99.9 | p99.99 | max |
|---|---:|---:|---:|---:|---:|---:|---:|
| add-passive | 1467934 | 83.8 <sub>37.1–92.0</sub> | 117 <sub>49.2–125</sub> | 168 <sub>69.6–182</sub> | 264 <sub>113–325</sub> | 878 <sub>305–2316</sub> | 18740 <sub>15338–33770</sub> |
| cancel-hit-front | 1143558 | 85.3 <sub>38.2–92.8</sub> | 129 <sub>57.4–138</sub> | 193 <sub>85.3–208</sub> | 300 <sub>123–347</sub> | 1920 <sub>354–3045</sub> | 20309 <sub>12922–31128</sub> |
| cancel-hit-middle | 1143553 | 81.0 <sub>35.7–88.8</sub> | 126 <sub>54.2–136</sub> | 192 <sub>81.7–208</sub> | 306 <sub>125–355</sub> | 2006 <sub>1206–2570</sub> | 20488 <sub>11699–25868</sub> |
| cancel-hit-back | 1145020 | 76.7 <sub>34.6–83.8</sub> | 109 <sub>47.1–117</sub> | 163 <sub>67.8–177</sub> | 254 <sub>101–303</sub> | 745 <sub>341–2016</sub> | 21032 <sub>10416–24184</sub> |
| cancel-miss | 99935 | 54.9 <sub>23.9–58.5</sub> | 77.4 <sub>31.7–82.4</sub> | 108 <sub>44.2–113</sub> | 182 <sub>61.4–211</sub> | 1412 <sub>208–2064</sub> | 2836 <sub>491–15890</sub> |

**w3-cancel90 / map**: churn 0.00 levels/op; pmr upstream allocations per run: 0, 0, 0, 0, 0; clock probe per run (ticks): 57, 59, 97, 97, 34

| class | ops | p50 | p90 | p99 | p99.9 | p99.99 | max |
|---|---:|---:|---:|---:|---:|---:|---:|
| add-passive | 489702 | 126 <sub>58.1–171</sub> | 169 <sub>74.6–218</sub> | 251 <sub>101–295</sub> | 408 <sub>171–689</sub> | 1505 <sub>317–10724</sub> | 16310 <sub>8103–25775</sub> |
| cancel-hit-front | 1471573 | 92.8 <sub>44.6–130</sub> | 138 <sub>66.4–194</sub> | 218 <sub>99.2–288</sub> | 398 <sub>148–840</sub> | 3821 <sub>332–4689</sub> | 20515 <sub>12564–30404</sub> |
| cancel-hit-middle | 1469834 | 88.8 <sub>42.1–122</sub> | 136 <sub>63.1–186</sub> | 217 <sub>95.6–279</sub> | 379 <sub>145–900</sub> | 5842 <sub>319–10548</sub> | 25404 <sub>8443–31087</sub> |
| cancel-hit-back | 1468771 | 82.4 <sub>40.3–117</sub> | 116 <sub>54.2–159</sub> | 181 <sub>78.8–231</sub> | 320 <sub>116–495</sub> | 2161 <sub>236–4055</sub> | 24929 <sub>7871–34733</sub> |
| cancel-miss | 100120 | 58.5 <sub>27.5–81.0</sub> | 83.8 <sub>37.5–110</sub> | 122 <sub>52.8–158</sub> | 202 <sub>128–427</sub> | 483 <sub>299–11592</sub> | 7764 <sub>433–17525</sub> |

**w3-cancel90 / dense**: churn 0.00 levels/op; pmr upstream allocations per run: 0, 0, 0, 0, 0; clock probe per run (ticks): 57, 61, 97, 97, 36

| class | ops | p50 | p90 | p99 | p99.9 | p99.99 | max |
|---|---:|---:|---:|---:|---:|---:|---:|
| add-passive | 489702 | 92.8 <sub>40.0–126</sub> | 126 <sub>54.9–166</sub> | 184 <sub>78.1–236</sub> | 327 <sub>126–347</sub> | 1418 <sub>391–2431</sub> | 18997 <sub>9814–31466</sub> |
| cancel-hit-front | 1471573 | 92.0 <sub>42.1–128</sub> | 137 <sub>63.1–193</sub> | 207 <sub>94.9–286</sub> | 393 <sub>140–433</sub> | 1980 <sub>389–9263</sub> | 21697 <sub>12547–23785</sub> |
| cancel-hit-middle | 1469834 | 87.4 <sub>38.9–118</sub> | 133 <sub>59.2–182</sub> | 205 <sub>90.3–271</sub> | 372 <sub>139–424</sub> | 2066 <sub>405–8333</sub> | 27168 <sub>20058–48226</sub> |
| cancel-hit-back | 1468771 | 82.4 <sub>37.8–116</sub> | 115 <sub>51.4–158</sub> | 173 <sub>74.6–227</sub> | 308 <sub>106–320</sub> | 1386 <sub>251–1699</sub> | 23028 <sub>15316–25681</sub> |
| cancel-miss | 100120 | 57.8 <sub>26.0–79.6</sub> | 81.7 <sub>35.7–108</sub> | 113 <sub>49.9–156</sub> | 207 <sub>75.3–232</sub> | 605 <sub>141–11261</sub> | 12101 <sub>935–17979</sub> |

**w4-collide / map**: churn 0.00 levels/op; pmr upstream allocations per run: 0, 0, 0, 0, 0; clock probe per run (ticks): 29, 57, 97, 54, 59

| class | ops | p50 | p90 | p99 | p99.9 | p99.99 | max |
|---|---:|---:|---:|---:|---:|---:|---:|
| add-passive | 237751 | 8487 <sub>3541–11341</sub> | 11575 <sub>10268–11632</sub> | 16324 <sub>11826–22443</sub> | 54977 <sub>20239–71917</sub> | 95525 <sub>47990–118904</sub> | 152987 <sub>111411–562666</sub> |
| cancel-hit | 237239 | 12739 <sub>5091–14075</sub> | 17169 <sub>16312–19223</sub> | 25091 <sub>17842–31301</sub> | 61415 <sub>26301–81461</sub> | 115433 <sub>66712–595068</sub> | 339076 <sub>102646–755295</sub> |
| cancel-miss | 25010 | 4189 <sub>1718–5582</sub> | 5673 <sub>5091–5710</sub> | 7300 <sub>5773–8978</sub> | 23162 <sub>9035–57580</sub> | 78968 <sub>46712–364347</sub> | 78968 <sub>46712–364347</sub> |

**w4-collide / dense**: churn 0.00 levels/op; pmr upstream allocations per run: 0, 0, 0, 0, 0; clock probe per run (ticks): 54, 59, 58, 55, 97

| class | ops | p50 | p90 | p99 | p99.9 | p99.99 | max |
|---|---:|---:|---:|---:|---:|---:|---:|
| add-passive | 237751 | 6569 <sub>6358–11267</sub> | 10245 <sub>9589–11552</sub> | 12933 <sub>11324–16164</sub> | 28653 <sub>25319–72420</sub> | 66438 <sub>33653–151963</sub> | 130210 <sub>38099–161896</sub> |
| cancel-hit | 237239 | 9651 <sub>8670–14429</sub> | 16815 <sub>14646–18093</sub> | 21678 <sub>20102–25251</sub> | 36666 <sub>34178–62054</sub> | 71232 <sub>45639–132237</sub> | 167615 <sub>102838–857289</sub> |
| cancel-miss | 25010 | 3273 <sub>3167–5630</sub> | 5162 <sub>4823–5722</sub> | 6501 <sub>5679–7831</sub> | 17237 <sub>13504–26872</sub> | 52024 <sub>22993–87407</sub> | 52024 <sub>22993–87407</sub> |

**w4-load50 / map**: churn 0.00 levels/op; pmr upstream allocations per run: 0, 0, 0, 0, 0; clock probe per run (ticks): 97, 97, 97, 56, 56

| class | ops | p50 | p90 | p99 | p99.9 | p99.99 | max |
|---|---:|---:|---:|---:|---:|---:|---:|
| add-passive | 2498829 | 279 <sub>156–289</sub> | 397 <sub>204–414</sub> | 548 <sub>285–566</sub> | 802 <sub>433–901</sub> | 17306 <sub>9258–63105</sub> | 409972 <sub>152745–627517</sub> |
| cancel-hit | 2501171 | 377 <sub>193–415</sub> | 538 <sub>295–577</sub> | 728 <sub>450–768</sub> | 3590 <sub>582–4329</sub> | 39064 <sub>18413–70137</sub> | 313531 <sub>87120–659955</sub> |

**w4-load50 / dense**: churn 0.00 levels/op; pmr upstream allocations per run: 0, 0, 0, 0, 0; clock probe per run (ticks): 97, 97, 97, 56, 56

| class | ops | p50 | p90 | p99 | p99.9 | p99.99 | max |
|---|---:|---:|---:|---:|---:|---:|---:|
| add-passive | 2498829 | 182 <sub>115–215</sub> | 289 <sub>155–321</sub> | 428 <sub>237–469</sub> | 589 <sub>391–642</sub> | 17580 <sub>1079–48356</sub> | 187166 <sub>31748–722448</sub> |
| cancel-hit | 2501171 | 326 <sub>189–383</sub> | 488 <sub>286–540</sub> | 674 <sub>436–725</sub> | 2841 <sub>561–3972</sub> | 23082 <sub>9315–58219</sub> | 358511 <sub>150844–707421</sub> |

**w5-burst / map**: churn 0.92 levels/op; pmr upstream allocations per run: 0, 0, 0, 0, 0; clock probe per run (ticks): 54, 55, 54, 54, 54

| class | ops | p50 | p90 | p99 | p99.9 | p99.99 | max |
|---|---:|---:|---:|---:|---:|---:|---:|
| add-passive | 2388416 | 135 <sub>134–136</sub> | 185 <sub>184–187</sub> | 248 <sub>247–255</sub> | 322 <sub>315–334</sub> | 5725 <sub>4797–5856</sub> | 88774 <sub>78551–107911</sub> |
| add-aggr-1 | 291615 | 106 <sub>106–107</sub> | 239 <sub>237–241</sub> | 311 <sub>310–324</sub> | 402 <sub>400–424</sub> | 5822 <sub>2234–6067</sub> | 80460 <sub>8516–129321</sub> |
| add-aggr-2..4 | 132640 | 186 <sub>185–187</sub> | 327 <sub>326–331</sub> | 448 <sub>446–462</sub> | 587 <sub>564–598</sub> | 6181 <sub>5807–22511</sub> | 15009 <sub>5970–37702</sub> |
| add-aggr-5+ | 734 | 441 <sub>432–451</sub> | 555 <sub>535–585</sub> | 665 <sub>629–898</sub> | 682 <sub>641–4522</sub> | 682 <sub>641–4522</sub> | 682 <sub>641–4522</sub> |
| cancel-hit | 2186595 | 152 <sub>151–153</sub> | 207 <sub>205–210</sub> | 273 <sub>271–288</sub> | 366 <sub>354–387</sub> | 5733 <sub>1920–6107</sub> | 135067 <sub>89526–166651</sub> |
| response | 5000000 | 253 <sub>252–254</sub> | 377 <sub>374–391</sub> | 8567 <sub>2077–32625</sub> | 79406 <sub>62602–131050</sub> | 321278 <sub>132785–436895</sub> | 424440 <sub>345533–571251</sub> |

**w5-burst / dense**: churn 0.92 levels/op; pmr upstream allocations per run: 0, 0, 0, 0, 0; clock probe per run (ticks): 56, 55, 56, 55, 56

| class | ops | p50 | p90 | p99 | p99.9 | p99.99 | max |
|---|---:|---:|---:|---:|---:|---:|---:|
| add-passive | 2388416 | 84.5 <sub>83.8–85.3</sub> | 107 <sub>106–107</sub> | 127 <sub>125–128</sub> | 179 <sub>170–193</sub> | 2303 <sub>709–5559</sub> | 33475 <sub>14267–116411</sub> |
| add-aggr-1 | 291615 | 111 <sub>110–112</sub> | 174 <sub>173–174</sub> | 213 <sub>210–213</sub> | 270 <sub>258–284</sub> | 5676 <sub>1810–6318</sub> | 39669 <sub>7387–69032</sub> |
| add-aggr-2..4 | 132640 | 151 <sub>150–153</sub> | 234 <sub>233–235</sub> | 307 <sub>305–312</sub> | 387 <sub>371–398</sub> | 5762 <sub>605–6118</sub> | 13105 <sub>11478–33177</sub> |
| add-aggr-5+ | 734 | 311 <sub>305–316</sub> | 385 <sub>375–400</sub> | 463 <sub>441–496</sub> | 500 <sub>458–6168</sub> | 500 <sub>458–6168</sub> | 500 <sub>458–6168</sub> |
| cancel-hit | 2186595 | 90.3 <sub>89.9–90.6</sub> | 114 <sub>114–115</sub> | 142 <sub>142–143</sub> | 175 <sub>172–184</sub> | 5548 <sub>343–5576</sub> | 61541 <sub>10756–94359</sub> |
| response | 5000000 | 194 <sub>193–195</sub> | 268 <sub>264–269</sub> | 2737 <sub>471–3327</sub> | 63835 <sub>37716–270137</sub> | 432511 <sub>186666–444566</sub> | 540639 <sub>384603–578620</sub> |

</details>

## Phase A3: map-ladder tail

**Question.** Where does the map ladder's tail come from? Hypothesis: node allocation + pointer
chasing over scattered nodes + rebalancing. M1 / M2 remove the first; only the dense and hybrid
ladders remove all three.

**Books.** `map` = M0 (pool over the heap, the default `OrderBook`), `map-m1` (pool over a
preallocated arena, pre-warmed), `map-m2` (preallocated single-size free list), `dense`, `hybrid`
(16,384-tick window; a second session repeats hybrid with a 4,096-tick window next to dense).
M1, M2 and hybrid are sized to the workload's peak number of levels per side + 25 % + 64.

### Environment

As in A2 (i7-13650HX, CPU 2 = P-core, Windows 11, Clang 21.1.8 `-O3`), except: **on AC power**,
still the **Balanced** power plan. Source: git `4aebaa8` (A3a). Clock probe (empty-timer p50):
51–55 ticks in almost every run of the main session, with three exceptions, each shown in the
tables: W1 map-m1 run 5 (45), W2-churn hybrid run 5 (43), and W5 run 5 for **all** books
(42–43, so W5 books stay comparable with each other). The 4,096-window session ran at 42–45
throughout, a different clock state: compare it only with the dense rows of the same session.
Every run: 0 outcome mismatches. Files: `bench/results/a3-clang-21.1.8/` and
`bench/results/a3-clang-21.1.8-hybrid4096/`.

### What the data shows

1. **Removing node allocation barely moves the map ladder** (W2-drift, all 25 runs at probe
   51–52). Passive add p50 / p99 / p99.9: M0 161 / 267 / 383 ns, M1 159 / 260 / 354, M2
   153 / 250 / 351. Cancel: M0 198 / 322 / 397, M1 197 / 324 / 396, M2 191 / 316 / 383. That is
   ≤ 7 % at p50–p99 and ≤ 10 % at p99.9. M0 fetched memory from upstream 2–3 times per run of
   1,000,000 timed ops, while a run's p99.99 is set by its slowest ~100 ops, so those events cannot
   explain the p99.99 either; the p99.99 ranges of M0, M1 and M2 overlap.
2. **The tree itself is the cost.** In the same runs the dense ladder is about 2–3× faster through
   p99.9 than M0 (1.9–2.6× for passive adds: 62 / 130 / 197 ns; 2.3–3.0× for cancels: 65 / 123 / 174 ns). W2-churn shows the same
   pattern (map p50 99–105 ns vs dense 51 ns for adds; 122–126 vs 61 ns for cancels). These data
   do not separate pointer chasing from rebalancing; both are what remains once allocation is
   gone. **Verdict on the hypothesis:** the allocation part is real (M0 does allocate) but small in
   these workloads; almost all of the map's extra latency comes from the parts only the dense /
   hybrid layouts remove.
3. **M2 is consistently the fastest map variant**, by 4–6 % at p50 in W2-churn, W2-drift and
   W5 (for example W2-drift add p50 153 vs 159 / 161 ns). A plausible reason, not tested: its
   nodes sit in one contiguous array.
4. **The hybrid ladder matches the dense one when its window covers the active band.** W2-drift
   (band ±4000 ticks, window 16,384): passive add p50 / p99 / p99.9 68 / 133 / 208 ns vs dense
   62 / 130 / 197; cancel identical (65 / 123 / 171 vs 65 / 123 / 174). Unlike the dense ladder it
   accepts any price.
5. **With a window smaller than the band it loses most of the benefit.** 4,096-tick window, same
   session as dense: W2-drift passive add p50 96 vs 50 ns, p99.9 554 vs 153 ns; cancel p90 199 vs
   72 ns. In W2-churn (band ±1000, fits the window) it stays close to dense (add p50 47 vs 42 ns).
   The window size is a per-instrument setting that has to cover where the trading happens.
6. **Hybrid aggressive adds have a p99.9 spike** in W2-drift (add-aggr-1 p99.9 1,538 ns vs dense
   345 ns). Probable cause, not instrumented: an aggressive add that rests moves the touch and
   triggers recentering, which may move up to 8 levels with all their orders in that one add.
7. **W1** (the old demo stream, 524,000 resting orders): all five books within about 10 % of each
   other (passive add p50 162–173 ns, cancel p50 284–323 ns); with a book this large the order
   index and cache misses dominate, not the ladder.
8. **W5** (open loop, Hawkes bursts): service times rank as in W2 (passive add p50 M0 129, M2 123,
   dense 73, hybrid 82 ns). Response-time percentiles above p99 vary by 10× between runs of the
   same book (for example M0 p99 676–17,796 ns), so they cannot rank the books.
9. **The far tail** (p99.99 ≈ 5 µs, max 10 µs–1 ms) is the same for every book, including dense
   (W2-drift dense p99.99 5.0 µs): in this environment it is set by the machine, not by the
   ladder.

### Recommendation (owner's decision)

- For instruments with a known, bounded active price band: **dense**, or **hybrid** with a
  window covering the band if prices are unbounded.
- Where a tree is needed: **M2** (no allocation after construction, and the fastest map variant
  measured). M0 should not be used where NFR-02 applies.
- `OrderBook` is still M0, so the default type allocates after warm-up. Making a bounded type the
  default changes the API (it needs `maxLevels`); not done without your decision.

### Results: A3 main session (Clang 21.1.8, AC, Balanced)

Latencies in ns; median over 5 runs, min–max in small print.

**w1 / map**: churn 0.08 levels/op; pmr upstream allocations per run: 0, 1, 1, 1, 0; clock probe per run (ticks): 55, 55, 53, 53, 54

| class | ops | p50 | p90 | p99 | p99.9 | p99.99 | max |
|---|---:|---:|---:|---:|---:|---:|---:|
| add-passive | 5300656 | 173 <sub>171–178</sub> | 265 <sub>257–273</sub> | 409 <sub>403–412</sub> | 520 <sub>505–536</sub> | 10045 <sub>9343–17283</sub> | 352238 <sub>139466–713920</sub> |
| add-aggr-1 | 328066 | 167 <sub>165–174</sub> | 279 <sub>268–289</sub> | 424 <sub>412–431</sub> | 590 <sub>559–605</sub> | 9846 <sub>9423–13778</sub> | 49446 <sub>18260–64890</sub> |
| add-aggr-2..4 | 363393 | 194 <sub>189–201</sub> | 357 <sub>345–364</sub> | 543 <sub>519–555</sub> | 759 <sub>709–807</sub> | 9298 <sub>5362–25022</sub> | 23182 <sub>20813–101679</sub> |
| add-aggr-5+ | 9093 | 379 <sub>368–386</sub> | 655 <sub>626–679</sub> | 926 <sub>907–961</sub> | 1397 <sub>1122–5473</sub> | 4459 <sub>1210–10700</sub> | 4459 <sub>1210–10700</sub> |
| cancel-hit | 2358547 | 323 <sub>307–340</sub> | 453 <sub>429–472</sub> | 621 <sub>602–629</sub> | 811 <sub>775–847</sub> | 18299 <sub>10422–36278</sub> | 110609 <sub>86254–129703</sub> |
| cancel-miss | 640245 | 177 <sub>174–182</sub> | 228 <sub>220–244</sub> | 392 <sub>386–397</sub> | 486 <sub>476–499</sub> | 12716 <sub>9229–32465</sub> | 51222 <sub>27947–88533</sub> |

**w1 / map-m1**: churn 0.08 levels/op; pmr upstream allocations per run: 0, 0, 0, 0, 0; clock probe per run (ticks): 54, 53, 55, 51, 45

| class | ops | p50 | p90 | p99 | p99.9 | p99.99 | max |
|---|---:|---:|---:|---:|---:|---:|---:|
| add-passive | 5300656 | 172 <sub>169–175</sub> | 264 <sub>255–268</sub> | 408 <sub>406–412</sub> | 521 <sub>513–528</sub> | 9383 <sub>8767–12317</sub> | 161339 <sub>124875–850271</sub> |
| add-aggr-1 | 328066 | 167 <sub>163–170</sub> | 276 <sub>268–281</sub> | 422 <sub>415–433</sub> | 584 <sub>561–609</sub> | 9965 <sub>5245–36027</sub> | 62943 <sub>10945–90280</sub> |
| add-aggr-2..4 | 363393 | 193 <sub>189–196</sub> | 353 <sub>343–357</sub> | 531 <sub>509–541</sub> | 747 <sub>707–787</sub> | 9383 <sub>8961–13070</sub> | 74591 <sub>66516–89940</sub> |
| add-aggr-5+ | 9093 | 367 <sub>362–377</sub> | 625 <sub>593–664</sub> | 897 <sub>864–966</sub> | 1188 <sub>1028–5742</sub> | 4354 <sub>1306–16722</sub> | 4354 <sub>1306–16722</sub> |
| cancel-hit | 2358547 | 311 <sub>286–329</sub> | 456 <sub>429–464</sub> | 621 <sub>609–629</sub> | 834 <sub>793–842</sub> | 13721 <sub>9646–22545</sub> | 208561 <sub>111886–748230</sub> |
| cancel-miss | 640245 | 171 <sub>167–176</sub> | 225 <sub>213–232</sub> | 389 <sub>387–393</sub> | 487 <sub>477–498</sub> | 9012 <sub>4643–12739</sub> | 66427 <sub>18073–84800</sub> |

**w1 / map-m2**: churn 0.08 levels/op; pmr upstream allocations per run: 0, 0, 0, 0, 0; clock probe per run (ticks): 53, 55, 52, 53, 53

| class | ops | p50 | p90 | p99 | p99.9 | p99.99 | max |
|---|---:|---:|---:|---:|---:|---:|---:|
| add-passive | 5300656 | 170 <sub>169–180</sub> | 258 <sub>254–277</sub> | 408 <sub>401–416</sub> | 515 <sub>509–537</sub> | 9503 <sub>8989–12751</sub> | 132156 <sub>81556–866958</sub> |
| add-aggr-1 | 328066 | 163 <sub>162–175</sub> | 265 <sub>261–285</sub> | 412 <sub>408–422</sub> | 581 <sub>540–622</sub> | 8915 <sub>5659–24908</sub> | 35308 <sub>19788–66358</sub> |
| add-aggr-2..4 | 363393 | 186 <sub>184–199</sub> | 339 <sub>335–359</sub> | 509 <sub>500–548</sub> | 732 <sub>713–776</sub> | 9686 <sub>5491–30593</sub> | 49020 <sub>31188–81554</sub> |
| add-aggr-5+ | 9093 | 346 <sub>345–371</sub> | 595 <sub>580–684</sub> | 903 <sub>840–965</sub> | 4506 <sub>1202–7500</sub> | 24301 <sub>1297–35462</sub> | 24301 <sub>1297–35462</sub> |
| cancel-hit | 2358547 | 299 <sub>291–347</sub> | 435 <sub>432–482</sub> | 610 <sub>608–639</sub> | 794 <sub>790–849</sub> | 16917 <sub>10040–34474</sub> | 137040 <sub>94116–476478</sub> |
| cancel-miss | 640245 | 171 <sub>168–182</sub> | 216 <sub>213–250</sub> | 388 <sub>383–395</sub> | 481 <sub>471–493</sub> | 9115 <sub>7517–12796</sub> | 76358 <sub>18867–151753</sub> |

**w1 / dense**: churn 0.08 levels/op; pmr upstream allocations per run: 0, 0, 0, 0, 0; clock probe per run (ticks): 55, 55, 55, 54, 55

| class | ops | p50 | p90 | p99 | p99.9 | p99.99 | max |
|---|---:|---:|---:|---:|---:|---:|---:|
| add-passive | 5300656 | 162 <sub>161–172</sub> | 219 <sub>216–239</sub> | 385 <sub>381–387</sub> | 471 <sub>464–484</sub> | 9195 <sub>6027–10314</sub> | 103029 <sub>93412–178165</sub> |
| add-aggr-1 | 328066 | 161 <sub>158–170</sub> | 236 <sub>232–255</sub> | 391 <sub>390–394</sub> | 492 <sub>488–529</sub> | 8618 <sub>6957–10296</sub> | 83221 <sub>24726–100897</sub> |
| add-aggr-2..4 | 363393 | 181 <sub>178–190</sub> | 315 <sub>312–334</sub> | 484 <sub>475–517</sub> | 742 <sub>719–1641</sub> | 10102 <sub>5365–12580</sub> | 41143 <sub>21613–1252620</sub> |
| add-aggr-5+ | 9093 | 322 <sub>315–341</sub> | 573 <sub>553–647</sub> | 862 <sub>828–959</sub> | 4212 <sub>1185–9806</sub> | 8668 <sub>1252–97182</sub> | 8668 <sub>1252–97182</sub> |
| cancel-hit | 2358547 | 284 <sub>274–336</sub> | 412 <sub>405–454</sub> | 591 <sub>588–607</sub> | 846 <sub>779–1799</sub> | 10160 <sub>9874–13196</sub> | 153860 <sub>109290–171024</sub> |
| cancel-miss | 640245 | 167 <sub>164–179</sub> | 207 <sub>203–231</sub> | 381 <sub>378–386</sub> | 459 <sub>450–473</sub> | 9332 <sub>7979–10245</sub> | 98583 <sub>64214–136991</sub> |

**w1 / hybrid**: churn 0.08 levels/op; pmr upstream allocations per run: 0, 0, 0, 0, 0; clock probe per run (ticks): 55, 54, 55, 54, 56

| class | ops | p50 | p90 | p99 | p99.9 | p99.99 | max |
|---|---:|---:|---:|---:|---:|---:|---:|
| add-passive | 5300656 | 168 <sub>166–171</sub> | 230 <sub>228–238</sub> | 391 <sub>388–395</sub> | 491 <sub>469–498</sub> | 10177 <sub>9343–10896</sub> | 191198 <sub>100664–843958</sub> |
| add-aggr-1 | 328066 | 163 <sub>160–166</sub> | 249 <sub>244–256</sub> | 399 <sub>395–401</sub> | 519 <sub>513–525</sub> | 9417 <sub>5570–10348</sub> | 26584 <sub>16338–54421</sub> |
| add-aggr-2..4 | 363393 | 187 <sub>184–190</sub> | 330 <sub>326–338</sub> | 503 <sub>492–519</sub> | 751 <sub>693–789</sub> | 10958 <sub>9463–13356</sub> | 41034 <sub>23733–128177</sub> |
| add-aggr-5+ | 9093 | 342 <sub>339–352</sub> | 598 <sub>589–650</sub> | 894 <sub>884–936</sub> | 5787 <sub>1120–10793</sub> | 9591 <sub>1206–24227</sub> | 9591 <sub>1206–24227</sub> |
| cancel-hit | 2358547 | 294 <sub>285–319</sub> | 421 <sub>413–436</sub> | 595 <sub>587–600</sub> | 825 <sub>783–891</sub> | 12854 <sub>12420–23447</sub> | 127189 <sub>108625–178568</sub> |
| cancel-miss | 640245 | 168 <sub>164–173</sub> | 210 <sub>206–221</sub> | 382 <sub>378–386</sub> | 464 <sub>453–476</sub> | 9406 <sub>7220–14897</sub> | 91691 <sub>17342–1340660</sub> |

**w2-churn / map**: churn 1.00 levels/op; pmr upstream allocations per run: 0, 0, 0, 0, 2; clock probe per run (ticks): 51, 51, 51, 51, 51

| class | ops | p50 | p90 | p99 | p99.9 | p99.99 | max |
|---|---:|---:|---:|---:|---:|---:|---:|
| add-passive | 2366359 | 105 <sub>103–107</sub> | 141 <sub>141–144</sub> | 182 <sub>179–186</sub> | 244 <sub>230–249</sub> | 4794 <sub>2130–5628</sub> | 65642 <sub>33993–149978</sub> |
| add-aggr-1 | 299095 | 122 <sub>120–125</sub> | 184 <sub>179–189</sub> | 243 <sub>231–250</sub> | 313 <sub>286–323</sub> | 6073 <sub>4029–7951</sub> | 32130 <sub>12977–66748</sub> |
| add-aggr-2..4 | 7558 | 151 <sub>144–157</sub> | 240 <sub>233–250</sub> | 313 <sub>306–322</sub> | 505 <sub>347–1118</sub> | 512 <sub>352–14368</sub> | 512 <sub>352–14368</sub> |
| cancel-hit | 2326988 | 125 <sub>124–127</sub> | 166 <sub>163–169</sub> | 211 <sub>203–215</sub> | 269 <sub>253–273</sub> | 5159 <sub>3176–5993</sub> | 128588 <sub>41472–814284</sub> |

**w2-churn / map-m1**: churn 1.00 levels/op; pmr upstream allocations per run: 0, 0, 0, 0, 0; clock probe per run (ticks): 51, 51, 51, 51, 51

| class | ops | p50 | p90 | p99 | p99.9 | p99.99 | max |
|---|---:|---:|---:|---:|---:|---:|---:|
| add-passive | 2366359 | 104 <sub>101–106</sub> | 140 <sub>137–142</sub> | 180 <sub>175–184</sub> | 240 <sub>228–247</sub> | 3541 <sub>2460–5111</sub> | 71018 <sub>31104–81050</sub> |
| add-aggr-1 | 299095 | 120 <sub>116–124</sub> | 180 <sub>172–185</sub> | 237 <sub>224–243</sub> | 307 <sub>284–331</sub> | 5550 <sub>5108–9885</sub> | 33387 <sub>8416–74039</sub> |
| add-aggr-2..4 | 7558 | 149 <sub>140–158</sub> | 237 <sub>222–240</sub> | 305 <sub>276–310</sub> | 412 <sub>367–544</sub> | 1443 <sub>381–31895</sub> | 1443 <sub>381–31895</sub> |
| cancel-hit | 2326988 | 126 <sub>122–129</sub> | 167 <sub>160–170</sub> | 212 <sub>200–216</sub> | 268 <sub>256–271</sub> | 5117 <sub>1589–5139</sub> | 141294 <sub>85198–331879</sub> |

**w2-churn / map-m2**: churn 1.00 levels/op; pmr upstream allocations per run: 0, 0, 0, 0, 0; clock probe per run (ticks): 51, 50, 51, 51, 51

| class | ops | p50 | p90 | p99 | p99.9 | p99.99 | max |
|---|---:|---:|---:|---:|---:|---:|---:|
| add-passive | 2366359 | 99.2 <sub>98.5–101</sub> | 136 <sub>134–137</sub> | 176 <sub>172–178</sub> | 239 <sub>234–258</sub> | 2303 <sub>1465–5051</sub> | 89983 <sub>64451–98382</sub> |
| add-aggr-1 | 299095 | 113 <sub>111–117</sub> | 172 <sub>166–174</sub> | 231 <sub>215–232</sub> | 307 <sub>276–320</sub> | 6130 <sub>938–7996</sub> | 38749 <sub>5595–368105</sub> |
| add-aggr-2..4 | 7558 | 144 <sub>137–151</sub> | 225 <sub>213–228</sub> | 289 <sub>266–303</sub> | 361 <sub>329–412</sub> | 432 <sub>332–5090</sub> | 432 <sub>332–5090</sub> |
| cancel-hit | 2326988 | 122 <sub>121–124</sub> | 165 <sub>160–166</sub> | 211 <sub>203–213</sub> | 270 <sub>259–276</sub> | 5071 <sub>3952–5901</sub> | 80860 <sub>33973–148584</sub> |

**w2-churn / dense**: churn 1.00 levels/op; pmr upstream allocations per run: 0, 0, 0, 0, 0; clock probe per run (ticks): 51, 51, 51, 52, 51

| class | ops | p50 | p90 | p99 | p99.9 | p99.99 | max |
|---|---:|---:|---:|---:|---:|---:|---:|
| add-passive | 2366359 | 51.4 <sub>50.7–52.1</sub> | 67.8 <sub>63.1–69.6</sub> | 88.8 <sub>83.1–92.8</sub> | 144 <sub>139–163</sub> | 2204 <sub>524–4811</sub> | 82589 <sub>35493–746795</sub> |
| add-aggr-1 | 299095 | 78.1 <sub>75.3–80.3</sub> | 108 <sub>99.2–112</sub> | 153 <sub>133–177</sub> | 250 <sub>200–331</sub> | 5117 <sub>5054–9035</sub> | 66508 <sub>20565–79870</sub> |
| add-aggr-2..4 | 7558 | 118 <sub>113–121</sub> | 163 <sub>148–170</sub> | 215 <sub>191–224</sub> | 253 <sub>230–323</sub> | 432 <sub>374–11891</sub> | 432 <sub>374–11891</sub> |
| cancel-hit | 2326988 | 61.4 <sub>59.9–62.1</sub> | 79.6 <sub>76.7–81.0</sub> | 108 <sub>95.6–112</sub> | 143 <sub>123–146</sub> | 2119 <sub>410–4997</sub> | 80789 <sub>47836–97086</sub> |

**w2-churn / hybrid**: churn 1.00 levels/op; pmr upstream allocations per run: 0, 0, 0, 0, 0; clock probe per run (ticks): 51, 51, 51, 52, 43

| class | ops | p50 | p90 | p99 | p99.9 | p99.99 | max |
|---|---:|---:|---:|---:|---:|---:|---:|
| add-passive | 2366359 | 57.1 <sub>56.4–57.8</sub> | 72.4 <sub>69.6–75.3</sub> | 93.8 <sub>88.1–96.3</sub> | 148 <sub>124–168</sub> | 3362 <sub>1273–5011</sub> | 85110 <sub>42989–125223</sub> |
| add-aggr-1 | 299095 | 85.6 <sub>83.8–91.3</sub> | 117 <sub>111–123</sub> | 162 <sub>146–204</sub> | 253 <sub>223–375</sub> | 4828 <sub>1943–5085</sub> | 11185 <sub>7651–36601</sub> |
| add-aggr-2..4 | 7558 | 127 <sub>122–135</sub> | 172 <sub>164–179</sub> | 219 <sub>213–239</sub> | 280 <sub>249–426</sub> | 435 <sub>279–15906</sub> | 435 <sub>279–15906</sub> |
| cancel-hit | 2326988 | 61.4 <sub>57.8–63.1</sub> | 81.0 <sub>77.4–83.1</sub> | 108 <sub>99.9–114</sub> | 144 <sub>126–152</sub> | 1762 <sub>569–5037</sub> | 47997 <sub>23985–888731</sub> |

**w2-drift / map**: churn 0.89 levels/op; pmr upstream allocations per run: 2, 3, 2, 3, 2; clock probe per run (ticks): 51, 52, 52, 52, 52

| class | ops | p50 | p90 | p99 | p99.9 | p99.99 | max |
|---|---:|---:|---:|---:|---:|---:|---:|
| add-passive | 2381063 | 161 <sub>156–163</sub> | 205 <sub>202–206</sub> | 267 <sub>264–270</sub> | 383 <sub>378–396</sub> | 7260 <sub>6929–10930</sub> | 99984 <sub>59163–707435</sub> |
| add-aggr-1 | 288861 | 160 <sub>149–163</sub> | 249 <sub>242–251</sub> | 334 <sub>327–340</sub> | 448 <sub>425–472</sub> | 13641 <sub>4948–25137</sub> | 44305 <sub>27988–75457</sub> |
| add-aggr-2..4 | 12940 | 182 <sub>174–186</sub> | 325 <sub>318–337</sub> | 464 <sub>448–470</sub> | 1987 <sub>558–4035</sub> | 4243 <sub>1403–7123</sub> | 4243 <sub>1403–7123</sub> |
| add-aggr-5+ | 3 | 372 <sub>354–385</sub> | 372 <sub>354–385</sub> | 372 <sub>354–385</sub> | 372 <sub>354–385</sub> | 372 <sub>354–385</sub> | 372 <sub>354–385</sub> |
| cancel-hit | 2317133 | 198 <sub>187–199</sub> | 263 <sub>257–265</sub> | 322 <sub>320–325</sub> | 397 <sub>387–407</sub> | 7191 <sub>6683–12545</sub> | 107986 <sub>76544–1264480</sub> |

**w2-drift / map-m1**: churn 0.89 levels/op; pmr upstream allocations per run: 0, 0, 0, 0, 0; clock probe per run (ticks): 52, 52, 52, 52, 52

| class | ops | p50 | p90 | p99 | p99.9 | p99.99 | max |
|---|---:|---:|---:|---:|---:|---:|---:|
| add-passive | 2381063 | 159 <sub>156–161</sub> | 201 <sub>198–204</sub> | 260 <sub>251–261</sub> | 354 <sub>341–366</sub> | 6655 <sub>5856–7237</sub> | 86871 <sub>58910–184232</sub> |
| add-aggr-1 | 288861 | 160 <sub>151–161</sub> | 243 <sub>237–245</sub> | 323 <sub>316–325</sub> | 405 <sub>388–420</sub> | 5345 <sub>5211–7003</sub> | 47654 <sub>7187–56036</sub> |
| add-aggr-2..4 | 12940 | 176 <sub>169–179</sub> | 320 <sub>307–323</sub> | 443 <sub>429–456</sub> | 584 <sub>533–624</sub> | 5239 <sub>574–6545</sub> | 5239 <sub>574–6545</sub> |
| add-aggr-5+ | 3 | 374 <sub>270–397</sub> | 374 <sub>270–397</sub> | 374 <sub>270–397</sub> | 374 <sub>270–397</sub> | 374 <sub>270–397</sub> | 374 <sub>270–397</sub> |
| cancel-hit | 2317133 | 197 <sub>190–200</sub> | 261 <sub>258–266</sub> | 324 <sub>319–328</sub> | 396 <sub>383–398</sub> | 6392 <sub>5445–6866</sub> | 92414 <sub>83578–167418</sub> |

**w2-drift / map-m2**: churn 0.89 levels/op; pmr upstream allocations per run: 0, 0, 0, 0, 0; clock probe per run (ticks): 52, 52, 52, 52, 51

| class | ops | p50 | p90 | p99 | p99.9 | p99.99 | max |
|---|---:|---:|---:|---:|---:|---:|---:|
| add-passive | 2381063 | 153 <sub>150–156</sub> | 195 <sub>191–197</sub> | 250 <sub>243–253</sub> | 351 <sub>334–352</sub> | 6227 <sub>5242–6364</sub> | 80189 <sub>55997–98827</sub> |
| add-aggr-1 | 288861 | 148 <sub>143–153</sub> | 234 <sub>229–237</sub> | 312 <sub>303–319</sub> | 397 <sub>372–423</sub> | 5159 <sub>2062–7306</sub> | 43975 <sub>6862–82760</sub> |
| add-aggr-2..4 | 12940 | 168 <sub>161–174</sub> | 308 <sub>296–314</sub> | 426 <sub>412–443</sub> | 538 <sub>525–579</sub> | 5148 <sub>597–5234</sub> | 5148 <sub>597–5234</sub> |
| add-aggr-5+ | 3 | 327 <sub>299–364</sub> | 327 <sub>299–364</sub> | 327 <sub>299–364</sub> | 327 <sub>299–364</sub> | 327 <sub>299–364</sub> | 327 <sub>299–364</sub> |
| cancel-hit | 2317133 | 191 <sub>184–194</sub> | 256 <sub>251–258</sub> | 316 <sub>310–317</sub> | 383 <sub>375–386</sub> | 6792 <sub>5998–7026</sub> | 90408 <sub>71136–670220</sub> |

**w2-drift / dense**: churn 0.89 levels/op; pmr upstream allocations per run: 0, 0, 0, 0, 0; clock probe per run (ticks): 51, 52, 52, 52, 51

| class | ops | p50 | p90 | p99 | p99.9 | p99.99 | max |
|---|---:|---:|---:|---:|---:|---:|---:|
| add-passive | 2381063 | 62.1 <sub>57.1–62.4</sub> | 88.1 <sub>84.2–88.8</sub> | 130 <sub>119–132</sub> | 197 <sub>177–215</sub> | 5025 <sub>4837–5054</sub> | 56037 <sub>49775–93380</sub> |
| add-aggr-1 | 288861 | 88.8 <sub>83.8–89.5</sub> | 134 <sub>127–135</sub> | 211 <sub>195–215</sub> | 345 <sub>308–367</sub> | 5051 <sub>2261–6541</sub> | 44197 <sub>6626–740055</sub> |
| add-aggr-2..4 | 12940 | 137 <sub>132–139</sub> | 214 <sub>202–223</sub> | 329 <sub>308–340</sub> | 511 <sub>423–971</sub> | 5127 <sub>441–57468</sub> | 5127 <sub>441–57468</sub> |
| add-aggr-5+ | 3 | 312 <sub>282–375</sub> | 312 <sub>282–375</sub> | 312 <sub>282–375</sub> | 312 <sub>282–375</sub> | 312 <sub>282–375</sub> | 312 <sub>282–375</sub> |
| cancel-hit | 2317133 | 64.9 <sub>62.1–65.3</sub> | 88.5 <sub>85.3–89.5</sub> | 123 <sub>119–127</sub> | 174 <sub>163–177</sub> | 5005 <sub>4112–5031</sub> | 86332 <sub>37386–115127</sub> |

**w2-drift / hybrid**: churn 0.89 levels/op; pmr upstream allocations per run: 0, 0, 0, 0, 0; clock probe per run (ticks): 51, 52, 52, 51, 52

| class | ops | p50 | p90 | p99 | p99.9 | p99.99 | max |
|---|---:|---:|---:|---:|---:|---:|---:|
| add-passive | 2381063 | 67.8 <sub>64.9–70.3</sub> | 94.2 <sub>92.0–97.4</sub> | 133 <sub>129–138</sub> | 208 <sub>196–210</sub> | 5020 <sub>4834–5045</sub> | 83260 <sub>49514–126358</sub> |
| add-aggr-1 | 288861 | 97.4 <sub>96.0–101</sub> | 145 <sub>142–148</sub> | 233 <sub>215–235</sub> | 1538 <sub>1389–1582</sub> | 6056 <sub>5071–6284</sub> | 43219 <sub>8717–51434</sub> |
| add-aggr-2..4 | 12940 | 150 <sub>146–154</sub> | 234 <sub>230–248</sub> | 1366 <sub>682–1505</sub> | 2186 <sub>1886–2441</sub> | 8109 <sub>1943–34227</sub> | 8109 <sub>1943–34227</sub> |
| add-aggr-5+ | 3 | 360 <sub>292–391</sub> | 360 <sub>292–391</sub> | 360 <sub>292–391</sub> | 360 <sub>292–391</sub> | 360 <sub>292–391</sub> | 360 <sub>292–391</sub> |
| cancel-hit | 2317133 | 64.9 <sub>64.2–66.4</sub> | 89.9 <sub>88.8–92.0</sub> | 123 <sub>122–128</sub> | 171 <sub>168–179</sub> | 5002 <sub>557–5054</sub> | 78815 <sub>44136–104823</sub> |

**w5-burst / map**: churn 0.92 levels/op; pmr upstream allocations per run: 0, 0, 0, 0, 0; clock probe per run (ticks): 52, 51, 52, 52, 42

| class | ops | p50 | p90 | p99 | p99.9 | p99.99 | max |
|---|---:|---:|---:|---:|---:|---:|---:|
| add-passive | 2388416 | 129 <sub>103–130</sub> | 173 <sub>137–174</sub> | 223 <sub>179–226</sub> | 282 <sub>239–310</sub> | 5622 <sub>2100–5998</sub> | 61627 <sub>12526–98065</sub> |
| add-aggr-1 | 291615 | 101 <sub>80.3–103</sub> | 229 <sub>184–234</sub> | 312 <sub>249–315</sub> | 397 <sub>324–411</sub> | 5690 <sub>588–6900</sub> | 38635 <sub>2612–53115</sub> |
| add-aggr-2..4 | 132640 | 167 <sub>136–172</sub> | 289 <sub>234–295</sub> | 403 <sub>327–414</sub> | 530 <sub>436–548</sub> | 7106 <sub>5699–8476</sub> | 30462 <sub>6956–65972</sub> |
| add-aggr-5+ | 734 | 370 <sub>297–377</sub> | 467 <sub>373–502</sub> | 615 <sub>516–684</sub> | 650 <sub>542–5950</sub> | 650 <sub>542–5950</sub> | 650 <sub>542–5950</sub> |
| cancel-hit | 2186595 | 136 <sub>108–137</sub> | 189 <sub>152–193</sub> | 253 <sub>205–258</sub> | 329 <sub>281–373</sub> | 5673 <sub>2327–8498</sub> | 88249 <sub>13397–95524</sub> |
| response | 5000000 | 236 <sub>186–238</sub> | 347 <sub>270–363</sub> | 15216 <sub>676–17796</sub> | 78675 <sub>7825–488401</sub> | 367853 <sub>223744–1201830</sub> | 492000 <sub>375191–1367130</sub> |

**w5-burst / map-m1**: churn 0.92 levels/op; pmr upstream allocations per run: 0, 0, 0, 0, 0; clock probe per run (ticks): 52, 52, 52, 51, 42

| class | ops | p50 | p90 | p99 | p99.9 | p99.99 | max |
|---|---:|---:|---:|---:|---:|---:|---:|
| add-passive | 2388416 | 128 <sub>102–129</sub> | 172 <sub>136–173</sub> | 221 <sub>177–222</sub> | 279 <sub>234–293</sub> | 5610 <sub>5185–5625</sub> | 113450 <sub>75593–446826</sub> |
| add-aggr-1 | 291615 | 100 <sub>80.3–102</sub> | 223 <sub>179–227</sub> | 301 <sub>244–306</sub> | 382 <sub>322–409</sub> | 5659 <sub>5294–6004</sub> | 29474 <sub>7702–118100</sub> |
| add-aggr-2..4 | 132640 | 164 <sub>132–165</sub> | 284 <sub>230–287</sub> | 394 <sub>320–399</sub> | 520 <sub>435–558</sub> | 6290 <sub>5268–7288</sub> | 10899 <sub>7185–29609</sub> |
| add-aggr-5+ | 734 | 362 <sub>285–376</sub> | 454 <sub>366–473</sub> | 585 <sub>439–638</sub> | 587 <sub>532–16646</sub> | 587 <sub>532–16646</sub> | 587 <sub>532–16646</sub> |
| cancel-hit | 2186595 | 137 <sub>108–137</sub> | 191 <sub>153–193</sub> | 255 <sub>206–256</sub> | 328 <sub>283–357</sub> | 5665 <sub>5194–5836</sub> | 85777 <sub>7843–314548</sub> |
| response | 5000000 | 235 <sub>186–236</sub> | 342 <sub>267–347</sub> | 2892 <sub>713–6506</sub> | 69817 <sub>7032–123744</sub> | 284383 <sub>84794–698082</sub> | 448181 <sub>129951–832719</sub> |

**w5-burst / map-m2**: churn 0.92 levels/op; pmr upstream allocations per run: 0, 0, 0, 0, 0; clock probe per run (ticks): 52, 51, 52, 51, 43

| class | ops | p50 | p90 | p99 | p99.9 | p99.99 | max |
|---|---:|---:|---:|---:|---:|---:|---:|
| add-passive | 2388416 | 123 <sub>97.7–124</sub> | 165 <sub>132–166</sub> | 214 <sub>172–215</sub> | 268 <sub>226–276</sub> | 5619 <sub>5159–5650</sub> | 51658 <sub>16482–100477</sub> |
| add-aggr-1 | 291615 | 100 <sub>80.3–101</sub> | 210 <sub>169–211</sub> | 281 <sub>234–283</sub> | 355 <sub>316–362</sub> | 5528 <sub>5225–5662</sub> | 12530 <sub>5654–79905</sub> |
| add-aggr-2..4 | 132640 | 159 <sub>129–160</sub> | 269 <sub>218–271</sub> | 365 <sub>304–371</sub> | 469 <sub>405–473</sub> | 5625 <sub>5151–16004</sub> | 7104 <sub>5968–42096</sub> |
| add-aggr-5+ | 734 | 340 <sub>281–344</sub> | 433 <sub>346–442</sub> | 540 <sub>400–609</sub> | 576 <sub>439–654</sub> | 576 <sub>439–654</sub> | 576 <sub>439–654</sub> |
| cancel-hit | 2186595 | 134 <sub>106–134</sub> | 187 <sub>150–188</sub> | 249 <sub>203–251</sub> | 327 <sub>279–329</sub> | 5682 <sub>5217–5710</sub> | 80750 <sub>26567–102359</sub> |
| response | 5000000 | 231 <sub>182–232</sub> | 337 <sub>261–339</sub> | 2788 <sub>2173–10673</sub> | 70548 <sub>35160–224292</sub> | 310685 <sub>92968–1073240</sub> | 404313 <sub>143800–1259880</sub> |

**w5-burst / dense**: churn 0.92 levels/op; pmr upstream allocations per run: 0, 0, 0, 0, 0; clock probe per run (ticks): 53, 52, 53, 57, 43

| class | ops | p50 | p90 | p99 | p99.9 | p99.99 | max |
|---|---:|---:|---:|---:|---:|---:|---:|
| add-passive | 2388416 | 72.8 <sub>58.1–73.8</sub> | 93.1 <sub>73.8–93.1</sub> | 113 <sub>90.3–114</sub> | 163 <sub>147–178</sub> | 5499 <sub>4289–5508</sub> | 79469 <sub>12929–130320</sub> |
| add-aggr-1 | 291615 | 94.2 <sub>76.0–94.9</sub> | 147 <sub>118–148</sub> | 180 <sub>147–181</sub> | 237 <sub>232–252</sub> | 5559 <sub>5174–6107</sub> | 13088 <sub>5905–20246</sub> |
| add-aggr-2..4 | 132640 | 131 <sub>104–131</sub> | 199 <sub>159–199</sub> | 263 <sub>211–264</sub> | 350 <sub>284–368</sub> | 5608 <sub>4697–5756</sub> | 6223 <sub>5760–7029</sub> |
| add-aggr-5+ | 734 | 260 <sub>209–267</sub> | 325 <sub>260–330</sub> | 395 <sub>296–421</sub> | 405 <sub>321–6296</sub> | 405 <sub>321–6296</sub> | 405 <sub>321–6296</sub> |
| cancel-hit | 2186595 | 84.5 <sub>66.7–84.5</sub> | 106 <sub>84.9–107</sub> | 132 <sub>106–132</sub> | 162 <sub>133–164</sub> | 5502 <sub>1544–5533</sub> | 12758 <sub>6682–62660</sub> |
| response | 5000000 | 180 <sub>142–180</sub> | 246 <sub>191–248</sub> | 2117 <sub>919–3975</sub> | 60045 <sub>20000–321278</sub> | 618447 <sub>91324–869406</sub> | 806226 <sub>130537–1059760</sub> |

**w5-burst / hybrid**: churn 0.92 levels/op; pmr upstream allocations per run: 0, 0, 0, 0, 0; clock probe per run (ticks): 53, 52, 52, 52, 43

| class | ops | p50 | p90 | p99 | p99.9 | p99.99 | max |
|---|---:|---:|---:|---:|---:|---:|---:|
| add-passive | 2388416 | 82.4 <sub>65.6–82.4</sub> | 105 <sub>83.5–106</sub> | 129 <sub>104–129</sub> | 189 <sub>177–210</sub> | 5516 <sub>1865–5528</sub> | 13748 <sub>12450–75638</sub> |
| add-aggr-1 | 291615 | 99.2 <sub>80.3–101</sub> | 163 <sub>130–164</sub> | 202 <sub>166–203</sub> | 271 <sub>261–287</sub> | 5613 <sub>1820–5727</sub> | 12454 <sub>6384–31513</sub> |
| add-aggr-2..4 | 132640 | 142 <sub>114–143</sub> | 219 <sub>177–220</sub> | 289 <sub>240–289</sub> | 381 <sub>342–427</sub> | 6278 <sub>5787–13904</sub> | 7540 <sub>7292–29769</sub> |
| add-aggr-5+ | 734 | 284 <sub>231–292</sub> | 360 <sub>280–367</sub> | 426 <sub>329–443</sub> | 443 <sub>418–481</sub> | 443 <sub>418–481</sub> | 443 <sub>418–481</sub> |
| cancel-hit | 2186595 | 86.0 <sub>68.1–86.0</sub> | 109 <sub>87.4–110</sub> | 132 <sub>107–132</sub> | 168 <sub>148–180</sub> | 5510 <sub>2123–5528</sub> | 14095 <sub>9024–92937</sub> |
| response | 5000000 | 186 <sub>147–186</sub> | 255 <sub>199–256</sub> | 1741 <sub>484–7996</sub> | 28561 <sub>10816–243470</sub> | 195068 <sub>83607–1225940</sub> | 366594 <sub>122717–1377770</sub> |

### Results: hybrid with a 4,096-tick window (separate session, compare with its own dense rows)

**w1 / hybrid**: churn 0.08 levels/op; pmr upstream allocations per run: 0, 0, 0, 0, 0; clock probe per run (ticks): 45, 45, 45, 45, 45

| class | ops | p50 | p90 | p99 | p99.9 | p99.99 | max |
|---|---:|---:|---:|---:|---:|---:|---:|
| add-passive | 5300656 | 144 <sub>142–145</sub> | 196 <sub>192–198</sub> | 359 <sub>355–361</sub> | 446 <sub>438–479</sub> | 7140 <sub>5182–7340</sub> | 123529 <sub>86057–286250</sub> |
| add-aggr-1 | 328066 | 139 <sub>138–140</sub> | 210 <sub>206–211</sub> | 365 <sub>362–370</sub> | 459 <sub>453–491</sub> | 7226 <sub>5139–7591</sub> | 66990 <sub>12974–102645</sub> |
| add-aggr-2..4 | 363393 | 160 <sub>156–161</sub> | 282 <sub>276–283</sub> | 435 <sub>431–439</sub> | 638 <sub>629–878</sub> | 7574 <sub>5231–8008</sub> | 15433 <sub>12758–42350</sub> |
| add-aggr-5+ | 9093 | 286 <sub>281–287</sub> | 505 <sub>497–512</sub> | 769 <sub>721–797</sub> | 1058 <sub>946–1156</sub> | 4345 <sub>1138–5589</sub> | 4345 <sub>1138–5589</sub> |
| cancel-hit | 2358547 | 232 <sub>224–242</sub> | 354 <sub>347–359</sub> | 529 <sub>525–532</sub> | 725 <sub>713–3775</sub> | 7842 <sub>7089–8013</sub> | 81278 <sub>70710–133027</sub> |
| cancel-miss | 640245 | 144 <sub>142–146</sub> | 177 <sub>172–179</sub> | 351 <sub>344–351</sub> | 418 <sub>412–436</sub> | 7117 <sub>5159–7528</sub> | 25981 <sub>12614–120709</sub> |

**w1 / dense**: churn 0.08 levels/op; pmr upstream allocations per run: 0, 0, 0, 0, 0; clock probe per run (ticks): 45, 45, 45, 45, 45

| class | ops | p50 | p90 | p99 | p99.9 | p99.99 | max |
|---|---:|---:|---:|---:|---:|---:|---:|
| add-passive | 5300656 | 141 <sub>140–141</sub> | 189 <sub>187–190</sub> | 350 <sub>350–351</sub> | 417 <sub>415–431</sub> | 6832 <sub>5157–7123</sub> | 120377 <sub>78201–707893</sub> |
| add-aggr-1 | 328066 | 139 <sub>138–139</sub> | 201 <sub>200–203</sub> | 351 <sub>351–356</sub> | 431 <sub>425–447</sub> | 7169 <sub>7060–7323</sub> | 23618 <sub>12486–121164</sub> |
| add-aggr-2..4 | 363393 | 154 <sub>152–154</sub> | 266 <sub>262–268</sub> | 418 <sub>417–423</sub> | 607 <sub>590–621</sub> | 5296 <sub>5088–7283</sub> | 45595 <sub>12904–160774</sub> |
| add-aggr-5+ | 9093 | 260 <sub>254–261</sub> | 482 <sub>481–495</sub> | 743 <sub>722–769</sub> | 1144 <sub>905–5499</sub> | 6903 <sub>4600–7551</sub> | 6903 <sub>4600–7551</sub> |
| cancel-hit | 2358547 | 228 <sub>227–240</sub> | 344 <sub>342–351</sub> | 517 <sub>515–521</sub> | 680 <sub>657–722</sub> | 7785 <sub>7231–8550</sub> | 128472 <sub>114448–482044</sub> |
| cancel-miss | 640245 | 144 <sub>144–146</sub> | 176 <sub>175–179</sub> | 347 <sub>346–349</sub> | 410 <sub>408–415</sub> | 5068 <sub>3901–7425</sub> | 16482 <sub>8246–78665</sub> |

**w2-churn / hybrid**: churn 1.00 levels/op; pmr upstream allocations per run: 0, 0, 0, 0, 0; clock probe per run (ticks): 42, 42, 42, 42, 42

| class | ops | p50 | p90 | p99 | p99.9 | p99.99 | max |
|---|---:|---:|---:|---:|---:|---:|---:|
| add-passive | 2366359 | 47.1 <sub>47.1–47.4</sub> | 61.4 <sub>56.7–61.4</sub> | 77.4 <sub>71.7–78.1</sub> | 117 <sub>107–129</sub> | 333 <sub>299–352</sub> | 24830 <sub>12040–94310</sub> |
| add-aggr-1 | 299095 | 74.6 <sub>72.4–74.6</sub> | 97.7 <sub>91.3–98.1</sub> | 131 <sub>117–134</sub> | 200 <sub>183–209</sub> | 569 <sub>384–4808</sub> | 12867 <sub>5954–55182</sub> |
| add-aggr-2..4 | 7558 | 109 <sub>104–110</sub> | 144 <sub>134–146</sub> | 179 <sub>173–189</sub> | 209 <sub>205–255</sub> | 254 <sub>222–4904</sub> | 254 <sub>222–4904</sub> |
| cancel-hit | 2326988 | 51.4 <sub>50.7–51.4</sub> | 67.4 <sub>63.9–67.8</sub> | 91.3 <sub>81.7–92.0</sub> | 117 <sub>104–118</sub> | 238 <sub>182–301</sub> | 59174 <sub>12857–84366</sub> |

**w2-churn / dense**: churn 1.00 levels/op; pmr upstream allocations per run: 0, 0, 0, 0, 0; clock probe per run (ticks): 42, 42, 42, 42, 42

| class | ops | p50 | p90 | p99 | p99.9 | p99.99 | max |
|---|---:|---:|---:|---:|---:|---:|---:|
| add-passive | 2366359 | 42.1 <sub>41.7–42.1</sub> | 55.7 <sub>51.4–55.7</sub> | 72.4 <sub>67.4–72.4</sub> | 107 <sub>94.2–125</sub> | 284 <sub>229–369</sub> | 25714 <sub>12242–87806</sub> |
| add-aggr-1 | 299095 | 64.9 <sub>63.9–64.9</sub> | 88.5 <sub>81.7–88.8</sub> | 123 <sub>108–125</sub> | 184 <sub>166–194</sub> | 499 <sub>349–1468</sub> | 9972 <sub>4840–58454</sub> |
| add-aggr-2..4 | 7558 | 97.7 <sub>93.8–98.5</sub> | 132 <sub>122–134</sub> | 173 <sub>153–178</sub> | 222 <sub>201–339</sub> | 309 <sub>229–4835</sub> | 309 <sub>229–4835</sub> |
| cancel-hit | 2326988 | 50.7 <sub>49.9–50.7</sub> | 65.6 <sub>62.8–65.6</sub> | 90.6 <sub>81.0–91.3</sub> | 116 <sub>99.2–118</sub> | 327 <sub>186–388</sub> | 15938 <sub>13650–62931</sub> |

**w2-drift / hybrid**: churn 0.89 levels/op; pmr upstream allocations per run: 0, 0, 0, 0, 0; clock probe per run (ticks): 42, 42, 42, 42, 42

| class | ops | p50 | p90 | p99 | p99.9 | p99.99 | max |
|---|---:|---:|---:|---:|---:|---:|---:|
| add-passive | 2381063 | 96.0 <sub>95.6–96.3</sub> | 160 <sub>159–161</sub> | 208 <sub>206–210</sub> | 554 <sub>526–590</sub> | 4871 <sub>4808–4903</sub> | 57434 <sub>29467–386779</sub> |
| add-aggr-1 | 288861 | 83.8 <sub>83.5–83.8</sub> | 123 <sub>122–124</sub> | 182 <sub>182–189</sub> | 1249 <sub>1247–1277</sub> | 4885 <sub>1566–5000</sub> | 10524 <sub>5483–96942</sub> |
| add-aggr-2..4 | 12940 | 127 <sub>126–129</sub> | 195 <sub>192–203</sub> | 1245 <sub>1208–1290</sub> | 1491 <sub>1409–1561</sub> | 1709 <sub>1474–6187</sub> | 1709 <sub>1474–6187</sub> |
| add-aggr-5+ | 3 | 273 <sub>260–320</sub> | 273 <sub>260–320</sub> | 273 <sub>260–320</sub> | 273 <sub>260–320</sub> | 273 <sub>260–320</sub> | 273 <sub>260–320</sub> |
| cancel-hit | 2317133 | 79.6 <sub>79.6–79.6</sub> | 199 <sub>198–200</sub> | 257 <sub>255–259</sub> | 305 <sub>301–310</sub> | 4943 <sub>4891–4968</sub> | 74662 <sub>25565–100240</sub> |

**w2-drift / dense**: churn 0.89 levels/op; pmr upstream allocations per run: 0, 0, 0, 0, 0; clock probe per run (ticks): 42, 42, 42, 42, 42

| class | ops | p50 | p90 | p99 | p99.9 | p99.99 | max |
|---|---:|---:|---:|---:|---:|---:|---:|
| add-passive | 2381063 | 49.9 <sub>49.2–49.9</sub> | 71.0 <sub>70.3–71.3</sub> | 99.9 <sub>97.7–102</sub> | 153 <sub>151–163</sub> | 388 <sub>373–410</sub> | 62613 <sub>14582–98159</sub> |
| add-aggr-1 | 288861 | 72.4 <sub>71.7–73.1</sub> | 108 <sub>106–110</sub> | 168 <sub>162–177</sub> | 279 <sub>268–313</sub> | 4771 <sub>432–5456</sub> | 13442 <sub>7043–31953</sub> |
| add-aggr-2..4 | 12940 | 112 <sub>111–113</sub> | 172 <sub>169–176</sub> | 266 <sub>260–320</sub> | 421 <sub>396–660</sub> | 664 <sub>459–13017</sub> | 664 <sub>459–13017</sub> |
| add-aggr-5+ | 3 | 227 <sub>206–253</sub> | 227 <sub>206–253</sub> | 227 <sub>206–253</sub> | 227 <sub>206–253</sub> | 227 <sub>206–253</sub> | 227 <sub>206–253</sub> |
| cancel-hit | 2317133 | 52.8 <sub>52.8–52.8</sub> | 72.4 <sub>71.7–72.8</sub> | 101 <sub>98.8–103</sub> | 137 <sub>134–142</sub> | 1410 <sub>194–4783</sub> | 76729 <sub>13274–96924</sub> |

**w5-burst / hybrid**: churn 0.92 levels/op; pmr upstream allocations per run: 0, 0, 0, 0, 0; clock probe per run (ticks): 42, 43, 42, 43, 42

| class | ops | p50 | p90 | p99 | p99.9 | p99.99 | max |
|---|---:|---:|---:|---:|---:|---:|---:|
| add-passive | 2388416 | 65.3 <sub>64.9–65.6</sub> | 83.5 <sub>83.5–83.5</sub> | 103 <sub>102–103</sub> | 186 <sub>173–191</sub> | 4563 <sub>4181–5122</sub> | 43739 <sub>13383–99112</sub> |
| add-aggr-1 | 291615 | 79.6 <sub>78.8–81.0</sub> | 130 <sub>129–130</sub> | 163 <sub>163–166</sub> | 280 <sub>237–305</sub> | 5205 <sub>4623–5442</sub> | 11426 <sub>6091–72504</sub> |
| add-aggr-2..4 | 132640 | 113 <sub>113–114</sub> | 175 <sub>174–176</sub> | 234 <sub>231–239</sub> | 344 <sub>317–381</sub> | 5162 <sub>4532–5274</sub> | 7121 <sub>5144–11471</sub> |
| add-aggr-5+ | 734 | 229 <sub>224–233</sub> | 294 <sub>280–300</sub> | 377 <sub>346–444</sub> | 411 <sub>357–1455</sub> | 411 <sub>357–1455</sub> | 411 <sub>357–1455</sub> |
| cancel-hit | 2186595 | 68.1 <sub>68.1–68.1</sub> | 87.0 <sub>86.7–87.4</sub> | 106 <sub>106–107</sub> | 151 <sub>147–158</sub> | 5091 <sub>1501–5114</sub> | 46562 <sub>25959–148319</sub> |
| response | 5000000 | 147 <sub>146–147</sub> | 197 <sub>197–198</sub> | 3108 <sub>2735–3381</sub> | 63059 <sub>45022–70411</sub> | 341917 <sub>164840–549041</sub> | 453688 <sub>330620–745500</sub> |

**w5-burst / dense**: churn 0.92 levels/op; pmr upstream allocations per run: 0, 0, 0, 0, 0; clock probe per run (ticks): 45, 42, 44, 42, 43

| class | ops | p50 | p90 | p99 | p99.9 | p99.99 | max |
|---|---:|---:|---:|---:|---:|---:|---:|
| add-passive | 2388416 | 57.1 <sub>57.1–57.4</sub> | 73.8 <sub>73.8–73.8</sub> | 90.3 <sub>89.9–90.6</sub> | 168 <sub>127–181</sub> | 4218 <sub>1575–5094</sub> | 44467 <sub>11606–92979</sub> |
| add-aggr-1 | 291615 | 76.0 <sub>75.3–76.0</sub> | 117 <sub>117–117</sub> | 146 <sub>144–146</sub> | 225 <sub>204–237</sub> | 5094 <sub>1639–5165</sub> | 10921 <sub>5153–23692</sub> |
| add-aggr-2..4 | 132640 | 104 <sub>103–105</sub> | 158 <sub>158–160</sub> | 213 <sub>209–215</sub> | 295 <sub>273–296</sub> | 5177 <sub>1448–5359</sub> | 13469 <sub>5257–86638</sub> |
| add-aggr-5+ | 734 | 210 <sub>208–213</sub> | 266 <sub>252–270</sub> | 320 <sub>280–388</sub> | 347 <sub>320–568</sub> | 347 <sub>320–568</sub> | 347 <sub>320–568</sub> |
| cancel-hit | 2186595 | 66.7 <sub>66.7–66.7</sub> | 84.5 <sub>84.5–84.9</sub> | 106 <sub>106–106</sub> | 142 <sub>128–152</sub> | 4403 <sub>441–5128</sub> | 25377 <sub>13475–58435</sub> |
| response | 5000000 | 142 <sub>141–142</sub> | 190 <sub>188–192</sub> | 2744 <sub>395–4631</sub> | 56073 <sub>29543–200548</sub> | 309954 <sub>90685–1242740</sub> | 433418 <sub>285072–1586170</sub> |

## Phase A4: cancel-path tail

**Question.** How long is the cancel path's tail with the current order index, how much of it is
the index's probing, and what do a hardened hash index and engine-issued handles change?

**Books** (all on the dense ladder, so the ladder is the same and cheap): `dense` (current
`OrderIndex`), `dense-hardened` (`HardenedOrderIndex`, random seed, `maxProbe` 128),
`dense-direct` (`DirectOrderTable`). For `dense-direct` the harness translates workload ids to
handles **outside** the timed region; an add's timed region covers `reserveOrderId()` +
`addOrder`.

### Environment

As A3 (i7-13650HX, CPU 2 = P-core, Windows 11, Clang 21.1.8 `-O3`, AC, Balanced plan). Source:
git `303e223` plus the dense id-translation change in `bench/harness/executor.hpp` (`-dirty`).
**Plain session: every run at clock probe 42–44 ticks** (one clock state throughout, the
steadiest session so far). Instrumented session: probe 51–54 except W3-cancel90 run 5 (43).
Every run: 0 outcome mismatches, 0 ids refused by the hardened index. Files:
`bench/results/a4-clang-21.1.8/` (plain), `bench/results/a4-clang-21.1.8-instrumented/` (probe
distributions, `index_probes_*.csv`), `bench/results/a4-clang-21.1.8-sorted-lookup/` (first plain
session, see finding 6).

### Probe lengths of the current index (instrumented build, all runs merged)

Bucket floors: lengths below 64 are exact, above that powers of two. "erase-scan" counts the slots
the backward shift inspects after the hole, "erase-shift" the entries it moves.

| Workload | find p99 / p99.9 / max | insert p99 / max | erase-scan p99 / p99.9 / max | erase-shift p99 / max |
|---|---|---|---|---|
| W3-cancel70 (≈2,000 live) | 4 / 7 / 17 | 5 / 17 | 4 / 7 / 18 | 2 / 9 |
| W3-cancel90 | 4 / 7 / 20 | 5 / 20 | 4 / 7 / 24 | 2 / 10 |
| W4-load50 (131,071 live, load 1/2) | 11 / 20 / 57 | 13 / 57 | 14 / 23 / 58 | 5 / 18 |
| W4-collide (4,096 crafted ids) | ≥ 4,096 / ≥ 4,096 / 4,097 | ≥ 4,096 / 4,097 | ≥ 2,048 / ≥ 2,048 / 4,096 | ≥ 2,048 / 4,096 |

### What the data shows (plain session)

1. **The attack works on the current index and not on either replacement.** W4-collide cancel
   p50 / p99 / p99.9: current 7,072 / 7,854 / 11,701 ns; hardened 60 / 96 / 150 ns; direct
   51 / 84 / 91 ns. Every crafted id walks a 4,096-slot cluster in the current index (table
   above); with the hardened index's secret seed the same ids spread out (longest cluster 3), and
   the direct table never looks at the id's bits.
2. **Without an attacker, the hardened index costs nothing measurable.** W3 and W4-load50 cancels
   are within 5 % of the current index at every percentile up to p99.9 (for example W4-load50
   cancel p99 236 vs 239 ns).
3. **Engine-issued handles flatten the cancel distribution when the book is cache-resident.**
   W3-cancel70, front-of-queue cancel p50 / p99 / p99.9: current 55 / 121 / 164 ns, direct
   43 / 53 / 59 ns; back of queue 48 / 93 / 123 vs 37 / 49 / 54 ns. A cancel of an unknown id
   takes 19 ns at p50 and 30 ns at p99.9 (current: 35 / 86 ns). The hash index's variable probe
   length (p99 4, max 17 here) and its branches are what the direct table removes.
4. **With a large book the order itself is the cost, not the locator.** W4-load50 (131,071
   resting orders): cancel p50 119 (current) vs 93 ns (direct), but p99 236 vs 247 ns and p99.9
   386 vs 403 ns: within about 5 %, direct slightly behind at the tail. The resting order and its
   level are cache misses for every locator; the instrumented build shows the index probing only
   11 slots at p99 there.
5. **Adds get cheaper with handles too**: passive add p50 37–38 ns (direct) vs 50–80 ns (hash
   indexes, outside the attack) in W3, W4-load50 and W4-collide, because `admit` is a slot check instead of a probe
   walk and `insert` is one store.
6. **A measurement artifact, found and fixed.** In the first plain session the harness translated
   ids with a binary search over the sorted workload ids (about 20 cache lines touched per op,
   untimed, right before the timed cancel). There, W4-load50 direct cancel p99 was 40 % *above*
   the current index (411 vs 294 ns, same session). With a direct-indexed translation table (one
   cache line) the gap is 5 % (247 vs 236 ns). Only the direct book does a translation, and only
   cancels and modifies are affected, so the first session's direct cancel numbers are not used;
   the files are kept.
7. **The far tail** (p99.99, max) is again set by the machine: p99.99 of 3.6–4.9 µs in W4-load50
   for all three books.

### Recommendation (owner's decision)

- Matching thread: **engine-issued handles** (`DirectOrderTable`). O(1) worst case by
  construction, immune to chosen ids, and the flattest cancel distribution measured; client
  references are mapped at the gateway (Part B).
- Anywhere client ids must be indexed directly: `HardenedOrderIndex`, never the default
  `OrderIndex` (finding 1).
- The default locator is still `OrderIndex`; changing it is not done without your decision.

### Results: A4 plain session (Clang 21.1.8, AC, Balanced; probe 42–44)

Latencies in ns; median over 5 runs, min–max in small print.

**w3-cancel70 / dense**: churn 0.00 levels/op; pmr upstream allocations per run: 0, 0, 0, 0, 0; clock probe per run (ticks): 43, 42, 42, 43, 42

| class | ops | p50 | p90 | p99 | p99.9 | p99.99 | max |
|---|---:|---:|---:|---:|---:|---:|---:|
| add-passive | 1467934 | 49.6 <sub>49.2–49.9</sub> | 68.5 <sub>68.1–68.5</sub> | 94.9 <sub>94.5–96.0</sub> | 133 <sub>128–139</sub> | 203 <sub>187–286</sub> | 57673 <sub>6311–96545</sub> |
| cancel-hit-front | 1143558 | 54.9 <sub>54.2–54.9</sub> | 83.1 <sub>82.8–83.1</sub> | 121 <sub>121–122</sub> | 164 <sub>158–167</sub> | 291 <sub>200–321</sub> | 13726 <sub>5534–31610</sub> |
| cancel-hit-middle | 1143553 | 49.2 <sub>49.2–49.9</sub> | 77.4 <sub>77.4–78.1</sub> | 115 <sub>113–116</sub> | 159 <sub>150–163</sub> | 336 <sub>209–4797</sub> | 23295 <sub>12256–54135</sub> |
| cancel-hit-back | 1145020 | 47.8 <sub>47.8–47.8</sub> | 65.6 <sub>65.6–66.0</sub> | 92.8 <sub>92.4–93.1</sub> | 123 <sub>121–126</sub> | 210 <sub>173–274</sub> | 35176 <sub>24104–253557</sub> |
| cancel-miss | 99935 | 35.3 <sub>35.3–35.3</sub> | 47.1 <sub>46.4–47.1</sub> | 66.4 <sub>65.6–66.7</sub> | 86.3 <sub>81.0–99.9</sub> | 130 <sub>118–317</sub> | 184 <sub>140–4779</sub> |

**w3-cancel70 / dense-hardened**: churn 0.00 levels/op; pmr upstream allocations per run: 0, 0, 0, 0, 0; clock probe per run (ticks): 42, 43, 42, 42, 43

| class | ops | p50 | p90 | p99 | p99.9 | p99.99 | max |
|---|---:|---:|---:|---:|---:|---:|---:|
| add-passive | 1467934 | 50.3 <sub>50.3–50.7</sub> | 69.9 <sub>69.9–69.9</sub> | 94.9 <sub>94.2–95.6</sub> | 140 <sub>138–143</sub> | 212 <sub>198–307</sub> | 16262 <sub>5809–34434</sub> |
| cancel-hit-front | 1143558 | 55.7 <sub>55.7–56.0</sub> | 86.0 <sub>85.6–86.0</sub> | 125 <sub>125–127</sub> | 169 <sub>166–174</sub> | 276 <sub>234–4135</sub> | 59118 <sub>14015–76650</sub> |
| cancel-hit-middle | 1143553 | 51.4 <sub>50.7–51.4</sub> | 79.6 <sub>79.6–79.9</sub> | 118 <sub>117–121</sub> | 163 <sub>157–172</sub> | 312 <sub>228–4763</sub> | 14699 <sub>11760–116977</sub> |
| cancel-hit-back | 1145020 | 49.9 <sub>49.9–49.9</sub> | 68.8 <sub>68.8–68.8</sub> | 97.7 <sub>97.0–97.7</sub> | 131 <sub>130–134</sub> | 210 <sub>187–277</sub> | 59476 <sub>13548–102667</sub> |
| cancel-miss | 99935 | 34.2 <sub>34.2–34.2</sub> | 46.4 <sub>45.7–46.4</sub> | 69.2 <sub>68.8–69.6</sub> | 90.6 <sub>87.4–109</sub> | 161 <sub>115–1384</sub> | 5646 <sub>163–18414</sub> |

**w3-cancel70 / dense-direct**: churn 0.00 levels/op; pmr upstream allocations per run: 0, 0, 0, 0, 0; clock probe per run (ticks): 44, 42, 44, 42, 43

| class | ops | p50 | p90 | p99 | p99.9 | p99.99 | max |
|---|---:|---:|---:|---:|---:|---:|---:|
| add-passive | 1467934 | 37.1 <sub>36.7–37.5</sub> | 41.0 <sub>41.0–41.4</sub> | 48.5 <sub>48.5–52.1</sub> | 55.3 <sub>55.3–57.8</sub> | 65.6 <sub>63.5–72.8</sub> | 14715 <sub>6076–85876</sub> |
| cancel-hit-front | 1143558 | 42.8 <sub>42.8–42.8</sub> | 48.5 <sub>48.5–48.5</sub> | 52.8 <sub>52.8–53.5</sub> | 58.5 <sub>57.8–60.3</sub> | 200 <sub>141–454</sub> | 17639 <sub>12216–24593</sub> |
| cancel-hit-middle | 1143553 | 38.9 <sub>38.2–39.2</sub> | 45.7 <sub>45.3–46.4</sub> | 52.1 <sub>51.4–52.1</sub> | 58.9 <sub>57.8–71.0</sub> | 256 <sub>147–332</sub> | 13268 <sub>6454–65410</sub> |
| cancel-hit-back | 1145020 | 37.1 <sub>37.1–37.5</sub> | 42.8 <sub>42.8–43.2</sub> | 48.9 <sub>48.5–49.2</sub> | 53.5 <sub>53.5–54.9</sub> | 118 <sub>75.3–148</sub> | 50336 <sub>5958–65746</sub> |
| cancel-miss | 99935 | 18.9 <sub>18.9–18.9</sub> | 22.5 <sub>22.5–23.2</sub> | 24.6 <sub>23.9–25.3</sub> | 29.6 <sub>28.2–32.1</sub> | 44.9 <sub>29.6–4726</sub> | 79.6 <sub>33.5–4802</sub> |

**w3-cancel90 / dense**: churn 0.00 levels/op; pmr upstream allocations per run: 0, 0, 0, 0, 0; clock probe per run (ticks): 43, 42, 42, 43, 42

| class | ops | p50 | p90 | p99 | p99.9 | p99.99 | max |
|---|---:|---:|---:|---:|---:|---:|---:|
| add-passive | 489702 | 49.6 <sub>49.2–49.6</sub> | 68.1 <sub>68.1–68.8</sub> | 95.6 <sub>95.2–98.1</sub> | 140 <sub>134–143</sub> | 327 <sub>234–348</sub> | 10134 <sub>5424–14582</sub> |
| cancel-hit-front | 1471573 | 54.9 <sub>54.9–54.9</sub> | 83.1 <sub>82.8–83.1</sub> | 122 <sub>121–123</sub> | 166 <sub>165–168</sub> | 330 <sub>264–342</sub> | 54102 <sub>12977–76540</sub> |
| cancel-hit-middle | 1469834 | 49.2 <sub>48.9–49.2</sub> | 76.7 <sub>76.7–77.1</sub> | 114 <sub>114–115</sub> | 160 <sub>155–162</sub> | 357 <sub>299–392</sub> | 16664 <sub>11290–32840</sub> |
| cancel-hit-back | 1468771 | 47.8 <sub>47.1–47.8</sub> | 65.6 <sub>65.3–65.6</sub> | 92.0 <sub>91.7–92.4</sub> | 123 <sub>122–126</sub> | 205 <sub>184–212</sub> | 41762 <sub>6133–62018</sub> |
| cancel-miss | 100120 | 35.3 <sub>35.0–35.3</sub> | 46.4 <sub>46.4–47.4</sub> | 66.0 <sub>64.9–66.4</sub> | 89.5 <sub>81.7–116</sub> | 241 <sub>153–5436</sub> | 6170 <sub>238–84649</sub> |

**w3-cancel90 / dense-hardened**: churn 0.00 levels/op; pmr upstream allocations per run: 0, 0, 0, 0, 0; clock probe per run (ticks): 42, 43, 42, 42, 43

| class | ops | p50 | p90 | p99 | p99.9 | p99.99 | max |
|---|---:|---:|---:|---:|---:|---:|---:|
| add-passive | 489702 | 50.7 <sub>49.9–50.7</sub> | 70.3 <sub>69.9–70.3</sub> | 95.6 <sub>94.9–97.0</sub> | 138 <sub>129–140</sub> | 194 <sub>174–221</sub> | 6690 <sub>5599–10935</sub> |
| cancel-hit-front | 1471573 | 56.4 <sub>56.0–56.4</sub> | 86.0 <sub>86.0–86.0</sub> | 125 <sub>125–126</sub> | 168 <sub>167–170</sub> | 268 <sub>222–304</sub> | 13461 <sub>6360–97252</sub> |
| cancel-hit-middle | 1469834 | 50.7 <sub>50.7–50.7</sub> | 79.6 <sub>79.2–79.6</sub> | 117 <sub>117–118</sub> | 160 <sub>158–162</sub> | 265 <sub>237–306</sub> | 21306 <sub>10100–31330</sub> |
| cancel-hit-back | 1468771 | 49.9 <sub>49.9–49.9</sub> | 68.5 <sub>68.1–68.8</sub> | 95.6 <sub>95.6–97.7</sub> | 128 <sub>126–133</sub> | 199 <sub>179–262</sub> | 21390 <sub>6276–51297</sub> |
| cancel-miss | 100120 | 34.2 <sub>34.2–34.2</sub> | 45.7 <sub>45.7–46.4</sub> | 67.4 <sub>66.0–68.8</sub> | 88.5 <sub>87.0–89.9</sub> | 131 <sub>109–217</sub> | 5168 <sub>146–5859</sub> |

**w3-cancel90 / dense-direct**: churn 0.00 levels/op; pmr upstream allocations per run: 0, 0, 0, 0, 0; clock probe per run (ticks): 42, 42, 42, 42, 42

| class | ops | p50 | p90 | p99 | p99.9 | p99.99 | max |
|---|---:|---:|---:|---:|---:|---:|---:|
| add-passive | 489702 | 37.5 <sub>37.1–37.5</sub> | 41.4 <sub>41.0–41.4</sub> | 48.5 <sub>48.5–48.5</sub> | 56.4 <sub>55.7–56.4</sub> | 129 <sub>117–248</sub> | 5244 <sub>4990–11950</sub> |
| cancel-hit-front | 1471573 | 43.2 <sub>43.2–43.5</sub> | 49.2 <sub>49.2–49.2</sub> | 53.5 <sub>52.8–53.5</sub> | 61.4 <sub>59.9–66.0</sub> | 261 <sub>173–316</sub> | 55103 <sub>12516–89393</sub> |
| cancel-hit-middle | 1469834 | 37.8 <sub>37.8–38.9</sub> | 44.9 <sub>44.9–46.4</sub> | 51.4 <sub>51.4–52.1</sub> | 60.6 <sub>58.5–120</sub> | 242 <sub>179–320</sub> | 11888 <sub>11005–93357</sub> |
| cancel-hit-back | 1468771 | 37.5 <sub>36.7–37.5</sub> | 42.8 <sub>42.8–42.8</sub> | 48.5 <sub>48.5–49.2</sub> | 54.2 <sub>53.5–54.9</sub> | 136 <sub>108–158</sub> | 7958 <sub>5781–28399</sub> |
| cancel-miss | 100120 | 18.9 <sub>18.9–18.9</sub> | 23.2 <sub>22.8–23.2</sub> | 25.3 <sub>24.6–27.8</sub> | 29.6 <sub>28.2–31.0</sub> | 46.4 <sub>41.4–118</sub> | 176 <sub>55.3–4783</sub> |

**w4-collide / dense**: churn 0.00 levels/op; pmr upstream allocations per run: 0, 0, 0, 0, 0; clock probe per run (ticks): 42, 42, 42, 42, 42

| class | ops | p50 | p90 | p99 | p99.9 | p99.99 | max |
|---|---:|---:|---:|---:|---:|---:|---:|
| add-passive | 237751 | 6495 <sub>6495–6506</sub> | 6512 <sub>6512–6586</sub> | 6649 <sub>6604–6786</sub> | 10582 <sub>10490–15456</sub> | 66027 <sub>56803–73287</sub> | 204328 <sub>68817–593822</sub> |
| cancel-hit | 237239 | 7072 <sub>7026–7077</sub> | 7694 <sub>7694–7705</sub> | 7854 <sub>7848–7865</sub> | 11701 <sub>11495–18596</sub> | 63972 <sub>56529–68675</sub> | 98329 <sub>79367–326314</sub> |
| cancel-miss | 25010 | 3881 <sub>3881–3881</sub> | 3889 <sub>3889–3889</sub> | 3941 <sub>3901–3967</sub> | 7859 <sub>7089–12580</sub> | 35904 <sub>10352–60705</sub> | 35904 <sub>10352–60705</sub> |

**w4-collide / dense-hardened**: churn 0.00 levels/op; pmr upstream allocations per run: 0, 0, 0, 0, 0; clock probe per run (ticks): 42, 42, 42, 42, 42

| class | ops | p50 | p90 | p99 | p99.9 | p99.99 | max |
|---|---:|---:|---:|---:|---:|---:|---:|
| add-passive | 237751 | 56.7 <sub>56.4–57.1</sub> | 63.1 <sub>63.1–64.2</sub> | 126 <sub>124–149</sub> | 265 <sub>232–312</sub> | 370 <sub>348–9178</sub> | 13950 <sub>4767–58227</sub> |
| cancel-hit | 237239 | 59.9 <sub>59.9–69.6</sub> | 76.0 <sub>75.3–89.5</sub> | 96.3 <sub>95.2–134</sub> | 150 <sub>142–261</sub> | 358 <sub>296–4800</sub> | 10836 <sub>6101–105276</sub> |
| cancel-miss | 25010 | 52.8 <sub>52.8–53.5</sub> | 60.6 <sub>60.6–62.1</sub> | 120 <sub>118–147</sub> | 207 <sub>145–342</sub> | 5531 <sub>258–61234</sub> | 5531 <sub>258–61234</sub> |

**w4-collide / dense-direct**: churn 0.00 levels/op; pmr upstream allocations per run: 0, 0, 0, 0, 0; clock probe per run (ticks): 42, 42, 42, 42, 42

| class | ops | p50 | p90 | p99 | p99.9 | p99.99 | max |
|---|---:|---:|---:|---:|---:|---:|---:|
| add-passive | 237751 | 37.5 <sub>37.5–37.8</sub> | 42.8 <sub>42.5–42.8</sub> | 51.4 <sub>49.9–51.4</sub> | 58.5 <sub>57.1–59.9</sub> | 103 <sub>81.0–420</sub> | 5267 <sub>269–13418</sub> |
| cancel-hit | 237239 | 50.7 <sub>49.9–57.1</sub> | 69.9 <sub>68.8–75.3</sub> | 84.2 <sub>83.8–86.0</sub> | 91.3 <sub>90.6–92.8</sub> | 162 <sub>127–310</sub> | 6168 <sub>4803–101565</sub> |
| cancel-miss | 25010 | 22.5 <sub>22.5–22.8</sub> | 27.5 <sub>26.8–27.5</sub> | 28.9 <sub>28.5–28.9</sub> | 40.7 <sub>39.6–44.2</sub> | 54.2 <sub>46.4–5421</sub> | 54.2 <sub>46.4–5421</sub> |

**w4-load50 / dense**: churn 0.00 levels/op; pmr upstream allocations per run: 0, 0, 0, 0, 0; clock probe per run (ticks): 42, 42, 42, 42, 42

| class | ops | p50 | p90 | p99 | p99.9 | p99.99 | max |
|---|---:|---:|---:|---:|---:|---:|---:|
| add-passive | 2498829 | 79.6 <sub>79.6–80.3</sub> | 103 <sub>103–104</sub> | 139 <sub>137–143</sub> | 200 <sub>187–209</sub> | 490 <sub>407–1440</sub> | 31376 <sub>18255–131542</sub> |
| cancel-hit | 2501171 | 119 <sub>118–120</sub> | 163 <sub>158–165</sub> | 236 <sub>225–243</sub> | 386 <sub>347–394</sub> | 4943 <sub>4897–6187</sub> | 79444 <sub>64101–108290</sub> |

**w4-load50 / dense-hardened**: churn 0.00 levels/op; pmr upstream allocations per run: 0, 0, 0, 0, 0; clock probe per run (ticks): 42, 42, 42, 42, 42

| class | ops | p50 | p90 | p99 | p99.9 | p99.99 | max |
|---|---:|---:|---:|---:|---:|---:|---:|
| add-passive | 2498829 | 80.3 <sub>80.3–80.3</sub> | 105 <sub>105–106</sub> | 141 <sub>139–149</sub> | 204 <sub>199–238</sub> | 1438 <sub>456–1481</sub> | 74099 <sub>38430–908331</sub> |
| cancel-hit | 2501171 | 118 <sub>117–120</sub> | 163 <sub>161–173</sub> | 239 <sub>233–263</sub> | 383 <sub>372–415</sub> | 4928 <sub>4866–5411</sub> | 67161 <sub>32656–82010</sub> |

**w4-load50 / dense-direct**: churn 0.00 levels/op; pmr upstream allocations per run: 0, 0, 0, 0, 0; clock probe per run (ticks): 42, 42, 44, 43, 43

| class | ops | p50 | p90 | p99 | p99.9 | p99.99 | max |
|---|---:|---:|---:|---:|---:|---:|---:|
| add-passive | 2498829 | 37.8 <sub>37.5–37.8</sub> | 42.8 <sub>42.5–42.8</sub> | 53.5 <sub>53.5–53.5</sub> | 62.8 <sub>61.7–63.5</sub> | 162 <sub>151–163</sub> | 67662 <sub>17174–96018</sub> |
| cancel-hit | 2501171 | 93.1 <sub>92.4–94.2</sub> | 139 <sub>122–149</sub> | 247 <sub>232–263</sub> | 403 <sub>392–410</sub> | 3613 <sub>1083–3907</sub> | 626230 <sub>17899–969460</sub> |
