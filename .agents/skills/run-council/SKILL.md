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
spends real frontier budget (member rounds × N members, plus the chair's R4 synthesis). When in doubt,
draft solo; convene only if the stakes justify the cost. This skill is always **opt-in** — never on a
default path.

**Once you have decided to convene, pick a `depth`** (how much debate, below):

- **non-critical → don't convene at all** (draft solo) — the cheapest correct answer.
- **medium-stakes → `challenge`** (the default): one adversarial round + chair synthesis. The common case.
- **brainstorm / "give me N independent options" → `panel`** (opt-down): diversity, no debate.
- **highest-stakes** (architecture, error-contract, concurrency/lifetime, ABI, risky/large diff) **→
  `full`** (opt-up): adds the R3 self-revise so each member converges its own position before synthesis.

Depth trades **debate depth**; the orthogonal cost lever is **roster size** (fewer members = less
diversity), already adjustable via `ROUTE_C3_MEMBERS` / `COUNCIL_MIN` in `routing.env` — shrink the
roster when you want a cheaper panel without giving up the rounds.

## Protocol (rounds)

```
R1  BLIND DRAFT   each routed member drafts independently, with NO peer context
R2  CHALLENGE     each member is shown the OTHERS' drafts and critiques them (adversarial, specific)
R3  SELF-REVISE   each member revises its OWN draft having seen the critiques aimed at it
R4  SYNTHESIS     the chair verifies key claims, then writes the FINAL plan/review
```

`council.sh` runs the **member** rounds (R1–R3, the C0 plumbing) and stops. **R4 is yours**, in-loop —
it is the one irreducibly-frontier act and **always runs regardless of depth**. Blindness in R1 is the
point: do not leak peer drafts into R1, or the panel anchors and the diversity that makes a committee
worth convening collapses.

How many member rounds run is the **`depth`** axis (see below); R4 (the chair) is constant.

> [!NOTE]
> **Forensic mode (Btrfs + bwrap):** When available, each member gets a private read-only Btrfs
> snapshot of the repo mounted at its real path via bubblewrap. Members may use `git log`, `git show`,
> `git blame`, `git diff`, and `rg` for independent reconnaissance. R1 blindness is a mount property
> (each member's `/agent/out` is a private run dir). An evidence fingerprint gate verifies all snapshots
> share the same baseline before R1 starts. Legacy mode (no Btrfs) falls back to writable copies and
> tree-hash canaries.

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
depth: challenge      # panel | challenge | full  — member rounds to run (optional; default challenge)
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
- Exit `0` = dossier emitted (read it); `2` = no usable draft (quorum failure); `3` = configuration or
  system error (unsafe inputs, repo copy failure); `5` = routing table missing; `64` = bad packet (bad
  `kind`/`mode`/`depth`, empty body, or a forbidden `validation:` key).
- The dossier frontmatter carries two **independent** status axes — read both:
  - `quorum: ok | degraded` — *accidental*: `degraded` means fewer than `COUNCIL_MIN` drafts came back,
    so the cross-challenge could not really run; treat it as close to a solo draft.
  - `shallow: full | by-design` — *intentional*: `by-design` means you chose a `depth` below `full`
    (`panel`/`challenge`), so the rounds **beyond that depth** were a deliberate cost choice, **not** a
    failure (any accidental shortfall is reported under quorum, separately). A healthy `panel` with four
    drafts is `quorum: ok`, `shallow: by-design` — synthesize the independent drafts; do not mistake it
    for a degraded `full`.

## Writing a correct packet (the body decides whether the council earns its cost)

The frontmatter is mechanical; the **body** is what makes a council useful or wasted. Drafting it well
matters more than the depth/roster knobs. In order of importance:

1. **Lead with motivation and goal — not the mechanism.** A panel's real job is to judge whether a
   design is *proportionate* to its purpose: over-built, under-built, or solving the wrong problem. If
   the body opens with "replace X with Y," members can only critique the mechanics of Y; they cannot
   tell you that Y is the wrong fight. State **why this decision exists and what success looks like**,
   *then* the proposed approach. This is the single most common packet defect — the body that reads as
   a finished spec instead of a question.
2. **Make the body self-contained, and prefer an investigation index over feeding code.** In forensic
   mode, members can run `git log`, `git blame`, `rg`, and normal file reads independently — you don't
   need to paste every excerpt. Instead, provide an **investigation index**: the goal, files/symbols
   likely relevant, suggested starting queries, settled premises (NOT under review), and numbered
   questions to answer. For `mode: review`, still include the actual diff/change (either paste it or
   note the commit so members can inspect it via `git show`); don't make members hunt for the target.
3. **Fence off settled premises.** Mark anything already decided as **"NOT under review — do not
   relitigate,"** with its reason. A `panel`/`challenge` council has only one or two rounds; if you
   leave a closed question open, members spend that budget re-deciding it instead of stress-testing
   what is actually live. (Example: "this repo has no ABI/API-compat requirement — assume callers are
   in-tree and freely updatable.")
4. **Ask enumerated, specific questions — and demand a steel-man.** "Review this" yields mush. List the
   exact claims to attack, numbered, and require each member to argue the **opposite** position at
   least once. The adversarial round is the entire point of a committee; hand it a target.
5. **Specify the output contract** so drafts come back comparable and synthesizable. Review: findings
   as **(severity / location / problem / fix)** + an explicit **verdict** + the **single biggest
   risk**. Plan: **Approach / Files to change / Risks / Alternatives / Open questions**.
6. **`inputs:` is a hint, not a substitute.** Those paths are safety-checked (no flags, no traversal)
   and only *emphasise* where to look; they never replace pasting the change or stating the question.

### Trimming or swapping the roster for one run (e.g. excluding the chair's own model)

`council.sh` honours `ROUTE_C3_MEMBERS` when it is set and non-empty (`council.sh:88`), **but**
`routing.env` assigns that array **unconditionally** (`routing.env:233`), so simply *exporting*
`ROUTE_C3_MEMBERS` before the run is clobbered the moment the table is sourced. To change the roster
for a single run without editing the committed table, point `AOBUS_ROUTING_ENV` at a thin override
that sources the real table (for all member functions + labels) and *then* trims the array:

```bash
cat > /tmp/roster.env <<'EOF'
. /home/<you>/dev/Aobus/script/agent/routing.env          # every member fn + label
ROUTE_C3_MEMBERS=(route_c3_member_codex route_c3_member_gemini route_c3_member_dspro)  # drop claude
EOF
AOBUS_ROUTING_ENV=/tmp/roster.env \
  nix-shell --run "script/agent/council.sh /tmp/packet.md"
```

When the chair is reviewing **its own** proposal, consider dropping the chair's own model from the
roster (e.g. `route_c3_member_claude_opus` when the chair is Claude): seating it is same-model
self-sampling and dilutes the cross-vendor diversity that justifies convening at all. The trade is one
fewer blind draft — keep at least `COUNCIL_MIN` (default 2) members from **different** vendors.

## Depth tiers (how much debate)

`depth:` caps how many **member** rounds run; the chair's R4 synthesis always runs.

| `depth`     | member rounds | what you get                              | when |
|-------------|---------------|-------------------------------------------|------|
| `panel`     | R1            | N independent drafts, no debate           | brainstorm / "give me N options" |
| `challenge` | R1 + R2       | diversity + one adversarial round         | **default** — medium-stakes plan/review |
| `full`      | R1 + R2 + R3  | + each member self-revises before synthesis | highest-stakes architecture / error-contract / risky diff |

`depth` is orthogonal to `mode` (below): any of the six combinations is valid. Omitting `depth` defaults
to `challenge`. `panel` shines in plan-mode brainstorming (you want a spread of options, not a debate);
`challenge` is the review sweet spot; reach for `full` only when a wrong call is genuinely costly.

## Two modes

- **`mode: plan`** — the body is a task; members read the repo (read-only) and each return a structured
  plan (Approach / Files to change / Risks / Alternatives / Open questions). Use during plan mode for a
  high-stakes design before `ExitPlanMode`.
- **`mode: review`** — the body is a diff/change; members return findings (severity, location, problem,
  fix) and a verdict. Pairs with `code-review` / `diagnose-issue` for a high-stakes review.

## Read-only safety

Council members are **read-only**: they produce an opinion, never a patch, and never touch the tree.

**Forensic mode (Btrfs + bwrap, the default when infrastructure is available):** Each member gets a
private read-only Btrfs snapshot of the repo mounted at its real path inside a bubblewrap namespace.
Source writes fail at the filesystem level. An evidence fingerprint gate (`HEAD` + all refs + worktree
hash) verifies all snapshots share an identical baseline before R1 starts. R1 blindness is enforced by
the mount: each member's output dir (`/agent/out`) is a distinct private run directory.

**Legacy mode (no Btrfs or no bwrap):** Each member runs in its own disposable writable copy of the
repo; `council.sh` content-hashes that copy before/after each call — any member that mutated it has its
output **discarded and flagged**. This is process isolation for a **trusted** roster, not a hard sandbox;
an agentic CLI could still read outside its cwd (see §10.3).

In both modes, the real-repo canary (`agent_tree_hash` of the source repo) runs before fan-out and on
every exit path to catch any member that escapes its environment.
