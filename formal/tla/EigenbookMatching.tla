--------------------------- MODULE EigenbookMatching ---------------------------
(***************************************************************************)
(* Matching rules of one Eigenbook limit order book (Phase B2.1).          *)
(*                                                                         *)
(* Per side and price a FIFO queue of order ids; per id the resting order. *)
(* An incoming order sweeps the opposite side best price first, FIFO       *)
(* inside a price, always at the maker's price; a limit remainder rests    *)
(* (GTC), IOC / market remainders are cancelled, FOK trades in full or     *)
(* not at all, post-only never takes liquidity. Ghost variables account    *)
(* for every unit of quantity, and `last` records the last action and its  *)
(* trades so that the trade rules can be stated as state invariants.       *)
(*                                                                         *)
(* Abstractions (docs/plans/PHASE-B2.md): ids are not reused within a      *)
(* behaviour; DAY = GTC; no minimum quantity other than FOK; no limits,    *)
(* participants or halts (they reject before any mutation).                *)
(***************************************************************************)
EXTENDS Integers, Sequences, FiniteSets

CONSTANTS
    Prices,   \* limit prices (integers)
    Ids,      \* order ids
    MaxQty,   \* largest order quantity
    MaxSeq,   \* cap on the priority sequence (bounds repeated re-entries)
    Tifs      \* time-in-force values offered for limit orders, subset of {"GTC","IOC","FOK"}

ASSUME /\ Prices \subseteq Int /\ Prices # {}
       /\ Ids # {}
       /\ MaxQty \in Nat \ {0}
       /\ MaxSeq \in Nat
       /\ Tifs \subseteq {"GTC", "IOC", "FOK"}

Sides == {"BUY", "SELL"}
Opp(s) == IF s = "BUY" THEN "SELL" ELSE "BUY"
Qty == 1..MaxQty
Min(a, b) == IF a < b THEN a ELSE b
MinOf(S) == CHOOSE x \in S : \A y \in S : x <= y
MaxOf(S) == CHOOSE x \in S : \A y \in S : y <= x
Range(f) == {f[k] : k \in DOMAIN f}

\* No resting order for this id.
NoOrder == [side |-> "none", price |-> 0, qty |-> 0, stamp |-> 0, tif |-> "none", po |-> FALSE]

VARIABLES
    q,          \* q[s][p]: ids resting on side s at price p, in time priority
    ord,        \* ord[i]: the resting order i, or NoOrder
    used,       \* ids already used by an accepted add
    seq,        \* priority sequence: the stamp of the last order that rested or re-entered
    requested,  \* ghost: quantity requested for id i (add, plus every re-entry)
    filled,     \* ghost: quantity of id i filled, as taker or maker
    cancelled,  \* ghost: quantity of id i cancelled (remainders, cancels, reductions, re-entries)
    reqType,    \* ghost: "LIMIT" or "MARKET" for each used id
    volume,     \* ghost: quantity traded so far (every trade fills one maker and one taker)
    last        \* the last action and the trades it produced

vars == <<q, ord, used, seq, requested, filled, cancelled, reqType, volume, last>>

Resting(i) == ord[i].qty > 0
Levels(Q, s) == {p \in Prices : Q[s][p] # <<>>}

\* A taker on side s (market, or limit `lim`) can trade with a maker level at price p.
Crosses(s, isMkt, lim, p) ==
    \/ isMkt
    \/ s = "BUY" /\ p <= lim
    \/ s = "SELL" /\ p >= lim

\* Best maker price for a taker on side s: the lowest ask or the highest bid.
BestFor(Q, s) == IF s = "BUY" THEN MinOf(Levels(Q, "SELL")) ELSE MaxOf(Levels(Q, "BUY"))

\* A limit order on side s at p would trade on arrival.
WouldTake(s, p) == Levels(q, Opp(s)) # {} /\ Crosses(s, FALSE, p, BestFor(q, s))

RECURSIVE SumOver(_, _)
SumOver(f, S) == IF S = {} THEN 0
                 ELSE LET x == CHOOSE x \in S : TRUE IN f[x] + SumOver(f, S \ {x})

\* Quantity a taker could trade right now: read-only, as the book's FOK pre-check.
Available(s, isMkt, lim) ==
    SumOver([i \in Ids |-> IF /\ ord[i].side = Opp(s)
                              /\ Resting(i)
                              /\ Crosses(s, isMkt, lim, ord[i].price)
                           THEN ord[i].qty ELSE 0], Ids)

IndexOf(sq, x) == CHOOSE k \in 1..Len(sq) : sq[k] = x

\* Total quantity of a sequence of trades.
SumQty(tr) == SumOver([k \in DOMAIN tr |-> tr[k].qty], DOMAIN tr)

(***************************************************************************)
(* The sweep: trade quantity n of a taker on side s against the opposite  *)
(* side, one maker at a time. Returns the new queues, orders and fills,   *)
(* the unfilled remainder and the trades in execution order.              *)
(***************************************************************************)
RECURSIVE Sweep(_, _, _, _, _, _, _, _)
Sweep(Q, O, F, s, isMkt, lim, n, tr) ==
    LET done == [Q |-> Q, O |-> O, F |-> F, rem |-> n, tr |-> tr]
    IN IF n = 0 \/ Levels(Q, Opp(s)) = {} THEN done
       ELSE LET bp == BestFor(Q, s)
            IN IF ~Crosses(s, isMkt, lim, bp) THEN done
               ELSE LET os   == Opp(s)
                        m    == Head(Q[os][bp])
                        open == O[m].qty
                        t    == Min(n, open)
                        fill == [maker |-> m, price |-> bp, makerPrice |-> O[m].price,
                                 makerStamp |-> O[m].stamp, qty |-> t]
                        Q2   == IF t = open THEN [Q EXCEPT ![os][bp] = Tail(@)] ELSE Q
                        O2   == IF t = open THEN [O EXCEPT ![m] = NoOrder]
                                            ELSE [O EXCEPT ![m].qty = open - t]
                        F2   == [F EXCEPT ![m] = @ + t]
                    IN Sweep(Q2, O2, F2, s, isMkt, lim, n - t, Append(tr, fill))

(***************************************************************************)
(* Executes an incoming order i that passed validation, starting from     *)
(* queues Q, orders O, fills F and cancellations C. The order may only    *)
(* rest if it is a limit order with a resting time in force.              *)
(***************************************************************************)
Exec(Q, O, F, C, i, s, isMkt, lim, n, tif, po) ==
    LET ok    == tif # "FOK" \/ Available(s, isMkt, lim) >= n
        r     == Sweep(Q, O, F, s, isMkt, lim, IF ok THEN n ELSE 0, <<>>)
        rem   == IF ok THEN r.rem ELSE n
        rests == ~isMkt /\ tif = "GTC" /\ rem > 0
    IN [Q  |-> IF rests THEN [r.Q EXCEPT ![s][lim] = Append(@, i)] ELSE r.Q,
        O  |-> IF rests
                  THEN [r.O EXCEPT ![i] = [side |-> s, price |-> lim, qty |-> rem,
                                           stamp |-> seq + 1, tif |-> tif, po |-> po]]
                  ELSE r.O,
        F  |-> [r.F EXCEPT ![i] = @ + (n - rem)],
        C  |-> [C EXCEPT ![i] = @ + (IF rests THEN 0 ELSE rem)],
        seq |-> IF rests THEN seq + 1 ELSE seq,
        tr |-> r.tr]

LastRecord(kind, i, s, isMkt, lim, n, tif, po, tr, oldStamp, oldPos) ==
    [kind |-> kind, id |-> i, side |-> s, isMkt |-> isMkt, limit |-> lim, qty |-> n,
     tif |-> tif, po |-> po, trades |-> tr, oldStamp |-> oldStamp, oldPos |-> oldPos]

\* A rejected request changes nothing but `last`.
Reject(i, s) ==
    /\ last' = LastRecord("reject", i, s, FALSE, 0, 0, "none", FALSE, <<>>, 0, 0)
    /\ UNCHANGED <<q, ord, used, seq, requested, filled, cancelled, reqType, volume>>

Init ==
    /\ q = [s \in Sides |-> [p \in Prices |-> <<>>]]
    /\ ord = [i \in Ids |-> NoOrder]
    /\ used = {}
    /\ seq = 0
    /\ requested = [i \in Ids |-> 0]
    /\ filled = [i \in Ids |-> 0]
    /\ cancelled = [i \in Ids |-> 0]
    /\ reqType = [i \in Ids |-> "none"]
    /\ volume = 0
    /\ last = LastRecord("init", 0, "BUY", FALSE, 0, 0, "none", FALSE, <<>>, 0, 0)

\* Takes the effect of Exec as the next state of the book variables.
ApplyExec(X) ==
    /\ q' = X.Q
    /\ ord' = X.O
    /\ filled' = X.F
    /\ cancelled' = X.C
    /\ seq' = X.seq
    /\ volume' = volume + SumQty(X.tr)

AddLimit(i, s, p, n, tif, po) ==
    /\ i \notin used
    /\ seq < MaxSeq
    /\ ~(po /\ tif # "GTC")           \* post-only rests or is rejected: GTC only
    /\ IF po /\ WouldTake(s, p)
          THEN Reject(i, s)             \* POST_ONLY_WOULD_TRADE
          ELSE LET X == Exec(q, ord, filled, cancelled, i, s, FALSE, p, n, tif, po)
               IN /\ ApplyExec(X)
                  /\ used' = used \cup {i}
                  /\ requested' = [requested EXCEPT ![i] = n]
                  /\ reqType' = [reqType EXCEPT ![i] = "LIMIT"]
                  /\ last' = LastRecord("addLimit", i, s, FALSE, p, n, tif, po, X.tr, 0, 0)

AddMarket(i, s, n, tif) ==
    /\ i \notin used
    /\ LET X == Exec(q, ord, filled, cancelled, i, s, TRUE, 0, n, tif, FALSE)
       IN /\ ApplyExec(X)
          /\ used' = used \cup {i}
          /\ requested' = [requested EXCEPT ![i] = n]
          /\ reqType' = [reqType EXCEPT ![i] = "MARKET"]
          /\ last' = LastRecord("addMarket", i, s, TRUE, 0, n, tif, FALSE, X.tr, 0, 0)

Cancel(i) ==
    /\ Resting(i)
    /\ LET o == ord[i]
       IN /\ q' = [q EXCEPT ![o.side][o.price] = SelectSeq(@, LAMBDA x : x # i)]
          /\ ord' = [ord EXCEPT ![i] = NoOrder]
          /\ cancelled' = [cancelled EXCEPT ![i] = @ + o.qty]
          /\ last' = LastRecord("cancel", i, o.side, FALSE, o.price, o.qty, o.tif, o.po, <<>>,
                                o.stamp, 0)
    /\ UNCHANGED <<used, seq, requested, filled, reqType, volume>>

\* Same price, quantity not larger: in place, time priority kept.
ModifyReduce(i, n) ==
    /\ Resting(i)
    /\ n <= ord[i].qty
    /\ LET o == ord[i]
       IN /\ ord' = [ord EXCEPT ![i].qty = n]
          /\ cancelled' = [cancelled EXCEPT ![i] = @ + (o.qty - n)]
          /\ last' = LastRecord("reduce", i, o.side, FALSE, o.price, n, o.tif, o.po, <<>>,
                                o.stamp, IndexOf(q[o.side][o.price], i))
    /\ UNCHANGED <<q, used, seq, requested, filled, reqType, volume>>

\* New price or larger quantity: the order leaves the book and re-enters as a new limit order
\* with the same id, time in force and post-only flag (it may trade). All-or-nothing: a post-only
\* re-entry that would trade is rejected and the order stays as it was.
ModifyReenter(i, p, n) ==
    /\ Resting(i)
    /\ seq < MaxSeq
    /\ p # ord[i].price \/ n > ord[i].qty
    /\ LET o == ord[i]
       IN IF o.po /\ WouldTake(o.side, p)
             THEN Reject(i, o.side)
             ELSE LET Q1 == [q EXCEPT ![o.side][o.price] = SelectSeq(@, LAMBDA x : x # i)]
                      O1 == [ord EXCEPT ![i] = NoOrder]
                      C1 == [cancelled EXCEPT ![i] = @ + o.qty]
                      X  == Exec(Q1, O1, filled, C1, i, o.side, FALSE, p, n, o.tif, o.po)
                  IN /\ ApplyExec(X)
                     /\ requested' = [requested EXCEPT ![i] = @ + n]
                     /\ last' = LastRecord("reenter", i, o.side, FALSE, p, n, o.tif, o.po, X.tr,
                                           o.stamp, 0)
                     /\ UNCHANGED <<used, reqType>>

Next ==
    \/ \E i \in Ids, s \in Sides, p \in Prices, n \in Qty, tif \in Tifs, po \in BOOLEAN :
           AddLimit(i, s, p, n, tif, po)
    \/ \E i \in Ids, s \in Sides, n \in Qty, tif \in Tifs \ {"GTC"} :
           AddMarket(i, s, n, tif)
    \/ \E i \in Ids : Cancel(i)
    \/ \E i \in Ids, n \in Qty : ModifyReduce(i, n)
    \/ \E i \in Ids, p \in Prices, n \in Qty : ModifyReenter(i, p, n)

Spec == Init /\ [][Next]_vars

-----------------------------------------------------------------------------
(* Invariants *)

Fill == [maker : Ids, price : Prices, makerPrice : Prices, makerStamp : 0..MaxSeq, qty : Qty]
OrderRec == [side : Sides, price : Prices, qty : Qty, stamp : 1..MaxSeq, tif : {"GTC"},
             po : BOOLEAN] \cup {NoOrder}

TypeOK ==
    /\ q \in [Sides -> [Prices -> Seq(Ids)]]
    /\ ord \in [Ids -> OrderRec]
    /\ used \subseteq Ids
    /\ seq \in 0..MaxSeq
    /\ requested \in [Ids -> Nat]
    /\ filled \in [Ids -> Nat]
    /\ cancelled \in [Ids -> Nat]
    /\ reqType \in [Ids -> {"none", "LIMIT", "MARKET"}]
    /\ volume \in Nat
    /\ \A k \in DOMAIN last.trades : last.trades[k] \in Fill

\* Every queued id is a resting order of that side and price, queued exactly once; every
\* resting order is queued.
QueueConsistent ==
    /\ \A s \in Sides, p \in Prices :
           \A k \in 1..Len(q[s][p]) :
               LET i == q[s][p][k] IN ord[i].side = s /\ ord[i].price = p
    /\ \A i \in Ids : Resting(i) =>
           Cardinality({k \in 1..Len(q[ord[i].side][ord[i].price]) :
                            q[ord[i].side][ord[i].price][k] = i}) = 1

\* A price level that exists holds open quantity: a fully filled maker leaves immediately.
NoEmptyLevel ==
    \A s \in Sides :
        \A p \in Levels(q, s) :
            \A k \in 1..Len(q[s][p]) : Resting(q[s][p][k])

NoCross ==
    Levels(q, "BUY") # {} /\ Levels(q, "SELL") # {} =>
        MaxOf(Levels(q, "BUY")) < MinOf(Levels(q, "SELL"))

\* FR-18: every unit requested is filled, resting or cancelled.
QuantityConservation ==
    \A i \in used : requested[i] = filled[i] + ord[i].qty + cancelled[i]

\* FIFO inside a level is priority order: stamps strictly increase along every queue.
PriorityStampsIncrease ==
    \A s \in Sides, p \in Prices :
        \A k \in 1..(Len(q[s][p]) - 1) :
            ord[q[s][p][k]].stamp < ord[q[s][p][k + 1]].stamp

\* Maker j has priority over the maker of trade t, for a taker on side s.
Ahead(s, j, t) ==
    IF s = "BUY"
       THEN ord[j].price < t.price \/ (ord[j].price = t.price /\ ord[j].stamp < t.makerStamp)
       ELSE ord[j].price > t.price \/ (ord[j].price = t.price /\ ord[j].stamp < t.makerStamp)

\* No maker still resting had priority over one that traded, and the trades of one request
\* follow priority order.
PriceTimePriority ==
    /\ \A t \in Range(last.trades), j \in Ids :
           (Resting(j) /\ ord[j].side = Opp(last.side)) => ~Ahead(last.side, j, t)
    /\ \A k \in 1..(Len(last.trades) - 1) :
           LET a == last.trades[k]
               b == last.trades[k + 1]
           IN IF last.side = "BUY"
                 THEN a.price < b.price \/ (a.price = b.price /\ a.makerStamp < b.makerStamp)
                 ELSE a.price > b.price \/ (a.price = b.price /\ a.makerStamp < b.makerStamp)

MakerPrice == \A t \in Range(last.trades) : t.price = t.makerPrice

NoTradeThroughLimit ==
    ~last.isMkt => \A t \in Range(last.trades) : Crosses(last.side, FALSE, last.limit, t.price)

\* Only limit orders with a resting time in force ever rest.
MarketNeverRests == \A i \in Ids : Resting(i) => reqType[i] = "LIMIT" /\ ord[i].tif = "GTC"

\* Reduce keeps stamp and queue position; a re-entry that rests is the newest order at the back.
ModifyPriorityRule ==
    /\ last.kind = "reduce" =>
           LET o == ord[last.id]
           IN /\ o.stamp = last.oldStamp
              /\ IndexOf(q[o.side][o.price], last.id) = last.oldPos
    /\ (last.kind = "reenter" /\ Resting(last.id)) =>
           LET o == ord[last.id]
           IN /\ o.stamp = seq
              /\ q[o.side][o.price][Len(q[o.side][o.price])] = last.id

TradedQty == SumQty(last.trades)

FokAllOrNothing ==
    (last.kind \in {"addLimit", "addMarket"} /\ last.tif = "FOK") =>
        TradedQty \in {0, last.qty}

PostOnlyNeverTakes == last.po => last.trades = <<>>

\* Fills come only from trades: each unit traded fills one maker and one taker, so the fills of
\* all ids add up to twice the traded volume.
FilledMatchesTrades == SumOver(filled, Ids) = 2 * volume

=============================================================================
