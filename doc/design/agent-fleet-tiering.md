# Aobus: Capability-Tiered Workflow and Cross-Harness Model Routing Design

> Status: design draft v17 / for discussion. Date: 2026-06-07.
> Scope: **general, not bound to any single harness or vendor**. Abstract workflow stages into capability
> classes, then route a heterogeneous agent fleet (Opus / GPT-5.5 / Gemini 3 Pro·Flash / DeepSeek V4
> Pro·Flash / …) by class.

## 0. Settled premises (not open for debate)

- **Low-cost-model token cost is approximately negligible.** The primary cost of this design is not the
  tokens of models like Gemini Flash / DeepSeek Flash, but slow validation time, the frontier's attention
  reviewing patches/reviews, state-management complexity, silent-mis-fix risk, and the per-CLI
  rate/concurrency/latency constraints that candidate-parallelism brings (multi-candidate fan-out hits
  rate limits before it hits a token budget).
- **`run-clang-tidy.sh --fix` does not enter the automated path.** Practice has shown it corrupts files or
  produces unacceptable mechanical changes; v1 uses tidy diagnostics only as input, never auto-replacement
  as a C0 fix mechanism.

## 1. Background: a portable substrate already exists

The Aobus repo is **already on a harness-neutral track**, and this design extends it rather than starting
over:
- **`.agents/skills/`**: a vendor-neutral skill directory (`.claude/skills/` is a symlink to it). The
  skills: `develop-lint-checker`, `diagnose-issue`, `execute-plan`, `improve-test-coverage`,
  `manage-git-flow`, `run-council`, `use-clang-tidy`, `write-unit-test`. (`execute-plan` is the C2
  delegation skill — §12; `run-council` is the C3 council skill — §11.)
- **Single instruction source + multi-harness naming**: `CLAUDE.md` and `GEMINI.md` are both symlinks to
  `AGENTS.md`. One spec, read by multiple harnesses under their own conventional names.
- **The fleet is already on disk**: `~/.claude` (Opus), `~/.codex` (GPT-5.5), `~/.gemini` + Antigravity
  (Gemini 3 Pro/Flash), `~/.config/opencode` (DeepSeek V4).

**The problem**: today every stage — from "add a missing clang-format blank line" to "decide between
`ao::Result` and `std::optional`" — runs in the same frontier loop. Mechanical grunt work dilutes the
frontier's tokens/loop, while cheaper or more suitable models in the fleet (the Flash family, DeepSeek)
sit idle.

**The goal**: abstract stages into **capability classes**, so each class is handled by the most
cost-effective executor — possibly a zero-cost script, possibly Gemini Flash, possibly Opus — with the
whole mechanism **open to participation by any harness**.

## 2. Two core principles

**Principle A: a tier is a capability class, not a specific model.**
Classify by what capability the *work* requires, then use a **routing table** to fill in the fleet's
concrete (harness, model). Swapping models or adding vendors changes only the routing table — not the
skills, not the workflow.

**Principle B: a skill is a portable contract, orchestrated above the harness.**
A single CLI's subagent mechanism (e.g. Claude Code's `Agent` tool) **can only spawn its own vendor's
model** and cannot route cross-vendor. So orchestration cannot depend on any one vendor's tool API; it
must land on the **lowest common denominator across all harnesses**:
- Triggering: each CLI's **non-interactive invocation** (`claude -p` / `codex exec` / `gemini -p` /
  `opencode run`);
- Handoff: a phase packet of **YAML frontmatter + markdown body**;
- Acceptance: **allowlist validation** (fixed command IDs + enumerated args + exit codes), executed in **a
  single already-configured tree** (see §5 "why validation cannot run in an isolated worktree"), not
  depending on any harness's built-in judgment.

## 3. Capability classes (vendor-neutral)

Classified on three criteria: **can a script judge correctness (determinism) / the context scope a
decision needs (local vs global) / the semantic risk of a wrong judgment**.

| Class | Capability requirement | Typical work | Existing skill |
|---|---|---|---|
| **C0 mechanical·deterministic** | No reasoning needed, a script can judge correctness | format, run lint, run tests, build, **diff guard, validation execution, dispatch routing lookup** | the *execution* face of `manage-git-flow`, `build.sh`, `run-clang-tidy.sh` |
| **C1 bounded·mechanical** | Local rewrite with a deterministic acceptance gate as backstop | fix residual tidy diagnostics from a report, lay down test boilerplate/fixtures, generate scaffolding | `use-clang-tidy` (fixing), `develop-lint-checker` (scaffolding), test boilerplate |
| **C2 scoped·implementation** | Write code/tests within a settled design — capability, not novel reasoning | implement a settled plan, fill in a set of edge cases | `execute-plan` (the sole C2 delegation skill; the worker self-loads `write-unit-test`/`improve-test-coverage` for specifics) |
| **C3 frontier·reasoning** | Global/semantic/architecture judgment, where a wrong call is costly | plan, root-cause diagnose, design review, **signature/ABI and semantic-equivalence review**, error-contract choice (§5) | `diagnose-issue`, `code-review`, plan, the matcher design of `develop-lint-checker` |

Your original examples land as: "run lint" = **C0**, "fix lint diagnostics" = **C1**, "review/plan/
diagnose" = **C3**. Splitting "execution" from "fixing/judgment" is the main payoff of this model.

> **Note C0 includes the *execution* of dispatch / guard / validation.** These are deterministic logic,
> merely **executed by the frontier opportunistically** in v1 (see §6); that does not make them C3 work.
> It also shows that extracting the dispatcher out of the frontier (Step E) is not a "downgrade", just
> moving C0 logic out of the reasoning loop.
>
> **C2 is the least-validated layer**: its boundaries with C1 (local-mechanical) and C3 (reasoning) are
> the fuzziest, and a deterministic guard struggles to adjudicate it. Do not invest in C2 before C1 is
> proven; it likely collapses into "large-scope C1" or "narrow-scope C3".
>
> **C3 can run solo or convene a council (§11)**: plan/review can be produced by the chair (the current
> in-loop frontier agent) alone, or by convening a cross-vendor council (draft → challenge → revise →
> chair synthesis). The council is opt-in and expensive (real frontier tokens × members × rounds), used
> only for high-stakes judgments; it has **no deterministic gate** — which is exactly what separates C3
> from C1/C2.

## 4. Fleet routing table (mapping capability classes to concrete models)

Each class gets a **primary + alternate** (cross-vendor), traded off by cost / capability / latency /
context window / tool availability. Example:

| Class | Primary | Alternate | Rationale |
|---|---|---|---|
| C0 | explicit scripts (0 tokens) | — | deterministic work should burn no model; v1 has no always-on hook |
| C1 | Gemini 3 Flash + DeepSeek V4 Flash multi-candidate | Haiku / other low-cost fast models | token cost ~negligible; use multiple candidates to raise the fix rate, but validate only the best post-guard patch |
| C2 | Gemini 3.1 Pro via agy | GPT-5.5 (Codex) / Sonnet | strong coding, can implement to a design |
| C3 | chair (the current in-loop agent) | Claude Opus / GPT-5.5 (high) / Gemini 3 Pro / DeepSeek V4 Pro | deep reasoning, architecture and semantic judgment |

> This table is **externalized into `script/agent/routing.env`** (delegable capabilities use
> `route_<class>_worker` / `route_c3_member_*` functions and their label maps; the C3 chair is the current
> in-loop agent, whose identity does not enter the routing table): swapping models / adding vendors
> changes only this one file, with runner and phase contract untouched. For tests, point
> `AOBUS_ROUTING_ENV` at a mock (replacing only `route_c1_worker`) — which is itself the validation of
> "swap the routing table, leave the rest unchanged". This table is **the only thing that must be
> maintained as the fleet changes**. Skills and phase contract do not change. The default form of C1 is
> not "primary fails then alternate" but **producing multiple candidate patches in parallel**; the
> expensive part is the subsequent validation and C3 review, so the dispatcher/frontier must first filter
> with a deterministic guard and patch ranking, rather than run slow validation on every candidate.

> **Measured headless invocations (Step 0, §9)**: `claude -p` / `codex exec -s read-only` / `gemini -p
> --approval-mode plan` / `opencode run -m opencode-go/deepseek-v4-flash`. opencode defaults to a local
> `ollama/gemma4:31b`, so the cloud model **must be pinned explicitly**; the Pro tier is
> `opencode-go/deepseek-v4-pro`.

> **C3 alternates are no longer doc-only**: routing.env's `ROUTE_C3_MEMBERS` realizes the callable models
> as **read-only council members** (one each of claude / codex / gemini / opencode, §11), each running in
> its vendor's read-only headless mode. If Claude/Opus participates in R1/R2/R3 it is an ordinary member;
> the chair does not draft, only verifies and synthesizes in R4. Swapping vendors / models changes only
> routing.env, with council.sh and the phase contract untouched.
>
> ⚠️ **Isolation limitation**: although members' repo copies achieve content isolation via
> `agent_tree_hash`, all members share one council output directory (`$AGENT_COUNCIL_OUT`). R1's "blind
> draft" isolation currently relies on member models not actively probing other files and on CLI
> tool-call boundaries, rather than hard filesystem isolation.

## 5. Phase Contract (harness-agnostic stage contract)

Each delegable skill gets a uniform header so it can be executed as a standalone stage by **any model of
any harness**:

```
## Phase Contract
- Capability:    C0 | C1 | C2 | C3
- Inputs:        minimal inputs (diff / file list / failure log / design link)
- Scope:         what may be touched, what is forbidden (file list)
- Validation:    a validation_id from the allowlist + enumerated args, exit code is the verdict
- Output:        patch + structured result (YAML/JSON: status / changed_files /
                 validation / escalate_reason / residual_risks) + markdown notes
- Escalate when: on a hit, stop and hand up to a higher class
                 (need to change a public API / choose an error contract / change architecture / validation repeatedly fails)
```

### 5.1 Safety foundation: patch + guard + temporal isolation

