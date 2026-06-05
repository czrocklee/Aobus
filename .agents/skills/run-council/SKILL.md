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
the **chair**: it convenes the council, sits on it as a blind member, and writes the final answer.

It is the C3 analogue of the C1 lint fan-out, but C3 has **no deterministic gate** — a plan or a review
is prose, and "correct" is a judgment. So the convergence mechanism is **adversarial cross-examination +
chair synthesis**, not ranking + validation. See `doc/design/agent-fleet-tiering.md` §11.

## When to convene (and when not to)

Convene only when the decision is **high-stakes** and a wrong call is costly:
- architecture / module boundaries, an error-contract choice (`ao::Result` vs `std::optional` vs throw),
  a concurrency/lifetime design, a public-API or ABI change, a risky or large diff under review.

Do **not** convene for routine work — a local refactor, a small bugfix, an obvious review. A council
spends real frontier budget (≈ 4 rounds × N members ≈ a dozen slow frontier calls). When in doubt, draft
solo; convene only if the stakes justify the cost. This skill is always **opt-in** — never on a default
path.

## Protocol (4 rounds)

```
R1  BLIND DRAFT   each member (incl. the chair) drafts independently, with NO peer context
R2  CHALLENGE     each member is shown the OTHERS' drafts and critiques them (adversarial, specific)
R3  SELF-REVISE   each member revises its OWN draft having seen the critiques aimed at it
R4  SYNTHESIS     the chair reads the dossier and writes the FINAL plan/review, resolving consensus vs dissent
```

`council.sh` runs R1–R3 (the C0 plumbing) and stops. **R4 is yours**, in-loop — it is the one
irreducibly-frontier act. Blindness in R1 is the point: do not leak peer drafts into R1, or the panel
anchors and the diversity that makes a committee worth convening collapses.

## The chair's two acts

1. **Before** invoking, write your own **blind** draft to `$AGENT_COUNCIL_OUT/draft.chair.md` — your
   independent plan/review, written without consulting the members. It is then a peer the members
   challenge in R2 and appears in the dossier.
2. **After** `council.sh` prints the dossier path, read `dossier.md` and write the **final** plan/review:
   weigh the revised drafts and the full challenge log (including the challenges aimed at your own draft),
   state where the council agreed and where it split, and resolve the splits with reasons. For plan mode
   the synthesis becomes the plan-file / `ExitPlanMode` artifact; for review it is the review you report.

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
constraints and context the members need. For a review, paste/point at the diff here.
```

```bash
export AGENT_COUNCIL_OUT=/tmp/aobus-council/$(date +%s)   # an out dir OUTSIDE the repo
# (optional) write your blind chair draft first:
#   ...your independent plan/review... > "$AGENT_COUNCIL_OUT/draft.chair.md"
nix-shell --run "script/agent/council.sh /tmp/my-council-packet.md"   # prints the dossier path
```

- A council has **no `validation:` field** (no deterministic gate) and does **not** go through
  `dispatch.sh`; `council.sh` is its own entry.
- Roster, labels, and per-vendor read-only invocation live in `routing.env`
  (`ROUTE_C3_MEMBERS` / `ROUTE_C3_MEMBER_LABELS`). Cross-vendor by default — a same-vendor council
  defeats the purpose. Swap models there, not here.
- Exit `0` = dossier emitted (read it; `quorum: degraded` inside means too few drafts to really debate —
  treat it as close to a solo draft); `2` = no usable draft; `64` = bad packet.

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
