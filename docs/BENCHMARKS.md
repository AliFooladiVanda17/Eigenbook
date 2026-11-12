# Benchmarks

What is measured, how, on what, and the results. Every number here comes from
`bench/results/<compiler>/summary.csv`, written by `eigenbook_latency`; the tables are generated
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

## Results

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
