# Aobus Agent Fleet Architecture

> Status: implemented baseline. `aobus-fleet`, `ao_fleet`, and `config/agent-fleet.yaml` are the
> production mechanism and registry. This document is the governing architecture; implementation gaps
> are defects rather than alternate workflow contracts.
>
> Scope: vendor-neutral and harness-neutral. The fleet routes a heterogeneous set of agents
> (frontier and cheap, across vendors) at software-engineering work on the Aobus repository.
>
> Current boundary: unattended acceptance is intentionally excluded. Every delegated result remains a
> proposal or advisory until chair review.

### Production Interface

The production executable is built at `/tmp/build/debug/tool/fleet/aobus-fleet` by
`./build.sh debug --target aobus-fleet`. Its stable commands are:

```text
aobus-fleet validate-config --registry <file>
aobus-fleet run --registry <file> --repo <path> --out <path> <intent>...
aobus-fleet review record --out <path> --phase <id> --verdict accept|modify|reject --reason <text>
aobus-fleet stats --out <path> [--window N]
aobus-fleet route reset --out <path> --route <route-id>
```

Exit `0` means a proposal, advisory, or synthesis dossier was produced; `2` is policy rejection or
escalation; `3` is infrastructure failure; `5` is registry/configuration failure; and `64` is CLI or
intent failure. There is no apply, commit, merge, or unattended-acceptance command.

All structured protocols are strict, versioned YAML. The protected registry is
`config/agent-fleet.yaml` (`aobus-fleet-registry/v1`); chair requests are
`aobus-fleet-intent/v1`; manifests, evidence, traces, audit records, and review outcomes each have an
independent `aobus-fleet-*/v1` schema. Patch files remain unified diff, dossiers remain Markdown, and
process logs remain text. JSON and JSONL are not compatibility formats.

---

## 0. Purpose

A single human-supervised frontier agent (the **chair**) cannot personally write every line, run every
check, and review every diff at the throughput modern work demands. The fleet exists to let the chair
**delegate** generation and checking to a heterogeneous pool of cheaper or more specialized agents,
**without ever lowering the bar on correctness**.

Three goals, in priority order:

1. **Safe.** The non-negotiable invariant is **`silent-wrong = 0`**: no delegated result may be
   *presented as accepted, or applied to the real tree by the fleet, without chair review*. A
   conservative false rejection (escalate to the chair) is always acceptable; a false acceptance is
   never.
2. **Flexible.** Any sane combination of *what work*, *which model*, *which acceptance check*, and
   *which permissions* must be **expressible as data**, not require new code. "Run the lint task with a
   strong model" and "let a cheap model attempt an oracle-less rename" must both be one-line bindings.
3. **Complexity-controlled.** The number of **mechanisms (code)** is small and fixed. Everything that
   grows with usage — models, checks, task kinds, permission profiles — grows as **registry rows
   (data)**. New capability must not mean new scripts.

These goals are in tension, and the entire architecture is the resolution of that tension. The reader
should hold goal (1) as a hard constraint and read goals (2) and (3) as "maximize, subject to (1)."

---

## 1. First principles (the axioms)

Everything below is derived from these. If a design decision cannot be traced to one of these, it is
suspect.

1. **Mechanical evidence comes from the oracle, not the model.** Trust is never placed in a worker's
   reputation or self-report. The harness extracts the artifact and runs independent checks; the chair
   remains the semantic acceptance gate.
2. **Oracle results are evidence, not verdicts.** "The check passed" is not the same as "the work is
   correct." Each result states what was checked and what residual risks remain (§2). Delegated code
   remains a proposal until the chair reviews it.
3. **Delegation is separate from acceptance.** *Who may attempt* a piece of work and *who accepts it*
   are two different questions. Delegation may be broad; acceptance remains with the chair.
4. **The harness independently re-validates.** Every check that gates a result is run by the harness
   itself, on a harness-extracted artifact, against an allowlisted oracle. A worker's self-reported
   success, self-written diff, or self-declared scope is *evidence*, never the gate.
5. **Environment is a uniform capability; authority is a per-phase clamp.** Every agent runs in the
   same rich substrate (full repo, history, tools). What a given phase is *permitted* to do is
   subtracted from that capability by an explicit authority policy. Capability is uniform so that
   competence is measurable; authority is least-privilege so blast radius is bounded.
