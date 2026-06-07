---
name: execute-plan
description: >-
  Hand an already-decided, scoped implementation plan to the C2 full-tree proposal executor
  (script/agent/c2_proposal_phase.sh). C2 executes the plan in an ISOLATED repo copy and returns a
  validated patch + review dossier — a PROPOSAL, never an auto-landed change; the chair (this agent)
  reviews, applies, and re-validates on the real tree. This is the ONE C2 delegation skill: a refactor,
  a multi-file sweep, or test-writing are all delegated through it (the worker self-loads the relevant
  domain skill for specifics). Opt-in; spends a worker model + full build/test cycles per round. Not for
  deciding the plan, not for forbidden surfaces (CMake/scripts/docs), not for trivial work you can do
  inline.
---

# execute-plan

This skill turns a **decided C3 plan** into validated code without you writing it yourself. You stay the
**chair**: you decide *what* to do (the C3 act), delegate the *doing* to a mid-tier worker, then review
and land the result. The mechanism is `script/agent/c2_proposal_phase.sh` plus the `ROUTE_C2_PROPOSAL_*`
route in `script/agent/routing.env`. It is the C2 analogue of `run-council` (C3) and the C1 lint
fan-out: a skill is the portable contract; the runner is the execution mechanism.

It is the **single** C2 delegation path. There is no separate per-task C2 route any more — writing or
extending tests is just a proposal whose worker reads `write-unit-test` / `improve-test-coverage` from
its own work copy for conventions. See `doc/design/agent-fleet-tiering.md` §12 for the full contract.

## Mental model — proposal, not autonomous keep

C2 is a **proposal executor, not an autonomous keeper.** A successful run means *"a validated proposal
was produced,"* not *"the tree changed."* The real worktree is **never** mutated — a tree-hash canary
aborts the run if any byte of it moves.

```text
C3 plan  →  C2 executes in an ISOLATED /tmp copy  →  emits {patch, manifest, dossier, logs}
         →  C3 (you) reviews, applies/modifies/rejects, and validates in the REAL worktree
```

The chair owns judgment (planning, architecture, error-contract / concurrency decisions, the final
semantic review, and the *apply* to the real tree). C2 only executes a scoped plan against a hard
oracle and proposes.

## When to delegate (and when not)

Delegate when the plan is **already decided, scoped, and concrete**, and you'd rather a cheaper worker
draft it behind a hard gate than type it yourself:
- a settled multi-file refactor / rename sweep / mechanical change;
- filling in a settled design across a known file set;
- writing or extending tests (`intent: behavior-change` — see below).

Do **not** delegate:
- the **deciding** — choosing the approach, the boundary, the error contract is the chair's C3 act;
- work that needs design judgement *mid-flight* (the worker cannot make architecture/API calls);
- anything touching **forbidden surfaces**: CMake / `.clang-tidy` / `script/` / `doc/design/` /
  `.agents/` (these define the oracle itself — "ruler protection");
- trivial work you can finish inline more cheaply than a build/test cycle.

A conservative escalation back to you is fine; a false *acceptance* — presenting an unvalidated change
as "validated" — is not. That is why the chair always re-validates on the real tree before landing.

## Writing the handoff draft (the proposal Phase Packet)

The "draft" is a **Phase Packet**: YAML frontmatter + a markdown body. The body is the plan; the
frontmatter is the contract. Write it to a file outside the repo (e.g. `/tmp/my-proposal-packet.md`).

Keep the frontmatter clean — values are parsed to the end of the line, so do **not** put inline
`#` comments after a value (a trailing comment becomes part of the value and breaks the exact-match
fields). Whole-line `#` comments (e.g. a commented-out optional field) are fine.

```yaml
---
schema: aobus-phase-packet/v1
kind: proposal
skill: execute-plan
capability: C2
mode: proposal
intent: refactor
validation: test-all
inputs:
  - lib/audio/RingBuffer.cpp
  - lib/audio/RingBuffer.h
# id: my-stable-id        (optional; else the runner mints proposal-<utc>-<pid>)
# escalate_to: C3         (optional)
---
Refactor RingBuffer to take the capacity as a constructor argument instead of a compile-time
template parameter, preserving all current behavior.

- In RingBuffer.h: replace `template <std::size_t N>` with a runtime `std::size_t capacity_`
  member set in the constructor; keep the public method signatures identical.
- In RingBuffer.cpp: update the out-of-line definitions to match.
- Do not change any other file. You may edit ONLY the files listed in inputs.
- Do not add, delete, rename, chmod, or symlink any file.

Follow .agents/skills/write-unit-test/SKILL.md conventions if you adjust any test.
Acceptance: the whole test suite still passes (the runner enforces this).
```

Field rules (verified against `agent_packet_validate` and `agent_proposal_input_ok` in
`script/agent/common.sh`):

- **`schema` / `kind` / `skill` / `capability` / `mode`** — all required and must be exactly the values
  above. A missing key, or `skill` ≠ `execute-plan` / `capability` ≠ `C2` / `mode` ≠ `proposal`, is a
  bad packet (exit 64).