A cheap model **never edits the main worktree directly**, only files in an **isolated scratch/worktree**;
**the patch is generated by the dispatcher via `git diff` (scratch vs base), not trusting a model's
self-written unified diff** (§9 Step 0 measured: codex's self-written diff had corrupt hunk headers,
`git apply` reported corrupt; letting the harness generate it eliminates a whole class of failures).
**The robust approach (settled by the Step B field test)**: the worker edits the copy in place inside an
**isolated sandbox cwd** (holding only copies of the target files), and the dispatcher simply `diff`s that
copy — neither trusting the model's self-written diff (codex/ds4f hunks often corrupt) nor relying on
sentinel stdout (an agentic CLI will narrate, or switch to Read/Edit-ing files instead of echoing; gemini
wrote "my fix plan…" into the file, ds4f under an isolated cwd tried to Read the real path and was blocked
by its own permissions into 0 output — all measured). The sandbox + the CLI's own external-dir permission
interception **doubly** guarantee the worker cannot touch the real tree. The safety gates are applied in
this order:

1. **Deterministic guard (before apply, runs on the diff, no build needed)** — see §5.2; filters all
   out-of-bounds candidates;
2. **Candidate ranking (deterministic first, frontier tie-break)**: for candidates that pass the guard,
   rank first by **deterministic proxies** (least churn, fewest files touched, best fit to scope);
   **only when multiple candidates tie on the deterministic dimensions and are hard to separate** does the
   frontier do a semantic ranking (minimal intent, report quality). If it can be ranked deterministically,
   do not burn frontier attention;
3. **Take the repo lock** (only one build-class stage may touch the main tree at a time);
4. **Scope clean check**: the paths a patch touches must have no pre-existing staged/unstaged/untracked
   changes in the main tree;
5. **Apply the top-ranked candidate to the main tree** (which has a configured `/tmp/build/...`), recording
   this patch as the sole rollback unit;
6. **Run Validation** (allowlist, §5.3), exit code is the verdict;
7. **Pass → keep** (hand to C3 review); **fail → reverse-apply this patch to roll back, take the next
   candidate by rank and retry, validating at most K (K small, e.g. 2, capping minute-scale slow
   validation); all K fail or an escalate is hit → escalate to C3**;
8. **Release the lock, clean up the temp worktree (`git worktree remove`)**.

> **No bare `git restore` rollback.** The main worktree may already have user changes; `git restore` would
> wrongly discard state not belonging to this phase. v1 either requires the patch scope to be clean before
> apply, or refuses to run; on failure it only `git apply -R`s **this patch**. If the reverse-apply fails,
> stop immediately and hand to C3/a human — never widen the rollback scope.

> **Operational constraint: target files must be clean/committed first.** The scope clean check will
> (correctly) refuse to touch the user's WIP files — this repo currently has a dozen-odd uncommitted M
> files, so a build-dependent delegated stage that overlaps WIP is refused outright. This is a safety
> feature, not a bug; commit or stash the target files before a pilot. **Benefit**: this repo's build is
> out-of-tree (`binaryDir: /tmp/build`), so validation build artifacts do not land in the source tree, and
> `git apply -R` reverse rollback is clean.

> **Why validation cannot run in an isolated worktree (verified)**: `run-clang-tidy.sh` fixes `BUILD_DIR`
> to `/tmp/build/debug-clang-tidy`, whose `compile_commands.json` entries all point at **absolute paths in
> the main source tree**. Running tidy in a worktree at another path would, per compile_commands, analyze
> **the main tree's unmodified files**, ignoring the worktree's changes — yielding **silent green (false
> pass)**, more dangerous than an error. build/test likewise depend on an already-configured tree.
> Therefore: **where the model edits (it can produce a patch in scratch/worktree) is decoupled from where
> validation runs (it must run in the configured main tree).** Isolation relies on **time** (apply →
> validate → keep/discard, locked to guarantee rollback), not **space** (a separate worktree). Spatial
> worktree isolation only holds for *build-independent* validation (pure format checks, grep-class).

### 5.2 Diff guard: the deterministic part vs the C3-review part (must be separated)

**Deterministic guard (C0, runs on the diff, path + size judgments, done in v1)** — reject and escalate
on a hit:
- the change touches `include/**` (the public header directory);
- the change touches any `*/CMakeLists.txt`, CMake config, `.clang-tidy`, a lint config, or `script/**`;
- the change touches `doc/design/**`;
- a file change appears outside the packet's `scope.files`;
- churn over threshold: changed lines / deleted lines / files touched exceed the packet's caps (a proxy
  for "large-scale reshuffling").

**C3 review checklist (semantic judgment, undecidable by diff, hand to the frontier, do not hang it under
the deterministic guard)**:
- did it change a function/type signature or ABI;
- did it introduce a user-visible behavior change;
- is the change semantically equivalent to the minimal intent of "fix this diagnostic".

**Iron rule (C1/C2)**: edit only within scope → produce only patch/report → pass the deterministic guard →
take the lock and apply to the main tree → pass allowlist validation with exit code 0 → hand to C3 review
→ only then is it settled. On an Escalate hit, stop immediately and do not improvise. A self-reported
`escalate_reason` is still kept, but only as an **extra signal**; the real gates are the deterministic
guard + validation + C3 review.

> **Landed — the C2 proposal executor's scope gate is no longer path-based.** The
> `include/**` row above is a **C1** rule; the **C2 proposal executor** no longer treats a header edit as
> an automatic escalate. Because this repo has no API/ABI-compat requirement, a header is allowed and
> falsified by a **single, fixed full-suite oracle** rather than by path: every proposal — cpp or header,
> core or app — validates against `test-all` (build + run the *whole* core and GTK suites), and the
> packet cannot downgrade it. The runner does **not** select a test subset from the changed paths: the
> whole suite runs in seconds and any change ultimately reaches the app, so a per-change blast-radius
> gate bought complexity, not safety — and running everything closes the gap where a core edit silently
> broke an app-only test. CMake / `.clang-tidy` / `script` / `doc` / `.agents` stay forbidden as the
> oracle's own measurement apparatus (ruler-protection). See §9 Step H and §12.

### 5.3 Validation allowlist

A packet cannot carry an arbitrary shell string, only reference a fixed ID; **arguments must also be
enumerated or structurally validated** — e.g. the `folder` of `validation_id: clang_tidy_folder` may only
be one of `{lib, app, include, test, lint}`, and the `files` of `clang_tidy_files` must lie in the repo
and belong to the packet scope. This way the injection surface does not move from the command string to
the argument string.

> **Landed (Step C per-arg contract, 2026-06-05)**: `validation.env` declares
> `VALIDATION_ARGSPEC[id]="<type> <min> <max>"` for each `v_<id>` (`tidy`=`path 1 -`,
> `test-core`/`test-gtk`=`filter 1 1`, `build-debug`=`any 0 -`). `agent_validation_args_ok` rejects
> mistyped/mis-counted packets by arity + per-arg type (`agent_argtype_re`: `path`/`filter`/`any`, enum
> being literal alternation) **before** running slow validation. `dispatch.sh` passes this
> gate before routing the runner; an id with no declared spec falls back to the `agent_arg_safe` charset
> gate only (backward-compatible, mock-friendly). For `use-clang-tidy/C1`, even if a packet carries
> `validation_args`, they must exactly match the `inputs` file set; otherwise it validates the wrong
> scope, and the dispatcher rejects before the runner. `v_tidy` must also preserve `run-clang-tidy.sh`'s
> non-zero exit status: a tool/config failure is not a clean gate. Implementation pitfall: the two env
> files are `source`d by a loader function, so the arrays use `declare -gA` — an in-function `declare -A`
> would become function-local and be lost on return (plain assignment and function definitions are
> unaffected).

> **Validation is not a cheap exit code**: `run-clang-tidy.sh` must build a plugin + enter nix-shell +
> analyze — it is **minute-scale**; `build.sh` is longer. Every escalate-retry reruns this slow
> validation — the cost model (§10) must count it.

## 6. Orchestration layer: in v1, the frontier doubles as the Dispatcher

Cross-vendor work cannot use any one CLI's built-in subagent, so two neutral primitives are used.

**Phase Packet (the handoff, YAML frontmatter + markdown body)**:
```
---
skill: .agents/skills/use-clang-tidy
capability: C1
scope:
  files:
    - lib/audio/Foo.cpp
  max_changed_lines: 80
inputs:
  tidy_log: /tmp/aobus-phase/<id>/in/tidy.log
validation:
  id: clang_tidy_files
  files:
    - lib/audio/Foo.cpp
artifacts:
  in: /tmp/aobus-phase/<id>/in/
  out: /tmp/aobus-phase/<id>/out/     # the model writes patch + report here
escalate_to: C3
---

Markdown body: background, diagnostic excerpts, allowed/forbidden actions, the expected done signal.
```

**v1 orchestration**: the frontier main loop plays dispatcher directly, without first writing a standalone
program. Note that **dispatch routing lookup, guard, and validation are themselves C0 deterministic
logic**, here merely executed by the frontier opportunistically — not C3 work:
1. Generate the packet and input artifacts (scratch/worktree is only the model's workspace, producing a
   patch, not validating here);
2. The frontier consults the routing table (§4) and invokes one or more low-cost target harnesses in
   non-interactive mode: `claude -p` / `codex exec` / `gemini -p` / `opencode run` (each vendor's flag
   names differ, recorded in the routing table);
3. The cheap model produces one or more **patch + structured reports** (not touching the main tree);
4. The frontier runs the **deterministic guard (C0)** on the diff, filters out-of-bounds candidates, then
   ranks by smallest patch / least churn / report quality;
5. Take the lock → confirm patch scope is clean → apply the best candidate to the main tree → run
   **allowlist validation (C0)**;
6. On failure, reverse-apply this patch to roll back; on pass, the frontier does the **C3 review**
   (signature/behavior/semantic equivalence) → settle or escalate;
7. Release the lock, clean up the temp worktree.

This keeps v1 **harness-independent** while not paying upfront for a standalone dispatcher, concurrent
execution, queues, a lock framework, failure recovery, etc. A standalone dispatcher is a second-phase
need: extract it once the flow is stable and unattended or concurrent runs are required.

**Example: the pre-commit flow (commit-flow)** — each line is annotated with **the capability that step
needs**, all orchestrated by the frontier in v1:
```
C0  script          explicitly run clang-format / build / test                 [0 tokens]
C0  script          run-clang-tidy.sh (changed files, diagnostics only, no --fix) → diagnostic list  [0 tokens]
C1  Flash*          produce multiple tidy-fix patches/reports in parallel in scratch (not touching the main tree)
C0  script/frontier guard + ranking → take lock → scope clean → apply one candidate → validation → on fail reverse-patch
C3  chair           signature/behavior/semantic-equivalence review + write the commit message
```