6. **Mechanisms are code; bindings are data.** A small fixed set of interpreters (substrate,
   convergence engines, scheduler) is written, tested, and versioned as code. The
   open-ended combinatorial space (which model, which oracle, which permissions, which task) is
   expressed as typed, versioned, statically-validated registry data — *not* as a Turing-complete
   workflow language.

---

## 2. The spine: output modes

The organizing axis of the fleet is **not** a capability ladder. Capability, model
power, scope, and authority are independent routing concerns. The output boundary asks one simpler
question:

> **Does the delegated artifact carry independently reproduced oracle evidence?**

The answer selects one of two output modes. It does **not** grant authority to modify the real tree;
every delegated result still requires chair review.

### 2.1 Evidence contract

A phase declares an **invariant**: the property its output must satisfy (e.g. *"behavior-preserving
refactor of `X`"*, *"adds passing coverage for case `Y`"*, *"removes diagnostic `D` without changing
behavior"*). The harness records concrete evidence, not a global completeness label:

- which registered oracle ran, with which arguments and version;
- the exact property that oracle checks;
- pass/fail and infrastructure status;
- linked deterministic risk checks and whether they fired;
- known gaps relevant to the declared invariant.

For example, `tidy-clean` proves that the selected diagnostic is absent. It does not claim behavior
preservation; that gap is recorded for chair review. This concrete contract avoids turning a subjective
`complete | partial` label into unused safety ceremony.

### 2.2 The two output modes

```
oracle evidence   ──▶   output mode   ──▶   what the output means
───────────────        ───────────       ───────────────────────────────────
present                PROPOSAL          harness-reproduced checks and residual-risk
                                         notes accompany the artifact
absent                 ADVISORY          the output is an unvalidated draft; the chair
                                         reviews, rewrites, or discards it
```

- **PROPOSAL** is the safe default for the large middle. The oracle did real work (a green `test-all`
  is meaningful), but the output remains a reviewable proposal. Stronger or more targeted evidence
  may reduce the chair's review burden; it does not remove the review step. **This boundary —
  "validated but not accepted" — is the single most important safety property in the system.**
- **ADVISORY** exists so that oracle-less-but-useful work (docs, comments, renames, exploratory
  cleanups) can still be *drafted* by a cheap model instead of consuming the chair's writing time. The
  output carries no certification; the chair's review is its acceptance gate.

### 2.3 Why this is the spine

- It directly encodes axiom 2 (evidence ≠ verdict) and axiom 3 (delegation ≠ acceptance): every mode
  permits delegation, while none bypasses chair review.
- It dissolves the old fusion. "Capability class," "scope," "model power," and "recipe" are no longer
  smeared along one ladder — they become independent registries (§3). The spine governs what evidence
  accompanies the result, while the chair owns the acceptance decision.
- It makes the dangerous move impossible by construction: there is no fleet path from "oracle passed"
  to "applied to the real tree." Every delegated artifact is a proposal or advisory the chair must
  inspect.

---

## 3. Object model

The system is a small set of **mechanisms** (code, fixed in number) operating over a set of
**registries** (data, open-ended).

```
                              ┌──────────────────────────────────────────┐
   CHAIR  (single in-loop frontier; policy + judgment + synthesis)        │
   └─ decides what to spawn · authors phase intents · reviews PROPOSAL/   │
      ADVISORY · synthesizes J-work · applies accepted changes · vetoes   │
                              └─────────────────────┬────────────────────┘
   ── MECHANISMS (code; the count never grows) ──   │ phase intents
                                                    ▼
   SCHEDULER       DAG ordering · concurrency · infra retry · graph
                   expansion for search. Sees opaque phase nodes; knows no task semantics.
   ENGINES ×3      gate · synthesis · search — the only convergence interpreters.
   SUBSTRATE       uniform full-repo snapshot + path-view + hermetic oracle isolation.
   ── REGISTRIES (data; grows by rows, never by code) ──
   AGENTS          {id → (harness, model, default-authority)} + measured competence/rejection rate.
   ORACLES         {id → validation fn} + checked property + known gaps + isolation/risk-check links.
   AUTHORITIES     {id → subtractive permission clamp}  (read-only | writable-copy | mutate) × net.
   BINDINGS        {task-kind → default (agent, engine, oracle, authority, params)} + override rules.
   ESCALATIONS     {failure-reason → next action}  (the explicit, non-ordinal escalation policy).
```

Mechanisms: **4** (Scheduler, the Engine set, Substrate, plus the Chair which is the one irreducible
frontier). Registries: **5**. That count is the current decomposition; it
does not grow when the fleet learns a new model, a new check, or a new task kind.

The fleet scheduler and engines use the shared `ao_async` coroutine runtime from `lib/async`. That
runtime is deliberately below `ao_app_runtime`: it contains only the callback executor abstraction and
Boost.Asio coroutine primitives, so command-line tools can use `ao::async::Runtime` without linking
GTK, application services, audio initialization, or app-specific state.

---

## 4. Substrate (uniform environment)

**Principle:** every agent — proposer, council member, search worker — runs in an *identical* rich
environment. There is no "cheaper tier gets a poorer workspace." Uniformity is what makes a model's
failures *attributable to the model* rather than to blindness.

### 4.1 What every workspace is

- A **full-repository snapshot** (copy-on-write; btrfs/overlay or equivalent), including complete
  `.git` history, all sibling files, and inherited build caches.
- **Mounted at the real repository path** inside a mount namespace (bubblewrap or equivalent), so that
  absolute-path-bearing toolchains (e.g. a `compile_commands.json` full of absolute paths) resolve to
  the *copy*, not the real tree. This single trick eliminates the "validation silently analyzes the
  unmodified real tree → false green" failure that historically forced workers into impoverished
  single-file sandboxes.
- **All tools available** (build, test, lint, `git`, `rg`), so an agent can do real reconnaissance and
  real validation against real context.

### 4.2 Capability, not mandate

The rich substrate is a **capability the substrate layer always offers**, *not* a cost every phase must
pay. A binding declares `context-view: minimal | full`. A three-line lexical fix that needs one file
and its includes may request `minimal`; a refactor that must read callers requests `full`. The
*mechanism* is uniform (one substrate, one snapshot/mount path); the *cost* is a per-phase choice,
measured and defaulted per task-kind, not imposed globally. Snapshots are COW and cheap to create; the
real cost to watch is mount-namespace setup and writable-layer growth under wide fan-out, so
`context-view` and fan-out width are the cost levers the scheduler accounts for.

> **Known gap:** `context-view` is parsed, intersected, and recorded in the route key, but the
> namespace runner does not yet enforce `minimal` by narrowing the mounted view; both values currently
> provision the full snapshot. The field participates in routing and competence keys so that enforcing
> it later does not invalidate recorded statistics.

### 4.3 Oracle hermeticity (the worker→oracle boundary)

This is a hard requirement, not an optimization, and it is the most commonly missed one.

When a phase both *edits* (worker) and *checks* (oracle), the oracle's verdict must be **causally
independent of everything the worker did except the source change itself.** Concretely:

- The oracle validates from an **immutable base** (a read-only snapshot the worker never touched) plus
  **only the harness-extracted patch**. Nothing else crosses the worker→oracle boundary.
- The oracle's build uses a **cache namespace the worker cannot write.** A shared writable build cache
  between worker and oracle is a poisoning channel: a worker with write access to the cache the oracle
  trusts can make a broken source compile "clean." Inherited caches are allowed only **read-only** to
  the oracle; oracle cache *writes* go to a throwaway namespace.
- The oracle runs in a **fresh build directory** it owns.

Anything the worker emits other than the patch — narration, self-reported diffs, self-run test logs —
is evidence for the chair, never input to the oracle.

### 4.4 Namespace binds (implementation notes)

- **Agent processes bind the host `$HOME`.** The mount namespace is a path virtualizer, not a security
  jail (§17): vendor model CLIs need their host credentials and configuration to authenticate, so
  worker sandboxes bind `$HOME` while the repository view stays a snapshot mounted at the real path.
  This is part of the trusted-roster assumption of §5.
- **Oracle processes do not bind `$HOME`.** Oracles run deterministic local toolchains and get neither
  a host home nor agent credentials.
- **Oracle build tree.** `/tmp` inside the namespace is a tmpfs, so the harness binds a
  host-persistent, per-phase build directory (`<out>/.oracle-build/<phase-id>`) at `/tmp/build` inside
  the oracle namespace. The worker never sees this directory; the first oracle invocation configures
  and builds it, later invocations build incrementally.
- **ccache.** The host ccache directory is bound into the oracle namespace (via `CCACHE_DIR`) so
  repeated oracle builds stay cheap. Because trusted-roster workers could in principle reach the same
  host cache through their `$HOME` bind, this is a declared trade-off of the trusted roster, not a
  hermeticity proof; the untrusted-model profile of §5/§17 removes it.

### 4.5 Real-tree canary

Before and after any fan-out, the harness fingerprints the real repository (content + file mode +
symlink targets, excluding designated noise dirs). Any change to the real tree during a delegated
phase aborts the phase. This catches an agent that escapes its namespace and edits the real tree
directly. It *detects and discards*; it does not *prevent* — prevention is the authority layer's job
(§5). The chair applies an accepted proposal only after the delegated phase has ended.

---

## 5. Authority (per-phase permission clamp)

The substrate is *capable* of everything; **authority subtracts** from that capability per phase. It is
the security and blast-radius axis, strictly separate from environment.

| Authority | May read | May write | Network | Who uses it |
|---|---|---|---|---|
| **read-only** | full snapshot | nothing | harness profile | council members; reviewers; search probes that only observe |
| **writable-copy** | full snapshot | its own copy only | harness profile | proposers; gate workers; search workers that mutate |
| **mutate-real-tree** | real tree | real tree | as needed | **only the chair, outside delegated phases** |

- Authority is **subtractive and intersected.** Effective authority for a phase is the intersection of
  all clamps that apply (binding default ∩ engine-role requirement ∩ phase override). It can only ever
  get *more* restrictive, never less.
- Network policy is orthogonal to filesystem authority. Deterministic local phases run network-off.
  Authenticated cloud-model CLIs require vendor egress and credentials, so the trusted roster uses an
  explicit net-on profile; this is a declared exfiltration and supply-chain trust assumption.
- **Trusted vs untrusted roster.** For a *trusted* vendor roster, namespace confinement + the canary
  (§4.5) are sufficient. To admit *not-fully-trusted* models, authority must additionally provide a
  hard sandbox: bind-mounts restricted to the snapshot, no host `$HOME` exposure, and either no network
  or narrowly brokered egress. This hard-sandbox profile is a first-class authority entry, and it
  carries real engineering cost — it is the one part of the system that is genuinely expensive (§17).
  A fully network-isolated worker must be a local model; an untrusted cloud model requires egress
  restricted to its vendor endpoint, with credentials held outside the worker.

---

## 6. Oracles (first-class validation functions)

An oracle is the load-bearing source of mechanical evidence. It does not replace chair judgment.

### 6.1 What an oracle is

A **registered, named, deterministic** function `O(base, patch) → pass | fail`, plus metadata. Oracles
are referenced by **id**, never by arbitrary command string; their arguments are **enumerated or
structurally typed**, so the injection surface never moves from the command to the argument. The
command lines themselves are **code-owned**: the registry selects a runner id, typed arguments, and an
optional `timeout-ms`, while the engine builds the argv — the measurement apparatus is never assembled
from configuration strings.

### 6.2 Hard properties (all required)

1. **Independently re-runnable by the harness** on a harness-extracted patch (axiom 4).
2. **Isolatable** — runs hermetically (§4.3) so its verdict reflects only the patch.
3. **Self-protecting ("ruler protection")** — an oracle must refuse to certify any change that edits
   *its own measurement apparatus*. The build system, the oracle definitions, the scripts that run
   them, and the skill/spec files that define conventions are forbidden edit surfaces for delegated
   work, because a worker that can edit the ruler can make any change measure "clean." The ruler set is
   layered: the engine hardcodes the non-negotiable self-protection core (`tool/fleet/`,
   `config/agent-fleet.yaml`) and rejects every `CMakeLists.txt` by basename at any depth, so a patch
   to the registry cannot disarm the guard; the remaining base set (build and format scripts, lint
   configuration, CI/skill surfaces) is the registry's top-level `ruler-paths:`, merged with each
   oracle's own `ruler-paths` (which also feed that oracle's version fingerprint).
