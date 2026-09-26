# SVA compiler equivalence suite (M1)

`run_equiv.py` checks that every monitor `qfv compile-sva` generates behaves exactly like
the original SVA. No single open-source tool implements IEEE 1800 §16 correctly for the whole
v1 subset, so three oracles are combined:

| Oracle | What it checks | Where it can't be used |
|---|---|---|
| **EBMC 6.0** (formal, bound 10) | Both directions: `assume P; assert monitor(P)` and `assume monitor(P); assert P`; plus the same verdict on each alone | Any `disable iff` (see deviation 1) |
| **Verilator 5.053** (simulation, 3000 random cycles, rst ≈5%) | Original SVA and monitor side by side; every failure must be matched within the attempt window | Ranged antecedents (deviation 3); some ranged sequences it rejects outright |
| **Golden tests** (`DIRECTED` in `run_equiv.py`) | Fixed stimulus with expected failure cycles **derived by hand from the LRM**; only the monitor (plain Verilog) is simulated | — (13 cases, covering every gap above) |

Plus: every line of `unsupported.txt` must be **rejected** with a reason.

Run: `python3 tests/sva_equiv/run_equiv.py` (about 2.5 min). Last result: **68/71 passed, 0 failed**:
- 20 cases confirmed by both EBMC and Verilator;
- 13/13 golden tests;
- 12/12 rejections.

3 random-suite cases have no automatic oracle (`disable iff` combined with forms Verilator
can't handle). Their semantics are covered by the golden tests.

## Deviations from IEEE 1800 found in the oracles

Each was traced cycle by cycle against the LRM. Each has a golden test showing the correct
behaviour, which QuickFV implements.

1. **EBMC ignores a `disable iff` that becomes true in the middle of an attempt.**
   `disable iff (rst) a |-> ##[1:3] b` with `a` at cycle 1, no `b`, and `rst` at cycle 2, 3 or 4:
   EBMC reports REFUTED. Per §16.12 the attempt is disabled (Verilator agrees), so there is no
   failure.
2. **Verilator reports a failure at the end of the longest window, not when it becomes certain.**
   `a |-> b ##[1:2] c ##1 d`, attempt at 25 with `c@26`, `!d@27`, `!c@27`: both threads are dead
   at 27. Verilator reports 28. Consequence: with `disable iff`, an `rst` at 28 makes Verilator
   drop a failure that had already happened.
3. **Verilator misses failures of ranged antecedents.** `a ##[1:2] b |=> c ##1 d` with `a@7`,
   `b@8`, `!c@9` must fail at 9. Verilator never reports it; EBMC agrees with QuickFV.

What this means for the case study: when QuickFV and another tool disagree about a property
with `disable iff` or ranged delays, **check the trace against the LRM before calling either one
wrong.** JasperGold's behaviour on these cases should be checked at work with `export-jg`
(SPEC §7.5).

## Monitor semantics in one paragraph

A delay chain is matched with shift registers. The antecedent uses one merged matcher, since
we only need to know whether *some* attempt matches now. Each consequent attempt is tracked
separately in an age pipeline, because a younger attempt must not keep an older, failing one
alive. The failure is flagged in the **earliest** cycle in which no thread of the attempt can
still match. `disable iff` clears attempt state and masks that cycle. `$past`/`$rose`/... helper
registers ignore `disable iff`, per the LRM. Details are in the header comment of
`src/sva/monitor.cpp`.
