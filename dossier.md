---
schema: aobus-phase-packet/v1
kind: council-dossier
mode: plan
quorum: ok
drafts: 3
---
# Council dossier — plan

- mode: `plan`  |  drafts: 3  |  quorum: **ok**
- members: Claude Opus via claude; GPT-5.5 (high) via codex; Gemini 3 Pro via gemini; DeepSeek V4 Pro via opencode; 
- emphasized inputs: doc/design/agent-fleet-tiering.md script/agent/routing.env script/agent/dispatch.sh script/agent/test_phase.sh
- NEXT (chair, R4): independently verify the dossier's key claims, then write the FINAL implementation PLAN, resolving consensus vs dissent.

## The question
```
We need a C3 council review of a proposed direction change for the Aobus agent fleet.

Context:
- We implemented C0/C1/C2/C3 plumbing in this repo.
- C1 currently works well for cheap deterministic mechanical work, especially lint-style fanout with deterministic validation.
- C3 can either act solo or convene a multi-model council for high-stakes plans/reviews.
- Current C2 implementation has become narrow and fragmented: the only gated C2 path that is actually implemented is test augmentation in one existing registered Catch2 file, with hard validation, anchor/source binding, dossier, audit, and C3 review.
- We started discussing adding more C2 capability types like implement-private-cpp/C2, script-edit/C2, local-refactor/C2, etc.
- The user challenged this direction: C1 + C3 council may be enough for deterministic and high-stakes work; for mid-tier work, C3 should produce a plan, hand it to one C2-level executor, then C3 reviews the final diff. Splitting C2 into many tiny capability-specific auto-keep paths may be over-engineering.

My current proposed direction:
1. Stop expanding many narrow C2 auto-keep capabilities by default.
2. Keep C1 for deterministic mechanical fixes and keep C3/council for plan/review/root-cause/architecture.
3. Reframe C2 primarily as a general executor/proposal layer:
   - C3 writes the plan and constraints.
   - A C2 executor runs in an isolated sandbox/copy, edits according to the plan, and emits a patch/dossier/log.
   - The patch is not trusted merely because C2 produced it.
   - C3 reviews the diff, runs validation, and decides accept/modify/reject.
4. Keep the current narrow C2 test auto-keep path as an optional special case where the oracle is hard enough, not as the main C2 architecture.
5. Avoid creating many capability-specific C2 subrunners unless a category proves high-volume and has a strong deterministic gate.

Question for the council:
- Is this direction correct for Aobus, or should we continue building multiple narrow gated C2 capability runners?
- What is the minimal robust C2 executor/proposal contract?
- What safety boundaries are still necessary if C2 no longer auto-keeps but only proposes patches?
- Which parts of the current C2 test-phase infrastructure should be kept, generalized, or deleted/de-emphasized?
- What concrete implementation plan should the chair follow next?

Please be direct. Prefer fewer moving parts unless the additional machinery buys real safety or productivity.
```

## R1 — blind drafts

### GPT-5.5 (high) via codex

**Approach**

The proposed direction is correct: stop growing many narrow C2 auto-keep runners. In Aobus, C1 already has the right shape for deterministic mechanical work, and C3/council is the right place for planning, diagnosis, architecture, and final review. C2 should become a proposal executor, not a trusted keeper.

The minimal C2 contract should be:

- C3 writes a concrete plan, constraints, allowed files, forbidden behavior, and suggested validation.
- One C2 executor edits only an isolated copy.
- The harness, not the model, generates the patch.
- C2 emits patch + dossier + logs + self-reported validation, but does not apply to the main tree.
- C3 applies/reviews/validates the patch and decides accept, modify, or reject.
- The existing C2 test augmentation path remains as a special case because `script/agent/test_phase.sh` has a strong oracle: registered Catch2 file, baseline pass, focused filter, `target_anchor`, source binding, assertion delta, dossier, audit, and final C3 review.

Do not build `implement-private-cpp/C2`, `script-edit/C2`, `local-refactor/C2`, etc. as separate auto-keep paths now. Add a single generic C2 proposal path first. Promote a category to narrow gated auto-keep only after it is high-volume and has a hard deterministic oracle.

**Files to change (real paths)**

- `/tmp/aobus-council-c2-simplification-20260605-175813/out/copy.codex/doc/design/agent-fleet-tiering.md`
  - Update to a new design version.
  - Reframe C2 from “scoped implementation with future expansion” to “proposal executor by default”.
  - Mark current `test_phase.sh` as optional special-case auto-keep, not the architectural center.
  - Remove/de-emphasize future narrow runners such as production body-fill, checker implementation, and script-edit auto-keep.

- `/tmp/aobus-council-c2-simplification-20260605-175813/out/copy.codex/script/agent/routing.env`
  - Keep current `ROUTE_C2_WORKER` for the existing single-file test path or rename it clearly to a test-phase worker.
  - Add a separate generic proposal worker selector, e.g. `ROUTE_C2_PROPOSAL_WORKER`.
  - Prefer a full-copy-capable worker first. `route_c2_worker_codex` is closer to this than the current agy single-file `_route_agy_edit` path, which depends on `AGENT_REL` and flat-file staging.

- `/tmp/aobus-council-c2-simplification-20260605-175813/out/copy.codex/script/agent/c2_proposal_phase.sh`
  - New runner.
  - Input: C2 proposal packet.
  - Stage disposable repo copy outside the repo.
  - Run one C2 worker in that copy.
  - Generate patch by harness diff.
  - Enforce exact allowed-file scope and churn limits.
  - Emit dossier, manifest, worker log, patch.
  - Never apply to the main tree.

- `/tmp/aobus-council-c2-simplification-20260605-175813/out/copy.codex/script/agent/common.sh`
  - Generalize dossier/manifest/audit helpers currently shaped around C2 test review.
  - Add shared helpers for proposal packet validation, exact changed-file extraction, and proposal audit entries.
  - Remove or de-emphasize unused `c2-private-cpp` scope helpers if they imply a narrow auto-keep runner.

- `/tmp/aobus-council-c2-simplification-20260605-175813/out/copy.codex/script/agent/dispatch.sh`
  - Do not route generic C2 proposals through the current “runner kept + independent validation gate” semantics.
  - Either reject generic proposals with a pointer to `c2_proposal_phase.sh`, or add a non-mutating proposal route that exits with artifact paths only.

- `/tmp/aobus-council-c2-simplification-20260605-175813/out/copy.codex/script/agent/test_phase.sh`
  - Keep it.
  - Update comments to say this is a hard-oracle special case.
  - Avoid treating it as the template for all C2 implementation.

- `/tmp/aobus-council-c2-simplification-20260605-175813/out/copy.codex/script/agent/record_review.sh`
  - Extend verdict recording to generic C2 proposal dossiers, not only kept C2 test phases.

- `/tmp/aobus-council-c2-simplification-20260605-175813/out/copy.codex/.agents/skills/write-unit-test/SKILL.md`
  - Clarify that C2 test augmentation is special and review-ready, not broadly representative of C2.

- `/tmp/aobus-council-c2-simplification-20260605-175813/out/copy.codex/.agents/skills/improve-test-coverage/SKILL.md`
  - Same clarification.

- `/tmp/aobus-council-c2-simplification-20260605-175813/out/copy.codex/.agents/skills/diagnose-issue/SKILL.md`
  - Replace “production-source body-fill delegation is future work” with “production-source C2 may only propose patches from a C3 plan; no auto-keep”.

- `/tmp/aobus-council-c2-simplification-20260605-175813/out/copy.codex/.agents/skills/develop-lint-checker/SKILL.md`
  - Keep checker design/registration as C3.
  - If C2 is mentioned, limit it to proposal-only implementation after C3 has proven the matcher and validation plan.

- `/tmp/aobus-council-c2-simplification-20260605-175813/out/copy.codex/test/integration/agent/run_test_phase_test.sh`
  - Keep existing coverage.

- `/tmp/aobus-council-c2-simplification-20260605-175813/out/copy.codex/test/integration/agent/run_dispatch_test.sh`
  - Add tests proving generic C2 proposals do not mutate the main tree.

- `/tmp/aobus-council-c2-simplification-20260605-175813/out/copy.codex/test/integration/agent/run_agent_fleet_test.sh`
  - Add helper tests for proposal scope/churn/dossier parsing.

- `/tmp/aobus-council-c2-simplification-20260605-175813/out/copy.codex/test/integration/agent/run_c2_proposal_test.sh`
  - New offline deterministic suite for the proposal runner.

**Risks & edge cases**

- The current agy C2 path is single-file oriented and relies on `AGENT_REL`; it is not a safe full-repo proposal executor without more sandbox work.
- A proposal patch can still be bad while compiling or passing a focused test. C3 review remains mandatory.
- Validation inside an isolated copy can be misleading because existing build artifacts may point at the main tree. Treat C2 validation as advisory unless the chair applies the patch through the normal controlled validation path.
- Broad C2 scope turns C2 into weak C3. Keep proposal packets small: explicit files, churn budget, no architecture choice.
- Sensitive files such as `script/**`, `doc/design/**`, `.agents/**`, public headers, and CMake should remain C3-only by default. If C2 proposes changes there later, the dossier must flag them as sensitive and never auto-apply.
- The current C2 test path “keeps” a passing edit in the tree. That is acceptable only because it has strong gates and `record_review.sh`; it should not be generalized.

**Alternatives considered**

- Continue building narrow gated C2 runners: safer per category, but too much machinery and too many bespoke contracts.
- Delete C2 entirely: simpler, but loses value for “execute this already-decided plan” work.
- Expand C2 auto-keep to private `.cpp` implementation: tempting, but deterministic gates cannot reliably prove semantic correctness.
- Make C3 council execute patches directly: wrong split. C3 should plan/review; C2 can do draft implementation work.

**Open questions**

- What is the initial maximum C2 proposal scope: one file, three files, or churn-based only?
- Should generic C2 proposal use Codex first and leave agy disabled until hard sandboxing exists?
- Should there be a separate C0 “apply proposal for review” helper, or should the chair apply patches manually at first?
- What review outcome metrics are required before any category graduates from proposal-only to narrow auto-keep?
- Should sensitive-path proposals be rejected outright in v1, or allowed only with an explicit C3-authored flag?

### Gemini 3 Pro via gemini