4. **Honest about exit semantics** — a tool/config failure is *not* a clean pass; only a genuine green
   counts.

### 6.3 Evidence metadata

Each oracle records the exact property it checks and its known gaps. This metadata is
a chair-facing review aid, not an authorization claim. The oracle implementation remains normally
versioned so a proposal can identify which check actually ran; no separate completeness approval or
version-pinning ceremony exists. A materially changed oracle starts a fresh competence sample (§13).

### 6.4 Risk-oracles (making the residual-risk seam concrete)

The gap between what an oracle checks and the declared invariant is **residual risk**. Where that gap
can be measured deterministically, it is represented by a second registered oracle of a distinct kind
— a `risk-oracle` — linked to the primary validation oracle. Examples: `assertion-delta` (did any test assertion change?), `coverage-delta`
(did the change touch lines no test exercises?), `signature-delta` (did a public signature change?).

- If a validation oracle passes but its linked risk-oracle fires, the phase is **escalated from
  ordinary chair review toward a heavier review** (for example, a council for high stakes).
- If no risk-oracle can measure a known gap, the gap remains an explicit chair-review note. The system
  never silently assumes it is absent.

This keeps the D-vs-J leak from cracking: the "is this proposal actually safe" judgment is either a
*registered deterministic risk-oracle* (stays mechanical) or an *explicit escalation to judgment*
(honestly J), never a hand-wave.

