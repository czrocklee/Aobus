# C2 Proposal Executor Design

## Summary

C2 should be a proposal executor, not an autonomous keeper.

The intended flow is:

```text
C3 plan
  -> C2 executes the plan in an isolated repository copy
  -> C2 emits proposal artifacts: patch, manifests, dossier, logs
  -> C3 reviews, applies/modifies/rejects, and validates in the real worktree
```

This preserves C2's useful space: it can turn an already-decided plan into code,
compile/test it, iterate on validation failures, and hand a review-ready patch
back to C3. It avoids the risky part: letting a mid-tier executor decide that a
passing build or focused test is enough to permanently land a change.

The generic C2 proposal runner is deliberately non-keep. A successful run means
"a validated proposal was produced," not "the real tree was changed."

## Tier Boundaries

### C1 stays deterministic and mechanical

C1 remains for tasks with a hard oracle and low semantic risk, such as
lint-style mechanical fixes. C1 can keep changes only when deterministic gates
prove the requested property and scope guards pass.

### C3 stays responsible for judgment

C3 owns planning, architecture, root-cause analysis, API/error-contract
decisions, concurrency/lifetime decisions, final semantic review, and the act of
applying a proposal to the real worktree. C3 may convene a council for high-risk
or contentious decisions, but routine proposal review should default to a single
C3 chair.

### C2 becomes scoped execution plus proposal

C2 receives a C3-authored plan and a precise scope. It may implement the plan,
run allowlisted validation through the harness, and revise the implementation
based on harness-produced validation output. It must not permanently mutate the
main worktree, expand the scope on its own, or promote a proposal to an accepted
change.

## Why Not Many Narrow C2 Auto-Keep Runners

Building many runners such as `implement-private-cpp/C2`, `script-edit/C2`, or
`local-refactor/C2` would create many bespoke contracts without enough hard
oracles. For production code and local refactors, a successful build or test run
does not prove semantic correctness.

The repository already has one successful narrow C2 path:
`script/agent/test_phase.sh`. It is safe only because it has unusually strong
constraints:

- one existing, already registered Catch2 test file;
- `test-core` / `test-gtk` validation only;
- a focused Catch2 filter;
- `target_anchor` binding;
- baseline validation before worker execution;
- source/anchor binding after validation;
- assertion-count risk metadata;
- review dossier and audit records.

That path should remain a hard-oracle fast path for registered Catch2 test
augmentation, not become the template for all C2 work. The generic proposal
runner must be a separate path.

## Proposal Packet Contract

A generic C2 proposal request is a first-class proposal packet, not a normal
`kind: request` dispatch packet:

```yaml
---
schema: aobus-phase-packet/v1
kind: proposal
id: base64-refactor-001   # optional; harness mints proposal-<utc>-<pid> if omitted
skill: execute-plan
capability: C2
mode: proposal
intent: refactor          # refactor | behavior-change (optional; default refactor)
inputs:
  - app/runtime/Foo.cpp
  - app/runtime/Bar.cpp
validation: test-core
validation_args:
  - "[runtime]"
escalate_to: C3
---
C3-authored plan, constraints, forbidden behavior, and review notes.
```

Important properties:

- `kind: proposal` selects proposal-specific packet validation and avoids the
  keep-oriented assumptions in `script/agent/dispatch.sh`.
- `id` is optional. If present it is charset-validated and used as the phase id
  verbatim; if omitted the harness mints a unique `proposal-<utc>-<pid>` id. It
  keys the audit/outcome/breaker logs, so the reserved `unknown` value is rejected.
- `mode: proposal` is fixed metadata that makes the non-keep semantics explicit.
- `intent` (optional, default `refactor`) declares whether the change is
  behavior-preserving. `behavior-change` deterministically requires a registered
  test to change; `refactor` is exempt but flagged in the dossier. The runner
  never infers behavior change from worker output — the planner declares it.