## 7. Class assignment and refactoring of existing skills

- **manage-git-flow**: push the *execution* of format / validation down into explicit C0 scripts; v1 has
  no hook. The skill degrades to orchestration + commit conventions (still BLOCKING). **Wired (2026-06-05)**:
  the skill's §3 "Mechanical Pre-Commit Delegation" explicitly delegates the mechanical first half (C0
  format + C1 lint via `commit_flow.sh` → `dispatch.sh`), keeping the skill itself **C3** — the commit
  decision / message / semantic review. The wiring obeys two hard constraints: ① commit_flow runs
  clang-tidy, so it is **opt-in** (used only when this commit explicitly includes lint cleanup; the
  default commit path does not run it); ② commit_flow **never commits/stages** — on READY continue the
  Commit Procedure, on NEEDS C3 first handle the escalation packet / guarded path.
- **use-clang-tidy**: C1; add the Phase Contract, with Escalate covering "change a public API / choose an
  error contract / cross-file refactor". **v1 never uses `run-clang-tidy.sh --fix` (see §10, proven to
  corrupt files)**. C0 only runs tidy to produce the diagnostic list; C1 handles only the **clear, local,
  verifiable** fixes among those diagnostics, always through the patch + guard + validation flow. **Two
  iron rules from the Step B field test**: (i) **iterate to fixpoint** — one fix round may surface new
  warnings (measured: fix magic-`7`→`constexpr addend` → then triggers `addend` must be `kAddend`), so it
  must fix → rerun tidy → feed the new diagnostics back → fix again, until 0 warnings or the budget/no
  progress stops it; (ii) **process isolation** — an agentic CLI (opencode `run`) will directly Read/Edit
  worktree files, so the worker must run in a **non-repo isolated cwd**, the patch taken only from sentinel
  stdout, and the real tree changed only by the dispatcher under temporal isolation.
- **write-unit-test / improve-test-coverage**: C3 decides *what to test / edge cases* + C1/C2 lays down
  boilerplate and fixtures.
- **diagnose-issue / code-review**: C3; add clear Inputs (reproduction commands / failure logs / scope).
- **develop-lint-checker**: C3 designs the matcher logic + C1 generates scaffolding/fixtures/CMake wiring.

## 8. Skill cross-harness portability

Each harness discovers skills differently (Claude reads SKILL.md frontmatter; Codex/Gemini mainly read
AGENTS.md; OpenCode has its own convention). v1 does not treat "auto-discovery of `.agents/skills/`" as a
hard dependency:
- the skill body uses **pure markdown + a standard Phase Contract header**, with no vendor-specific syntax;
- the packet can carry an **expanded skill-contract summary** as the primary path, ensuring any headless
  CLI that can read a prompt/file can execute it; that summary is **generated from the source skill
  `.agents/skills/<name>` at dispatch time and version/hash-stamped**, not hand-copied — the single source
  of truth remains the source skill, avoiding packet-copy drift; a useful side effect: carrying the
  contract means a cheap model needs no repo-wide skill read access, shrinking the delegation surface and
  helping sandboxing;
- reuse the existing **`AGENTS.md` symlink** trick: hang a "Skills index + Phase Contract spec" section in
  `AGENTS.md` that each harness can reach through its own `*.md` symlink as an optimization path;