---

## 7. Agents

An **agent** is a leaf: `(harness, model, default-authority)`, plus a **measured competence profile**.

- **No ordinal rank.** Agents are not "tier 1/2/3." They carry *measured* statistics per route (§13):
  fix rate, chair rejection rate, scope-violation rate, latency. Selection consults these, it does not
  consult a hard-coded ladder.
- **Harness adapter is encapsulated here.** Each agent knows its vendor's headless invocation, its
  read-only mode, its prompt-delivery channel, its rate limits. This is the cross-vendor **lowest
  common denominator**: orchestration cannot depend on any one vendor's subagent API, so the only
  primitives are *non-interactive CLI invocation* + *file-based handoff*. Engines and the scheduler
  never see vendor specifics.
- **Cross-vendor by default** where diversity matters (notably synthesis councils): a same-vendor panel
  defeats the purpose.

---

## 8. Convergence engines (exactly three families)

A convergence engine is the interpreter for *how a phase reaches a result*. There are **three** — no
more, no fewer — distinguished by their **convergence signal**. The worker's behavior *inside a step*
(an agent looping build→fix→build) is **opaque to the engine** and is *not* a fourth family; engines
classify *fleet-level* convergence, not intra-agent execution.

### 8.1 Gate-convergent (signal: a binary predicate)

