---
name: run-council
description: >-
  Convene a multi-model C3 "council" — a cross-vendor panel of frontier models that draft, challenge
  each other, and revise — to produce a high-stakes implementation PLAN or code REVIEW. Use for
  architecture decisions, risky diffs, error-contract choices, or any plan/review where a single model's
  blind spot is costly. Opt-in and expensive (real frontier tokens × members × rounds); not for routine
  work.
---

# run-council

This skill turns a **C3** task (plan / review) from a solo act into a **committee**. The mechanism is
`script/agent/council.sh` plus the C3 roster in `script/agent/routing.env`. The current in-loop agent is
the **chair**: it convenes the council, independently verifies the dossier's key claims, and writes the
final answer. The chair does not draft in R1; if the chair's model should produce an R1/R2/R3 opinion,
it belongs in `ROUTE_C3_MEMBERS` as an ordinary member.

It is the C3 analogue of the C1 lint fan-out, but C3 has **no deterministic gate** — a plan or a review
is prose, and "correct" is a judgment. So the convergence mechanism is **adversarial cross-examination +
chair synthesis**, not ranking + validation. See `doc/design/agent-fleet-tiering.md` §11.

## When to convene (and when not to)

Convene only when the decision is **high-stakes** and a wrong call is costly:
- architecture / module boundaries, an error-contract choice (`ao::Result` vs `std::optional` vs throw),
  a concurrency/lifetime design, a public-API or ABI change, a risky or large diff under review.

Do **not** convene for routine work — a local refactor, a small bugfix, an obvious review. A council
spends real frontier budget (R1/R2/R3 × N members, plus the chair's R4 synthesis). When in doubt, draft
solo; convene only if the stakes justify the cost. This skill is always **opt-in** — never on a default
path.

## Protocol (4 rounds)

```
R1  BLIND DRAFT   each routed member drafts independently, with NO peer context
R2  CHALLENGE     each member is shown the OTHERS' drafts and critiques them (adversarial, specific)
R3  SELF-REVISE   each member revises its OWN draft having seen the critiques aimed at it
R4  SYNTHESIS     the chair verifies key claims, then writes the FINAL plan/review
```

`council.sh` runs R1–R3 (the C0 plumbing) and stops. **R4 is yours**, in-loop — it is the one
irreducibly-frontier act. Blindness in R1 is the point: do not leak peer drafts into R1, or the panel
anchors and the diversity that makes a committee worth convening collapses.

> [!CAUTION]
> **Shared Artifact Exposure:** While repository copies are isolated via tree-hash canaries, all members
> share the same `$AGENT_COUNCIL_OUT` directory for artifact collection. R1 blindness relies on member
> cooperation and CLI tool-calling limits rather than hard filesystem isolation.

## The chair's act

After `council.sh` prints the dossier path, read `dossier.md` and write the **final** plan/review. The
chair stays in the verifier/synthesizer role instead of becoming another contestant.

For R4:
- State the council's main consensus points.
- List the key disagreements and high-risk claims.
- Independently inspect the relevant code, diff, tests, and design docs for those claims.
- Explicitly accept or reject important member claims with evidence.
- Resolve the splits with reasons. For plan mode the synthesis becomes the plan-file / `ExitPlanMode`
  artifact; for review it is the review you report.

## Invoking the council

Write a council Phase Packet (YAML frontmatter + markdown body) and run the orchestrator:

```yaml
---
schema: aobus-phase-packet/v1
kind: council
mode: plan            # plan | review  — selects the prompt templates
inputs:               # optional repo-relative paths to emphasise (safety-checked; no flags/traversal)
  - lib/audio/Player.cpp
---
The QUESTION goes in the body: the task to plan, or the change to review, plus the
constraints and context the members need. For a review, PASTE the full diff here (members
have no git access and cannot resolve "HEAD" or commit hashes).
```

```bash
export AGENT_COUNCIL_OUT=/tmp/aobus-council/$(date +%s)   # an out dir OUTSIDE the repo
nix-shell --run "script/agent/council.sh /tmp/my-council-packet.md"   # prints the dossier path
```

- A council has **no `validation:` field** (no deterministic gate) and does **not** go through
  `dispatch.sh`; `council.sh` is its own entry.
- Roster, labels, and per-vendor read-only invocation live in `routing.env`
  (`ROUTE_C3_MEMBERS` / `ROUTE_C3_MEMBER_LABELS`). Cross-vendor by default — a same-vendor council
  defeats the purpose. The default roster includes a Claude/Opus member as an ordinary member; the chair
  is still outside the roster and only performs R4. Swap models there, not here.
- Exit `0` = dossier emitted (read it; `quorum: degraded` inside means too few drafts to really debate —
  treat it as close to a solo draft); `2` = no usable draft (quorum failure); `3` = configuration or
  system error (unsafe inputs, repo copy failure); `5` = routing table missing; `64` = bad packet.

## Two modes

- **`mode: plan`** — the body is a task; members read the repo (read-only) and each return a structured
  plan (Approach / Files to change / Risks / Alternatives / Open questions). Use during plan mode for a
  high-stakes design before `ExitPlanMode`.
- **`mode: review`** — the body is a diff/change; members return findings (severity, location, problem,
  fix) and a verdict. Pairs with `code-review` / `diagnose-issue` for a high-stakes review.

## Read-only safety

Council members are **read-only**: they produce an opinion, never a patch, and never touch the tree. Each
member runs in its own disposable repo copy, and `council.sh` content-hashes that copy before/after the
call — any member that mutated it has its output **discarded and flagged** (a member writing a file is a
protocol violation). This is process isolation for a **trusted** roster, not a hard sandbox; an agentic
CLI could still read outside its cwd (see §10.3). Do not feed untrusted review input to a council without
the harder sandbox noted there.
