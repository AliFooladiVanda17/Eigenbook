---------------------------- MODULE EigenbookTrace ----------------------------
(***************************************************************************)
(* Trace validation (Phase B2.2).                                          *)
(*                                                                         *)
(* A trace is what the C++ book did, one event per request, recorded by    *)
(* tests/trace/trace_recorder.hpp in the specification's vocabulary and    *)
(* converted to TLA+ by eigenbook_trace_to_tla (module TraceData).         *)
(*                                                                         *)
(* TraceSpec has exactly one behaviour candidate: step l applies the       *)
(* specification's own action for event Trace[l] and requires the         *)
(* resulting state to equal the state the book reported. So:               *)
(*   - TraceIncomplete violated: every event was matched; the trace is a   *)
(*     behaviour of EigenbookMatching (accepted).                          *)
(*   - deadlock at l: event Trace[l] has no matching step; the book and    *)
(*     the specification disagree there (rejected).                        *)
(*   - any other invariant violated: the book reached a state that breaks  *)
(*     a rule of the specification.                                        *)
(***************************************************************************)
EXTENDS EigenbookMatching, TraceData

VARIABLE l      \* index of the next trace event

traceVars == <<vars, l>>

RestingIds(O) == {i \in Ids : O[i].qty > 0}

\* The specification's action for event e. For a modify, the specification decides between
\* reduce and re-entry, from the order's current price and open quantity.
Step(e) ==
    CASE e.kind = "addLimit"  -> AddLimit(e.id, e.side, e.price, e.qty, e.tif, e.po)
      [] e.kind = "addMarket" -> AddMarket(e.id, e.side, e.qty, e.tif)
      [] e.kind = "cancel"    -> Cancel(e.id)
      [] e.kind = "modify"    -> IF e.price = ord[e.id].price /\ e.qty <= ord[e.id].qty
                                    THEN ModifyReduce(e.id, e.qty)
                                    ELSE ModifyReenter(e.id, e.price, e.qty)

\* The state after the step equals the state the book reported: queues, resting orders, trades
\* of this request, and whether it was rejected.
MatchesBook(e) ==
    /\ q' = [s \in Sides |-> [p \in Prices |-> e.q[s][p]]]
    /\ RestingIds(ord') = {e.resting[k].id : k \in DOMAIN e.resting}
    /\ \A k \in DOMAIN e.resting :
           LET r == e.resting[k]
           IN /\ ord'[r.id].qty = r.qty
              /\ ord'[r.id].stamp = r.stamp
              /\ ord'[r.id].tif = r.tif
              /\ ord'[r.id].po = r.po
    /\ Len(last'.trades) = Len(e.trades)
    /\ \A k \in DOMAIN e.trades :
           /\ last'.trades[k].maker = e.trades[k].maker
           /\ last'.trades[k].price = e.trades[k].price
           /\ last'.trades[k].qty = e.trades[k].qty
    /\ (last'.kind = "reject") <=> (e.result = "rejected")

TraceInit == Init /\ l = 1

TraceNext ==
    /\ l <= Len(Trace)
    /\ Step(Trace[l])
    /\ MatchesBook(Trace[l])
    /\ l' = l + 1

TraceSpec == TraceInit /\ [][TraceNext]_traceVars

TraceIncomplete == l <= Len(Trace)

=============================================================================
