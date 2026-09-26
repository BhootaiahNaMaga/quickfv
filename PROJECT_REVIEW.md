# QuickFV project review

**Reviewed:** 26 September 2026  
**Revision:** `d4e806a`  
**Scope:** First-party C++ core and command interfaces, SVA lowering and both monitor backends, transition-system conversion, search and certification, session lifecycle, Tcl integration, build/setup/container configuration, test harnesses, and project/case-study documentation. Downloaded third-party implementations and generated `build/`, `tools/`, and `work/` contents were not audited as project source.

## Overall assessment

QuickFV has a promising architecture and substantial validation for a compact project. The persistent design session, separate monitor backends, incremental SAT engine, and independent witness tools are valuable design choices. The implementation is readable, with unusually useful comments explaining semantic decisions and earlier failures.

**It is not ready to serve as a trusted verdict gate.** Targeted review probes found a false `VACUOUS` verdict for an immediately failing assertion, omitted assertions in repeated module instances, and inconsistent expression semantics between session and one-shot modes. These matter more than adding engines or improving performance. Passing certificates cannot compensate for checking an incomplete or incorrectly translated property.

This review changes no implementation code. Findings below distinguish runtime reproductions from source-level findings. Severity reflects impact on the project's stated purpose, not just crash frequency.

## Validation performed

| Check | Result in this review |
| --- | --- |
| `cmake --build build -j 2` | Passed; existing build was already up to date. Not a clean dependency/bootstrap build. |
| `python3 tests/bitblast/run_ops.py` | 261/261 operator/width combinations agreed with btorsim, 25 vectors each. |
| `python3 tests/sva_equiv/run_equiv.py` | 68/71 passed; 3 unverified; 0 failed; bound 10. Only 20 cases were confirmed by both EBMC and Verilator. |
| `python3 tests/session_equiv/run_session_equiv.py` | 46/46 session/flow comparisons agreed, including replay checks. |
| `python3 tests/mcp_agent_loop.py` | 14/14 checks passed. Safe assertions in this run returned `PASS_BOUNDED`; this did not establish proof-certificate availability. |
| Tcl smoke test with bundled CAD tools on `PATH` | Loaded successfully; expected `PASS_BOUNDED`, `VACUOUS`, and `CEX` outputs observed. |
| Additional targeted probes | Reproduced findings 1–6, 8, 10, and the false-success behavior in finding 11. |

The Tcl test initially hit a sandbox temporary-file error. Outside the sandbox, running without the CAD tools on `PATH` exposed finding 11; rerunning with the correct `PATH` produced the expected outputs.

Not performed: Linux/container build, clean installation, full mutation/performance campaign rerun, JasperGold comparison, sanitizer builds, or exhaustive language conformance testing. Existing case-study numbers are historical project evidence, not independently re-established performance results from this review.

## Prioritized findings

### 1. Critical — Vacuity pre-pass can discard a real initial-state failure

**Evidence:** Runtime reproduced.  
**Location:** `src/engine/hunt.cpp:121–131`, with `tryInduction` and `settleVacuous` in the same file.

At depth 0, a reachable trigger returns SAT, but the pre-pass merely continues. At depth 1, an unreachable trigger can satisfy the induction step and be declared globally unreachable. This skips the required fact that **all earlier base cases must be UNSAT**. `settleVacuous` then marks the associated assertion done before simulation or BMC can find its failure.

Minimal RTL:

```systemverilog
module t(input clk);
  reg q = 1'b1;
  always @(posedge clk) q <= 0;
  P: assert property (@(posedge clk) q |-> 1'b0);
endmodule
```

Loaded through `serve`, followed by `check_all` with `budget_s: 0.2` and `sim_s: 0`, this returned:

```json
{"verdicts":{"P":"VACUOUS"}}
```

The trigger is true and the consequent false in the initial state. The emitted trigger result incorrectly said `UNREACHABLE`, with `k-induction, k=1`.

**Recommendation:** Track base-case outcomes per property. Never conclude unreachability after a SAT or UNKNOWN base case. Preserve/report a SAT trigger witness immediately, or remove the special pre-pass and use the main loop's base-case discipline. Add the example above and initial-only cover goals as regression tests.

### 2. High — Session mode checks only one instance of a repeated module assertion

**Evidence:** Runtime reproduced.  
**Location:** `src/sva/lower.cpp:410,468–487`; `src/session/design_session.cpp:257,338–365`.

The assertion collector deduplicates by syntax node. That is appropriate when rewriting a module's source text once, but session mode reuses the same collection to emit monitors bound to elaborated signals. Only the first instance's assertion is emitted.