```
propose(N agents ∥) → harness-extract patches → static guard (scope/churn)
                    → rank (deterministic; semantic tie-break only on a true tie)
                    → run ORACLE on top-K when configured
                    → PROPOSAL | ADVISORY | retry≤K | escalate(per failure reason)
```

- Parameters: `fanout` (parallel proposers), optional `oracle`, `risk-oracle`, `K` (validation budget), guard
  caps, `context-view`.
- Produces a PROPOSAL with concrete oracle evidence, or an ADVISORY when no oracle is configured.
- `oracle: none, fanout: 1` is the degenerate draft form for simple oracle-less work. It is a parameter
  choice inside this engine, not a fourth execution path.
- Covers the entire "produce a change that a check certifies" space: a wide-fanout lint fix and a
  single-proposer plan implementation are the **same engine** at different parameters. Iteration across
  rounds (feeding a failure log back) is a parameter of this engine, not a new family.

### 8.2 Synthesis-convergent (signal: adversarial cross-examination + chair synthesis)

```
blind-draft(N, no peer context) → cross-challenge(see peers, critique) → self-revise
                                → CHAIR synthesizes the single answer (always; the irreducible step)
```

- Parameters: `depth` (draft-only / +challenge / +revise), `roster`, `quorum`.
- For **absent**-oracle judgment work: plans, reviews, architecture, error-contract choices,
  root-cause conclusions. Produces a chair-owned artifact (a plan, or a review verdict). Never
  applies itself; its product is judgment, consumed by the chair.
- The convergence is *diversity then adversarial pressure then trusted synthesis* — there is no gate
  because no oracle exists.

### 8.3 Search-convergent (signal: a continuous objective over accumulating state)

```
seed → workers explore ∥ → results accumulate into shared state (corpus / frontier)
     → score against an OBJECTIVE → scheduler expands the graph toward high-score regions
     → repeat under budget → best aggregate artifact wins → PROPOSAL
```

- Parameters: `objective` (continuous score, not a binary pass), `budget`, `expansion policy`,
  `reduce` (how independent results combine).
- For work that converges on *more/better* rather than *pass/fail*: coverage-guided test generation,
  performance optimization, a map over independent shards with a reduce. The distinguishing feature is
  **accumulating state + a continuous objective + dynamic graph expansion** — which gate-convergence
  (binary, first-acceptable-wins) structurally cannot express without hiding control flow in a worker.
- The *final* artifact still passes a normal oracle to grade its evidence; search decides *which*
  candidate, and the chair decides whether to accept it.

> Three is a claim, and it is falsifiable: a genuinely new fleet-level convergence signal (not an
> intra-agent loop, not a parameter of the above) would justify a fourth engine. Until one is
> demonstrated, the engine set is closed at three, and "the chair does it directly" is the escape for
> anything that fits none.