- harness-specific trigger metadata (e.g. Claude's SKILL.md frontmatter) acts as a **thin adapter**,
  uniformly pointing at `.agents/skills/<name>`, with the body not duplicated.

## 9. Implementation roadmap (incremental, independently acceptable/rollbackable)

0. **Step 0: harness headless capability probe (v1 prerequisite blocker, before eval)**. Confirm per
   candidate CLI whether it can (a) run fully headless, (b) point at the repo + workspace, (c) read the
   expanded skill contract in the packet or read `.agents/skills/` directly, (d) write a patch to
   `artifacts.out` without exceeding scope, (e) have auth/rate/context-window available. If any candidate
   fails, remove it from the corresponding class in the routing table (§4).

   **Step 0 measured results (2026-06-04, `/tmp/agent-fleet-pilot/probe.sh`)**: all 4 CLIs installed,
   headless-callable, with the measured non-interactive entries matching the §2 assumption (claude `-p` /
   codex `exec` / gemini `-p` / opencode `run`). Task: headlessly fix one `std::endl → '\n'` and produce a
   patch:
   - **gemini** (default model, 14s): **PASS** — clean appliable diff, in-scope.
   - **claude** (`-p`, 10s): **PASS**.
   - **codex** (`exec -s read-only`, 8s): semantically correct, but the **self-written unified diff had a
     corrupt hunk header** (`@@ -3,5 +3,5 @@` line counts mismatched → `git apply` reported corrupt);
     switching to "model writes the file + dispatcher `git diff`" passed.
     → **The design fix is folded into §5.1: patches are always harness-generated, never trusting a
     model's self-written diff.**
   - **opencode** (default `ollama/gemma4:31b` local model → 180s hang, 0 output; **pinning `-m
     opencode-go/deepseek-v4-flash` returned in 3s**): semantically correct, but like codex the
     **self-written diff hunk header was corrupt** (`@@ -4,5` out of range) → passed under the harness-diff
     contract. → Lesson: the routing table must **explicitly pin a cheap cloud model** for each CLI; the
     default may be a local small model.
   Conclusion: all 4 candidates can enter C1 under the **harness-diff contract** (gemini/claude's
   self-written diffs also work; codex/opencode must rely on harness-generated patches); self-diff is
   corrupt on **2/4 vendors**, further confirming §5.1's "patches always harness-generated".
1. **Step A: manual eval (the trust-boundary gate)**. Pick 5–10 real `run-clang-tidy.sh` diagnostics and
   manually feed them to the candidate cheap CLIs via a uniform eval packet. Low-cost-model tokens are
   approximately free, so eval is not about proving "worth invoking" but about proving "can it enter the
   automated C1 route and reduce frontier hand-writing". Record four metrics and set hard bars:
   - **fix rate ≥ 80%** (fixed correctly and passing validation);
   - **small-sample silent wrong = 0** — in the 5–10 pilot cases, no case may pass validation (tidy clean +
     build green) while still being semantically wrong. Note 0/N is not "low" (rule of three: 0/10 at 95%
     confidence still allows a true rate of ~26%), so a small-sample 0 is only an **entry gate**; the real
     risk rate is established by later rolling statistics — and the rolling statistics must pair with a
     **circuit breaker**: the **first** silent-wrong in production immediately pauses that candidate's C1
     route and triggers a postmortem, not just a dashboard;
   - **escalation rate ≤ threshold** — if most diagnostics must escalate to C3, the C1 layer saves nothing;
   - **out-of-scope count = 0** (being stopped by the deterministic guard is not a failure, but the model's
     tendency to exceed bounds should be tracked).
   Any unmet bar → pause that candidate's C1 route, do not invest in contract/dispatcher infrastructure.

   **Step A measured results (2026-06-04, seeded 9 real diagnostics into `lib/tag/Open.cpp`, harness-diff
   contract, 2 candidates)**:
   - **DeepSeek V4 Flash** (`opencode-go/deepseek-v4-flash`): **9/9 cleared, 0 silent-wrong, in-scope** —
     and **idiomatic for Aobus**: `int→std::int32_t` plus adding `<cstdint>`, `std::endl→'\n'`, magic
     `7`→`constexpr kAddend` (k prefix), merging an optional declaration + `.has_value()` into one
     `if (const auto optX = std::optional<...>{}; optX)` (clearing use-if-init / const-correctness /
     local-init / optional all at once). → **C1-usable, primary.**
   - **Gemini 3 Flash** (`gemini-3-flash-preview -p`): **failed, but a contract problem, not capability**.
     It ignored the "output the file only" constraint, first spat out a "my fix plan…" passage and tried
     the `read_file` tool → the file was polluted with narration, the first line `I have analyzed…` →
     compile error.
   → **Design fix (folded into §5.1)**: whole-file output must be fenced between explicit **sentinels**
   (`<<<BEGIN_FILE>>>`/`<<<END_FILE>>>`), the harness taking only what is between; for agentic headless
   CLIs (gemini `-p`) also pin a "pure transform, no narration/no tools" invocation form and retest.
   **Net conclusion**: cheap-model C1 mechanical fixing is **viable** on Aobus (DeepSeek Flash cleared a
   single case fully and idiomatically); the bottleneck is not model capability but **output-contract
   robustness**. Next: add the sentinel contract, expand to multiple cases, run rolling silent-wrong.

   **Cross-vendor second candidate = Gemini 3.5 Flash via native `agy` (re-enabled 2026-06-07)**:
   Native `agy` runs directly in the sandbox CWD, just like `opencode`. This replaces the old 
   `steam-run agy -p` workaround which suffered from "isolation escape" to the real repo. 
   Native `agy` is safe and fast in a standard `/tmp` sandbox.
2. **Step B: `use-clang-tidy` Phase Contract pilot**. After eval passes, add the Phase Contract; **no
   `--fix`**; C0 only produces diagnostics, C1 only handles the clear, local, verifiable tidy fixes.

   **Step B measured results (2026-06-04, `/tmp/agent-fleet-pilot/lint_phase.sh`, seeded 9 diagnostics into
   `lib/tag/Open.cpp`, worker=ds4f)**: ran the end-to-end C0 diagnostics → C1 (ds4f, sentinel output) →
   harness-diff → deterministic guard → temporal-isolation apply → rerun tidy → keep/rollback. Two fixes
   that only a real run exposes:
   - **Iterate to fixpoint**: v1 fixed magic-`7` into `constexpr addend` in one round, which then triggered
     the `addend→kAddend` naming warning (1 residual); v2 changed to a fix → rerun → feed-back loop, **0
     warnings after 1 fix round, FIXPOINT** (ds4f got it in one round after the "constant kCamelCase" hint).
   - **Process isolation (more critical)**: v1 ran opencode in the repo cwd, and `worker.err` showed it
     `Read`+`Edit lib/tag/Open.cpp`, **directly editing the real file** — a prompt-level "output patch only"
     cannot stop an agentic CLI. v2 confined the worker to an isolated `/tmp` cwd, took the patch only from
     sentinel stdout, with the real tree changed only by the dispatcher under temporal isolation and rolled
     back on failure; a rerun confirmed the repo stayed clean throughout.
   **Landing (end of Step B)**: the runner went into **`script/agent/lint_phase.sh`** (repo infrastructure,
   not skill content); the skill `use-clang-tidy/SKILL.md` adds a "Phase Contract — C1 delegation" section
   referencing it (skill = portable contract, runner = execution mechanism). The worker mechanism finally
   settled as **sandbox-copy in-place edit + diff the copy**: sentinel-stdout in an isolated cwd was
   blocked into 0 output by opencode's external-dir permissions and failed, so switching to sandbox-diff
   converged stably to 0 warnings in 1 round.

   **Step C/D hardening field test (2026-06-04, `script/agent/{routing.env,common.sh,lint_phase.sh}`)**:
   hardened the pilot runner into reusable infrastructure, landing the four concrete items of Step C/D:
   - **Routing externalized**: routing moved from the runner into `routing.env` (`route_c1_worker` = ds4f
     via opencode); `common.sh` provides
     `agent_load_routing`/`agent_repo_lock`/`agent_harness_diff`/`agent_emit_packet`/`agent_guard_path`.
   - **Repo lock (Step D)**: `agent_repo_lock` uses `flock` to serialize all tree-mutating stages; a
     concurrent second instance exits with code 4 after the `AGENT_LOCK_WAIT` timeout, preventing two
     phases from overwriting each other.
   - **Multi-file scope**: accepts multiple files, or `--changed` to derive the changed C++ set from `git
     status`; runs fixpoint and rollback independently per file, summarizing kept/escalated, with any
     escalate → process exit 2.
   - **C3 handoff packet (Step C embryo)**: each escalation (forbidden path / no progress / no-op / churn
     over limit / rounds exhausted) writes the residual diagnostics + rejected patch into
     `escalate/<file>.packet.md` for the frontier reviewer; the real tree is rolled back first, then the
     packet is written.
   - **deterministic guard** extended to include `.agents/**`; churn/round budgets unchanged.
   Validation matrix: ① mock-good single file → 9 diagnostics converge in 1 round FIXPOINT exit 0, tree
   restored; ② multiple files (allowed + forbidden) → 1 kept / 1 escalate, packet written, zero changes to
   the forbidden file, exit 2; ③ mock-noop → no-op detected → rollback + escalate; ④ `--changed` clean tree
   → nothing-to-do exit 0; ⑤ lock contention → exit 4; ⑥ **real ds4f** via routing.env ran through, cleared
   in 1 round exit 0.

   **Step C landing field test (2026-06-04, `script/agent/{validation.env,dispatch.sh}` + `common.sh`
   packet/allowlist)**:
   - **Machine-readable packet schema**: the Phase Packet is defined as **YAML frontmatter + markdown body**
     (`schema: aobus-phase-packet/v1`; fields `kind/skill/capability/validation/escalate_to/inputs[]`).
     `agent_emit_packet` produces a frontmatter-bearing escalation packet; `agent_packet_scalar`/
     `agent_packet_list` parse it. **Inbound requests and outbound escalations share one schema.**
   - **validation allowlist**: `validation.env` registers the allowed validations as `v_<id>` functions
     (`tidy`/`build-debug`/`test-core`/`test-gtk`); a packet's `validation:` may only be one of those
     **IDs**, never an arbitrary shell string. `agent_validate <id> [arg...]` checks the ID exists + the
     args are safe (`agent_arg_safe`: reject flag injection `-*`, path traversal `..`, shell metacharacters;
     allow `[],:` to support Catch2 tags) and invokes with **quoted positional parameters** — args never
     enter shell parsing, closing the injection surface.
   - **per-arg enum/type contract (Step C wrap-up, 2026-06-05)**: the allowlist registers not just the ID
     but also a per-validation arg contract `VALIDATION_ARGSPEC` (`<type> <min> <max>`).
     `agent_validation_args_ok` rejects mistyped/mis-counted packets by arity + per-arg type before running
     slow validation (feeding a Catch2 filter to `tidy`, or a file path to `test-core`, or a wrong arg
     count), narrowing the injection/mis-route surface from charset to type. Fixed one real bug: the two
     env files are `source`d by a loader function, so an in-function `declare -A` becomes function-local
     and is lost on return → changed to `declare -gA` (plain assignment and function definitions are
     unaffected, which is why it had not surfaced earlier).
   - **thin dispatcher** (§6): `dispatch.sh <packet>` reads the packet → validates the contract (the
     capability has a runner, the validation is in the allowlist, inputs are safe) → routes to the runner
     (`use-clang-tidy/C1` → `lint_phase.sh`) → **independently** reruns the gate with the allowlist (not
     trusting the runner's self-report) → keep / escalate. It is itself C0 logic, no model.
   Validation matrix: ① packet parsing (scalar+list) correct; ② injection case `validation: rm -rf /` →
   reject, zero tree change; ③ flag-injection input `--all` → rejected before the runner; ④ unregistered
   `write-unit-test/C2` → escalate; ⑤ **real ds4f via dispatch end-to-end**: round1 fixed 9 but surfaced a
   new include-cleaner warning → round2 added `<cstdint>` → round3 cleared **FIXPOINT (2 rounds)**, the
   independent `v_tidy` gate passed → PASS exit 0 (confirming the necessity of "can't fix in one round"
   iteration).

   **commit-flow chain field test (2026-06-04, `script/agent/commit_flow.sh`)**: wired §6's commit-flow
   into one C0 orchestration: `C0 clang-format (changed C++)` → `C1 lint phase (generate a Phase Packet for
   the changed set → dispatch.sh, fix to fixpoint)` → `C0 dispatch's independent tidy gate`. **Iron rule:
   commit_flow never commits** — no `git commit/add/checkout/reset/stash`; on passing it only prints the
   handoff summary, leaving the commit decision / message / semantic review to C3 (`manage-git-flow`). It
   holds no repo lock (format is fast and idempotent; the heavy work's serialization is locked by the
   `lint_phase` it calls, avoiding a parent/child same-file lock deadlock). Forbidden paths (e.g. editing a
   header under `include/**`) go straight to C3-only and do not enter C1. Measured: ① a tree with only
   non-C++ changes → no-op exit 0; ② mock-seed 1 change → format + generate packet + dispatch fix to
   fixpoint + gate pass → **READY FOR C3 exit 0**, file restored; ③ **real ds4f on a real change**: format →
   2-round fixpoint (again fix → include-cleaner → add `<cstdint>`) → gate pass → `git status` still `M`
   (the change kept, formatted and lint-clean, uncommitted) → hand to C3.

   **Regression coverage** (six **offline-deterministic** suites, 208 assertions total, no model / no
   clang-tidy, all in CI):
   - `test/integration/agent/run_agent_fleet_test.sh` (63 assertions): arg sanitizer, path guard,
     validation allowlist reject paths + id normalization (hyphen→underscore), **per-arg contract**
     (`agent_argtype_re`'s path/filter/any typing + `agent_validation_args_ok`'s arity/type, against real
     specs), harness-diff churn counting, **candidate ranking** (`agent_rank_candidates` stable-sorts by
     "fewer files → less churn → id"; `agent_patch_files` counts `+++` headers), Phase Packet schema
     emit→parse round-trip (including validation_args and body).
   - `test/integration/agent/run_lint_fanout_test.sh` (11 assertions): **end-to-end** runs the real
     `lint_phase.sh`, mocking the worker via `AOBUS_ROUTING_ENV`, the slow tidy via `AOBUS_LINT_TIDY`, and
     the target tree at a temp tree under `AOBUS_AGENT_REPO`, deterministically validating Step D's
     multi-candidate path: ranking selects the low-churn surgical candidate (even if it starts **later** in
     the fan-out) over a correct-but-sprawling rewrite, all-no-op → escalate + packet + tree restored,
     churn over limit → escalate, an old routing (no candidate array) falls back to a single worker.
   - `test/integration/agent/run_dispatch_test.sh` (22 assertions): **end-to-end** runs the real
     `dispatch.sh` (§6), with a mock route + mock allowlist (`AOBUS_VALIDATION_ENV`) + temp tree. Covers the
     PASS path and **every** reject/escalate branch: a non-allowlist validation (tree untouched), missing
     required fields (exit 64), an unsafe input path (traversal), a (skill, capability) with no registered
     runner → escalate, **Step C's mistyped-arg upstream rejection** (a filter fed to tidy → rejected before
     the runner, tree untouched), and **the critical independent gate** — the runner self-reports rc 0 /
     keeps changes, but the dispatcher's own allowlist gate is red → still escalate (never trust the
     runner's self-report); and verifies `validation_args` rather than `inputs` is fed to the independent
     gate.
   - `test/integration/agent/run_test_phase_test.sh` — **removed 2026-06-07** together with the narrow
     test-augment C2 route (`test_phase.sh`). The C2 proposal executor is covered end-to-end by
     `run_c2_proposal_phase_test.sh`, and the dispatcher's retirement of the test-C2 route (a `…/C2`
     packet now escalates with "no runner registered") is pinned in `run_dispatch_test.sh`.
   - `test/integration/agent/run_commit_flow_test.sh` (15 assertions): **end-to-end** runs the real
     `commit_flow.sh` (§6's commit chain) on a temp **git** tree (`clang-format` via a PATH stub),
     validating format → split off guarded → C1 lint via dispatch → handoff. The key safety property:
     **commit_flow never commits / stages** (even on a full pass, the commit count is unchanged, the index
     is empty, changes remain in the worktree); also covers no-change → no-op, a guarded path in the change
     set → NEEDS C3, a lintable that cannot converge → C1 escalate → NEEDS C3.
   - `test/integration/agent/run_council_test.sh` (49 assertions): **end-to-end** runs the real
     `council.sh` (§11), with members mocked via `AOBUS_ROUTING_ENV` (behavior switched by `COUNCIL_MUTATE`
     / `COUNCIL_MUTATE_R2` / `COUNCIL_FAIL` / `COUNCIL_SILENT` / `COUNCIL_ROSTER`) + a temp tree. Covers the
     happy path (four members → full dossier, quorum ok), the **read-only canary** (a member that edited its
     own copy is discarded + attributed to the specific member, others survive), **R1 blindness** (the R1
     prompt has no peer text, R2/R3 do), bad mode → reject (64), unsafe input → reject (3), single member →
     quorum degraded + skip challenge/revise, a silent member → recorded absent and continues with
     survivors, plan/review picking the right prompt template; **added after the self-audit fixes**: a
     member exiting non-zero → discarded, not seated; an R2-phase violation → quarantined entirely (not even
     its R1 draft appears), late-quarantine updates metadata (drafts/quorum); **added after the second
     round of fixes**: `validation:` field rejection (64), OUT-in-repo rejection (3), all-members-violate
     rejection (2), repo staging failure rejection (3).
   The end-to-end lint/dispatch/commit/test chains are separately validated against **real workers** (see
   above); the council's real-frontier validation is an opt-in manual smoke (the frontier is not in CI).

   **C2 test phase landing field test (2026-06-05, `script/agent/test_phase.sh`, worker=codex/GPT-5.5)**:
   the first generalization of Step E.
   - **Structural finding (the value of eval-first)**: Aobus tests are **explicitly registered** in
     `test/CMakeLists.txt` (`add_executable(ao_test …)`, no glob), so **creating a new test file** must edit
     CMakeLists (a guarded path) → that is **C3** work, not C1/C2. What can be cleanly delegated to C2 is
     **augmenting cases in an already-registered existing test file** (validation needs no CMake change).
   - **C2 eval**: gave codex an **upstream-settled test plan** (add a SECTION to `Base64Test.cpp` covering
     the Base64 alphabet `+`/`/` (62/63)), sandbox-isolated + harness-diff, and after apply
     `run-tests.sh --core [base64]` really compiled and ran: **passed in one round, in-scope (only +7
     lines), zero out-of-bounds files, idiomatic style** (27 assertions all passed).
   - **runner + orchestration**: `test_phase.sh` is a **packet-driven** C2 runner (richer than lint:
     `inputs[0]`=the existing test file, `validation`+`validation_args`=a Catch2 filter, body=the test
     plan); the iteration signal is "build+run passes", and on failure it feeds the compile/test output
     back to the worker (round budget). `dispatch.sh` adds the route `improve-test-coverage/C2 →
     test_phase`, and generalizes the independent gate to "use `validation_args` if present, else inputs".
     `improve-test-coverage/SKILL.md` adds a C2 Phase Contract.
   - **End-to-end field test**: `dispatch <packet>` → test_phase → codex writes the SECTION → the inner
     `v_test_core [base64]` build+run passes → the independent gate rerun passes → PASS exit 0, the file
     kept as a reviewable change. **The chain found and fixed one real bug**: the validation id used a hyphen
     (`test-core`) but the allowlist function uses an underscore (`v_test_core`), so `type -t` failed to
     resolve → added `agent_validation_fn` normalization (hyphen→underscore) unified across
     `agent_validate`/dispatch/test_phase, plus 5 regression assertions.
   - **Retired 2026-06-07**: this narrow auto-keep path (`test_phase.sh` + the `improve-test-coverage/C2`
     / `write-unit-test/C2` dispatch routes, plus their dossier/manifest/test-list helpers) was removed in
     favor of the **single `execute-plan` proposal path** (§12). Test-writing is now just a proposal whose
     worker self-loads `write-unit-test`/`improve-test-coverage` from its work copy. The
     hyphen→underscore `agent_validation_fn` fix above stays (used everywhere).
   **Step D multi-candidate parallel + deterministic ranking landing field test (2026-06-05,
   `script/agent/{routing.env,common.sh,lint_phase.sh}`)**: turned §4/§5.1's "C1 defaults to multi-candidate
   fan-out, validate only the best" from design into code. The other half of Step D (lock + guard +
   temporal isolation + rollback) had long landed; this added the **candidate generation and ranking**
   half:
   - **The routing table gives a candidate set**: routing.env adds `ROUTE_C1_CANDIDATES` (an array of worker
     function names) + a second candidate `route_c1_worker_pro` (deepseek-v4-pro). **One entry = the old
     single-worker behavior** (zero fan-out, cheapest); adding a worker turns on parallel multi-candidate.
     Gemini Flash is not yet listed due to the Step A narration/output-contract issue. An old routing.env
     (no such array) auto-falls-back to `(route_c1_worker)`, backward-compatible.
   - **Deterministic ranking primitives**: `common.sh` adds `agent_patch_files` (count `+++` headers = files
     touched) and `agent_rank_candidates` (reads `<files> <churn> <id>`, stable-sorts by "fewer files →
     less churn → id"). **Pure functions, offline unit-testable**; only a semantic tie hands up to the
     frontier (§5.1 step 2), not burning frontier attention here.
   - **Runner rework**: `lint_phase.sh` per fix round now: ① fans out the candidate set in **parallel**, each
     worker editing only its own sandbox copy; ② harness-diffs each candidate patch, the churn guard
     filtering no-op / over-limit; ③ `agent_rank_candidates` ranks; ④ runs slow tidy validation only on
     **top-K** (`MAX_VALIDATE`, default 2) by rank, with the **first candidate that makes progress**
     accepted and entering the next round (fixpoint unchanged); if all top-K make no progress → escalate. A
     failed candidate reverts to **the round's start** (not the phase start), preserving progress accepted
     in prior rounds; only an escalate reverts the whole thing.
   - **Offline-deterministic e2e**: `run_lint_fanout_test.sh` uses three test seams — `AOBUS_LINT_TIDY` (fake
     tidy) + `AOBUS_ROUTING_ENV` (mock candidates) + `AOBUS_AGENT_REPO` (temp tree) — to run the whole
     control flow **without any model or clang-tidy**: proving "ranking, not start order, decides the
     winner" (a rewrite that starts first still loses to a surgical one that starts later). This folds Step
     D's new logic into CI rather than relying only on spot tests against real workers.

3. **Step C: structured packet + validation allowlist — landed and continuing to harden (see field tests
   above)**. packet = YAML frontmatter + markdown body; a mutating request packet requires `schema:
   aobus-phase-packet/v1` + `kind: request` and goes through a closed-schema gate. validation allows only a
   fixed ID from the allowlist + safe args (supporting `validation_args`), never an arbitrary shell string;
   the per-arg enum/type contract has landed via `VALIDATION_ARGSPEC` + `agent_validation_args_ok`. At the
   time, C2 test validation was additionally restricted to `test-core` / `test-gtk`, binding the selected
   filter back to the target source file + `target_anchor` via Catch2 high-verbosity list output, and the
   dispatcher's runner registry contained C1 (lint) + C2 (test). **Retired 2026-06-07**: the C2 test route
   was removed when C2 collapsed to the single `execute-plan` proposal path (§12); the dispatcher now
   routes C1 only, and the per-arg contract is still enforced by `dispatch.sh`.
4. **Step D: patch + deterministic guard + temporal isolation + multi-candidate parallel/ranking — landed
   (see field tests above)**. lock + guard + temporal isolation + rollback had long landed; the **candidate
   parallel fan-out + deterministic ranking + validate top-K** half is now also landed
   (`ROUTE_C1_CANDIDATES`, `agent_rank_candidates`/`agent_patch_files`, `lint_phase.sh`'s multi-candidate
   round, the `run_lint_fanout_test.sh` offline e2e). Cheap models produce multiple patches in parallel;
   before apply, run diff guard + ranking, send only the top-K into slow validation, and on failure revert
   to the round start and take the next. **Pending**: adaptive K, real silent-wrong rolling statistics for
   the cross-vendor second candidate, semantic tie-break between candidates (handed to the frontier only on
   a tie).
5. **Step E: C2 generalization and abstraction (narrowed)**. Generalized to `improve-test-coverage/C2` +
   `write-unit-test/C2`, but the shared contract is limited to **C3-decided test augmentation inside one
   existing, registered Catch2 test file**. A C2 request must carry a focused validation filter and a
   `target_anchor` that is absent in the baseline yet visible in the list output; the runner runs the
   baseline filtered test before the worker, on keep confirms the selected filter lists the target source +
   anchor, and produces a review dossier + audit, with C3 doing the semantic review and recording
   accept/reject via `record_review.sh`. Production-code body-fill, lint-checker bodies, and multi-file
   helper edits are not yet routed; they must wait until that C0 safety envelope's statistics prove the
   risk controllable before building another runner. When unattended, concurrent, or queued execution is
   genuinely needed, extract the dispatcher (already C0 logic) into a standalone tool.
   **Superseded 2026-06-07**: rather than build a separate runner per C2 capability, the full-tree
   proposal executor (Steps G–H, §12) became the **single** C2 route, and this narrow auto-keep
   test-augment path was retired. Multi-file edits and test-writing are now the same `execute-plan`
   proposal; the worker self-loads the relevant domain skill for specifics.
6. **Step F: C3 council (landed, 2026-06-05)**. Upgraded C3 from a doc-only roster into a convenable
   multi-model council. Landed: `council.sh`'s four rounds (R1 blind draft → R2 challenge → R3 self-revise
   → R4 chair verifies and synthesizes; the script runs only R1–R3, R4 left to the in-loop chair),
   read-only members + per-member repo copies + the `agent_tree_hash` before/after hash canary (edit the
   tree → discard + attribute), quorum (≥2 member drafts by default for a real debate, otherwise `quorum:
   degraded` still emits a dossier), dossier assembly; routing.env `ROUTE_C3_MEMBERS` (four read-only
   members, of which Claude/Opus is also an ordinary member) + `ROUTE_C3_MEMBER_LABELS`; the `run-council`
   skill unifies the plan/review two-mode contract, with review convened via code-review / diagnose-issue;
   the offline suite `run_council_test.sh` (63 assertions). See §11.