- `inputs[]` is the exact editable file set. C2 may not edit outside it.
- `validation` is an allowlisted validation ID, not a shell command. For a header
  change the runner overrides it with the forced `test-core-all` oracle (below);
  the packet may request more validation, never less.
- `validation_args[]` must satisfy the validation argument contract.
- The markdown body is the C3 plan. C2 should execute it, not redesign it.

The proposal runner should be invoked through a dedicated entry point such as
`script/agent/c2_proposal_phase.sh`. It should not be routed through the current
`dispatch.sh` request pipeline unless that pipeline is later refactored to
distinguish keep phases from proposal-only phases.

## Scope Policy (v16: oracle-coverage ⊇ blast-radius)

The scope gate is no longer path-based. This repo has no API/ABI-compat
requirement, so "the edit changed the public surface" is not a danger by itself;
the protected invariant is silent-wrong = 0 (validated-but-semantically-wrong).
A change is admissible when the validation oracle's coverage contains the
change's blast radius. A conservative false rejection (escalate to C3) is
acceptable; a false acceptance that lets C2 present an unvalidated change as
"validated" is not.

Every `inputs[]` entry must:

- be a safe repo-relative argument that exists at phase start and is a regular
  file (no symlinks);
- classify (via `agent_classify_path`) as `private-cpp-source`, a `public-header`
  (`include/**`), or an existing **registered** test source;
- pass `AGENT_PROPOSAL_FORBID` — the oracle-foundation paths stay forbidden:
  CMake / `.clang-tidy` / `script/**` / `doc/design/**` / `.agents/**`. These are
  excluded as **ruler-protection** (they define the build/test oracle itself),
  not because of a path taboo. (This is `AGENT_FORBID` minus the now-allowed
  `include/` headers.)

When any in-scope path is a **header**, the runner:

- **forces the `test-core-all` oracle** (build `ao_test` + run the whole core
  suite) for both baseline and work validation. A single Catch2 filter cannot
  cover a header's blast radius, so the packet's `validation`/`validation_args`
  are ignored for the gate — the packet may request *more*, never *less*. This is
  the atomicity rule: opening headers without forcing the oracle would let a
  header change "validate" under a narrow filter.
- **computes the blast radius** with `agent_proposal_compute_blast_radius` (an
  over-approximating transitive `#include` closure; over-inclusion only escalates
  a borderline case, it never hides one) and **escalates to C3 before the worker
  runs** if the radius reaches the GTK/app frontend (`agent_proposal_blast_core_only`
  is false — not covered by the core oracle) or exceeds `PROPOSAL_BLAST_MAX`
  (default 12 TUs). The build is a *compile/link coherence* oracle, not a semantic
  one; the budget keeps the residual C3 review surface bounded.

A `cpp`-only change keeps the packet's filtered validation (status quo).

The changed-file gate still runs after each worker round (defense in depth): only
`modify` of a declared input is accepted — new files, deletions, symlinks, binary,
and mode-only changes are rejected, and out-of-scope edits reject the round. A
`behavior-change` proposal must additionally change a registered test
(`agent_changes_touch_registered_test`), and the dossier records `intent`,
`header_touched`, `blast_radius`, and `assertion_delta` (a `header-touched +
assertion-delta:0` change carries an explicit RISK marker for C3).

Still out of scope (separate later steps): CMake registration of new translation
units, GTK/app-implicated header autonomy, the `nm`-based undeclared-symbol-deletion
guard, and proposal→auto-keep promotion. C2 may report that a wider scope is
needed; it may not silently add files or edit a forbidden path.

## Dedicated Proposal Worker Contract

The generic proposal runner must not reuse the current `ROUTE_C2_WORKER`
contract. Existing C2 routing is single-file oriented: the runner provides a
sandbox containing one target file, and the default agy-backed worker relies on a
flat filename staging workaround to avoid known path-collision escapes. A
full-repository proposal copy needs real repo-relative paths for includes and
builds, so that mitigation is not valid.