---

## 9. Bindings & recipes (the data layer — and its hard limit)

This is where flexibility lives, and where the "complexity-controlled" claim is either honored or
betrayed. The discipline: **bindings are data; convergence is code.**

### 9.1 What is data

A **binding** parameterizes a phase without changing any mechanism:

```yaml
task-kind: fix-lint
engine:    gate
agent:     <agent-id>        # overridable: "fix-lint with a strong model" = override this one field
oracle:    tidy-clean
risk-oracle: behavior-risk   # tidy-clean alone does not prove behavior preservation
authority: writable-copy
params:    { fanout: 4, K: 2, context-view: minimal }
```

The **default-binding policy** gives every task-kind a complete default row, so a caller **overrides
one knob**, never assembles five axes by hand. This is the ergonomic that makes the system usable.

Oracle definitions may also declare an optional `timeout-ms`; long-running oracles (sanitizer builds)
raise it explicitly instead of inflating the global default.

### 9.2 What is NOT data (the anti-DSL rule)

A declarative role-graph with conditional escalation, retries, quorum, fixpoint detection, and oracle
composition is a **workflow programming language**. Fixing the number of interpreter components does
*not* fix semantic complexity — it relocates it into config that becomes an untestable, unversioned,
undebuggable second codebase. We refuse this.

Therefore:

- **Convergence control flow stays in the three engines as explicit, tested code.** Bindings select
  and parameterize an engine; they do not *define* control flow.
- The binding grammar is a **small, closed, typed schema** — fixed fields with enumerated/typed values,
  no arbitrary conditions or hooks. It is **versioned**, **statically validated**, and covered by
  **golden-trace tests** (a binding + a mocked fleet must produce a pinned phase trace).
- The break-even test for "data over code" is **measured duplication**, not an asserted mechanism
  count: extract a parameter into the binding schema only when ≥2 task-kinds genuinely share the same
  control flow and differ only in that parameter. A task that needs *new* control flow gets a new,
  tested engine path (or stays chair-only) — never a new conditional smuggled into config.

---

## 10. Scheduler

Pure orchestration over **opaque phase nodes**. It is small precisely because all task semantics live
in engines and oracles, not here.

- Input: a set of nodes, each `{inputs, engine, oracle, authority, deps, params}`. The scheduler does
  not know lint from council.
- Responsibilities: **dependency ordering** (a DAG), **physical concurrency** (fan-out width, rate-limit
  awareness — multi-candidate work hits vendor rate limits before token budgets, so concurrency is
  rate-bound, not token-bound), **infra retries** (a crashed CLI ≠ a failed oracle), and **dynamic graph
  expansion** for search-convergent phases (§8.3), which is the one place the node set grows during
  execution.
- The scheduler **never makes a judgment**. It routes, parallelizes, and retries. Every verdict
  is an oracle's or the chair's.

---

## 11. Escalation policy (keyed by failure reason, not by rank)

Dropping ordinal tiers removes the implicit "if cheap fails, go up" ladder. We replace it with
something **more precise**: an explicit registry mapping **failure reason → next action**. Different
failures demand different responses, and a scalar "try a stronger model" is often the *wrong* response.

| Failure reason | What it implies | Next action |
|---|---|---|
| oracle failed (red) | the attempt was wrong | retry (same engine) within K; then escalate by reason below |
| no in-scope candidate | the model couldn't produce a usable patch | try an agent with a higher measured fix-rate *for this route*; bounded |
| scope/churn guard rejected | the model overreached the declared scope | **escalate to judgment** — the task may be mis-scoped, not under-powered |
| risk-oracle fired | primary oracle passed but a known gap was detected | escalate ordinary chair review → council review |
| "cannot fix mechanically" signal | the problem is a category change, not a power problem | **escalate to a J-class (synthesis) path**, do *not* throw a stronger implementer at it |
| route breaker tripped | this route repeatedly produced unusable proposals | refuse the route; escalate to chair; demand a postmortem |

- This is **not** smuggled tiers. There is no global "agent A < agent B." Selection within a reason is
  driven by *measured competence for the specific route* (§7, §13), and several reasons escalate to
  *judgment* rather than to *more power* — a distinction a scalar ladder cannot make.
- The action resolved for a failed phase is emitted in its `manifest.yaml` as `escalation-action:`
  (`none` for successful phases), so the chair and skills consume the registry policy directly instead
  of re-deriving it from the failure reason.