7. **Step G: first generic-C2 eval + sanitizer oracles + observability loop (landed, 2026-06-06)**.
   Track A of the V2 cross-vendor council review. (a) **ds-pro
   proposal worker**: `route_c2_proposal_worker_dspro` (opencode/deepseek-v4-pro, cwd-confined) is the
   new `ROUTE_C2_PROPOSAL_WORKER` default; codex stays the alternate. (b) **Sanitizer oracles**
   `test-core-asan` / `test-core-tsan` in `validation.env` — own `-DAOBUS_ENABLE_ASAN/TSAN=ON` build
   dir, core-only `ao_test` target, isolatable, honest "no finding on EXERCISED paths" dossier caveat;
   genuinely new because `agent_validate_in_repo`'s `cmake --preset linux-debug` leaves the sanitizer
   options OFF. (c) **Circuit breaker + rolling stats**: `record_review.sh` auto-trips a per-worker
   breaker on a silent-wrong (a `keep`/`proposal-validated` phase C3 later `reject`s; `modify` /
   non-validated do not trip); `c2_proposal_phase.sh` refuses a tripped route (the C2-test breaker arm in
   `dispatch.sh` went away with that route on 2026-06-07);
   read-only `review_stats.sh` aggregates per-worker×capability accept/modify/reject + silent-wrong rate
   (and, with `--window N`, a rolling silent-wrong rate over each worker's last N validated+reviewed
   evals) and lists/`--reset`s breakers — all on the **pre-existing** `audit.log` / `review-outcomes.log`
   (recording already existed; only aggregation + breaker were missing). (d) **Eval datum**: ds-pro
   chunk-refactored `base64Encode`, **validated round 1 under `test-core-asan [base64]`, in-scope,
   graded `modify`**; eval-first caught + fixed gitignored-artifact pollution (`agent_clean_ignored`
   strips gitignored paths + emptied dirs from base+work symmetrically) and a `churn` unbound crash.
   Conclusion: generic C2 is viable for a settled, single-file, behavior-preserving task under a hard
   test+sanitizer oracle — the rolling silent-wrong statistic now exists (`review_stats.sh --window N`),
   so the remaining gate is widening the eval set to populate it before any C2 scope expansion. The
   proposal executor now mints a unique `proposal-<utc>-<pid>` phase id (honoring an optional
   charset-validated packet `id`) instead of the colliding `unknown` sentinel, so audit/outcome/breaker
   keying is clean. Open follow-ups: the hard sandbox and a real not-fully-trusted-worker isolation layer
   remain §10.3/§11.6 items. Offline agent suites: 8 suites, **448** assertions
   (`run_review_stats_test.sh` = 25; proposal suite 64 → 79).