- **`inputs[]`** — the only files the worker may edit. Each must, at phase start, exist, be a regular
  file (no symlinks), and classify as **private cpp source** (`lib/**`, `app/**`, `src/**` `.cpp`),
  a **public or private header** (`include/**`, `app/**.h`, `lib/**.h`), or an **existing registered
  Catch2 test**. Forbidden surfaces are rejected. List exactly what the plan needs — no more.
- **`validation`** — the schema requires the key, but the runner **ignores its value** and always forces
  the full-suite **`test-all`** oracle (build `ao_test` + `ao_test_gtk` + `aobus-gtk`, run both whole
  suites, headless). Write `test-all` so the packet reads honestly; you cannot weaken or change the gate.
- **`intent`** — `refactor` (default) asserts no behavior change. `behavior-change` is a **deterministic
  obligation**: the patch must change a registered test, or the proposal is rejected. The planner
  declares intent; the runner never infers it.
- **`id` / `escalate_to`** — optional. `id` must be a safe, non-reserved token (the runner mints one
  otherwise); it keys the audit / outcome / breaker logs.
- **Body = the plan to EXECUTE, not to redesign.** Be concrete and file-by-file. Restate the two hard
  invariants in the body so the worker can't drift: *edit only `inputs[]`*; *no add/delete/rename/
  chmod/symlink*. Point the worker at the relevant domain skill (e.g. "follow
  `.agents/skills/write-unit-test/SKILL.md`") so the body stays short and the worker self-loads the
  specifics — `.agents/skills/` is present in its work copy (readable; it is guarded, so the worker must
  not edit it). State the acceptance criterion.

## Invoking the executor

```bash
export AOBUS_AGENT_WORK=/tmp/aobus-c2/$(date +%s)   # output base dir, OUTSIDE the repo
nix-shell --run "script/agent/c2_proposal_phase.sh /tmp/my-proposal-packet.md"
```

Optional knobs (defaults shown): `MAX_PROPOSAL_ROUNDS=3` (retry budget), `PROPOSAL_CHURN_MAX=2000`
(±line cap), `WORKER_TIMEOUT=1200` (per-round seconds), and `ROUTE_C2_PROPOSAL_WORKER` /
`ROUTE_C2_PROPOSAL_LABEL` to pick the worker model (default DeepSeek V4 Pro via opencode; Gemini 3.1 Pro
via native agy and codex are documented alternates — swap them in `routing.env`, not here).

## Exit codes & artifacts

| exit | meaning |
|------|---------|
| `0`  | `validated` — a green, in-scope patch was produced (read the dossier, then review & apply) |
| `1`  | `diagnostic-budget-exhausted` — an in-scope patch was produced but never went green within the round budget |
| `2`  | rejected — no usable in-scope patch (out-of-scope edit, churn cap, breaker tripped, red baseline, real-repo mutation, or the worker produced no in-scope change) |
| `5`  | config / routing / table missing |
| `64` | bad packet / usage |

Artifacts land in `$AOBUS_AGENT_WORK/proposal_*/`:
- **`review.md`** — the review dossier. **Read this first** (status, validation, churn, intent,
  header-touched, the plan, the changed-files list, and an explicit "what this validation did NOT
  prove").
- **`manifest.json`** — machine summary (status / intent / `header_touched` / `assertion_delta` /
  rounds / churn / validation).
- **`patch`** — the **harness-computed** base→work diff. This is the source of truth; a model-authored
  diff is never trusted.
- `changed-files.txt`, per-round `round<N>.worker.log` / `round<N>.validation.log`, `baseline.log`.

## The chair's acceptance act

`c2_proposal_phase.sh` produces a *proposal*; landing it is **your** irreducible C3 act:

1. Read `review.md` + `manifest.json`.
2. Inspect the `patch` against the plan — does it do exactly what you decided, and only that?
3. **Apply it to the real tree and run REAL validation** (`./build.sh debug` / `script/run-tests.sh`).
   Do not trust the isolated run as the final word.
4. Accept (and possibly modify), or reject. Heed the dossier's **RISK** marker (a header changed with
   `assertion_delta` 0: the suite passed but need not exercise the changed path).
5. Record the outcome with `script/agent/record_review.sh`.

> [!CAUTION]
> Rejecting a `proposal-validated` phase is a **silent-wrong**: it auto-trips the per-worker circuit
> breaker (the route is refused until `script/agent/review_stats.sh --reset`). Record honestly.

For a high-risk patch (public API / architecture / error-contract, or where you're genuinely uncertain),
convene a **`run-council`** review before landing.

## Safety model (summary)

The main worktree is never mutated. The runner records the real repo's tree hash, stages sanitized
**base** + **work** copies (`.git`, build dirs, caches, `logs/`, and gitignored runtime artifacts
excluded; base and work cleaned identically so the diff is tracked-source-only), runs **baseline
validation** first (a red baseline rejects the packet — C2 is never asked to debug pre-existing
breakage), then per round: worker edits the work copy → the harness computes the typed base→work patch →
scope + churn guards (only `modify` of a declared input is accepted) → `test-all` re-validation → the
failure log feeds the next round. On exit it re-hashes the real repo; any change invalidates the
proposal. See `doc/design/agent-fleet-tiering.md` §12 for the authoritative contract.
