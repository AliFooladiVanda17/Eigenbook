#!/usr/bin/env bash
# Trace validation (Phase B2.2): checks that what the C++ book does is a behaviour of the TLA+
# specification.
#
#  1. eigenbook_trace_gen runs seeded request streams on every book type and writes NDJSON traces.
#  2. For each trace, eigenbook_trace_to_tla writes TraceData.tla and Trace.cfg, and TLC runs
#     EigenbookTrace.tla, which follows the trace step by step with the specification's actions:
#       "Invariant TraceIncomplete is violated"  -> every event matched: ACCEPTED
#       "Deadlock reached"                         -> event l has no matching step: REJECTED at l
#       any other invariant violated               -> the book reached a state that breaks a rule
#  3. Negative controls: one trace is corrupted in five different ways (wrong trade price, a
#     dropped trade, a wrong priority stamp, two queued orders swapped, a flipped result); each
#     corrupted trace must be rejected, at the corrupted step. A checker that accepts them would
#     be vacuous.
#
# Usage: check_traces.sh --gen PATH --to-tla PATH --jar PATH [--java PATH] [--work DIR]
#                        [--seeds K] [--steps N]
# Exit status 0: every trace accepted and every corrupted trace rejected.

set -u
here="$(cd "$(dirname "$0")" && pwd)"
gen="" ; to_tla="" ; jar="" ; java="java" ; work="$here/../../build/trace-validation"
seeds=2 ; steps=200
while [ $# -gt 0 ]; do
  case "$1" in
    --gen) gen="$2"; shift 2 ;;
    --to-tla) to_tla="$2"; shift 2 ;;
    --jar) jar="$2"; shift 2 ;;
    --java) java="$2"; shift 2 ;;
    --work) work="$2"; shift 2 ;;
    --seeds) seeds="$2"; shift 2 ;;
    --steps) steps="$2"; shift 2 ;;
    *) echo "unknown option $1" >&2; exit 2 ;;
  esac
done
if [ -z "$gen" ] || [ -z "$to_tla" ] || [ -z "$jar" ]; then
  echo "usage: check_traces.sh --gen PATH --to-tla PATH --jar PATH [--java PATH] [--work DIR]" >&2
  exit 2
fi
# TLC runs inside each trace's directory, so the jar path must be absolute; Java on Windows also
# needs it in native form.
jar="$(cd "$(dirname "$jar")" && pwd)/$(basename "$jar")"
if command -v cygpath >/dev/null 2>&1; then jar="$(cygpath -w "$jar")"; fi

rm -rf "$work"
mkdir -p "$work/traces" "$work/check"
"$gen" --out "$work/traces" --seeds "$seeds" --steps "$steps" || exit 1

# Runs TLC on one converted trace directory and prints: ACCEPTED | REJECTED <step> | VIOLATED <inv>
# | ERROR.
check_dir() {
  local dir="$1"
  cp "$here/EigenbookMatching.tla" "$here/EigenbookTrace.tla" "$dir/"
  # -Xss: the specification sums over all ids with a recursive operator, and a long trace has
  # many ids; the default thread stack is too small for that recursion depth.
  (cd "$dir" && "$java" -Xss64m -XX:+UseParallelGC -cp "$jar" tlc2.TLC -workers 1 -metadir states \
       -config Trace.cfg EigenbookTrace.tla > tlc.log 2>&1)
  if grep -q "Invariant TraceIncomplete is violated" "$dir/tlc.log"; then
    echo "ACCEPTED"
  elif grep -q "Deadlock reached" "$dir/tlc.log"; then
    # The last state of the error trace is the one with no successor; its l is the event that
    # failed to match.
    echo "REJECTED $(grep -E '^/\\ l = [0-9]+' "$dir/tlc.log" | tail -1 | sed 's/[^0-9]//g')"
  elif grep -qE "Invariant [A-Za-z]+ is violated" "$dir/tlc.log"; then
    echo "VIOLATED $(grep -oE 'Invariant [A-Za-z]+ is violated' "$dir/tlc.log" | head -1 | cut -d' ' -f2)"
  else
    echo "ERROR"
  fi
}

failures=0
start=$(date +%s)
printf '%-28s %-8s %s\n' trace events result
for trace in "$work"/traces/*.ndjson; do
  name="$(basename "$trace" .ndjson)"
  dir="$work/check/$name"
  "$to_tla" "$trace" "$dir" > "$dir.convert.log" 2>&1 || { echo "$name: conversion failed"; failures=$((failures + 1)); continue; }
  events=$(($(wc -l < "$trace") - 1))
  result="$(check_dir "$dir")"
  printf '%-28s %-8s %s\n' "$name" "$events" "$result"
  [ "$result" = "ACCEPTED" ] || failures=$((failures + 1))
done
elapsed=$(( $(date +%s) - start ))
echo "traces checked in ${elapsed}s"

echo
printf '%-14s %-28s %-14s %s\n' corruption trace "changed step" result
for kind in trade-price drop-trade stamp queue-order result-flip; do
  done_kind=0
  for trace in "$work"/traces/*.ndjson; do
    name="$(basename "$trace" .ndjson)"
    dir="$work/check/mutant-$kind"
    rm -rf "$dir"
    out="$("$to_tla" "$trace" "$dir" --mutate "$kind" 2>/dev/null)" || continue
    changed="$(printf '%s\n' "$out" | sed -n 's/^mutated step \([0-9]*\)$/\1/p')"
    result="$(check_dir "$dir")"
    printf '%-14s %-28s %-14s %s\n' "$kind" "$name" "$changed" "$result"
    case "$result" in
      "REJECTED $changed") ;;                 # rejected exactly where the trace was changed
      VIOLATED*) ;;                           # a rule of the specification caught it
      *) failures=$((failures + 1)) ;;
    esac
    done_kind=1
    break
  done
  if [ "$done_kind" -eq 0 ]; then
    printf '%-14s %s\n' "$kind" "no trace to apply it to"; failures=$((failures + 1))
  fi
done

echo
echo "failures: $failures"
exit $((failures != 0))
