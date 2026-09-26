# M5 — Portfolio, certificates, container

## Portfolio

Every checked property also goes to **rIC3** (IC3/BMC/k-induction; HWMCC 2024–25 winner), run
as a subprocess on an AIGER dump of the current model, **one property at a time**. The number
of processes is capped at the core count, and each is killed as a whole process group at the
deadline.

| rIC3 answer | What QuickFV does |
|---|---|
| SAT | Parses its AIGER witness into a QuickFV trace. BMC keeps looking for a shorter one; btorsim and RTL replay certify it like any other CEX |
| UNSAT | Checks rIC3's witness circuit with **Certifaiger**: 9 obligations (reset, transition, safety, base, inductive, …), each UNSAT under kissat. Only then is the assertion **PROVEN**. An uncertified proof is never reported |

Verdict priority: `CEX` > `VACUOUS` > `PROVEN` > `POSSIBLY_VACUOUS` > `PASS_BOUNDED`. A proof of
an assertion whose trigger can never fire says nothing about the design, so vacuity wins.

## Certificates

| Claim | Certificate | Independent check | Tested against tampering |
|---|---|---|---|
| CEX | the trace | btorsim (exact property and frame); Verilator on the original RTL+SVA (`--replay`) | corrupted witness → rejected (M2) |
| no CEX to depth k, and induction steps (`--certify`) | CaDiCaL **LIDRUP** proof plus **our own** interaction log (`.icnf`: every clause, query and claimed answer) | `lidrup-check` | a deleted unique clause, an UNSAT claim for a SAT query, a changed query: each rejected with a precise message |
| PROVEN | rIC3 witness circuit | Certifaiger + aigsplit + aigtocnf + kissat | the model as its own "witness" → the Inductive obligation is SAT → rejected |

A query cut off by the time budget proves nothing, so the certificate files are cut back to just
before it (`finishCertificate`). What remains covers every answer a verdict relies on.

**Trusted base, stated plainly:** slang + Yosys elaboration, and our BTOR2 → AIG → CNF encoding
(checked by 261/261 operator tests against btorsim and by differential tests, but not formally
verified; SPEC §8 plans a Lean-verified unroller). Certifaiger checks the proof against *our*
AIGER model, so the same encoding trust applies to PROVEN.

## Results

**Time to the first verdict: full QuickFV stack vs rIC3 alone** (`tests/bench_portfolio.py`, same
model, 60 s limit, 8-bit data). QuickFV times are solver time after load, and every verdict is
certified. rIC3 times are its process wall time on the ready-made model, uncertified.

| FIFO | Variant | QuickFV | rIC3 alone |
|---|---|---|---|
| 4 | DEEP / DATA | CEX < 10 ms (sim) | 0.15 / 0.06 s |
| 4 | clean | **PROVEN** 0.94 s | UNSAT 0.33 s |
| 8 | DEEP / DATA | CEX < 10 ms | 1.63 / 0.08 s |
| 8 | clean | **PROVEN** 0.87 s | UNSAT 10.3 s |
| 16 | DEEP | CEX < 10 ms | timeout |
| 16 | DATA | CEX < 10 ms | 0.09 s |
| 16 | clean | **PROVEN** 1.0 s (fifo_0) | timeout |

**Exit criterion 1** (time to first CEX at or below the reference tools): met in every case.
Proofs also come faster from 8 entries up, because rIC3 handles one property at a time much better
than all of them together. For the smallest design, QuickFV is slower to PROVEN (0.94 vs 0.33 s):
that's the simulation slice plus certification.

**Exit criterion 2** (`--certify` passes on every PASS_BOUNDED result):

| Workload | Verdicts | Certificates |
|---|---|---|
| FIFO 4×1, rIC3 off | 3 PASS_BOUNDED | bmc: 305 UNSAT claims **verified**; step: verified |
| vacuity set | 3 PROVEN (certified), 7 VACUOUS | bmc: 1600 verified; step: 3 verified |
| FIFO 8×8 | 2 PROVEN (certified), 1 PASS_BOUNDED | bmc: 34 verified; step: verified |

**Regressions:** MCP loop 14/14 (now also accepting PROVEN where the portfolio proves), session
vs flow 46/46, SVA equivalence 68/71 with 0 failures, FIFO bugs 17/17 CEXs certified by btorsim
and RTL replay. That includes the CEXs rIC3 found, which also validates the AIGER witness
translation.

## Container (not tested here)

`docker/Dockerfile` builds OSS CAD Suite (Linux), lidrup-check, Certifaiger (static, as upstream
intends on Linux) and qfv. Build where there is internet, then `docker save` / `docker load`, or
convert to Apptainer. **No Docker is available on the development Mac, so the image has not been
built.** The first build on a Linux machine is the test.

## Lessons

1. **Test the checker, again.** A tampering test that looked like a checker failure (a deleted
   clause, still VERIFIED) was a deleted *duplicate*. Distinguishing that from a real hole meant
   re-running with a unique clause.
2. **Tools write to the current directory.** aigsplit writes files named after the obligations, so
   parallel checks collided. External tools now get their own working directory.
3. **One property at a time helps the external engine.** rIC3 proved the clean 16-deep FIFO's
   control properties in 1 s alone, but timed out on the three together.
4. **CaDiCaL talks on stdout** once proof tracing is on ("c opening file…"), which corrupted the
   NDJSON stream. The solver is now `quiet`.
5. **Proof files are big:** 18–226 MB for a 10 s run. Fine for a server, but `--certify` stays
   opt-in. Binary LIDRUP would shrink them if lidrup-check accepts it (not yet tried).
6. **A splice that deleted setup code.** A text replacement between two anchors silently removed
   the `--certify` setup that sat between them. The certification run caught it, because an empty
   certificate directory fails loudly.
