# M3 — Vacuity (T1), random simulation, traces and RTL replay

```
qfv bmc --top fifo --clock clk --reset-expr '!reset_' [--budget 600] [--sim 0.25] [--replay] \
        rtl/fifo.sv props.sv
```

One run now gives an agent, per assertion: **is the trigger reachable, is there a CEX, and is
every trace independently confirmed?**

## How a run proceeds

| Step | What | Finds |
|---|---|---|
| 0. Vacuity pre-pass | BMC + k-induction (k ≤ 1) on each assertion's trigger | Constant or near-constant triggers, in < 1 ms |
| 1. Random simulation | 64 lanes per machine word, constraint-aware; run length doubles up to 16k cycles | Deep-but-likely events (a FIFO filling up) at millions of cycles/s |
| 2. Incremental BMC | Every property, depth by depth | The shortest CEX/witness; shortens simulation traces |
| 3. k-induction | Triggers only, interleaved with BMC | Triggers unreachable for local reasons |
| 4. rIC3 (IC3), concurrent | Triggers only, a one-goal BTOR2 per trigger, as a subprocess | Triggers unreachable because of reachable-state invariants |

Each assertion with an antecedent gets a trigger goal (`qfv_trigger__<label>`, compiled from the
antecedent match under `disable iff`). Verdicts: `CEX`, `VACUOUS` (trigger proven unreachable),
`POSSIBLY_VACUOUS` (not reached, no proof), `PASS_BOUNDED`, `TRIVIALLY_TRUE`.

**Each trace comes in four formats** (`trace_<label>_len<N>.*`): JSON for agents, VCD, a BTOR2
witness, and an SV replay testbench.

**Certification, per trace:**
- `btorsim` must reach exactly the claimed property in exactly the claimed frame.
- With `--replay`, Verilator runs the testbench on the **original RTL with the original SVA**, and
  the assertion must fail. Unsupported assertions are commented out of the copy. Trigger
  witnesses replay on the compiled monitors.
- Replays run in the background, so they don't take time from the search.

## Results

**Vacuity injection** (`fifo_vacuity.sv`: 3 reachable triggers, 7 injected vacuities, 30 s budget)

| Assertion | Why the trigger can't fire | Verdict | Proof | Time |
|---|---|---|---|---|
| r1, r2, r3 | (reachable) | not vacuous | sim/BMC witness, replay-confirmed | 0.9–252 ms |
| v1_comb | `wr_ready && count==DEPTH`, contradiction through a wire | VACUOUS | k-induction k=1 | 0.7 ms |
| v2_width | 2-bit pointer `== 3'd7` | VACUOUS | constant-folded by synthesis | 0 ms |
| v3_disabled | trigger is the `disable iff` condition | VACUOUS | k-induction k=1 | 0.8 ms |
| v4_sequence | count can't drop DEPTH → 0 in one cycle | VACUOUS | k-induction k=2 | 250 ms |
| v5_invariant | count never exceeds DEPTH | VACUOUS | rIC3 IC3 | 301 ms |
| v6_pointers | wr_ptr − rd_ptr ≡ count | VACUOUS | rIC3 IC3 | 304 ms |
| v7_rose_const | `$rose(count==6)`, unreachable value | VACUOUS | rIC3 IC3 | 300 ms |

**7/7 injected vacuities are detected, and 3/3 real triggers are shown reachable** (4/4 trigger
witnesses replay-confirmed).

**FIFO bugs, with `--replay`**: 17/17 CEXs (simulation and BMC traces) are confirmed by
btorsim **and** by Verilator on the original RTL+SVA. Simulation hits first (0.0–155 ms), and
BMC shortens every trace to the minimal length (e.g. DEEP `fifo_1`: 192 → 12 cycles).

**Deep bugs** (8-bit data, JasperGold-style reset, 60 s budget):

| FIFO | M2: qfv BMC / rIC3 BMC / rIC3 portfolio | M3: qfv with simulation |
|---|---|---|
| 16-deep, DEEP bug | timeout / timeout / timeout | **CEX in 2.8 ms** (194 cycles, btorsim-confirmed) |
| 32-deep, DEEP bug | (not run) | **CEX in 90 ms** (764 cycles, btorsim-confirmed) |

## Lessons

1. **Different vacuity has different proofs.** Constant folding, 1–2 step induction and full
   invariants (IC3) each caught cases the others couldn't. The cheap ones answer in under 1 ms,
   which is the agent feedback loop the spec wants.
2. **Random simulation needs the right reset semantics.** With reset left unconstrained after cycle 0,
   random stimulus resets the design half the time and never gets deep (the 16-deep bug went
   unfound in 60 s). With JasperGold-style reset (held inactive after reset) the same bug falls in 3 ms.
   `--reset-free` stays available but should be used deliberately.
3. **Run length matters.** A random walk gets about √n deep in n cycles, so runs double up to 16k
   cycles. With fixed 256-cycle runs the 32-deep bug was never found.
4. **Simulation traces are long** (194–778 cycles) and BMC can't shorten deep ones within the
   budget. Trace shortening for simulation CEXs (drop or merge cycles, then re-check) is v2 work.
5. **Two infrastructure bugs worth remembering.** (a) `fork()` in a multithreaded process and then
   allocating in the child crashed every external prover call. `posix_spawn` fixed it. (b) Verilator
   stops at the first failing assertion unless given `+verilator+error+limit`, which made a correct
   CEX look like "another assertion failed".
6. **Kill the whole process tree.** OSS CAD Suite's `bin/rIC3` is a wrapper script around
   `libexec/rIC3`. Killing the wrapper on timeout orphaned the real prover: 14 rIC3 processes from
   the M2 benchmark ran for 1.5 h at ~60% CPU each before they were noticed. External provers
   now run in their own process group, which is killed as a whole. That's tested with a fake
   wrapper prover under a 2 s budget: nothing survives.