```systemverilog
module child(input clk, input a);
  P: assert property (@(posedge clk) a);
endmodule
module t(input clk);
  child good(clk, 1'b1);
  child bad (clk, 1'b0);
endmodule
```

The load registered only one `P`; checking returned `PASS_BOUNDED`, missing the failing `bad.P` instance.

**Recommendation:** Separate source-rewrite collection from elaborated-instance collection. Emit a monitor for every instance in session mode and use hierarchical identifiers in results, traces, and handoff output. Test repeated modules, binds, generate blocks, and parameterized instances.

### 3. High — Out-of-range selects have different semantics in the two execution paths

**Evidence:** Runtime differential reproduction.  
**Location:** `src/sva/aig_emit.cpp:303–386`, especially the zero-initialized select results at lines 337 and 358.

The direct AIG emitter returns zero for an unmatched dynamic index or an out-of-range packed selection. The Yosys flow deliberately turns X into unconstrained values. This can suppress failures in session mode, despite the same property being accepted in both modes.

On a module with `input [1:0] v, idx`, check:

```systemverilog
idx == 2 |-> v[idx] == 0
```

Observed results:

| Path | Result |
| --- | --- |
| Session/direct AIG | `PASS_BOUNDED`, trigger reachable |
| One-shot `qfv bmc` | `CEX`, btorsim confirmed |

**Recommendation:** Define one explicit abstraction for undefined expression values. Represent out-of-range results consistently as unconstrained bits, or reject these expressions until supported. Audit X/Z constants and part-select behavior alongside this fix. Expand session equivalence tests beyond temporal operators to expression semantics.

### 4. High — Failed reload leaves old circuit state attached to new sources

**Evidence:** Runtime reproduced.  
**Location:** `src/session/design_session.cpp:171–183`; `src/session/design_session.h`, `loaded()`.

`load()` replaces `cfg` and `src` and clears entries before parsing/elaboration succeeds, but does not clear or transactionally replace the old transition system and unrollers. Early failures leave `loaded()` true.

Reproduction: load a module where `a=1`; reload a source where `a=0` plus an unrelated syntax error; then add/check assertion `a`. Reload correctly returned an elaboration error, but the subsequent assertion was accepted and returned `PASS_BOUNDED` against the old circuit. Diagnostics outside the inserted assertion are filtered during `add`, making this mixed state usable.

**Recommendation:** Build a candidate session and commit it only after a successful load. Alternatively, invalidate all session state on any load failure. Test failures at parse, elaboration, synthesis, and BTOR loading stages after a successful load.

### 5. High — Unsupported assumptions are removed while checks continue as successful

**Evidence:** Runtime reproduced in session mode; corresponding one-shot behavior confirmed in source.  
**Location:** `src/session/design_session.cpp:187–207,257–274,activeAssumptions()`; `tools_src/qfv_main.cpp:479–482`.

Session load strips source assertions and assumptions before synthesis, then only activates assumptions with status `ready`. An unsupported assumption is listed as unsupported, but load still returns `ok:true`, and later results do not require acknowledgement that the environment changed. The one-shot BMC path also hardcodes `dropUnsupported=true`.

```systemverilog
module t(input clk, input a);
  A: assume property (@(posedge clk) a[*2]);
  P: assert property (@(posedge clk) a);
endmodule
```

Observed: load succeeded with `A` unsupported; `check_all` returned `P:CEX` and `assumptions_active:0`. The original assumption excludes that behavior.

**Recommendation:** Treat unsupported assumptions as a blocking environment error by default. If an explicit opt-in permits dropping them, include that fact prominently in every result and exported handoff. Preserve setup-property diagnostics: `fileResult` and `setupResults` are currently collected but not propagated as load success criteria.

### 6. High — A failed witness checker does not prevent a final `CEX`

**Evidence:** Runtime reproduced.  
**Location:** `src/session/design_session.cpp:500–526,548–572`; `tools_src/qfv_main.cpp:601–630`.

With `QFV_BTORSIM=/nonexistent/review-btorsim`, checking a free input assertion returned `ok:true` and final verdict `CEX`, alongside `certified.btorsim:"NOT-REPRODUCED"`. Certification is an annotation, not an acceptance gate. The code also needs to consider process completion and exit status, not just a matching output substring.

This contradicts the documented promise that every reported counterexample has been independently certified. An agent consuming only `verdicts` cannot distinguish a validated failure from a candidate the checker could not confirm.

**Recommendation:** Separate candidate results from accepted results. Return an explicit verification failure/unknown state if replay is unavailable, times out, or disagrees. Preserve the candidate trace for diagnosis. Make summaries, events, and MCP error/result handling consistent with that decision.