8. **Step H: C2 scope widening — a single full-suite oracle (landed, 2026-06-07)**. The path-based scope
   gate is replaced by one fixed validation oracle. (a) **Wider input gate**: `agent_proposal_input_ok`
   accepts public and private headers (`include/**`, `lib/**`, `app/**`) and registered test sources, not
   just a private `.cpp`; the old `include/` taboo guarded nothing in a no-compat repo. (b) **Single fixed
   oracle**: every proposal — cpp or header, core or app — validates against `test-all` (build `ao_test` +
   `ao_test_gtk` + `aobus-gtk`, run *both* whole suites) for baseline and work validation; the packet's
   `validation`/`validation_args` cannot weaken it. The runner does **not** select a test subset from the
   changed paths. The earlier design tried to (force `test-core-all`/`test-gtk-all` by header domain,
   over-approximate the `#include` closure via `agent_proposal_compute_blast_radius`, bound it by
   `PROPOSAL_BLAST_MAX`, escalate a mixed core+app set), but the whole suite runs in seconds and any change
   ultimately reaches the app, so that machinery bought complexity, not safety — and running everything
   closes the gap where a core edit silently broke an app-only test. Test binaries run from a writable
   build-runtime working directory with the source tree's `test/` directory exposed there, so runtime
   artifacts stay out of read-only snapshots while relative fixture paths keep working. The GTK suite
   runs **headless** (no widget realization, no preset), so no DISPLAY/xvfb is needed. (c)
   **Deterministic test obligation**: a
   packet `intent: refactor | behavior-change`; `behavior-change` requires a registered test to change
   (`agent_changes_touch_registered_test`) — the planner declares intent, the runner never infers it;
   `refactor` is exempt with a dossier RISK marker. (d) Dossier carries `intent` / `header_touched` /
   `assertion_delta`; a header touched with assertion-delta 0 still carries an explicit RISK note for C3.
   Forbidden set unchanged (CMake/`.clang-tidy`/`script`/`doc`/`.agents`), re-justified as
   **ruler-protection**. Open follow-ups: `nm`-based undeclared-deletion guard, proposal→auto-keep
   (eval-stats-gated).

## 10. Settled facts, cost model, and open risks

### 10.1 Settled facts

- **Low-cost-model token cost is approximately negligible.** The design's optimization target is not
  saving Flash/DeepSeek tokens but reducing frontier hand-writing, while controlling the number of
  validations, review attention, and silent-mis-fix risk.
- **`--fix` proven disabled**: `run-clang-tidy.sh` does have `--fix` (via `-export-fixes` batch apply), but
  practice proves it is not yet mature and corrupts files (batch overlapping-replacement conflicts,
  damaging uncovered regions). **v1 never enters any automated path**; it uses tidy diagnostics only as
  input. Even a future retry must stay inside the full patch + deterministic guard + validation + C3 review
  flow, never bypassing it.

### 10.2 Cost model

- **Real cost**: `delegate ≈ headless-invocation cold start + candidate-parallelism rate/latency
  constraints + patch guard/ranking + apply/lock/rollback management + slow validation (× up to K retries)
  + frontier C3 review + silent-mis-fix risk`. The low-cost-model tokens are not the main term; multiple
  candidates are bound first by rate limits, and slow validation by the K budget.
- **Strategy shift**: even a small batch can have low-cost models produce candidate patches first, since
  generating candidates is nearly free; but do not run `run-clang-tidy.sh` / `build.sh` on every candidate.
  Guard + rank first, validate only the smallest, most trustworthy candidate.
- **Net-benefit criterion**: not "how many low-cost tokens were saved" but "do the candidate patches reduce
  frontier hand-writing/search time without adding silent wrong, validation reruns, and manual rollback
  cost". The pilot must measure this threshold.

### 10.3 Open risks

- **Capability boundary needs eval**: Flash/DeepSeek's real fix rate on the Aobus C++26 spec + the
  `aobus-*` tidy can only set the C1 trust boundary and alternate order after Step A's four metrics
  quantify it.
- **Isolation ≠ sandbox (confirmed by agy)**: git temporal isolation/worktree only isolates **git state**,
  not **process capability**. A headless cheap CLI, if its harness grants exec/network/broad-filesystem
  permissions, will run wild outside the tree past the boundary. **Instance**: `agy` under `steam-run`
  (which bind-mounts the whole `$HOME`, including the real repo, into the sandbox) once **directly edited
  the real repo file** via the repo-relative path in the prompt, rather than the sandbox copy in the cwd
  (it prefers a real git project). The opencode worker does not have this problem: it runs in a `/tmp` cwd
  and its own external-dir permissions intercept out-of-tree access; agy's "sandbox" is only a cwd
  convention, pierced by the full `$HOME` mount. **Current mitigation** (sufficient for **trusted** vendor
  models): stage the target as a **flat unique name colliding with no repo-relative path**, mention only
  that name in the prompt → agy stays editing the cwd copy, the canary confirms the real tree's sha256 is
  unchanged; and all edits still pass harness-diff + guard + independent re-validation. **But this is not a
  hard sandbox**: a sufficiently agentic CLI could still content-search `$HOME`. To run **not-fully-trusted**
  models, a real sandbox (container/firejail/restricted bind mount, mounting only the stage + that CLI's own
  auth config) is a necessary separate layer — this remains open.
- **Non-interactive invocation and auth**: each CLI's headless flags, auth, rate limits, and context-window
  differences are confirmed in the Step 0 probe and registered into the routing table.
- **Observability**: phase packet + validation exit codes + patch/report artifacts are naturally auditable;
  a C2 *proposal* additionally leaves a review dossier + manifest/audit, with C3 recording accept/reject
  after it applies the patch to the real tree. Without post-C3 review-outcome pass-rate statistics, C2
  quality is systematically overestimated.
- **Orthogonal stacking with RTK**: RTK compresses *output* tokens, this design compresses *model usage*;
  the two stack.

## 11. C3 council: multi-model adversarial-synthesis for plan/review

C1/C2 converge via "cheap worker + deterministic gate"; **C3 has no deterministic oracle** — plan and
review are prose, "correct" is a judgment, and no exit code can adjudicate. So C3 does not do
ranking+validation; it convenes a **cross-vendor council**: several frontier models each draft
independently, then **challenge each other**, then self-revise, and finally the **chair (the in-loop
frontier agent running this flow) synthesizes** the single answer. This is the C3 analogue of C1 fan-out,
but the convergence mechanism is **adversarial cross-examination + chair synthesis**, not deterministic
ranking.

### 11.1 Protocol (rounds)

```
R1  blind draft   each routed member drafts independently, with no peer context (preserving diversity)
R2  challenge     each member sees the others' drafts and critiques them line by line (adversarial, specific)
R3  self-revise   each member revises its own draft after seeing the critiques aimed at it
R4  synthesis     the chair reads the dossier, independently verifies key claims, writes the final plan/review, explicitly adjudicating consensus vs dissent
```

R1–R3 are the **member rounds**, and how many run is capped by the packet's `depth` field (`panel`=R1;
`challenge`=R1+R2, **default**; `full`=R1+R2+R3, see §11.6); **R4 is the chair round, always runs,
independent of depth**.

- **R1 blindness is a key quality property**: members must not see each other in R1, or they anchor on each
  other and diversity collapses (diversity is the entire value of convening a council). The offline suite
  asserts the R1 prompt contains no peer text, with R2/R3 injecting it.
- **The chair only does R4 verification and synthesis**, no longer competing as an R1 drafter. If the
  chair's same model should also produce R1/R2/R3 opinions, it must be invoked as an ordinary member in
  `ROUTE_C3_MEMBERS`.
- **R4 is evidence-driven synthesis**: after reading the dossier, the chair returns to the code, diff,
  tests, and design docs to independently verify key claims, then adjudicates which member positions to
  accept, which to reject, and on what basis.

### 11.2 C0/C3 split

`council.sh` is the **C0 plumbing**: fan-out, collection, prompt assembly, the read-only canary, and
assembling `dossier.md` — it **makes no judgments** and **deliberately does not do the R4 synthesis**. R4
is the one irreducibly-frontier act, left to the chair to do in-loop after reading the dossier. This is
isomorphic to `dispatch.sh` (a C0 runner routing C3 work) and keeps the script deterministic and
offline-unit-testable; the real in-loop frontier attention is concentrated on R4 verification and the
final synthesis.

### 11.3 Read-only safety model (different from C1/C2)

