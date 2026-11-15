#!/usr/bin/env bash
# Vacuity check for the TLA+ model: every invariant must be falsifiable.
#
# For each invariant, inject one fault into a copy of the specification that breaks exactly the
# rule the invariant states, run TLC with only that invariant, and require a violation. A model
# check that passes is only evidence if each invariant can fail; this script shows that it can.
#
# Usage: formal/tla/mutation_check.sh [path/to/tla2tools.jar]
# Writes its work files under build/tlc-mutations/. Exit status 0 = every mutant was caught.

set -u
here="$(cd "$(dirname "$0")" && pwd)"
root="$(cd "$here/../.." && pwd)"
jar="${1:-$root/build/tools/tla2tools-1.7.4.jar}"
work="$root/build/tlc-mutations"
mkdir -p "$work"

# name % invariant % text in the spec % replacement   (% never occurs in the spec)
mutants=(
  "maker-price%MakerPrice%fill == [maker |-> m, price |-> bp,%fill == [maker |-> m, price |-> IF isMkt THEN bp ELSE lim,"
  "lifo-queue%PriceTimePriority%m    == Head(Q[os][bp])%m    == Q[os][bp][Len(Q[os][bp])]"
  "no-fok-gate%FokAllOrNothing%ok    == tif # \"FOK\" \\/ Available(s, isMkt, lim) >= n%ok    == TRUE"
  "no-post-only-check%PostOnlyNeverTakes%IF po /\\ WouldTake(s, p)%IF FALSE"
  "ioc-rests%MarketNeverRests%rests == ~isMkt /\\ tif = \"GTC\" /\\ rem > 0%rests == ~isMkt /\\ rem > 0"
  "reduce-restamps%ModifyPriorityRule%ord' = [ord EXCEPT ![i].qty = n]%ord' = [ord EXCEPT ![i].qty = n, ![i].stamp = seq + 1]"
  "limit-never-matches%NoCross%IN IF ~Crosses(s, isMkt, lim, bp) THEN done%IN IF ~isMkt THEN done"
  "cancel-loses-qty%QuantityConservation%cancelled' = [cancelled EXCEPT ![i] = @ + o.qty]%cancelled' = [cancelled EXCEPT ![i] = @]"
  "trade-through%NoTradeThroughLimit%IN IF ~Crosses(s, isMkt, lim, bp) THEN done%IN IF FALSE THEN done"
  "filled-maker-stays%NoEmptyLevel%Q2   == IF t = open THEN [Q EXCEPT ![os][bp] = Tail(@)] ELSE Q%Q2   == Q"
  "filled-maker-stays-q%QueueConsistent%Q2   == IF t = open THEN [Q EXCEPT ![os][bp] = Tail(@)] ELSE Q%Q2   == Q"
  "constant-stamp%PriorityStampsIncrease%stamp |-> seq + 1, tif |-> tif, po |-> po]]%stamp |-> 1, tif |-> tif, po |-> po]]"
  "fill-without-trade%FilledMatchesTrades%r     == Sweep(Q, O, F, s, isMkt, lim, IF ok THEN n ELSE 0, <<>>)%r     == Sweep(Q, O, F, s, isMkt, lim, IF isMkt THEN n ELSE 0, <<>>)"
  "stamp-overflow%TypeOK%/\\ seq < MaxSeq\n    /\\ ~(po%/\\ seq <= MaxSeq\n    /\\ ~(po"
)

failures=0
printf '%-22s %-24s %-10s %s\n' mutant invariant result detail
for entry in "${mutants[@]}"; do
  IFS="%" read -r name inv from to <<<"$entry"
  dir="$work/$name"
  rm -rf "$dir"; mkdir -p "$dir"
  FROM="$from" TO="$to" perl -0pe '
      BEGIN { ($f, $t) = ($ENV{FROM}, $ENV{TO}); $f =~ s/\\n/\n/g; $t =~ s/\\n/\n/g; }
      $n = s/\Q$f\E/$t/; END { exit($n ? 0 : 3) }' \
      "$here/EigenbookMatching.tla" > "$dir/EigenbookMatching.tla"
  if [ $? -ne 0 ] || cmp -s "$here/EigenbookMatching.tla" "$dir/EigenbookMatching.tla"; then
    printf '%-22s %-24s %-10s %s\n' "$name" "$inv" "SETUP" "fault text not found in the spec"
    failures=$((failures + 1)); continue
  fi
  cat > "$dir/MC.cfg" <<EOF
SPECIFICATION Spec
CHECK_DEADLOCK FALSE
CONSTANTS
    Prices = {1, 2}
    Ids = {1, 2, 3}
    MaxQty = 2
    MaxSeq = 4
    Tifs = {"GTC", "IOC", "FOK"}
INVARIANT $inv
EOF
  (cd "$dir" && java -XX:+UseParallelGC -cp "$jar" tlc2.TLC -metadir states -workers 4 \
       -config MC.cfg EigenbookMatching.tla > tlc.log 2>&1)
  if grep -q "Invariant $inv is violated" "$dir/tlc.log"; then
    depth=$(grep -c '^State [0-9]*:' "$dir/tlc.log")
    printf '%-22s %-24s %-10s %s\n' "$name" "$inv" "caught" "counterexample of $depth states"
  else
    printf '%-22s %-24s %-10s %s\n' "$name" "$inv" "MISSED" "see $dir/tlc.log"
    failures=$((failures + 1))
  fi
done
echo "mutants not caught: $failures"
exit $((failures != 0))