### 7. High — Reachability proofs bypass the external certification requirement

**Evidence:** Source-level finding; no injected external-engine reproduction performed.  
**Location:** `src/engine/hunt.cpp:190–204`.

The external proof guard is `if (isAssert && !ev.certified)`. An uncertified `Holds` result for a reachability property therefore becomes `UNREACHABLE` and can settle its associated assertion as `VACUOUS`. `runRic3` explicitly returns `Holds` with `certified=false` when its proof cannot be checked.

**Recommendation:** Require certification for all externally established unreachability claims, including triggers and covers. Keep the goal open if certification fails. Test unavailable checkers, invalid witness circuits, and certification timeouts for assertions, covers, and vacuity separately.

### 8. High — Wide BTOR2 rotates ignore high shift-amount bits

**Evidence:** Runtime reproduced against btorsim.  
**Location:** `src/model/btor2_load.cpp:439–448`.

The rotate loop stops at `s < 63`. Higher bits still affect rotation modulo widths that do not divide the corresponding power of two.

Using the existing bit-blaster test helpers, a 65-bit `rol` with `a=1` and `b=2^63` produced:

```text
btorsim: b8@0
qfv:     b0@0
```

The regular suite misses this because its operator widths stop at 13; its 70-bit test checks constant parsing only.

**Recommendation:** Iterate every amount bit and update the distance modulo the operand width without a native-width overflowing shift. Add 63/64/65-bit boundaries and larger non-power-of-two widths to rotate and shift differential tests.

### 9. High — Shell command construction uses unquoted paths, including a deletion command

**Evidence:** Source-level finding; no command-injection payload executed.  
**Location:** `tools_src/qfv_main.cpp:402,437–440,515–517`; `src/engine/hunt.cpp:294–296`. Related raw path construction: `src/session/design_session.cpp:sessionYosysScript()` and `tcl/qfv.tcl:start`.

Several commands concatenate work paths, filenames, and executable paths into `system`/`popen` strings. Spaces break argument boundaries; shell metacharacters can become commands. Replay compilation includes `rm -rf ` followed by a derived, unquoted directory, making incorrect tokenization especially consequential. Exploitability depends on who supplies those paths; ordinary paths containing spaces are already a compatibility problem.

**Recommendation:** Use the existing argument-vector `runProcess` abstraction throughout. Replace shell deletion with a validated `std::filesystem` operation. Quote/escape separately for generated Yosys and Tcl syntax; avoiding a shell does not fix those parsers. Add path-with-spaces and metacharacter tests without executing injected commands.

### 10. Medium — Malformed request envelopes terminate persistent servers

**Evidence:** Runtime reproduced.  
**Location:** `tools_src/server.cpp:214,246–247`.

JSON syntax parsing is caught, but some envelope field extraction happens outside exception handling. These valid JSON inputs aborted the respective processes with return code `-6`:

```text
serve: []
mcp:   {"jsonrpc":"2.0","id":1,"method":7}
```

**Recommendation:** Validate the envelope and field types before access, catch errors around the entire request, and return a structured error while keeping the process alive. Also bound numeric budgets, input sizes, and monitor expansion before allocating resources.

### 11. Medium — Some tests can exit successfully without testing the intended behavior

**Evidence:** Tcl behavior reproduced; other harness issues found in source.  
**Location:** `tests/tcl_smoke.tcl`; `tests/session_equiv/run_session_equiv.py:flow()`; `casestudy/m6_report/design2sva.py:check()`; `CMakeLists.txt`.

Without Yosys on `PATH`, the Tcl smoke test printed `ok=0` and empty verdicts, yet exited 0 and printed that a handoff file existed. Its checks are output statements, not assertions. The session equivalence flow helper maps absence of a CEX to `PASS_BOUNDED` without requiring successful execution or a valid result. Case-study helpers also ignore some add results, and `all(...)` certification checks can be true for empty result sets.

There is no CTest registration or checked-in CI workflow tying the suites to a build.

**Recommendation:** Assert load/add/check success, expected result count, verdicts, and required artifact contents. Distinguish expected CEX exit codes from execution errors. Fail on missing results. Register a fast deterministic suite and a slower tool-dependent integration suite; make oracle unavailability visible as a separate outcome.

## Additional engineering feedback

### Preserve the architecture, strengthen its contracts

The separation between `sva`, `model`, `engine`, and `session` is useful. Keep that structure. Introduce typed verdict and certification states instead of repeating string comparisons across the hunt, session summary, CLI, and export code. A central verdict policy would reduce inconsistencies such as accepting uncertified vacuity while rejecting uncertified assertion proofs.

