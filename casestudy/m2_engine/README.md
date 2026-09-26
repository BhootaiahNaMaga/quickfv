# M2 — QuickFV's own engine: bit-blaster + incremental BMC

```
qfv bmc --top fifo --clock clk --reset-expr '!reset_' [--reset-free] [--budget 600] \
        rtl/fifo.sv sva/fifo_1r1w_props.sv
```

One command runs the whole flow and streams NDJSON events:

```
SV + SVA ──qfv compile-sva──► monitors ──Yosys (slang, memory_map, formal prep)──► BTOR2
   ──bit-blast──► AIG (strashed) ──incremental BMC on one CaDiCaL──► CEX + BTOR2 witness
```

| Piece | File | Notes |
|---|---|---|
| AIG | `src/model/aig.{h,cpp}` | Structural hashing and constant folding |
| BTOR2 → AIG | `src/model/btor2_load.cpp` | Every bit-vector operator; arrays rejected (memories are mapped to registers) |
| BMC | `src/engine/bmc.{h,cpp}` | Frames encoded lazily (cone of influence only) and iteratively. Each property is checked by *assuming* its bad literal, so all properties share the unrolling and learned clauses. Stopped by a time budget, not a depth |
| Simulator | `src/engine/sim.{h,cpp}` | Replays a witness on the AIG (`qfv sim`); the base for M3 random simulation |
| Reset env | `qfv_env.sv` (generated) | JasperGold-style `reset -expression`: reset in cycle 0, then held inactive (`--reset-free` leaves it unconstrained, as in M0) |

## Verification

**1. Bit-blaster vs btorsim, per operator** (`tests/bitblast/run_ops.py`): **261/261** operator ×
width combinations agree bit for bit over 25 random and corner-case vectors each. This covers
arithmetic (including division by zero), shifts and rotates, signed compares, extensions, slices,
ite, negative argument ids, and `constd`/`consth` parsing (negative, wider than 64 bits).

**2. FIFO bugs vs rIC3 on the identical BTOR2**: every shortest CEX length matches rIC3's BMC,
and **all 9 CEXs are confirmed by btorsim** (`tests/check_witness.py`). A corrupted witness is
correctly rejected.

| Bug | qfv shortest CEX | time | rIC3 BMC | btorsim |
|---|---|---|---|---|
| OVERFLOW | 7 | 1.4 ms | 7 | 3/3 confirmed |
| UNDERFLOW | 3 | 0.1 ms | 3 | 2/2 |
| DATA | 5 | 0.4 ms | 5 | 1/1 |
| DEEP | 10 | 39 ms | 10 | 3/3 |

The clean 4×1 FIFO passes to depth 30 in 1.5 s (M0: ABC bmc3 7 s, Pono 153 s via SymbiYosys).

**3. Scaling** (`tests/bench_fifo_scale.py`, 60 s limit, same BTOR2 for every engine; qfv times are solver time after load, rIC3 times are process wall time):

| FIFO | Bug | qfv BMC | rIC3 BMC | rIC3 portfolio (IC3) |
|---|---|---|---|---|
| 8×8 | DATA (shallow) | **CEX len 5, <0.01 s** | 0.03 s | 0.07 s |
| 8×8 | DEEP (len 18) | 10.2 s | 15.3 s | **1.7 s** |
| 8×8 | clean | depth 20 at 60 s | timeout | **proved, 9.8 s** |
| 16×8 | DATA (shallow) | **<0.01 s** | 0.06 s | 0.14 s |
| 16×8 | DEEP (len ≈34) | depth 19 at 60 s | timeout | timeout |
| 16×8 | clean | depth 16 at 60 s | timeout | timeout |

## Lessons

1. **Shallow bugs are where BMC shines.** Our BMC finds them in milliseconds, which is the
   pre-filter's main job. The comparison is not like for like: qfv's time is solving after load,
   while rIC3's is its whole process run on a ready-made BTOR2 (30–140 ms). qfv's full flow also
   pays ~120 ms for Yosys, which M4's persistent session removes from the per-assertion loop.
2. **Past depth ~20 on data-path properties, every BMC stalls**, rIC3's included. An 8-bit data
   integrity check against a shift-register reference model is expensive to unroll. IC3 finds the
   depth-18 bug 6× faster and *proves* the clean design. **→ Confirms the spec: race IC3 from
   t=0, and add simulation-seeded BMC (v2) for deep bugs.**
3. **A clocked assertion's `bad` in frame k reflects frame k−1's values.** Yosys registers
   the sampled condition, so the last frame's inputs in a witness never matter. SymbiYosys counts
   trace length the same way, so lengths are comparable across tools.
4. **Check the checker.** btorsim's exit code only says the property is reached at *some* frame.
   `check_witness.py` requires the exact property and frame, and is tested against a corrupted
   witness.

## Not in M2 (moved)

The spec's M2 exit criterion "adding an assertion never reloads the design" needs the SVA compiler
to emit monitors straight into the AIG and a long-lived session, since today each `qfv bmc` reruns
Yosys (~120–170 ms). It moves to **M4** together with the session daemon. See SPEC §11.