V1 should introduce a separate proposal worker route. The landed defaults (Step G) are **DeepSeek V4
Pro via opencode** and **Gemini 3.1 Pro via native agy**, both chosen because they run in the 
`/tmp` work-copy cwd with strong process-level isolation; codex is the alternate:

```bash
ROUTE_C2_PROPOSAL_WORKER="route_c2_proposal_worker_dspro"   # opencode/deepseek-v4-pro (default)
ROUTE_C2_PROPOSAL_LABEL="DeepSeek V4 Pro via opencode"
# alternate: ROUTE_C2_PROPOSAL_WORKER="route_c2_proposal_worker_gpro"  # Gemini 3.1 Pro via native agy
# alternate: ROUTE_C2_PROPOSAL_WORKER="route_c2_proposal_worker_codex" # GPT-5.5 via codex
```

The worker contract should use files for larger payloads rather than large argv
strings:

```text
AGENT_PROPOSAL_WORK          repository work-copy root; worker cwd
AGENT_PROPOSAL_INPUTS_FILE   newline-separated repo-relative editable files
AGENT_PROPOSAL_PLAN_FILE     original C3 plan body
AGENT_PROPOSAL_FEEDBACK_FILE validation failure feedback for rounds after 1
AGENT_PROPOSAL_ROUND         1-based round number
AGENT_PROPOSAL_OUT           artifact directory outside the repository
```

The worker edits only `AGENT_PROPOSAL_WORK`. It never receives the base-copy
path and never receives a writable path to the main worktree. Agy-backed proposal
workers are out of scope for V1 unless a full-tree isolation design is proven by
tests that specifically cover the known real-repo escape mode.

## V1 Execution Model

The first generic proposal runner avoids main-worktree mutation entirely.

```text
main repo hash recorded
        │
        ▼
copy sanitized repo to /tmp/aobus-c2/<phase>/base
copy sanitized repo to /tmp/aobus-c2/<phase>/work
        │
        ▼
make base read-only and keep it hidden from the worker
        │
        ▼
run baseline validation from base with an isolated build dir
        │
        ▼
round 1..N:
  C2 worker edits work copy
  harness computes typed base -> work change manifest and patch
  scope / sensitive-path / churn guards run before validation
  harness validates work copy through an isolated allowlist call
  failure log becomes next-round feedback
        │
        ▼
emit validated or diagnostic proposal artifacts
        │
        ▼
main repo hash checked again; any mutation invalidates the proposal
        │
        ▼
C3 reviews, applies/modifies/rejects, and validates in the real repo
```

This keeps the implementation simple:

- rollback is equivalent to deleting the temporary copies;
- no source transaction journal is needed;
- no reverse patch is needed for the real tree;
- a crash cannot leave the main tree half-applied;
- long-held repo locks are not needed when validation truly uses per-phase
  isolated build state.

If a selected validation touches shared build state such as `/tmp/build/debug`,
the runner must reject that validation for proposal mode or hold the normal repo
lock for the affected window. V1 should prefer rejection: proposal validation is
only meaningful if it is isolated.

## Repository Copy Rules

The copy step should follow the same safety shape as `council.sh`:

- the proposal output directory must resolve outside the repository;
- `.git` is excluded;
- known build directories, caches, and the output directory are excluded;
- **gitignored runtime artifacts are stripped from both base and work** (`agent_clean_ignored`):
  a worker's build/test run inevitably writes gitignored files (e.g. `logs/app.log`) into its work
  copy, and without this they register as out-of-scope changes and reject every otherwise-valid
  proposal (found by the Step G eval). Cleaning base and work identically — including pruning emptied
  directories — keeps the base→work diff about tracked source only. No-op outside a git repo;
- base and work are copied from the same main-tree snapshot;
- the base copy is made read-only after staging;
- the worker receives only the work-copy path;
- source copies are cleaned up with a trap, while proposal artifacts/logs are
  retained.

The runner records `agent_tree_hash` of the main repository before staging and
again before returning. The weaker fallback "check only target files" is not
sufficient: mode changes, symlink retargeting, or out-of-scope edits must not be
missed. If the main hash changes, the proposal is invalidated and escalated to
C3 because the base snapshot may no longer correspond to the real tree.