The arithmetic and selection logic in the BTOR2 blaster and direct SV emitter duplicate important semantics. Share well-tested bit-vector primitives where the semantics match; keep language-specific sizing and undefined-value rules explicit. Retain independent end-to-end oracle tests so shared code does not become the only reference.

### Make long-lived sessions bounded and observable

Removing an assertion leaves its AIG and monitor latches behind. Rebuilding unrollers limits SAT clauses but does not compact that model, and simulations/exports still traverse accumulated state. Add stress tests for thousands of add/remove cycles, expose model and clause counts, and implement compaction or a documented session reset policy.

`runRic3` limits child-process concurrency, but `Hunt` still launches one `std::async` task per property. Use a bounded worker pool for designs with many properties. Propagate cancellation and deadlines through compilation, search, replay, and certification. Session replay can add up to 30 seconds per callback; shell-based certificate/replay paths lack equivalent timeout handling. Document whether a budget means solver time or total response time.

### Make handoff and setup faithfully represent the checked environment

`exportJasperGold()` marks stale results in comments but does not add stale `PROVEN`/`VACUOUS` entries to the open-property selection (`src/session/design_session.cpp:663–667`). When other open properties exist, the selective `prove` command can omit those stale properties. Treat stale evidence as unchecked for scheduling.

Removing a source assumption from the session does not remove it from the original source files analyzed by the export. The handoff must explicitly reconcile removed source assumptions or refuse to claim equivalent environments. Also use unique hierarchical names and robust Tcl quoting.

`set_prove_time_limit` is parsed and returned in load metadata, but the server's default check budget is independently hardcoded to 30 seconds. The reset generator silently clamps cycle counts to 1–255. Validate and report effective settings instead of silently substituting values.

### Make installation reproducible

CMake pins several dependencies, but setup defaults to the latest OSS CAD Suite and fetches unpinned checker/benchmark heads. Record exact versions/commits and archive hashes in a toolchain manifest. Use download failure checking and hash verification. Add an explicit tool-health command so users know whether replay and proof certification are available.

The README's short `PATH` example includes only OSS CAD Suite, while the setup script prints a longer path including the certificate checkers. Unify these instructions. Add Linux/container CI before advertising the image as deployment-ready; the current untested label is appropriate. Document platform requirements and consider a CMake install target.

### Calibrate documentation and benchmark claims

Keep the clear distinction between `PASS_BOUNDED` and proof. Revise broader claims such as “every answer is independently certified” to match actual configuration and accepted-result policy. Certificates establish claims about the emitted model; they do not establish that all source instances or expression semantics were translated correctly.

The case study usefully discloses unsupported designs and weak property sets. Continue that transparency: scope “zero wrong verdicts” to the historical checked sample, report unverified cases separately, and separate solver detection time from end-to-end certified response latency. Assertions generated from RTL branches test internal consistency; complementary specification-derived properties are needed to assess functional correctness.

## Recommended implementation order

1. **Restore sound verdicts:** Fix the induction base-case bug, instance omission, select semantics, and wide rotates. Add each minimal reproduction to a fast regression suite.
2. **Enforce result integrity:** Gate certified verdicts, reject unsupported environments by default, and make loads transactional.
3. **Harden interfaces:** Remove shell construction, validate request envelopes and resource limits, and repair stale/modified-environment handoffs.
4. **Make validation dependable:** Eliminate false-green harness behavior, add CI/toolchain pinning, and run the full historical campaigns again.
5. **Validate readiness:** Exercise Linux packaging and compare supported properties on representative real blocks with JasperGold before relying on QuickFV to omit downstream proof work.

## Reproduction notes

For the session examples, save each module to a separate `.sv` file, start a fresh `build/qfv serve --work <temporary-directory>`, put `tools/oss-cad-suite/bin` on `PATH`, and set `QFV_NO_RIC3=1` to isolate the internal path. Send newline-delimited requests:

```json
{"id":1,"method":"load","params":{"files":["/absolute/path/to/example.sv"],"top":"t","clock":"clk"}}
{"id":2,"method":"check_all","params":{"budget_s":0.2,"sim_s":0}}
```

For an added property, use `check_assertion` with `sva`, `name`, `budget_s`, and `sim_s`. For finding 6, additionally set `QFV_BTORSIM` to a nonexistent executable before starting the server. For finding 4, load the valid source, attempt the invalid replacement, then call `check_assertion` with `sva:"a"`.

These probes deliberately isolate small behaviors. They establish specific defects; they are not an exhaustive soundness audit of the formal engine.