A council member is **read-only**: it produces an **opinion** not a patch, and never edits the tree — so
there is **none** of the sandbox-copy / guard / churn / rollback machinery (nothing is ever applied). The
safety gates are replaced by:
1. **Read-only invocation**: each vendor's read-only headless mode (`codex exec -s read-only`, `gemini -p
   --approval-mode plan`, an `opencode run` query, a read-only `claude -p`). Best-effort, with varying CLI
   fidelity.
2. **Tree-immutability canary (the hard backstop)**: each member runs in **its own disposable repo copy**
   (cwd = `AGENT_COUNCIL_CWD`), and council.sh hashes that copy before/after the call with `agent_tree_hash`
   (content + file mode + symlink target, so chmod / symlink-retarget are also caught); **any member that
   edited its copy has its output discarded and flagged** (a member writing a file is a violation, the
   opposite of C1's "diff is the deliverable"). The per-member copy keeps this check **attributable to the
   specific member** even under **parallel fan-out**. As of 2026-06-07 every member (incl. the
   agy/Gemini member) runs **natively in its council cwd** — the old `steam-run` staged-copy + sentinel
   workaround is gone.
3. **Real-repo canary (escape backstop, 2026-06-07)**: the per-member canary only watches each member's
   *copy*; a native CLI with full filesystem access could ESCAPE its cwd and touch the real repo
   directly. council.sh therefore also hashes `$AGENT_REPO` before fan-out and re-checks on every exit
   path (`_verify_repo_immutable`, EXIT trap), aborting with exit 3 on any change — the same backstop
   C2's `c2_proposal_phase.sh` has. `agent_tree_hash` excludes `.cache`/`logs`/`build*`, so the
   by-design shared-`.cache`/`logs` symlinks do not trip it (and `build.sh` is *not* excluded).
4. Member copies exclude `.git`; §10.3's "a sufficiently agentic CLI can escape the cwd" remains the
   reason the real-repo canary exists, and stays an open risk for **not-fully-trusted** review input
   (the canary detects+discards an escape but cannot prevent it; sufficient for a trusted vendor roster).

### 11.4 quorum, cost, and entries

- **quorum**: a council needs real debate, requiring at least `COUNCIL_MIN` (default 2) member drafts; below
  that council.sh still emits a dossier but flags `quorum: degraded`, telling the chair "this is close to a
  solo draft" and leaving it to decide whether to proceed or re-convene. Zero drafts → exit 2. This is the
  **accidental** axis, orthogonal to `depth`'s **intentional** axis (`shallow: by-design`, §11.6): a
  4-draft `panel` is `quorum: ok` + `shallow: by-design`, not a degraded `full`, and the chair must tell
  them apart.
- **cost**: N members each run the member rounds, then the chair does R4; each is real frontier tokens. The
  default `challenge` (R1+R2) is 2 serial slow rounds, `full` adds R3 for 3; in-round parallel fan-out
  compresses wall-clock to the number of serial rounds. Unlike C1 (tokens ~free), the council is
  **opt-in** — non-critical does not convene (solo), medium-stakes uses the default `challenge`, and only
  high-stakes (architecture, error contract, risky diff) opts up to `full`.
- **entries (both plan and review)**: the `run-council` skill is the unified contract for both modes; review
  is convened via `code-review` (built-in) / `diagnose-issue` (footnote), and plan is convened by the chair
  in plan mode, with the synthesized result being the plan file / `ExitPlanMode` artifact.
- **packet**: reuses the v1 schema, adding `kind: council` / `mode: plan|review` / `depth:
  panel|challenge|full` (optional, default `challenge`), with **no `validation:`** (no deterministic gate),
  so it **does not go through `dispatch.sh`'s allowlist path**; council.sh is its own entry.

### 11.5 Implementation field test (2026-06-05)

`script/agent/council.sh` (C0 orchestration) + routing.env `ROUTE_C3_MEMBERS` (four read-only member
functions for claude / codex / gemini / opencode + `ROUTE_C3_MEMBER_LABELS`, cross-vendor by default,
opt-in/configurable, falling back to the default four) + common.sh `agent_tree_hash` (a general directory
content hash, excluding `.git`, a typed manifest including mode/symlink target) +
`.agents/skills/run-council/SKILL.md` + the `diagnose-issue` footnote. Member prompts go over **stdin**
(from `AGENT_COUNCIL_PROMPT_FILE`, bypassing a single argv's `MAX_ARG_STRLEN` 128KB limit); `run_one` hard-
checks the member exit code (non-zero → discarded, not letting a timeout/crash's partial output sneak into
a seat). The offline-deterministic suite `run_council_test.sh` (63 assertions, no model / no network) uses
mock members (`AOBUS_ROUTING_ENV`, behavior switched by `COUNCIL_MUTATE` / `COUNCIL_MUTATE_R2` /
`COUNCIL_FAIL` / `COUNCIL_SILENT` / `COUNCIL_ROSTER`) + a temp tree to validate the whole pipeline: happy
path, the canary catching a tree-editing member (discard + attribution), R1 blindness, bad mode / unsafe
input / OUT-in-repo rejection, single member → quorum degraded, silent/non-zero-exit members → discard and
continue, an R2-phase violation → quarantined entirely, the canary's sensitivity to chmod/symlink-retarget,
inputs injected into the prompt, R3 excluding self-critique, and plan/review picking the right template.

**Self-audit hardening (dogfood)**: ran a real review council on the mechanism itself (chair=in-loop agent
+ GPT-5.5 high via codex + Gemini 3 Pro via gemini, read-only). The cross-challenge output caught a batch
of real bugs that single-model thinking and the offline assertions both missed — `printf '--'` option
parsing, `run_one`'s missed exit-code check (partial output treated as success), R3 feeding a member's own
challenge back as "critiques of it", R2/R3-phase violations not quarantined entirely (the R1 draft still
entered the dossier), `AGENT_COUNCIL_OUT` landing inside the repo copying in-flight artifacts into member
cwds and breaking R1 blindness — and pointed out that a single-argv prompt would hit `MAX_ARG_STRLEN`. Each
was fixed with a regression assertion. This is itself the best demonstration of the council's value:
diversity caught things that single-model + deterministic tests both missed.

**Pending**: a hard sandbox for **not-fully-trusted** input (container/firejail/bwrap, the §10.3 open item
— a read-only flag + canary cannot stop process-level network/exec); adaptive K (member count) and round
count; rolling statistics + a circuit breaker for "confident but wrong" members; a human evaluation of
chair synthesis quality.

### 11.6 Depth tiers (depth): a lightweight council for non-critical occasions (2026-06-06)

The four-round fixed protocol nails the cost at `members × 3 member rounds`, making the doctrine binary:
high-stakes runs full, otherwise solo. That left a gap at "medium-stakes" — worth one or two extra
independent pairs of eyes cross-checking, but not worth three rounds of adversarial debate. The `depth`
field turns "how many rounds run" into an **explicit, auditable** choice in the packet, filling that gap
without eroding the "non-critical → solo" rule.

| `depth`     | member rounds | what you get                              | for |
|-------------|---------------|-------------------------------------------|-----|
| `panel`     | R1            | diversity only, N independent drafts, no debate | plan brainstorm / "give me N independent options" |
| `challenge` | R1 + R2       | diversity + one adversarial round         | **default** — medium-stakes plan/review |
| `full`      | R1 + R2 + R3  | + each member self-revises before synthesis | highest-stakes: architecture / error contract / risky diff |

- **Why default `challenge`**: convening at all is already a high-stakes filter; once there, one adversarial
  round + chair synthesis covers most convocations. The lowest-marginal-value R3 (self-revise) becomes an
  explicit opt-up via `depth: full` rather than a default cost; `panel` is the opt-down for brainstorming.
- **`shallow` vs `quorum`, two orthogonal axes**: when `depth != full`, the dossier records `shallow:
  by-design` (**intentionally** shallow), recorded separately from `quorum: degraded` (**accidentally** few
  drafts), so the chair does not mistake a healthy `panel` for a failed `full`. R2 still needs ≥2 drafts to
  have something to compare, so `challenge`/`full` still degrade when drafts are insufficient (degraded).
- **`depth` tunes debate depth; member count is the other orthogonal cost lever** — `ROUTE_C3_MEMBERS` /
  `COUNCIL_MIN` have long been adjustable, and for more savings you can shrink the roster too (fewer members
  = less diversity), with no new code. The two axes solve different problems; v1 only adds `depth`.
- **`mode` and `depth` are orthogonal**: plan|review × panel|challenge|full, all six combinations valid.
- **Tests**: `run_council_test.sh` grew to 110 assertions — covering panel (R1 only, no R2/R3 prompt or
  dossier section, R1 blindness intact), challenge (R1+R2, no R3), default → challenge, unknown depth →
  exit 64, and the `shallow`/`quorum` dual-axis independence (single-member `full` → degraded+`shallow:
  full`; single-member `panel`/`challenge` → degraded+`shallow: by-design`, the latter's R2 skipped by
  quorum rather than by design). The standard fixture pins `depth: full` explicitly to keep covering the
  three-round rendering.
- **Self-audit of this mechanism (panel, 2026-06-06)**: reviewed this feature with a panel council
  (cross-vendor codex/gemini/deepseek, excluding Opus), which tightened the dossier prose in the
  degraded×depth mixed states — the `shallow` note now claims only "rounds beyond the current `depth`" as
  intentional, the `quorum` note no longer mentions cross-challenge under panel, and the degraded NEXT no
  longer asks to "adjudicate consensus vs dissent" (the machine-readable `depth`/`shallow`/`quorum` fields
  were already correct; what was tightened is the prose given to the chair).

## 12. C2 proposal executor (contract & safety model)

C2 is a **proposal executor, not an autonomous keeper**. It turns an already-decided C3 plan into
validated code inside an isolated repo copy and hands a review-ready patch back; it never lands a change
on the real tree. A successful run means "a validated proposal was produced," not "the tree changed."
(Landed mechanism + eval history: §9 Steps G–H. Entry point: `script/agent/c2_proposal_phase.sh`.)

> **Operator guide:** the `execute-plan` skill (`.agents/skills/execute-plan/SKILL.md`) is the chair's
> manual for this contract — when to delegate, how to author the proposal packet, how to invoke the
> executor, and the acceptance flow. It is the **single** C2 delegation skill.

```text
C3 plan → C2 executes in an isolated copy → emits {patch, manifests, dossier, logs}
        → C3 reviews, applies/modifies/rejects, and validates in the REAL worktree