- Named **assurance presets** (roughly: "mechanically-validated," "implement-and-propose," "council-judgment")
  may exist as *auditable convenience rows* over these registries, but they are derived policy, **not**
  the core ontology. They name common bindings; they do not define the system.

---

## 12. The Chair boundary

The chair is the one irreducible frontier agent and the only actor that accepts delegated results.
This is intentionally a serial semantic boundary: the current design optimizes generation,
reconnaissance, validation, and review preparation, but does not attempt unattended acceptance.

- Decides **what** to spawn and **authors phase intents** (a small declaration: task-kind + scope +
  invariant + binding overrides — *not* a hand-written diff).
- Performs all **synthesis** (R4 of every council) and all **semantic review** of PROPOSAL and ADVISORY
  outputs.
- Applies, modifies, or rejects proposals using the repository's ordinary development workflow; fleet
  mechanisms do not model Git staging, commits, or merges.
- Holds an absolute **veto** and is the only judgment that is itself ungated.

**The chair review boundary is a deliberate current constraint, not a claim that unattended acceptance
is impossible forever. Any future proposal to remove it requires a separate design and evidence base;
it is not latent in the registries defined here.**

---

## 13. Route competence lifecycle

Route statistics optimize selection and detect poor delegation routes. They never grant authority to
bypass chair review.

### 13.1 The competence key is a versioned tuple

Competence is **not** keyed on the agent alone — that is too coarse (an agent effective for one-line
lint fixes is not thereby effective for broad refactors under the same test oracle). The key is:

```
(agent-id, model-version, harness, engine, oracle-id, oracle-version, authority, scope-risk-class)
```

Any change to any component **invalidates or quarantines** the statistics for that key. Competence
does not transfer across versions.

### 13.2 The lifecycle

1. **Probation.** Every output is chair-reviewed. Outcomes (accept / modify / reject), oracle pass rate,
   scope violations, latency, and cost are recorded against the competence key.
2. **Selection.** Routes with sufficient evidence may become preferred defaults for a task kind. This
   changes which worker is tried first, not what acceptance gate applies.
3. **Breaker.** Repeated scope violations, unusable proposals, or a configured rejection threshold may
   pause a route pending a postmortem. A breaker is a *stop* (do not use this route), distinct from an
   escalation (use another route) — §11 handles the latter.

This discipline keeps the combinatorial space manageable as it grows: a new `(model, oracle, task)`
combination is expressible immediately, while observed competence controls whether it remains useful.

---

## 14. End-to-end safety argument

How `silent-wrong = 0` is preserved, as a chain where every link is independent of the worker:

1. **Generation** is delegated freely (any evaluated agent may draft, in any mode) — generation is not
   a trust surface.
2. **Extraction** is by the harness (`git diff` of the work copy), never the worker's self-written
   diff.
3. **Static guard** rejects out-of-scope / over-churn / ruler-touching patches before any build.
4. **Oracle** runs **hermetically** (§4.3): immutable base + harness patch only + worker-unwritable
   cache. Its verdict cannot be poisoned by the worker.
5. **Output mode** caps what a pass is worth: any oracle-backed delegated change is a PROPOSAL;
   oracle-absent work is ADVISORY. No route skips chair judgment.
6. **Risk-oracle** converts a known oracle gap into either a mechanical check or an explicit
   escalation — never a silent assumption.
7. **Chair review** is required before a delegated change is applied to the real tree or represented as
   accepted.
8. **Canary** detects any real-tree mutation during a delegated phase and invalidates its output.
9. **Competence lifecycle** removes consistently poor routes without weakening the acceptance boundary.

A worker or oracle may still be wrong; the architecture does not present either as sufficient semantic
authority. Their output remains evidence for the chair's decision.

---

## 15. Worked examples (one machinery, many jobs — as data)

Each row is a binding + output mode. **Zero new code** across all of them.