### Approach

**Direction Validation:**
The proposed direction is correct. Attempting to build multiple narrow, auto-keep C2 capability runners is a trap. The defining feature of C1 is a *deterministic gate* (like `clang-tidy` or an explicit syntax check). The defining feature of C3 is *semantic reasoning* with high risk. C2 exists precisely where tasks are too broad for a simple deterministic gate but too mechanical to waste C3's token budget on authoring. Re-framing C2 primarily as a **Propose-and-Escalate** layer correctly aligns with Aobus's safety philosophy: do not trust model output without a hard oracle. If a hard deterministic oracle doesn't exist (as is true for most mid-tier refactors or private API implementations), C3 *must* act as the semantic oracle.

**Minimal Robust C2 Executor/Proposal Contract:**
1. **Input:** C3's plan (packet body), target files to edit, and a baseline validation ID (e.g., `build-debug` to ensure basic syntax correctness before wasting C3's time).
2. **Execution:** C2 worker runs in the standard `AGENT_SANDBOX` (temporal and spatial isolation) with the plan as its prompt.
3. **Extraction:** Runner extracts the diff via `agent_harness_diff` (never trusting the model's self-generated diff).
4. **Guards:** Deterministic churn limits and path guards (`agent_guard_path`) run on the diff.
5. **Output:** C2 **never auto-keeps**. It unconditionally generates a dossier containing the proposed patch, the build output, and the C3 context, then hands it back to C3 for semantic review (`escalate_to: C3`).

**Safety Boundaries Still Necessary:**
Even without the auto-keep behavior, we must retain:
- **Spatial Isolation (`steam-run` / `AGENT_SANDBOX`):** C2 must not maliciously or accidentally write to the real repo during its thought process.
- **Harness-Diff Extraction:** To guarantee the patch only applies to the intended files and isn't polluted by narrative output.
- **Deterministic Guards:** `agent_guard_path` (protecting `include/`, `CMakeLists.txt`, `doc/`, etc.) and churn limits must still run *before* C3 review. We do not want to burn expensive C3 review tokens on a patch that trivially violates scope.

**Infrastructure to Keep / Generalize / Delete:**
- **Keep:** The `test_phase.sh` infrastructure as an *optional*, specialized runner. Catch2 test passing is one of the very few genuinely strong C2 oracles that justifies an auto-keep path.
- **Generalize:** The sandbox setup, worker invocation, error-feedback loop, and patch extraction from `test_phase.sh` should be generalized into a new `proposal_phase.sh`.
- **Delete/De-emphasize:** The ambition to create dozens of capability-specific auto-keep runners like `script-edit/C2` or `local-refactor/C2`.

---

### Files to change (real paths)

1. **`doc/design/agent-fleet-tiering.md`**
   - Update §3 (Capability Classes): Redefine C2 from "Implementation within design" to "Executor/Proposal layer". Explicitly document the C3-C2-C3 sandwich pattern (C3 Plan -> C2 Propose -> C3 Review).
   - Update §6 (Dispatcher): Clarify that C2 tasks no longer auto-keep by default, but instead propose patches for C3 review.

2. **`script/agent/dispatch.sh`**
   - Generalize the C2 routing. Keep `improve-test-coverage/C2` and `write-unit-test/C2` pointed to `test_phase.sh` (if retaining the specialized Catch2 runner), but add a generic catch-all for `*/C2` (e.g., `implement-private-cpp/C2`, `local-refactor/C2`) to route to the new `proposal_phase.sh`.

3. **`script/agent/proposal_phase.sh` (New File)**
   - Create this file by copying the robust sandbox, worker execution, error feedback, and harness-diff machinery from `script/agent/test_phase.sh`.
   - **Key difference:** Remove the `exit 0` (KEEP) path that applies the patch to the main tree. After the worker completes its compile/fix rounds (e.g., achieving a successful `build-debug`), the script must package the patch and logs into a new Phase Packet and run `agent_emit_packet` to unconditionally escalate the proposal to C3.

4. **`script/agent/routing.env`**
   - No code changes needed for the C2 worker definitions (`route_c2_worker`), but comments must be updated to reflect that C2 now generates proposals rather than autonomous commits.

---

### Risks & edge cases

- **C3 Review Bottleneck:** If C2 frequently proposes non-compiling or garbage patches, C3 will spin in a costly rejection loop.
  *Mitigation:* Retain the C0 error-feedback loop inside `proposal_phase.sh` (e.g., against `v_build_debug`). C2 must pass the compiler before the patch is escalated to C3.
- **Scope Creep in Proposals:** C2 might try to rewrite unrelated files.
  *Mitigation:* The deterministic path guards (`agent_guard_path`) and churn limits must remain fully active in `proposal_phase.sh`. A proposal that touches `include/` is rejected at the C0 level and never reaches C3.
- **No-op / Zero Churn:** C2 fails to make any edits.
  *Mitigation:* Explicitly handle `churn == 0` in `proposal_phase.sh` and escalate directly to C3 with a "Worker produced no change" reason, exactly as `test_phase.sh` currently does.

---

### Alternatives considered

- **Alternative 1: Keep building narrow C2 auto-keep runners.**
  *Rejected.* To make `local-refactor/C2` auto-keep safely, we would need a deterministic oracle that proves semantic equivalence. No such C0 script exists. We would end up with bloated infrastructure and false confidence.
- **Alternative 2: Remove C2 entirely and let C3 write all code.**
  *Rejected.* C3 models (Opus, GPT-5.5) are slow, expensive, and context-heavy. Using a cheaper C2 worker (e.g., Codex or Gemini 3.1 Pro) to grind out the typing and iteratively fix compiler errors saves massive frontier bandwidth, even when C3 still has to review the final diff.

---

### Open questions

1. **Feedback Loop Depth:** When `proposal_phase.sh` loops to fix compile errors, how many rounds (`MAX_ROUNDS`) should it get before giving up? If it fails, should we pass the broken patch to C3 for diagnosis, or just drop it?
2. **Reviewer Identity:** Does the C2 proposal go back to the *exact same* C3 chair that authored the plan, or does it trigger a multi-model council review (`council.sh`) by default? Council review might be overkill for simple mid-tier implementation tasks.
3. **Validation Arg Contract:** Should a generic C2 proposal require a specific validation ID (like `build-debug`) defined in its packet, or should `proposal_phase.sh` enforce a baseline compiler standard internally?

### DeepSeek V4 Pro via opencode

## Council Review: C2 Direction Change for Aobus Agent Fleet

### Approach

**Verdict: The proposed direction is correct.** The current design doc itself flagged C2 as the most fragile tier (§3: *"C2 is the most unverified layer... it may collapse into 'large-scope C1' or 'narrow-scope C3'"*). The only working C2 path took enormous scaffolding — 262 integration-test assertions across `run_test_phase_test.sh`/`run_dispatch_test.sh` — to make ONE capability (test augmentation) safe. Each additional narrow C2 capability would need its own oracle, scope classifier, anchor binding, and risk markers. For capabilities like `implement-private-cpp/C2` or `local-refactor/C2`, there IS no deterministic oracle — you cannot `exit 0` your way out of "is this implementation correct?". Those are inherently C3 problems, and attempting to gate them with C2 patterns would produce either false-clean gates (dangerous) or require C3 review anyway (making the C2 layer redundant overhead).

The executor/proposal model resolves this by making C2 a **tool C3 wields**, not an independent auto-keep tier. This maps naturally to the existing pattern already documented in `write-unit-test/SKILL.md`: *"C3 still decides what behavior and boundaries to test; C2 only writes the planned case."*

The critical architectural insight: **C1 has a hard oracle (clang-tidy exit 0), C3 has no oracle (it IS the oracle), C2 sits in between — it has partial oracles that are insufficient for trust but sufficient for feedback.** The executor model preserves the feedback value (worker edits, build/test fails → feed error back → retry) while removing the false pretense that C2 can autonomously gate correctness.

### Files to Change

#### Primary (implementation):

1. **`script/agent/common.sh`** — Add executor-contract primitives; de-emphasize test-augment-specific gating:
   - Add `agent_c2_is_executor_mode()` / `agent_c2_is_autokeep_mode()` to let `dispatch.sh` branch
   - Remove or comment-out the `c2-private-cpp` branch in `agent_scope_ok` (line 106-108) — it was speculative scaffolding, not a working path
   - Keep `agent_guard_path`, `agent_harness_diff`, `agent_audit_entry`, `agent_emit_review_dossier`, `agent_write_manifest`, `agent_repo_lock` — they're executor-agnostic
   - Keep `agent_classify_path` — scope classification remains useful for dossier metadata
   - Keep `agent_check_registered_test_for_validation` for the test auto-keep special case

2. **`script/agent/test_phase.sh`** — Refactor to support two modes:
   - **Auto-keep mode** (current behavior, gated by `target_anchor` + `validation` presence): applies to tree on pass, emits dossier for C3 pre-commit review. This is the narrow special case for registered-test augmentation.
   - **Executor/proposal mode** (new, gated by absence of validation or a new `kind: proposal` packet field): worker edits sandbox, build+test is run in an **applied worktree** (never the real tree), patch + dossier emitted for C3 to apply. The key change: lines 139-172 (the apply+validate+keep block) must branch — in executor mode, skip the real-tree apply and just emit the patch.

3. **`script/agent/dispatch.sh`** — Generalize the C2 routing:
   - Lines 73-94: The strict C2 test-phase contract gates (exactly one input, `test-core`/`test-gtk`, registered test, `target_anchor`, filter-nonempty) should apply ONLY in auto-keep mode. In executor mode, relax to: capability=C2, safe inputs, valid scope.
   - Lines 99-102: Keep the C2 rollback copy for auto-keep; skip for executor mode.
   - Lines 106-110: Route `*/C2` to `test_phase.sh` (or a new `proposal_phase.sh`), passing the mode flag.
   - Lines 114-121: Independent gate — in executor mode, there is no tree to re-validate; the gate is skipped.

4. **`doc/design/agent-fleet-tiering.md`** — Update:
   - §3 C2 row: change from "有范围·实现 / in-scope implementation" to "有范围·执行/提案 / scoped executor/proposal". Add: *"C2 does NOT auto-keep; it produces a validated patch that C3 reviews and decides to apply."*
   - Add §12 (or extend §9 Step E): Document the executor contract — packet fields, scope constraints, output artifacts, C3 handoff.
   - §9 Step E (lines 542-543): Rewrite to describe the executor model as the main C2 architecture, with test-augment auto-keep as a documented special case.

5. **`.agents/skills/write-unit-test/SKILL.md`** — Update Phase Contract section (lines 12-29):
   - Change "can be delegated to C2 through `script/agent/test_phase.sh`" to "can be delegated to C2 in executor mode through `script/agent/dispatch.sh` with capability C2"
   - Clarify: "C2 produces a validated patch + dossier; C3 reviews, applies, and records the verdict"

#### Secondary (documentation only):

6. **`script/agent/routing.env`** — No functional changes. The C2 worker selector already works for any C2 task; the executor model uses the same `ROUTE_C2_WORKER`.
7. **`script/agent/record_review.sh`** — No changes needed. Already records `accept`/`reject` verdicts for any phase_id.
8. **`.agents/skills/improve-test-coverage/SKILL.md`** — Same update as `write-unit-test`.
9. **`test/integration/agent/run_test_phase_test.sh`** — Add test cases for executor mode (no `target_anchor`, no auto-keep, patch emitted but tree untouched).
10. **`test/integration/agent/run_dispatch_test.sh`** — Add test cases for general C2 routing (non-test skills, executor mode contract).

### Risks & Edge Cases

| Risk | Severity | Mitigation |
|------|----------|------------|
| **C3 bottleneck**: Every C2 patch requires C3 attention (apply+review), slowing throughput for routine augmentations | Medium | The auto-keep special case for test augmentation preserves the fast path for the highest-volume C2 work. For new C2 work that doesn't exist yet, there is no regression. |
| **Loss of feedback loop**: If C2 can't apply+validate, the worker loses the error→feedback→retry cycle that made test-phase robust | Medium | Executor mode should validate in a **throwaway applied worktree** (not the real tree). The worker still gets build/test error feedback for retry; only the final "keep" decision moves to C3. See `test_phase.sh:175-177` — this feedback loop is preserved. |
| **Test-phase regression**: Refactoring a working 262-assertion-tested path | High | Implement as an additive mode flag, not a replacement. The auto-keep path stays identical; executor mode is a new `if` branch. Existing tests continue to pass unmodified. |
| **Scope creep in executor mode**: Without `target_anchor`/registered-test gates, a C2 worker could edit anything within its sandbox scope | Low | `agent_guard_path` still applies. The patch is never auto-applied. C3 reviews the diff before applying. The worst case is wasted C2+ C3 time, not a corrupted tree. |
| **agi/Gemini escape from sandbox** (§10.3) | Low | Same as current C2 — harness-diff + guard + C3 review mean even an escaped edit can't silently land. Executor mode is actually safer because nothing is auto-applied at all. |
| **Council overhead for simple C2 patches**: Using a full C3 council to review a 7-line test augmentation would be disproportionate | Low | C3 can review solo; council is opt-in for high-stakes. The executor dossier gives C3 enough context to make a quick solo decision. |

### Alternatives Considered

1. **Keep current C2 exactly as-is, just stop expanding it** — Simplest, zero regression risk. But leaves the architecture in an ambiguous state where the design doc hints at expansion (the `c2-private-cpp` classifier in `common.sh:106-108` already exists) without clear guidance on what to build next.

2. **Build a new `proposal_phase.sh` separate from `test_phase.sh`** — Cleaner separation but duplicates the sandbox/harness-diff/worker/audit scaffolding. The test_phase.sh already has 90% of what an executor needs; adding a mode flag is ~30 lines vs ~150 for a new script.

3. **Make "keep" vs "propose" a packet-level field (`auto_keep: true/false`)** — Maximum flexibility but adds a dimension to the contract that every C3 plan-writer must understand. The current proposal of "auto-keep only for test augmentation, executor for everything else" is a simpler rule that maps to whether a deterministic oracle exists.

4. **Eliminate C2 entirely, route everything to C1 (if oracle exists) or C3 (if not)** — The design doc §3 already warned this might happen. But the test-phase eval proved C2 has real value: a mid-tier model (Gemini 3.1 Pro) can write correct Catch2 sections from a C3 plan faster and cheaper than a frontier model doing the whole loop. Keeping C2 as an executor preserves this cost/quality sweet spot.

### Open Questions

1. **Should the executor run validation (build+test) in a throwaway worktree, or should C3 do it?** Running it in C2's throwaway worktree preserves the feedback loop for retries but requires the worktree to have a configured build (same `compile_commands.json` problem as §5.1). Running it in C3 means C3 does more mechanical work. **Recommendation**: C2 runs fast-validation (compile+run the specific test) in the throwaway tree for feedback; C3 runs the full validation gate on the real tree before accepting.

2. **What is the minimal packet schema for an executor-mode C2 request?** At minimum: `kind: request`, `schema: aobus-phase-packet/v1`, `skill`, `capability: C2`, `inputs` (scope files), and a markdown body (the C3 plan). No `validation`, no `target_anchor`. The dispatcher should accept this and route to the executor. Should there be a `constraints` field for explicit scope/behavior bounds beyond what's in the body?

3. **Do we need a `kind: proposal` distinct from `kind: request`?** A `request` packet currently implies "apply to tree if validation passes." A `proposal` packet would explicitly mean "produce a patch, never apply." This is semantically cleaner but adds schema complexity. **Recommendation**: Start without it — use the presence/absence of `validation` + `target_anchor` to distinguish auto-keep from executor mode. Add `kind: proposal` later if the distinction proves confusing in practice.

4. **What happens to the assertion-count delta oracle?** It's a weak signal (false positives: a refactor that consolidates assertions; false negatives: a test with one `REQUIRE` that covers the right behavior). In executor mode it becomes dossier metadata, not a gate. In auto-keep mode it stays as-is. **Recommendation**: Downgrade from gate to dossier risk marker in both modes.

5. **Should `council.sh` be used for reviewing C2 executor patches?** Council is expensive (4 members × 3 rounds × frontier tokens). For a 7-line test addition, solo C3 review is appropriate. Council should be reserved for patches that touch architecture, error contracts, or public API — exactly the cases C2's scope guard already escalates. **Recommendation**: C3 solo review by default; council opt-in for high-risk executor patches, triggered by the chair's judgment.

### Concrete Next Steps for the Chair

1. **Phase 1 (no regression)**: Add executor mode to `test_phase.sh` as a new branch — when `target_anchor` is absent and the packet has no `validation`, run worker in sandbox, optionally validate in throwaway worktree, emit patch+dossier, exit 0 without touching real tree. Add 3-4 test cases to `run_test_phase_test.sh`.

2. **Phase 2 (dispatcher generalization)**: Relax `dispatch.sh:73-94` to allow C2 packets without `target_anchor`/strict test gates to route to executor mode. Keep the strict gates for packets that DO have `target_anchor` (auto-keep special case). Add 2-3 test cases to `run_dispatch_test.sh`.

3. **Phase 3 (cleanup)**: Remove or comment the `c2-private-cpp` branch in `agent_scope_ok` (line 106-108). It's dead code that implies a direction we're not taking. Update design doc §9 Step E and add §12.

4. **Phase 4 (skill docs)**: Update `write-unit-test/SKILL.md` and `improve-test-coverage/SKILL.md` Phase Contract sections to describe the executor model as the primary C2 path, with auto-keep as a documented optimization for registered-test augmentation.

5. **Do NOT build**: `implement-private-cpp/C2`, `script-edit/C2`, `local-refactor/C2`, or any other capability-specific C2 subrunners. If a concrete, high-volume use case emerges with a strong deterministic oracle, revisit.

## R2 — challenges

### challenge by GPT-5.5 (high) via codex

## Council Challenge

I agree with the proposed direction: do not keep expanding narrow C2 auto-keep runners unless a category has a real deterministic oracle. But both peer drafts gloss over important implementation hazards in the current code.

## Problems In Gemini Draft

Gemini’s `proposal_phase.sh` idea is directionally cleaner than overloading `test_phase.sh`, but its contract is underspecified.

- It says C2 should run validation in the sandbox, but the design doc explicitly says validation cannot safely run in an isolated worktree because configured build artifacts and `compile_commands.json` point at the main source tree: [doc/design/agent-fleet-tiering.md](/tmp/aobus-council-c2-simplification-20260605-175813/out/copy.codex/doc/design/agent-fleet-tiering.md:207). A generic proposal runner either needs temporal apply-to-main-tree plus rollback, or a newly configured throwaway build tree. “Build output from sandbox” is not currently a valid assumption.

- It proposes a generic catch-all `*/C2` route in `dispatch.sh`, but the current dispatcher requires an allowlisted `validation` for every request: [script/agent/dispatch.sh](/tmp/aobus-council-c2-simplification-20260605-175813/out/copy.codex/script/agent/dispatch.sh:40). Also, request packets require `validation` at schema level: [script/agent/common.sh](/tmp/aobus-council-c2-simplification-20260605-175813/out/copy.codex/script/agent/common.sh:260). A generic proposal mode cannot be added just by routing `*/C2`.

- It says deterministic guards should reject `script/**`, `doc/design/**`, and `include/**`, but then names `script-edit/C2` as a possible generic C2 capability. Current guard forbids `script/` and `doc/design/`: [script/agent/common.sh](/tmp/aobus-council-c2-simplification-20260605-175813/out/copy.codex/script/agent/common.sh:16). Either script edits are C3-only, or the proposal contract needs an explicit plan-scoped allowlist that can override some default forbids only because the patch never auto-keeps. The draft does not resolve that contradiction.

- It underplays the agy isolation problem. `routing.env` documents that agy can reach all of `$HOME` and previously escaped to the real repo: [script/agent/routing.env](/tmp/aobus-council-c2-simplification-20260605-175813/out/copy.codex/script/agent/routing.env:42). “Spatial isolation” is not a hard boundary here. A generic C2 proposal runner should add a real-tree pre/post canary or hard sandbox, not rely on `AGENT_SANDBOX` alone.

## Problems In DeepSeek Draft

DeepSeek correctly calls out the C2 fragility, but its implementation plan is too entangled with `test_phase.sh`.

- “Executor mode by absence of validation or target_anchor” does not work with the current schema. `validation` is mandatory for all request packets, and C2 test requests without `target_anchor` are rejected in `common.sh`: [script/agent/common.sh](/tmp/aobus-council-c2-simplification-20260605-175813/out/copy.codex/script/agent/common.sh:274). This needs a new packet kind or explicit mode field plus schema changes.

- Reusing `test_phase.sh` risks turning a narrow, well-tested special runner into a confusing generic runner. The file is deeply test-specific: exactly one input, exactly one Catch2 filter, registered-test check, anchor absence/presence, list-output source binding, and assertion delta: [script/agent/test_phase.sh](/tmp/aobus-council-c2-simplification-20260605-175813/out/copy.codex/script/agent/test_phase.sh:56). A mode flag would create many branches where a separate `proposal_phase.sh` or shared helper extraction would be safer.

- Its throwaway-worktree validation recommendation conflicts with the design doc’s known failure mode, same issue as Gemini: [doc/design/agent-fleet-tiering.md](/tmp/aobus-council-c2-simplification-20260605-175813/out/copy.codex/doc/design/agent-fleet-tiering.md:207). If DeepSeek wants throwaway validation, it must include “configure a fresh build in that copy” as a costed contract, not assume current validation works there.

- It says no `kind: proposal` is needed. I disagree. Current `dispatch.sh` exit semantics mean `0` equals kept plus independent gate passed: [script/agent/dispatch.sh](/tmp/aobus-council-c2-simplification-20260605-175813/out/copy.codex/script/agent/dispatch.sh:112). A proposal runner returning `0` must not be interpreted as “kept.” The cleanest fix is a distinct packet mode/kind and distinct dispatcher result semantics.

## Minimal Robust C2 Proposal Contract

Use an explicit contract, not inference from missing fields.

Required input:

- `kind: request`
- `capability: C2`
- `mode: proposal`
- `skill`
- `inputs[]` as the maximum allowed edit scope
- optional `validation` plus `validation_args[]`
- body contains the C3 plan, constraints, non-goals, and required validation level

Required output:

- machine-generated patch from harness diff, never model-authored diff
- changed files and churn
- worker log
- validation log if validation was run
- status: `proposed`, `no-change`, `validation-failed`, `guard-rejected`, or `budget-exhausted`
- real-tree state: explicitly `unchanged` after completion, unless using the current test auto-keep path

Validation rule:

- For generic proposal mode, validation is best-effort and advisory unless C3 later applies and validates on the real tree.
- If C2 validation uses the main tree, it must take the repo lock, require clean scoped files, apply patch, validate, then rollback before returning the proposal.

## Safety Boundaries Still Needed

Even proposal-only C2 still needs hard boundaries:

- path allowlist from packet `inputs[]`
- churn and file-count budgets
- no auto-application to the real tree
- real-tree pre/post canary, especially for agy-backed workers
- clean-scope check before any temporal apply
- rollback by patch or exact scoped restore, never broad restore
- C3 review before acceptance
- C3 re-runs validation after applying the accepted patch

The current `test_phase.sh` can stay as the narrow special case. It has a strong enough envelope for registered Catch2 augmentation, but it should be described as “review-ready keep,” not the general C2 architecture.

## Recommended Chair Plan

1. Update `doc/design/agent-fleet-tiering.md` first: redefine C2 as proposal-first, with current test augmentation as a special hard-oracle path.

2. Add a new packet mode or kind for C2 proposals. Do not infer proposal mode from missing `validation` or missing `target_anchor`.

3. Build `proposal_phase.sh` separately, extracting shared helpers later only where duplication becomes real.

4. Do not route `*/C2` immediately. Start with one generic proposal skill name, such as `execute-plan/C2`, and require explicit `inputs[]`.

5. Keep `test_phase.sh` behavior stable. Only adjust docs/comments unless the new schema requires small compatibility changes.

6. Add integration tests for proposal packets: schema accept/reject, no real-tree mutation, guard rejection, validation-failed proposal artifact, and agy-style escaped edit detection if feasible.

Net: the strategic direction is correct, but the safe implementation is not “relax current C2 gates.” It is a new proposal contract with explicit semantics, strict scope, and no trusted keep path.

### challenge by Gemini 3 Pro via gemini

## C3 Council Chair Review: Aobus C2 Fleet Direction

I have reviewed the proposed direction and the draft implementations from **GPT-5.5 (high) via codex** and **DeepSeek V4 Pro via opencode**.

**Verdict:** The core direction is correct. We must halt the proliferation of narrow C2 auto-keep capabilities and reframe C2 as a generic proposal/executor tier. `test_phase.sh` will remain as an optional, hard-oracle special case.

However, both peer drafts contain critical architectural flaws and misunderstandings of Aobus's validation constraints. Below are my challenges and the synthesized implementation plan.

### Challenges to Peer Drafts

#### To DeepSeek V4 Pro via opencode:
1. **FATAL FLAW: Throwaway Worktree Validation.** You proposed running validation (build/test) in a "throwaway applied worktree" to preserve the C2 feedback loop while preventing main-tree mutation. This directly violates **Design Doc §5.1**. Aobus uses out-of-tree CMake builds (`/tmp/build`) where `compile_commands.json` is hard-bound to the **absolute paths of the main source tree**. A C++ build in `mktemp -d` will either fail to link or silently compile the unmodified main tree. Validation **must** happen in the configured main tree using temporal isolation (lock -> apply -> build -> rollback).
2. **WRONG ASSUMPTION: Overloading `test_phase.sh`.** You proposed adding an "executor mode" branch to `test_phase.sh`. This is a severe violation of separation of concerns. `test_phase.sh` is heavily coupled to Catch2 specifics (`target_anchor`, assertion counting, `agent_test_filter_mentions_target_anchor`). Shoving a generic, multi-file C++ implementation phase into a script carrying 262 Catch2-specific assertions guarantees regressions and a maintenance nightmare.

#### To GPT-5.5 (high) via codex:
1. **HIDDEN RISK: Blind Execution (Loss of Feedback Loop).** You correctly suggested a new `c2_proposal_phase.sh` but mandated: *"Never apply to the main tree."* If we never apply the patch to the main tree (even temporally), we cannot run the C++ compiler. Without compiler feedback, C2 degrades into a blind, one-shot string replacer. The power of C2 is the `error -> feedback -> retry` loop. C2 *must* apply the patch to the main tree under a lock to run the build, but it must **always rollback** before exiting, emitting only the artifacts.
2. **WRONG ASSUMPTION: Dismissing `agy`.** You suggested avoiding `_route_agy_edit` for proposals because it currently relies on single-file flat staging. The v13 design just established Gemini 3.1 Pro via `agy` as the default C2 worker due to its high coding capability. We can either restrict v1 C2 proposals to a single file (which is highly effective) or easily upgrade `_route_agy_edit` to stage an array of files. We should not regress our model choice over a trivial staging script limitation.

### The Minimal Robust C2 Executor Contract
The fundamental difference between an "Auto-keep" runner and a "Proposal" runner is what happens *after* validation passes.
- **C1 / C2 Test (Auto-keep):** Apply -> Validate Passes -> Keep tree as-is -> Exit 0.
- **C2 Proposal:** Apply -> Validate Passes -> **Rollback tree** -> Emit Dossier/Patch -> Exit 0.

The safety boundaries remain identical: process isolation (sandbox), harness-generated diffs, deterministic guard checks, and temporal main-tree locking.

### Concrete Implementation Plan

**1. Create `script/agent/c2_proposal_phase.sh` (Do not touch `test_phase.sh`)**
- Create a new packet-driven runner dedicated to generic proposals. This isolates generic implementation logic from Catch2 gating mechanics.
- **The Loop:** Read packet -> worker edits sandbox -> `agent_harness_diff` -> `agent_guard_path` -> `agent_repo_lock` -> `git apply` to main tree -> run validation (e.g., `v_build_debug`) -> collect errors.
- If validation fails: `git apply -R` (rollback) and feed errors back to the worker for the next round.
- If validation passes: `git apply -R` (**always rollback**), write the patch, manifest, and review dossier to `$AGENT_WORK/proposal/escalate/`, and exit 0.

**2. Update `script/agent/dispatch.sh`**
- Route packets with `capability: C2` and missing `target_anchor` to `c2_proposal_phase.sh`.
- Update the dispatcher's independent gate: if the route was a proposal, the dispatcher must also **rollback** the tree after its independent validation confirms the patch is clean.

**3. Single-File Scope for v1**
- To sidestep the `agy` staging complexity mentioned by GPT-5.5, the initial `c2_proposal_phase.sh` will strictly enforce `${#INPUTS[@]} -eq 1`. A generic single-file proposal (e.g., implementing a single `.cpp` file against its header) covers the vast majority of mid-tier C2 utility without risking multi-file staging collisions under `steam-run`. 

**4. Documentation Cleanup**
- **`doc/design/agent-fleet-tiering.md`**: Update §3 and §12 to formally declare C2 as a Proposal/Executor tier. Mark `test_phase.sh` as an optimization (auto-keep) for the strict Catch2 oracle.
- **`script/agent/common.sh`**: Remove dead references to speculative auto-keep branches like `c2-private-cpp`.
- **Skill Docs**: Update `write-unit-test/SKILL.md` to reflect that executor mode is the primary path.

By splitting the runner and enforcing an **always-rollback** policy on success, we preserve the compiler feedback loop (buying real productivity) while eliminating the risk of unreviewed C2 code polluting the tree (buying real safety).

### challenge by DeepSeek V4 Pro via opencode

# Council Challenge — C2 Simplification Review

Both drafts converge on the right direction (stop building many narrow C2 auto-keep runners), but each has consequential blind spots when held against the actual codebase.

---

## GPT-5.5 (codex): Specific Flaws

### 1. "self-reported validation" is a backslide

GPT-5.5 says: *"C2 emits patch + dossier + logs + **self-reported validation**, but does not apply to the main tree."*

This is wrong. The entire Phase Contract architecture (§5) is built on the principle of **never trusting model self-reports** — dispatch.sh runs an independent validation gate (`agent_validate`) even after the runner reports rc=0. The `test_phase.sh` itself already runs the actual `v_test_core`/`v_test-gtk` pipeline as its feedback signal. GPT-5.5's proposal to let C2 just "self-report" and then only have C3 do semantic review is weaker than what the current infrastructure already provides. C3 review without independent build/test validation is blind archaeology.

**The correct pattern**: The proposal runner should run `agent_validate` itself (just like test_phase.sh does), use the pass/fail as the *advisory* signal in the dossier, then rollback the tree and hand the dossier + patch to C3. Don't skip the C0 validation just because the patch won't be auto-kept.

### 2. Full-repo-copy sandbox is needlessly heavy and breaks agy

GPT-5.5 proposes: *"Stage disposable repo copy outside the repo."*

Two problems:
- **Heavy**: The current C2 sandbox copies only the single target file (see `test_phase.sh:109-110` — `cp "$TARGET" "$sbx"`). Copying the entire repo for every C2 proposal is wasteful when the scope is a few files.
- **agy incompatibility**: The agy isolation problem (§10.3, `routing.env` lines 37-50) requires HOME-staging with flat unique names because `steam-run` gives agy a private `/tmp`. A full-repo copy under `/tmp` would be invisible to agy. The proposal runner must handle both opencode-style workers (work from `/tmp` sandbox) and agy-style workers (need HOME staging).

The current single-file staging + `AGENT_REL` contract is battle-tested for this. Generalize to multi-file staging within the same pattern, don't throw it away.

### 3. Confusion about where validation runs

GPT-5.5 says: *"Validation inside an isolated copy can be misleading because existing build artifacts may point at the main tree."*

Correct observation — this is exactly what §5.1 of the design doc says. But then the same draft says C2 should run in a "disposable repo copy." If validation (build/compile) is running inside that copy against build artifacts that point at the main tree, the results are useless. This is a contradiction. The only safe pattern is:
1. Sandbox-edit (single-file or multi-file copy, no build artifacts)
2. Harness-diff
3. Temporal-apply to main tree
4. Run validation on main tree
5. Rollback (since we're not auto-keeping)
6. Escalate dossier + patch + validation output to C3

### 4. Deleting `c2-private-cpp` scope helper misses the point

GPT-5.5 says: *"Remove or de-emphasize unused `c2-private-cpp` scope helpers if they imply a narrow auto-keep runner."*

`agent_scope_ok c2-private-cpp` in `common.sh:106-108` is not a narrow runner — it's a **scope classification helper**, exactly the kind of utility a generic C2 proposal path needs. "Is this path a private .cpp that I'm allowed to touch?" is a useful gate, not an auto-keep commitment. The problem was that nothing called it, not that it existed.

### 5. `record_review.sh` schema doesn't support proposal verdicts

GPT-5.5 proposes extending `record_review.sh` to generic C2 dossiers, but misses that `record_review.sh:22-23` requires the phase to have a `"result":"keep"` audit entry: `grep '"result":"keep"'`. Proposal-only C2 phases that never commit to the tree won't have "keep" results — they'll have "passed-validation" or "failed" results. The verdict recording schema needs to handle this, or you need a separate audit path.

---

## Gemini 3 Pro (gemini): Specific Flaws

### 1. Hardcoding `build-debug` as validation removes the flexibility that IS the point

Gemini says: *"`proposal_phase.sh` must enforce a baseline compiler standard internally"* and references `v_build_debug`.

This hardcodes one specific validation. But the whole point of C2 proposal packets is that C3 defines the validation contract. A `local-refactor/C2` might want `build-debug`; an `implement-private-cpp/C2` might need `test-core [relevant-tag]` after the compile. Hardcoding `v_build_debug` means the C3 plan can't specify "compile AND pass specific tests" as the validation bar. The current `test_phase.sh` doesn't hardcode a validation — it reads `$VALID` from the packet, resolves `$VFN = agent_validation_fn "$VALID"`, and uses that. Do the same for the generic proposal runner.

### 2. The compile-or-deadlock problem

This is the sharpest edge case both drafts miss. Imagine:
- C3 writes a plan: "Implement the `applyCorrection()` method in `lib/audio/Processor.cpp`"
- C2 needs to add `#include <cmath>` for `std::fma` and `#include "audio/Processor.h"` for the class definition
- But `include/**` is guarded (`AGENT_FORBID` in `common.sh:16`)

The C2 worker can't add includes to make the code compile, and can't compile without them. The proposal is deadlocked at the C0 validation loop before it ever reaches C3.

The current `test_phase.sh` avoids this because test-augment files already have all needed includes. A generic proposal executor must solve this somehow — either:
- Allow the C3 plan to pre-authorize specific within-file modifications (like adding `#include` lines inside an already-allowed .cpp file)
- Allow the C3 plan to extend scope to specific public headers on a case-by-case basis
- Have the generic proposal runner auto-detect and escalate when includes are needed but guarded

Neither draft mentions this at all.

### 3. Proposing to remove the keep path from test_phase.sh conflates two things

Gemini says: *"Remove the `exit 0` (KEEP) path that applies the patch to the main tree."*

The "keep" in `test_phase.sh:172` is NOT "trust and auto-keep." It is:
1. The test built and passed (hard oracle)  
2. The `target_anchor` is present in the file (source binding)
3. The Catch2 list output binds filter → source file + anchor (output binding)
4. The assertion-count delta is non-negative (risk marker)
5. A review dossier is emitted
6. `record_review.sh` records C3 accept/reject before commit

This is keep-for-review with hard deterministic gates, not auto-keep. Removing it would break the 34-assertion test suite and lose the only working C2 path we actually have. Keep `test_phase.sh` exactly as-is for the special case; build `proposal_phase.sh` separately.

### 4. No generalization of the `target_anchor` mechanism

The `target_anchor` + Catch2 list-output source binding in `test_phase.sh:144-155` is the innovation that makes the current C2 test path safe — it proves deterministically that the worker touched the right file with a visible marker. For generic C2 proposals, this concept should be generalized: the C3 plan should specify **verification markers** that the harness can check deterministically before escalating. Examples:
- A specific new symbol/function signature that must appear in the diff
- A specific line-count expectation
- An expected compile error that should disappear

Neither draft considers how to generalize the anchor mechanism.

### 5. Council vs. single-chair for proposal review

Gemini asks this as an open question. The answer should be explicit for v1: **Proposal review goes to single chair (the same frontier agent that wrote the plan), not council.** Council is for when multiple frontier models disagree, or stakes are architectural. A C2 proposal that passed compile with deterministic guards and has a clear diff doesn't need 4 models debating it — one frontier agent comparing the diff against the original plan is the right split. Reserve council for when the chair can't decide.

---

## Points of Disagreement Between Drafts

| Issue | GPT-5.5 says | Gemini says | Actual infrastructure says |
|---|---|---|---|
| Where validation runs | Inside C2's isolated copy (advisory only) | Inside proposal_phase.sh before escalation | Must run on main tree after temporal-apply (design doc §5.1) |
| What validation runs | Whatever C3 specifies in plan | Hardcoded `v_build_debug` | Packet's `validation` field — use it |
| Keep existing test_phase.sh? | Yes, as special case | Remove keep path, use as template for proposal | Keep entire test_phase.sh intact |
| Proposal escalation target | C3 (underspecified) | Asks whether council or chair | Single chair for v1; council only for architecture-level decisions |
| Records verdict how? | Extend record_review.sh | Not discussed | Needs schema change for non-keep results |

---

## Hidden Risks and Missing Cases

### Risk 1: The `AGENT_REL` / flat-path staging is tightly coupled to single-file work

`_route_agy_edit` (routing.env:52-67) stages `$AGENT_SANDBOX/$rel` as a flat-named file, edits in-place, then copies back. This only works for single-file edits. A multi-file C2 proposal needs a generalization of this staging contract — perhaps staging a directory tree at a flat-unique prefix, or explicitly handling each file individually in the agy path. This must be solved before any multi-file C2 proposal runner ships.

### Risk 2: The build artifact problem

If C2's compile validation runs on the main tree (temporal-apply → build → rollback), the build artifacts in `/tmp/build/` will be polluted with the proposal's changes, then rolled back to a state where the artifacts no longer match. If the next C2 proposal arrives while stale build artifacts exist, its compile may fail spuriously. Mitigation: the proposal runner should use a **separate fast build target** (like `cmake --build ... --target ao_test` rather than full `build.sh debug`) and the validation function should clean or rebuild the target from scratch each time.

### Risk 3: Sensitive-path proposals need explicit rejection, not silence

Gemini correctly notes that path guards should reject touching `include/`, `CMakeLists.txt`, etc. But neither draft specifies what happens when the **C3 plan itself** asks C2 to touch a guarded path (e.g., the plan says "add a forward declaration to `include/audio/Processor.h`"). The proposal runner needs to validate that the plan's scope is itself guard-compliant BEFORE dispatching to C2 — rejecting the packet early rather than letting C2 waste time on an impossible task.

### Risk 4: Zero-churn escalation is underspecified

Both drafts mention handling `churn == 0` as escalation, but the current `test_phase.sh:126-130` rolls back AND exits with an escalation packet. For a proposal runner, `churn == 0` should be treated as "C2 had nothing to contribute" and escalated to C3 with the original plan and the empty diff — this is useful signal (the plan may be unimplementable or C2 couldn't understand it), not just an error.

### Risk 5: Budget exhaustion should preserve partial work

When `test_phase.sh` exhausts its round budget, it rolls back and exits (line 180-184). For a proposal runner, the last failed attempt's diff + validation output might be the most useful artifact for C3 to diagnose why the plan failed. The dossier should include the last failed patch and its validation output, not just an empty escalation.

---

## Concrete Recommendation

1. **Keep `test_phase.sh` exactly as-is** — it's the special oracle case, not the template.

2. **Build `proposal_phase.sh` as NEW code** reusing sandbox, worker invocation, and harness-diff patterns from `test_phase.sh`, but with these differences:
   - Accepts multi-file scope (controlled by packet `inputs[]`)
   - Reads `validation` from the packet, not hardcoded
   - **Never writes to the main tree permanently** — temporal-apply → validate → rollback
   - Includes the validation output in the dossier
   - Generalizes the "verification marker" concept from `target_anchor` (e.g., `verification_markers` in the packet)
   - Handles both single-file staging (like `test_phase.sh`) and the agy multi-file problem

3. **In `dispatch.sh`**, add a catch-all C2 route that passes unknown `*/C2` to `proposal_phase.sh`; keep `improve-test-coverage/C2` and `write-unit-test/C2` routing to `test_phase.sh`.

4. **In `routing.env`**, add a separate worker selector for generic C2 proposals (`ROUTE_C2_PROPOSAL_WORKER`), defaulting to the same worker as test_phase but swappable independently.

5. **Before building anything**, resolve the "guarded-includes deadlock" (Risk 2 above) — this is a design blocker, not an implementation detail. I recommend allowing the C3 plan to pre-authorize `#include` additions inside already-allowed `.cpp` files, flagged explicitly in the packet.

## R3 — revised drafts

### GPT-5.5 (high) via codex (revised)

**Approach**

The direction remains correct: stop expanding many narrow C2 auto-keep runners. C2 should be a proposal executor under a C3-authored plan, while C1 handles deterministic mechanical work and C3/chair or council handles planning, diagnosis, architecture, and final review.

Critiques aimed at my draft that I accept:

- I incorrectly made generic C2 “never apply to the main tree.” That loses the compiler/test feedback loop. Revised contract: C2 edits in sandbox, harness generates patch, the harness temporally applies the patch to the main tree under lock, runs allowlisted validation, then always rolls back for proposal phases.
- I called C2 validation “self-reported.” That is too weak. The runner must run C0 validation through `agent_validate`; the dossier records real validation output, not model claims.
- I proposed a disposable full repo copy. That conflicts with the design doc’s absolute-path build constraint and with `agy` staging. Revised v1 uses the existing single-file sandbox/staging pattern and validates only via temporal main-tree apply.
- I undervalued `agy`. Revised v1 keeps Gemini 3.1 Pro via `agy` as the default C2 worker and limits proposal scope to one file until `_route_agy_edit` supports multi-file staging.
- I suggested removing/de-emphasizing `c2-private-cpp`. That helper is useful as a scope classifier/gate; keep it.
- I missed that `record_review.sh` only accepts `"result":"keep"`. Proposal phases need a separate accepted audit result such as `proposal-pass` or schema support for non-keep review outcomes.

Minimal robust C2 proposal contract:

1. C3 writes a packet with plan body, one allowed input file for v1, validation id/args, churn budget, forbidden behavior, and optional deterministic verification markers.
2. `c2_proposal_phase.sh` copies only the target file into an isolated sandbox and invokes `ROUTE_C2_PROPOSAL_WORKER`.
3. The harness diffs sandbox target against the main-tree target. The model-authored diff is ignored.
4. The runner enforces guard checks before apply: input file scope, forbidden paths, churn budget, non-empty diff, clean target, validation args, and marker schema.
5. The runner takes the repo lock, applies the patch to the main tree, runs packet-specified `agent_validate`, captures output, then rolls back with the inverse patch before exit.
6. Failed validation is fed back to the worker for bounded retry rounds.
7. Passing validation emits patch, manifest, worker log, validation log, audit entry, and review dossier, but leaves the main tree unchanged.
8. C3 chair reviews the proposal against the original plan and may apply/modify/reject it. Council review is reserved for architectural/high-disagreement cases, not routine C2 proposal diffs.

I disagree with making v1 multi-file. Multi-file proposal execution is the right eventual shape, but the current `routing.env` `agy` path is explicitly single-file and flat-staged. A single-file `private-cpp-source` proposal covers the first useful mid-tier case without weakening the already-proven staging contract.

**Files to change (real paths)**

- `/tmp/aobus-council-c2-simplification-20260605-175813/out/copy.codex/doc/design/agent-fleet-tiering.md`
  - Update the C2 description from “scoped implementation” to “proposal executor by default.”
  - State the proposal lifecycle explicitly: sandbox edit -> harness diff -> temporal main-tree apply -> allowlisted validation -> rollback -> dossier.
  - Preserve `test_phase.sh` as a hard-oracle special case, not the generic C2 template.
  - Clarify that generic proposal review goes to the C3 chair by default; council is opt-in for high-stakes uncertainty.
  - Remove future-facing encouragement for many narrow auto-keep paths unless a category becomes high-volume and has a deterministic oracle.

- `/tmp/aobus-council-c2-simplification-20260605-175813/out/copy.codex/script/agent/routing.env`
  - Add `ROUTE_C2_PROPOSAL_WORKER` and `ROUTE_C2_PROPOSAL_LABEL`.
  - Default them to the current Gemini 3.1 Pro via `agy` worker.
  - Keep `ROUTE_C2_WORKER` for the existing C2 test phase.
  - Document that v1 proposal routing is single-file because `_route_agy_edit` stages one flat file.

- `/tmp/aobus-council-c2-simplification-20260605-175813/out/copy.codex/script/agent/c2_proposal_phase.sh`
  - New runner.
  - Accept only `capability: C2` proposal packets that are not the existing Catch2 test augmentation path.
  - Enforce exactly one input file in v1.
  - Restrict v1 positive scope to `agent_scope_ok c2-private-cpp`.
  - Use packet `validation` and `validation_args`; do not hardcode `build-debug`.
  - Run bounded feedback rounds.
  - Always rollback before exiting, including success.
  - Preserve last failed patch and validation output on budget exhaustion.

- `/tmp/aobus-council-c2-simplification-20260605-175813/out/copy.codex/script/agent/common.sh`
  - Keep `c2-private-cpp`.
  - Add proposal helpers only where they are genuinely shared: proposal dossier emission, proposal audit result names, verification marker parsing/checking, and exact changed-file extraction if needed.
  - Extend packet validation to permit proposal-only fields such as `verification_markers` or `max_churn` if adopted.

- `/tmp/aobus-council-c2-simplification-20260605-175813/out/copy.codex/script/agent/dispatch.sh`
  - Keep `improve-test-coverage/C2` and `write-unit-test/C2` routed to `test_phase.sh`.
  - Route generic `*/C2` packets without `target_anchor` to `c2_proposal_phase.sh`.
  - Do not apply proposal patches permanently in dispatcher.
  - If dispatcher performs an independent proposal gate after runner success, it must also temporal-apply, validate, and rollback.

- `/tmp/aobus-council-c2-simplification-20260605-175813/out/copy.codex/script/agent/test_phase.sh`
  - No functional change.
  - Keep its keep-for-review behavior intact because it has Catch2 registration, baseline pass, anchor/source binding, assertion delta, dossier, audit, and review recording.

- `/tmp/aobus-council-c2-simplification-20260605-175813/out/copy.codex/script/agent/record_review.sh`
  - Extend review recording beyond `"result":"keep"`.
  - Accept proposal audit results such as `proposal-pass` while preserving current kept-phase behavior.

- `/tmp/aobus-council-c2-simplification-20260605-175813/out/copy.codex/.agents/skills/write-unit-test/SKILL.md`
  - Clarify that current C2 auto-keep applies only to registered Catch2 augmentation.

- `/tmp/aobus-council-c2-simplification-20260605-175813/out/copy.codex/.agents/skills/improve-test-coverage/SKILL.md`
  - Same clarification.

- `/tmp/aobus-council-c2-simplification-20260605-175813/out/copy.codex/.agents/skills/diagnose-issue/SKILL.md`
  - Describe production implementation delegation as C2 proposal-only after C3 plan, not auto-keep.

- `/tmp/aobus-council-c2-simplification-20260605-175813/out/copy.codex/.agents/skills/develop-lint-checker/SKILL.md`
  - Keep checker design and matcher semantics C3-owned.
  - Allow C2 only as proposal executor for tightly scoped implementation after a C3 plan.

- `/tmp/aobus-council-c2-simplification-20260605-175813/out/copy.codex/test/integration/agent/run_c2_proposal_test.sh`
  - New deterministic offline suite for single-file proposals, rollback-on-success, validation feedback, churn rejection, no-op escalation, budget exhaustion artifacts, and guarded-path rejection.

- `/tmp/aobus-council-c2-simplification-20260605-175813/out/copy.codex/test/integration/agent/run_dispatch_test.sh`
  - Add routing coverage proving generic C2 proposals do not use `test_phase.sh` and do not leave main-tree mutations.

- `/tmp/aobus-council-c2-simplification-20260605-175813/out/copy.codex/test/integration/agent/run_agent_fleet_test.sh`
  - Add helper coverage for proposal marker parsing, proposal audit results, and `c2-private-cpp` scope use.

**Risks & edge cases**

- Temporal validation can pollute out-of-tree build artifacts even after source rollback. Use the existing validation contract, but keep proposal validations focused and serialized under `agent_repo_lock`.
- Guarded includes can deadlock C2. For v1, allow include additions inside the already-authorized `.cpp` file. Public header edits remain rejected unless C3 keeps them out of C2.
- If the C3 packet itself requests guarded paths such as `include/**`, `script/**`, `doc/design/**`, `.agents/**`, or CMake files, reject before invoking C2.
- Zero churn should escalate with an empty proposal dossier, not be treated as silent failure.
- Budget exhaustion should preserve the last failed patch and validation log for C3 diagnosis.
- Passing build/test validation does not prove semantic correctness. Proposal patches still require C3 chair review.
- Multi-file proposal support is blocked on a real `_route_agy_edit` multi-file staging contract.

**Alternatives considered**

- Continue building narrow gated C2 runners: rejected for now. It creates many bespoke contracts without enough hard oracles.
- Delete C2 entirely: rejected. A C3-authored plan plus one C2 executor remains useful for already-decided implementation work.
- Make generic C2 auto-keep after validation: rejected. Generic implementation lacks the hard oracle that makes `test_phase.sh` acceptable.
- Use full repo copies for C2 validation: rejected. Aobus validation relies on configured main-tree absolute paths.
- Start with multi-file proposal support: deferred. Useful later, but v1 should not outrun the current `agy` staging safety model.

**Open questions**

- Should v1 proposal packets use `verification_markers`, and if so should they be required or optional?
- What exact proposal audit result names should replace the current keep-only assumption: `proposal-pass`, `proposal-fail`, `proposal-noop`, `proposal-budget`?
- Should sensitive-path proposals be rejected absolutely in v1, or allowed only with an explicit `c3_sensitive_scope: true` flag that still prevents auto-apply?
- What validation presets are acceptable for first rollout: `build-debug` only when C3 chooses it, focused tests, or both?
- What metrics are sufficient before any proposal category graduates to a narrow auto-keep path?

### Gemini 3 Pro via gemini (revised)

### Identification of Critiques Aimed at My Draft (Gemini)

The following valid critiques from the council log were directed at my original draft:
- **Validation Execution:** Both GPT-5.5 and DeepSeek pointed out that my assumption of running validation inside the sandbox is flawed because build artifacts point to the main tree (per design doc §5.1).
- **Routing & Schema:** GPT-5.5 noted that routing `*/C2` is invalid because `validation` is a mandatory schema field, and DeepSeek highlighted that hardcoding `v_build_debug` removes the flexibility that C3 is supposed to provide via the `$VALID` packet field.
- **Path Guards & Deadlocks:** DeepSeek identified the "compile-or-deadlock" problem where C2 cannot add `#include` directives because of path guards, deadlocking the C0 validation loop. GPT-5.5 noted a similar contradiction where I proposed `script-edit/C2` despite `script/` being guarded.
- **Over-deletion in `test_phase.sh`:** DeepSeek corrected my proposal to remove the "keep" path from `test_phase.sh`, clarifying that it is a keep-for-review with hard deterministic gates, not an auto-keep, and should remain as a specialized oracle case.
- **Isolation Constraints:** GPT-5.5 and DeepSeek highlighted that my reliance on `AGENT_SANDBOX` underplays the agy isolation problem, and a multi-file generalization of the flat-path staging is required.
- **Missing Generalizations & Edge Cases:** DeepSeek pointed out that I failed to generalize the `target_anchor` mechanism, and underspecified how to handle zero-churn (escalate with empty diff) and budget exhaustion (preserve partial work).

### Approach

**Revised Direction Validation:**
The Propose-and-Escalate model is the correct future for C2, but my initial mechanics were dangerously naive. C2 will not auto-keep, but it must still perform rigorous, main-tree-backed validation before escalating. The current `test_phase.sh` will remain fully intact as our only valid hard-oracle special case.

**Minimal Robust C2 Executor/Proposal Contract (Revised):**
1. **Input:** C3's plan, `inputs[]` (multi-file scope), an explicit `$VALID` validation function, and `verification_markers` (generalizing `target_anchor`).
2. **Execution & Staging:** C2 worker runs using a generalized multi-file staging that respects agy's HOME/tmp constraints (flat-unique prefixing).
3. **Extraction & Temporal Validation:** Harness extracts the diff (`agent_harness_diff`). C2 performs a **temporal apply** to the main tree, runs the packet-specified validation (`agent_validate`), and unconditionally **rolls back**.
4. **Guards & Deadlock Resolution:** To solve the compile-or-deadlock problem, the C3 plan packet will explicitly pre-authorize specific `#include` additions within allowed `.cpp` files, preventing C0 loop deadlocks without dropping the broader `include/` guards.
5. **Output & Review:** Escalate to the **single C3 chair** that authored the plan (saving council for architecture disputes). The dossier will include the patch, worker log, and validation output.

### Files to change (real paths)

1. **`doc/design/agent-fleet-tiering.md`**
   - Redefine C2 as primarily an Executor/Proposal layer, while explicitly documenting `test_phase.sh` as the special-case hard oracle.
   - Document the temporal-apply validation requirement and the single-chair proposal review flow.

2. **`script/agent/dispatch.sh`**
   - Drop the `*/C2` catch-all. Instead, add a specific generic route: `execute-plan/C2` that points to `proposal_phase.sh`.
   - Ensure the dispatcher enforces the presence of an allowlisted `validation` and `inputs[]` for proposal packets.

3. **`script/agent/proposal_phase.sh` (New File)**
   - Build from scratch, reusing helper patterns but implementing the temporal-apply → validate → rollback lifecycle.
   - Support checking generalized `verification_markers` defined in the packet.
   - **Zero-churn handling:** Escalate to C3 with the original plan and an empty diff.
   - **Budget exhaustion:** Escalate the last failed patch and its validation output for C3 diagnosis.

4. **`script/agent/common.sh`**
   - Extend the packet schema to support `mode: proposal`, `verification_markers`, and a mechanism for C3 to explicitly pre-authorize scope overrides (e.g., `#include` additions).

5. **`script/agent/routing.env`**
   - Add a worker selector `ROUTE_C2_PROPOSAL_WORKER`.
   - Implement the generalized multi-file staging logic that resolves the agy isolation problem (staging directories safely without exposing the full repo).

6. **`script/agent/record_review.sh`**
   - Update schema to support logging "proposed", "validation-failed", and "guard-rejected" verdicts instead of strictly requiring a "keep" entry.

### Risks & edge cases

- **Build Artifact Staleness:** Temporal apply and rollback can corrupt build artifacts for future runs.
  *Mitigation:* `proposal_phase.sh` will use a separate, fast build target (e.g., `ao_test_proposal`) and clean it prior to temporal validation.
- **Sensitive Path Deadlocks:** C2 cannot satisfy C3's plan without touching a guarded file.
  *Mitigation:* Handled via the C3 pre-authorization of specific `#include` additions within the packet. If the plan itself violates un-authorized guards, the dispatcher rejects the packet before dispatching C2.
- **Agy Isolation Breaches:** Agy workers reaching outside the sandbox.
  *Mitigation:* The multi-file staging update in `routing.env` will map target files into flat, unique namespaces under the strict sandbox, explicitly preserving the safety boundary.

### Alternatives considered

- **Alternative 1: Disposable Full-Repo Copies for Validation (GPT-5.5's suggestion).**
  *Rejected (Disagreement with GPT-5.5).* I defend my disagreement here alongside DeepSeek. Copying the entire repo is heavyweight and completely breaks agy isolation, because a full copy staged under `/tmp` is invisible to agy's `steam-run` sandboxing. The temporal apply + rollback pattern on the main tree is the only correct way to achieve accurate validation without sacrificing the worker sandbox.
- **Alternative 2: Remove the `keep` path from `test_phase.sh` (My original draft).**
  *Rejected.* I concede to DeepSeek. The Catch2 test augmentation pipeline has hard deterministic gates (anchor binding, list-output source binding, positive assertion delta). It is a keep-for-review path, not an untrusted auto-keep. It stays intact.
- **Alternative 3: Self-reported validation (GPT-5.5's suggestion).**
  *Rejected.* The fundamental premise of Aobus C2 is zero-trust of model execution. The harness must perform the validation independently after the diff is generated.

### Open questions

1. **C3 Scope Pre-Authorization Syntax:** What is the safest schema format in `common.sh` for C3 to authorize `#include` additions? Should it be a `regex` whitelist on the patch, or a specific `allowed_includes[]` list applied to `agent_guard_path`?
2. **Multi-File Staging Scale:** If a plan requires touching 15 files, does the flat-prefix staging in `routing.env` scale cleanly, or do we need to synthesize a virtual directory structure inside the sandbox and map it back via harness-diff?

### DeepSeek V4 Pro via opencode (revised)

# Revised Implementation Plan: C2 Direction Change for Aobus Agent Fleet

## Approach

**The strategic direction is correct and all three council members agree:** stop expanding narrow C2 auto-keep capabilities; reframe C2 as a generic executor/proposal tier; keep `test_phase.sh` auto-keep as the sole hard-oracle special case.

Three valid critiques were leveled at my original draft. I accept all three and the revised plan incorporates them:

1. **Throwaway-worktree validation is impossible** (flagged by both GPT-5.5 and Gemini). My original plan assumed `compile_commands.json` would work in an arbitrary sandbox copy. Design doc §5.1 explicitly states that out-of-tree CMake builds bind `compile_commands.json` to absolute main-tree paths. The executor must use **temporal main-tree apply under lock → build → rollback**, never a throwaway worktree.

2. **Overloading `test_phase.sh` with an executor mode branch is architecturally wrong** (flagged by both GPT-5.5 and Gemini). `test_phase.sh` is deeply coupled to Catch2 specifics — assertion counting, `target_anchor` binding, registered-test checks, single-input constraint, list-output source binding. Adding a generic multi-file C++ implementation branch into 262 Catch2-test-guarded code creates a maintenance hazard. A separate script is cleaner.

3. **Inferring proposal mode from absent fields is fragile** (flagged by GPT-5.5). `validation` is mandatory at the schema level (`common.sh:260`), `target_anchor` absence is already a rejection condition for existing C2 (`common.sh:274`), and exit-code semantics differ (auto-keep exit 0 = tree mutated; proposal exit 0 = tree unchanged). An explicit `mode: proposal` field is the correct contract.

Points where I disagree with challengers and why:

- **Gemini says single-file-only for v1.** I disagree. The executor contract should not hardcode `${#INPUTS[@]} -eq 1`. `inputs[]` is the scope boundary; multi-file proposals (e.g., implementing a `.cpp` + updating its private header) are common and safe under the lock → apply → build → rollback pattern. The `agy` staging limitation is an implementation detail of one worker, not an architectural constraint. If `agy` currently struggles with multi-file staging, fix the staging script or use a different worker — don't bake the limitation into the contract. That said, I demote this to an open question rather than a hard position.

- **GPT-5.5 says C2 should not run validation at all.** I partially disagree. My revised plan aligns with Gemini: C2 runs validation *temporally* on the main tree under lock (for the feedback loop), then always rollbacks. GPT-5.5's "never apply to main tree" is too restrictive — it kills the compiler feedback loop and reduces C2 to a blind string replacer. The safety comes from *always rolling back*, not from *never applying*.

- **GPT-5.5 says `script-edit/C2` contradicts the guard that forbids `script/`.** My original draft did NOT propose `script-edit/C2` — that was the **other** member's proposal (Gemini's draft) being critiqued. My draft explicitly listed `script-edit/C2` in "Do NOT build" (Phase 5). I agree with GPT-5.5 that script edits are C3-only.

## Files to Change

### Phase 1: Schema and Contract Foundation

**1. `script/agent/common.sh`**
- Add `agent_c2_is_proposal_mode()` to detect the new `mode: proposal` packet field.
- Keep `agent_c2_is_autokeep_mode()` for the existing test-phase path.
- Add `agent_c2_apply_temporal()`: acquire repo lock → apply patch → return lock handle.
- Add `agent_c2_rollback_temporal()`: `git apply -R` → release lock → verify tree clean.
- Remove the dead `c2-private-cpp` branch in `agent_scope_ok` (line 106-108).
- Keep: `agent_guard_path`, `agent_harness_diff`, `agent_audit_entry`, `agent_emit_review_dossier`, `agent_write_manifest`, `agent_repo_lock`, `agent_classify_path`, `agent_check_registered_test_for_validation`.

**2. `script/agent/dispatch.sh`**
- Lines 73-94: Add a branch — if `mode: proposal`, skip `target_anchor`/registered-test/filter-nonempty gates; require `inputs[]` non-empty and scope-passing via `agent_guard_path`.
- Route packets with `capability: C2` + `mode: proposal` to the new `c2_proposal_phase.sh`.
- Route packets with `capability: C2` + no `mode: proposal` (i.e., existing test-phase) to `test_phase.sh` unchanged.
- Lines 114-121 (independent gate): Add proposal-mode awareness — if the route was a proposal and tree was temporally applied for validation, always rollback before the gate. The independent gate for proposals validates the *emitted patch*, not the tree.

### Phase 2: New Executor Runner

**3. `script/agent/c2_proposal_phase.sh`** (NEW file)
- Reads packet: `skill`, `inputs[]`, `mode: proposal`, optional `validation` + `validation_args[]`, C3 plan in body.
- The loop (budget-limited rounds):
  1. Worker edits sandbox (`$AGENT_SANDBOX`) per the C3 plan.
  2. `agent_harness_diff` produces a machine-generated patch.
  3. `agent_guard_path` validates the patch touches only files in `inputs[]`.
  4. `agent_c2_apply_temporal` (lock → `git apply` to main tree).
  5. Run validation (e.g., `v_build_debug` or specific test filter).
  6. If validation fails: `agent_c2_rollback_temporal`, feed errors back to worker, continue loop.
  7. If validation passes: `agent_c2_rollback_temporal` (MUST rollback), emit patch + dossier + manifest to `$AGENT_WORK/proposal/escalate/`.
- Exit 0 with status artifact (`proposed`, `no-change`, `validation-failed`, `guard-rejected`, `budget-exhausted`).
- Does NOT reference `target_anchor`, assertion counting, or Catch2-specific logic.

### Phase 3: Documentation and Cleanup

**4. `doc/design/agent-fleet-tiering.md`**
- §3 C2 row: Change description to "有范围·执行/提案 / scoped executor/proposal". Add: *"C2 does NOT auto-keep; it produces a validated patch + dossier that C3 reviews and decides to apply. The `test_phase.sh` auto-keep path is a documented optimization for the hard Catch2 registration oracle."*
- Add §12: Document the executor contract — packet fields (`mode: proposal`), scope constraints (`inputs[]`), output artifacts (patch, dossier, manifest, status), C3 handoff, temporal apply/rollback lifecycle.
- §9 Step E: Rewrite to describe the executor model as the primary C2 architecture.

**5. `.agents/skills/write-unit-test/SKILL.md`** — Update Phase Contract section (lines 12-29):
- Change "delegated to C2 through `test_phase.sh`" to "delegated to C2 through `c2_proposal_phase.sh` (executor mode) or `test_phase.sh` (auto-keep, registered-test only)."
- Clarify: "In executor mode, C2 produces a validated patch + dossier under temporal lock; C3 reviews, applies, and records the verdict."

**6. `.agents/skills/improve-test-coverage/SKILL.md`** — Same update as `write-unit-test`.

**7. `script/agent/routing.env`** — No functional changes. The C2 worker selector already works for any C2 task.

### Phase 4: Tests

**8. `test/integration/agent/run_c2_proposal_test.sh`** (NEW file)
- Test: Proposal packet accepted with valid `inputs[]`, `mode: proposal`.
- Test: Proposal packet rejected when `inputs[]` contains `script/` or `doc/design/`.
- Test: Guard rejects patch that touches files outside `inputs[]`.
- Test: Validation-failed artifact emitted when build breaks.
- Test: `git status --porcelain` is empty after proposal completes (tree unmodified).
- Test: Patch emitted correctly, dossier includes plan body reference.
- Test: Temporal lock is always released (even on worker failure).

**9. `test/integration/agent/run_dispatch_test.sh`** — Add test cases:
- Test: Packet with `capability: C2` + `mode: proposal` routes to `c2_proposal_phase.sh`, not `test_phase.sh`.
- Test: Packet with `capability: C2` without `mode: proposal` routes to `test_phase.sh` (existing behavior).

**10. `test/integration/agent/run_test_phase_test.sh`** — No code changes. Existing 262 assertions continue to pass. Add comments noting this tests the auto-keep special case only.

## Risks & Edge Cases

| Risk | Severity | Mitigation |
|------|----------|------------|
| **C3 bottleneck** | Medium | The auto-keep special case preserves the fast path for Catch2 augmentation. For new C2 work, the C3 load is intentional — C2 is a tool C3 wields, not an independent tier. |
| **Temporal apply race** | Medium | Repo lock (`agent_repo_lock`) serializes all temporal applies. Must verify lock release in all exit paths (including `trap EXIT`). Add test for lock release on worker crash. |
| **Rollback failure** | High | `git apply -R` can fail if the tree was modified between apply and rollback. Mitigation: `agent_c2_rollback_temporal` must capture pre-apply tree state (`git stash` snapshot) as a fallback. Add test for rollback-after-external-modification. |
| **`test_phase.sh` regression** | None | `test_phase.sh` is not modified. The new code is additive in a separate file. |
| **`agy` escape risk** | Medium | `routing.env:42` documents that `agy` can reach `$HOME`. The executor model is actually *safer* than auto-keep because nothing is permanently applied. The worst case is a bad patch + wasted C3 review time. Still, pre/post canary on the real tree (`git diff --stat` before/after) should be added to `c2_proposal_phase.sh`. |
| **Assertion-count delta as gate** | Low | Downgraded from gate to dossier risk marker in both executor and auto-keep modes (as proposed in my original draft, uncontested by challengers). |

## Alternatives Considered

1. **Keep everything as-is, stop expanding C2** — Zero risk, but leaves dead speculative code (`c2-private-cpp` branch) and no guidance for future C2 work. Rejected: the architecture needs a clear contract.

2. **Eliminate C2 entirely** — Simplest possible fleet. But the test-phase evaluation proved C2 has real cost/quality value (Gemini 3.1 Pro writing Catch2 sections from C3 plans). Rejected: C2 preserves that sweet spot.

3. **New `kind: proposal` instead of `mode: proposal`** — GPT-5.5 suggested a separate packet kind. This is semantically cleaner but requires schema versioning and dual-dispatch logic. `mode: proposal` as a field within `kind: request` is sufficient for distinguishing apply vs propose semantics while reusing the existing dispatcher schema. **Defended:** `mode: proposal` is the simpler path.

4. **Single-file-only for v1** — Gemini proposed this. Rejected as an unnecessary constraint. Multi-file proposals within `inputs[]` scope are safe under the lock → apply → validate → rollback pattern. The `agy` staging limitation should be fixed in the staging script, not enforced at the contract level.

5. **Build `proposal_phase.sh` by extracting shared helpers from `test_phase.sh` first** — GPT-5.5 suggested this. It's elegant but premature. The shared surface is small (harness diff, guard, lock, audit entry) and already lives in `common.sh`. Build `c2_proposal_phase.sh` directly using those `common.sh` functions; extract more only if duplication grows. No need to refactor a working `test_phase.sh`.

## Open Questions

1. **`mode: proposal` vs `kind: proposal`?** GPT-5.5 prefers a new `kind`. I prefer `mode: proposal` as a field within `kind: request` — it avoids schema versioning and dual dispatch while making the apply-vs-propose distinction clear. The exit-code semantics difference (auto-keep exit 0 = tree mutated; proposal exit 0 = tree clean) is handled by the dispatcher checking `mode` after the sub-script exits, not by exit code alone. **Recommendation**: `mode: proposal`. If the distinction proves confusing, add `kind: proposal` later.

2. **Should C2 run full `v_build_debug` or just the targeted test?** Full build gives comprehensive feedback but is slow. Targeted test is fast but may miss link-time errors from changes to a `.cpp` that affect other translation units. **Recommendation**: Run both — fast targeted test first for the feedback loop, then `v_build_debug` before emitting the proposal. If `v_build_debug` fails, that's `validation-failed` status.

3. **What is the retry budget for the executor loop?** The current test-phase uses 3 rounds. For a generic C++ implementation, more rounds may be needed but also risk infinite loops. **Recommendation**: 5 rounds, configurable via `AGENT_C2_PROPOSAL_MAX_ROUNDS`.

4. **How does the chair use the proposal dossier?** The dossier at `$AGENT_WORK/proposal/escalate/` should contain: the patch, the worker log, validation output, changed files + churn, status, and a pointer to the original C3 plan. The chair (or `record_review.sh`) reads this, reviews the diff, optionally re-runs validation on the real tree, and emits an `accept`/`reject` verdict with a phase_id.

5. **Should `council.sh` review C2 executor patches?** Council is expensive and should be opt-in for high-stakes patches. Default: C3 solo review. Council trigger: patches touching `include/` (public API), `doc/design/` (architecture docs, if the proposal somehow touches them despite the guard), or patches flagged by the chair. Matches my original recommendation, uncontested.

## Concrete Next Steps

| Phase | Action | Regression Risk |
|-------|--------|-----------------|
| 1 | Add `mode: proposal` handling to `common.sh` + `dispatch.sh`; remove `c2-private-cpp` dead branch | Low (additive) |
| 2 | Create `c2_proposal_phase.sh` with temporal apply/rollback loop | None (new file) |
| 3 | Write integration tests in new `run_c2_proposal_test.sh`; add dispatch routing tests | None (new file, additive) |
| 4 | Update design doc §3, §9, §12; update skill docs | None (docs only) |
| 5 | Do NOT build: `implement-private-cpp/C2`, `script-edit/C2`, `local-refactor/C2`, or any other narrow C2 auto-keep runners | N/A |

## Protocol log (absences & violations)
```
FAILED     draft    claude_opus [Claude Opus via claude]: exited non-zero (rc=1; timeout/crash) -> output discarded
```