## Harness-Generated Tree Diff

The patch must be produced by the harness by comparing the base copy to the work
copy. Model-authored patches are not trusted as the source of truth.

V1 needs a tree-diff primitive, not the existing single-file
`agent_harness_diff`. The primitive should produce:

- an apply-ready patch;
- a typed changed-file manifest;
- a changed-file list;
- churn counts per file and total.

The typed manifest should distinguish at least:

- content modification of an existing regular file;
- file addition;
- file deletion;
- type change;
- symlink target change;
- mode-only change;
- binary file change.

V1 accepts only content modifications to existing regular files that are a
subset of `packet.inputs[]`:

```text
changed_regular_files(base, work) ⊆ packet.inputs
```

Every other change kind is rejected before validation. Churn budgets are applied
to the accepted content changes after scope checking.

## Isolated Allowlisted Validation

C2 packets must reference validation IDs from `script/agent/validation.env`.
They must not carry arbitrary shell commands. The harness owns validation
execution and argument checking.

The proposal runner needs an isolated validation API distinct from the current
`agent_validate`, for example:

```bash
agent_validate_in_repo "$SOURCE_COPY" "$BUILD_DIR" "$VALIDATION_ID" "${ARGS[@]}"
```

The validation table is loaded from the trusted main repository. The isolated
API performs the same allowlist lookup and argument validation as
`agent_validate`, but runs the selected validation against an explicit source
copy and explicit build directory. It should not execute worker-modified harness
scripts as authority for what "validation" means.

V1 should support only validation IDs that can honor the source/build-dir seam.
At minimum, `test-core` and `test-gtk` can be made isolatable by configuring the
work copy into the per-phase build directory and invoking `script/run-tests.sh`
with `--path "$BUILD_DIR"`. `build-debug` should be rejected for proposal mode
until `build.sh` or the validation layer can run it without touching the normal
`/tmp/build/debug` tree.

The runner must run baseline validation before invoking C2. Baseline validation
uses the base copy and an isolated baseline build directory. A red baseline
means the packet is rejected/escalated with the baseline log; C2 is not asked to
debug pre-existing breakage.

For Catch2 validations, the runner should also run the available list helper and
reject filters that match zero tests. The dossier must still be honest that a
passing test filter may not semantically exercise every changed line.

## Validation Feedback Loop

C2 should get real compile/test feedback, but the model worker should never
self-report validation. The harness validates and feeds failures back:

```text
round 1: worker edits work copy -> harness validates -> failure log
round 2: worker receives failure log -> edits work copy again -> harness validates
round N: validation passes or budget is exhausted
```

The work copy may accumulate fixes across rounds. The harness always computes
the current patch and churn from base to work so the final proposal is
self-contained. Scope and sensitive-path guards run after every worker round and
before every validation attempt.

## Bounded Iteration

The runner enforces:

- a round budget;
- a total churn budget;
- optional per-file churn budgets;
- a worker timeout per round;
- an artifact retention policy.

Exhausting the round or churn budget never keeps changes. If the work copy still
contains an in-scope patch, the runner may emit diagnostic artifacts with status
`diagnostic-budget-exhausted`; otherwise it emits a rejected/escalation dossier.

## Proposal Status and Exit Codes

The runner's result must be machine-readable. Suggested statuses:

- `validated`: in-scope patch emitted and isolated validation passed;
- `diagnostic-budget-exhausted`: in-scope patch emitted, but validation never
  passed before the budget expired;
- `rejected-packet`: packet schema, args, validation ID, or scope was invalid;
- `rejected-baseline`: baseline validation failed before C2 ran;
- `rejected-guard`: worker changed unsupported or out-of-scope files;
- `rejected-validation`: selected validation cannot run in proposal isolation;
- `invalidated-main-mutation`: the main worktree hash changed during the phase;
- `system-error`: missing config, copy failure, or other harness failure.