| Job | engine | oracle evidence | mode | agent / fanout | authority |
|---|---|---|---|---|---|
| Fix residual lint | gate | `tidy-clean` + behavior gap noted | PROPOSAL | 4× cheap | writable-copy |
| Fix lint, but tricky | gate | `tidy-clean` + behavior gap noted | PROPOSAL | 1× **strong** (override) | writable-copy |
| Implement a settled plan | gate | `test-all` + `coverage-delta` | PROPOSAL | 1× strong | writable-copy |
| Add test coverage | gate | `test-all` + `assertion-delta` | PROPOSAL | 1× strong | writable-copy |
| Doc comment / rename | gate (`oracle: none`) | none | ADVISORY | 1× cheap | writable-copy |
| Coverage-guided test gen | search | objective=coverage; final `test-all` | PROPOSAL | N× ∥ | writable-copy |
| Perf optimization | search | objective=benchmark; final `test-all` | PROPOSAL | N× ∥ | writable-copy |
| Design / architecture | synthesis | none (absent) | (chair artifact) | cross-vendor roster | **read-only** |
| Root-cause diagnosis | synthesis | none (absent) | (chair artifact) | cross-vendor roster | **read-only** |

The flexibility goal: "lint with a strong model" and "cheap model attempts an oracle-less rename" are
both single-field overrides. Oracle-less drafting is the gate engine's degenerate form, not a fourth
convergence engine.

Sanitizer oracles `test-asan` and `test-tsan` are registered with raised `timeout-ms` values and build
dedicated sanitizer trees. They are chair-selected per intent (an `oracle:` override) rather than bound
to `implement-plan` by default: a full sanitizer rebuild costs tens of minutes, which is the wrong
default price for ordinary plan execution but the right gate for concurrency- or lifetime-sensitive
changes.

---

## 16. Phase lifecycle (a single trace)

```
chair authors a phase intent  (task-kind, scope, invariant, overrides)
   │
   ▼
binding resolution            (default row ∩ overrides → engine, oracle, authority, params)
   │
   ▼
scheduler places the node     (deps, concurrency, context-view cost)
   │
   ▼
substrate provisions          (COW snapshot @ real path; authority clamp applied)
   │
   ▼
engine runs                   (gate / synthesis / search; workers draft in writable copies or read-only)
   │
   ▼
harness extracts + guards     (git-diff patch; scope/churn/ruler static checks)
   │
   ▼
oracle (when configured, hermetic) + risk-oracle
   │
   ├─ oracle pass                      → PROPOSAL  → chair reviews evidence/gaps → apply | modify | reject
   ├─ no oracle configured             → ADVISORY  → chair review → apply | rewrite | discard
   └─ fail / risk-fired / guard-reject → ESCALATION (per failure reason) → retry | judgment | breaker
   │
   ▼
record outcome against the competence key   (feeds route selection / breaker)
```

---

## 17. Open problems & non-goals

- **The untrusted-model hard sandbox is genuinely expensive.** Namespace path-virtualization is a
  performance feature, *not* a security jail (host `$HOME`, network, and environment remain visible by
  default). Admitting not-fully-trusted models requires the restricted authority profile of §5 — no
  network for local models, or brokered/vendor-restricted egress for cloud models, plus minimal
  bind-mounts — and that is real work. For a trusted vendor roster, the canary + read-only/writable-copy
  clamps suffice; this is an explicit, bounded trust assumption, not an oversight.
- **Unattended acceptance is a non-goal for this design.** Every delegated result remains PROPOSAL or
  ADVISORY until chair review. If future evidence justifies revisiting this boundary, it requires a
  separate design rather than an extension flag in the current registries.
- **Search-convergent scheduling is the most complex mechanism** (dynamic graph expansion, objective
  scoring, budgets). It should be built *last*, only when a real search workload (coverage-guided gen,
  perf tuning) justifies it; until then the engine set is effectively two.
- **Chair-synthesis quality is itself unmeasured.** The fleet certifies *delegated* work via oracles
  and councils; the chair's own R4 synthesis and final reviews have no oracle. This is the irreducible
  human-trust core, and evaluating it is a standing, out-of-band concern.
- **Non-goal: a general workflow DSL.** §9.2 is a deliberate refusal. If a future task truly needs
  arbitrary control flow, it becomes a new tested engine path, not a config-language feature.

---

## Appendix A — glossary

- **Invariant** — the property a phase's output claims to establish; oracles are judged relative to it.
- **Oracle** — a registered deterministic function that contributes independently verified evidence.
- **Output mode** — `PROPOSAL | ADVISORY`: whether an artifact carries independent oracle evidence.
- **Risk-oracle** — a second registered oracle that measures a known gap left by the primary oracle;
  fires → escalate.
- **Route** — a versioned `(agent, model, harness, engine, oracle, authority, scope-risk-class)` tuple;
  the unit of competence accounting.
- **Chair** — the single in-loop frontier: policy, judgment, synthesis, acceptance, and veto.
- **Canary** — a before/after fingerprint of the real tree that aborts on mutation during delegation.