```

**Tier boundary.** C1 keeps mechanical changes behind a hard oracle; C3 owns judgment (planning,
architecture, error-contract/concurrency decisions, final semantic review, and the *apply* to the real
tree); C2 only executes a scoped plan and proposes. The proposal executor is the **sole** C2 route and
is deliberately **non-keep** — every C2 result is a chair-reviewed proposal, never auto-landed. (The
earlier narrow auto-keep test-augment path, `test_phase.sh`, was **retired 2026-06-07**: writing tests
is now just another proposal whose worker self-loads `write-unit-test`/`improve-test-coverage` from its
work copy. Retiring it removed the one place C2 could land a change without chair review, which fits the
`silent-wrong = 0` invariant.)

**Proposal packet** (`kind: proposal`, not a `dispatch.sh` `kind: request`):
- `id` optional — charset-validated and used verbatim, else the harness mints `proposal-<utc>-<pid>`
  (the reserved `unknown` is rejected; the id keys the audit/outcome/breaker logs).
- `intent: refactor | behavior-change` (default `refactor`) — the planner *declares* it; the runner
  never infers behavior change. `behavior-change` deterministically requires a registered test to change.
- `inputs[]` — the exact editable set; C2 may not touch anything else.
- `validation` / `validation_args[]` — required by the packet schema but **ignored by the proposal
  runner**, which always forces the full-suite `test-all` oracle (below). The body is the C3 plan to
  execute, not to redesign.

**Scope policy — a single fixed full-suite oracle** (no path taboo; this repo has no API/ABI-compat
requirement, so the protected invariant is *silent-wrong = 0*, not "the public surface changed"). A
conservative false rejection (escalate to C3) is fine; a false *acceptance* that presents an unvalidated
change as "validated" is not.
- Each input must exist at phase start, be a regular file (no symlinks), classify as
  `private-cpp-source` / `public-header` / a private `lib`/`app` header / a registered test, and pass
  `AGENT_PROPOSAL_FORBID` (CMake / `.clang-tidy` / `script` / `doc/design` / `.agents` stay forbidden as
  **ruler-protection** — they define the oracle itself).
- **Every** proposal — cpp or header, core or app — validates against the same fixed oracle `test-all`
  (build `ao_test` + `ao_test_gtk` + `aobus-gtk`, run *both* whole suites) for *both* baseline and work
  validation; the packet cannot weaken it. The runner does **not** derive a per-change test subset: the
  whole suite runs in seconds and any change ultimately reaches the app, so a blast-radius gate bought
  complexity, not safety — and running everything closes the gap where a core edit silently broke an
  app-only test. The GTK suite runs **headless** (no widget realization, no preset), so no DISPLAY/xvfb
  is needed.
- Defense in depth after each round: only `modify` of a declared input is accepted (adds, deletes,
  renames, symlinks, binary, mode-only → reject). The dossier records `intent` / `header_touched` /
  `assertion_delta`; a header changed with assertion-delta 0 carries an explicit RISK marker for C3 (the
  full suite passed, but it need not exercise the changed path).

**Execution & safety model** (the main worktree is never mutated):
- Record `agent_tree_hash` of the real repo; when the host supports it, stage **base** + **work** as full
  Btrfs subvolume snapshots under the repo-external work root (`/home/rocklee/dev/.aobus_work` by
  default). `base` is a read-only snapshot and hidden from the worker; `work` is writable and deliberately
  keeps the full repo state, including `.git`, `.cache`, `logs`, and build dirs. Validation uses
  `work/.cache/ccache` as a writable, per-proposal inherited cache for both baseline and worker rounds,
  so new cache entries never pollute the real repo and disappear when the subvolume is destroyed.
  `agent_validate_in_repo` always runs under the bwrap path view (below): it configures CMake with the
  plain `ccache` launcher and exposes each proposal-local build dir at the main developer build path shape
  (`/tmp/build/debug` by default, override with `AOBUS_AGENT_BWRAP_BUILD_VIEW`). Because the work copy is
  presented at the stable real-repo path, no per-snapshot source/build path leaks into the ccache key, so
  no compiler-argument rewriting is needed. Any remaining CMake-managed download dependencies are exposed
  at `/tmp/build/debug/_deps` while their host storage remains under the proposal snapshot cache. This lets
  the inherited snapshot `.cache/ccache` hit entries produced by the normal developer build; new misses are
  written only to the proposal-local cache and are discarded with the snapshot. Test-only FakeIt headers
  are provided by Nix, like lexy, so CMake configure does not clone FakeIt or depend on proposal-local
  `_deps` state for that library. Non-Btrfs or unprepared hosts fall back to the historical sanitized
  `rsync` copy for staging; the bwrap path view sits on top of either staging mode.
- Path virtualization (always on): both the C2 worker and `agent_validate_in_repo` run inside a
  `bubblewrap` mount namespace that binds the proposal work copy at the real repo path (for this host,
  `/home/rocklee/dev/Aobus`) and binds the proposal build dirs at the main-cache-shaped build view
  (`/tmp/build/debug` by default). bwrap is a hard dependency of the proposal phase (shipped via
  `shell.nix`); a host without it is rejected rather than silently downgraded. This is a performance and
  reproducibility feature, not a security sandbox: `$HOME`, network, and the caller's environment stay
  visible so model CLIs keep their normal authentication behavior. It also incidentally shadows the real
  repo path with the work copy, so a worker that targets the repo path can only mutate its own sandbox.
  The proposal-local `.cache` directory is bound at
  `<repo>/.cache`, so inherited `.cache/ccache` remains writable inside the snapshot lifecycle while
  ccache sees the same source/build/cache path shape as the main developer build.
- Manual cache warmup: `script/agent/warm-main-ccache.sh [target ...]` is an explicit maintenance entry
  for the one case where the normal build dir is up to date but `.cache/ccache` is not. It mounts a
  throwaway host build dir at the same main-cache-shaped build view (`/tmp/build/debug` by default),
  writes the real repo `.cache/ccache`, and removes the throwaway build on exit. This is not part of the
  C2 hot path; run it after cache/toolchain resets or when proposal baseline stats show an unexpectedly
  cold inherited cache.
- Run **baseline validation** from base in an isolated build dir — a red baseline rejects the packet (C2
  is never asked to debug pre-existing breakage).
- Rounds: the worker edits the work copy → the **harness** computes the typed base→work manifest + patch
  (model-authored patches are never trusted as the source of truth) → scope/churn guards → 
  `agent_validate_in_repo` runs the allowlisted validation against the work copy + isolated build dir →
  the failure log feeds the next round.
- On exit, re-hash the real repo; **any change invalidates the proposal** (catching mode/symlink/
  out-of-scope edits an isolated-target check would miss). `council.sh` carries the same real-repo canary
  (`_verify_repo_immutable`); `agent_tree_hash` excludes noise *directories* (`.cache`/`logs`/`build*`)
  but not the tracked `build.sh` file.
- Validation must honor the source/build-dir seam; one touching shared build state (`build-debug`) is
  rejected for proposal mode — proposal validation is only meaningful when isolated.

**Worker contract** — the dedicated `ROUTE_C2_PROPOSAL_WORKER` / `ROUTE_C2_PROPOSAL_LABEL` route. The
worker receives `AGENT_PROPOSAL_WORK` (its cwd, the only writable path), `AGENT_PROPOSAL_INPUTS_FILE`,
`AGENT_PROPOSAL_PLAN_FILE`, `AGENT_PROPOSAL_FEEDBACK_FILE`, `AGENT_PROPOSAL_ROUND`,
`AGENT_PROPOSAL_BUILD_DIR`, `AGENT_PROPOSAL_OUT`; it never sees the base copy or the real tree.
`BUILD_DIR` and `AOBUS_AGENT_REPO` are bound to the work copy for the worker, so accidental self-checks
such as `./build.sh debug` stay inside the proposal-local `build-worker` host dir instead of mutating the
real `/tmp/build/debug` or poisoning the harness-owned `build-work` validation tree. Because the worker
runs under the bwrap path view, those variables contain namespace paths shaped like the normal developer
build (`<repo>` and `/tmp/build/debug` by default), so worker self-builds can reuse the inherited main
cache while writes land in the proposal snapshot. The prompt also names the read-only `.agents/skills/` directory present in the
work copy, so a plan can point the worker at a domain skill
(`write-unit-test`/`improve-test-coverage`) instead of inlining conventions. Defaults: DeepSeek V4 Pro via
opencode
(`route_c2_proposal_worker_dspro`), Gemini 3.1 Pro via native agy (`_gpro`); codex is the alternate.

**Statuses / exit codes.** `validated` → 0 · `diagnostic-budget-exhausted` (an in-scope patch was
produced but never went green) → 1 · rejected — no usable in-scope patch (out-of-scope / churn / breaker
/ red baseline / real-repo mutation, or the worker produced no in-scope change) → 2 · config/routing/
table missing → 5 · bad packet → 64. (The forced `test-all` oracle is always isolatable and the runner
takes no repo lock, so the not-isolatable / lock paths do not arise here.) Bounded by round, total/
per-file churn, and per-round worker-timeout budgets; budget exhaustion never keeps changes.
Each proposal directory also gets a `phase-exit.env` trap artifact with the final exit code, phase id,
last round, and whether an in-scope patch was produced. The runner records `ccache -s` snapshots around
baseline, worker, and validation phases (`baseline.before.ccache`, `baseline.after.ccache`,
`roundN.worker.before/after.ccache`, `roundN.validation.before/after.ccache`) so cache behavior remains
auditable even though the proposal-local `.cache` is disposable and may be removed during teardown.

**Acceptance flow.** C3 reads the dossier, inspects the harness patch against the plan, applies (and
possibly modifies) it to the real tree, runs real validation, accepts/rejects, and records the outcome
(`record_review.sh`). A `reject` of a `proposal-validated` phase is a **silent-wrong** that auto-trips
the per-worker circuit breaker until `review_stats.sh --reset`. Reserve a *council* review for high-risk
cases (public-API / architecture / error-contract, or chair uncertainty).

**C2 must not**: mutate the real tree; edit outside `inputs[]`; create/delete/rename/symlink/chmod;
change the validation contract; edit harness/script/CMake/design-doc/skill surfaces; decide semantic
correctness or make architecture/API/ownership/threading decisions; promote a proposal to a keep.
**Out of scope** (later steps): CMake registration of new translation units, the `nm`-based
undeclared-symbol-deletion guard, and proposal→auto-keep promotion (eval-stats-gated).