Suggested exit codes:

- `0`: validated proposal artifacts emitted;
- `1`: diagnostic proposal artifacts emitted, but validation did not pass;
- `2`: rejected/escalated without a proposal patch;
- `4`: repo lock unavailable, if a lock is required by the chosen validation;
- `5`: configuration/routing/validation table missing;
- `64`: bad packet.

## Review-Ready Artifacts

Every validated or diagnostic proposal should emit:

- patch;
- typed change manifest;
- changed-file list;
- churn summary;
- baseline validation log;
- per-round worker logs;
- per-round validation logs;
- original C3 plan body;
- risk flags;
- machine-readable manifest;
- review dossier.

The dossier should answer:

- what plan was executed;
- what files changed;
- what validation ran;
- whether baseline validation passed before C2 ran;
- what failed before success or budget exhaustion;
- what scope and sensitive-path guards proved;
- what validation did not prove;
- what C3 still needs to review.

## What C2 May Do

C2 may:

- edit existing regular files explicitly listed in `inputs[]`;
- choose local implementation details inside the C3 plan;
- add local includes or helper code inside allowed files when needed;
- compile and test through harness-controlled validation;
- revise its implementation based on validation failures;
- produce a validated or diagnostic proposal patch.

## What C2 Must Not Do

C2 must not:

- permanently modify the main worktree;
- edit files outside `inputs[]`;
- create, delete, rename, symlink, or chmod files in V1;
- change the validation contract;
- edit harness, script, CMake, design-doc, skill, or public API surfaces in V1;
- decide semantic correctness;
- make architecture, API, ownership, threading, or error-contract decisions;
- promote a proposal to an accepted change.

## Acceptance Flow

After C2 emits a proposal, C3 should:

1. read the dossier;
2. inspect the harness-generated patch against the original plan;
3. apply the patch to the real worktree if it looks promising;
4. modify the patch if needed;
5. run the appropriate real validation in the real repository;
6. accept or reject the final real-tree change;
7. record the proposal review outcome.

**Landed (Step G):** `record_review.sh` accepts proposal audit results
(`proposal-validated` / `proposal-diagnostic` / `proposal-rejected`) and verdicts
`accept` / `modify` / `reject`, writing to `review-outcomes.log` without pretending
C2 kept a change. A `reject` of a `proposal-validated` (or `keep`) phase is a
**silent-wrong** and auto-trips the per-worker circuit breaker, pausing that route
until `review_stats.sh --reset`. `review_stats.sh` rolls the audit + outcome logs
into per-worker silent-wrong rates, and `--window N` adds a rolling rate over each
worker's last N validated+reviewed evals (recent trend vs the lifetime average,
which never recovers from an early miss). The phase `id` is **optional** on the
packet: when supplied it is charset-validated (rejecting unsafe or the reserved
`unknown`) and used verbatim; otherwise the harness mints a unique
`proposal-<utc>-<pid>` id, exactly as `test_phase.sh` does. The old id-less
`unknown` sentinel — which collided across runs in the audit/outcome/breaker keys —
can no longer occur, and the shared `agent_id_ok` guard keeps `record_review.sh`
from recording against an empty or reserved id.

Council review should be reserved for high-risk cases, such as public API
changes, architecture changes, error-contract choices, or cases where the C3
chair is uncertain.

## Implementation Plan Reference

The detailed implementation plan lives in
`doc/plan/c2-proposal-executor-implementation-plan.md`.

## Non-Goals For V1

- No generic C2 auto-keep.
- No new narrow auto-keep runners.
- No temporal apply to the main tree for generic proposals.
- No sensitive-path override mechanism.
- No edits to scripts, CMake, design docs, `.agents/**`, or `.clang-tidy` (the
  oracle's own measurement apparatus — ruler-protection). Public headers ARE now
  editable as of v16, gated by the forced `test-core-all` oracle + blast-radius
  budget rather than forbidden (see Scope Policy).
- No assumption that proposal validation replaces C3's final validation.
