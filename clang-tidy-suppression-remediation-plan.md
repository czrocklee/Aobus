# Clang-Tidy Suppression Remediation Plan

## Document Purpose

This document turns the current suppression analysis into an execution plan:

1. Decide per-rule whether to remove the rule or fix code.
2. Reduce `NOLINT*` usage in a controlled way.
3. Define reproducible validation steps for each phase.

## Scope And Data Source

This plan is based only on in-repo source suppressions (`NOLINT`, `NOLINTNEXTLINE`, `NOLINTBEGIN/END`) in `include/`, `src/`, `tool/`, `test/`, plus `.clang-tidy`.

Explicitly excluded from analysis: `report.txt` and `report`.

## Baseline Inventory

1. `NOLINT*` markers: **88**
2. Rule mentions in markers (single marker can include multiple rules): **93**
3. Distinct suppressed rules: **19**

## Decision Summary

### A) Remove Rule From Global Ruleset (4 rules, 46 mentions)

1. `cppcoreguidelines-pro-bounds-pointer-arithmetic` (22)
2. `bugprone-easily-swappable-parameters` (14)
3. `readability-convert-member-functions-to-static` (8)
4. `readability-static-definition-in-anonymous-namespace` (2)

Reasoning: these warnings are repeatedly triggered by intentional binary-layout/parser patterns and API-shape choices in this codebase, creating high noise and low defect yield.

### B) Keep Rule And Modify Code (13 rules, 41 mentions)

1. `cppcoreguidelines-special-member-functions` (13)
2. `readability-named-parameter` (8)
3. `bugprone-unchecked-optional-access` (3)
4. `cppcoreguidelines-avoid-magic-numbers` (2)
5. `readability-magic-numbers` (2)
6. `readability-function-cognitive-complexity` (2)
7. `readability-function-size` (1)
8. `readability-redundant-member-init` (2)
9. `cppcoreguidelines-pro-type-vararg` (2)
10. `hicpp-signed-bitwise` (2)
11. `readability-qualified-auto` (1)
12. `cppcoreguidelines-pro-type-static-cast-downcast` (2)
13. `cppcoreguidelines-pro-type-member-init` (1)

Reasoning: these checks can catch real issues or improve maintainability; local code changes are generally straightforward or bounded.

### C) Keep Rule, Allow Local Boundary Suppression (2 rules, 6 mentions)

1. `cppcoreguidelines-pro-type-reinterpret-cast` (5)
2. `cppcoreguidelines-pro-type-const-cast` (1)

Reasoning: these occur at unavoidable boundaries (LMDB C API + raw binary view casting). Prefer minimal, well-documented local suppression over global disable.

## Hotspot Map (Where Most Suppressions Cluster)

1. Binary parsing/serialization paths: `src/tag/flac/*`, `src/tag/mp4/*`, `src/tag/mpeg/*`, `include/rs/utility/ByteView.h`
2. LMDB adapter layer: `include/rs/lmdb/*`, `src/lmdb/*`
3. Query expression/evaluator: `src/expr/*`, `include/rs/expr/*`
4. Core hot/cold record accessors: `include/rs/core/TrackView.h`, `src/core/TrackLayout.cpp`, `src/core/TrackStore.cpp`

## Execution Plan

## Phase 0: Safety And Branch Hygiene

1. Create a dedicated branch for lint-remediation.
2. Because this is a multi-file, cross-module task, create a btrfs snapshot before first edit:

```bash
btrfs-snap /home/rocklee/dev rockstudio-clang-tidy-remediation-$(date +%Y%m%d-%H%M%S)
```

3. Record snapshot name in commit/PR description for traceability.

## Phase 1: Ruleset Cleanup In `.clang-tidy`

1. Add the following exclusions to `Checks`:

```text
-bugprone-easily-swappable-parameters
-cppcoreguidelines-pro-bounds-pointer-arithmetic
-readability-convert-member-functions-to-static
-readability-static-definition-in-anonymous-namespace
```

2. Keep all remaining currently enabled groups unchanged.
3. Re-run lint and capture delta of suppression count.

Expected result: immediate drop of ~46 rule mentions and many `NOLINTBEGIN/END` blocks becoming unnecessary.

## Phase 2: Mechanical Fix Pass (Low Risk)

Address rules with localized edits first:

1. `readability-named-parameter`: give explicit names or use `[[maybe_unused]]` where needed.
2. `cppcoreguidelines-special-member-functions`: replace explicit defaulted boilerplate with `= default` at class level where legal; remove unnecessary suppression blocks.
3. `readability-redundant-member-init`: remove redundant ctor init/comments and simplify constructors.
4. `readability-qualified-auto`: apply `const auto` form where required.
5. `cppcoreguidelines-avoid-magic-numbers` + `readability-magic-numbers`: introduce named constants (for formatting width/layout constants) when semantically meaningful.
6. `cppcoreguidelines-pro-type-vararg` + `hicpp-signed-bitwise`: replace `(void)`-style idioms with explicit helper patterns that do not trigger these checks.

Validation gate after this phase: clean build + no new suppressions introduced.

## Phase 3: Correctness-Focused Fixes (Medium Risk)

1. `bugprone-unchecked-optional-access` in expression normalization:
2. Rewrite access pattern to dereference optional only after extracting a local checked reference/pointer.
3. Add/extend focused tests for expression normalization behavior.

Validation gate after this phase: expression-related tests pass, no semantic regressions in parser/evaluator flow.

## Phase 4: Structural Refactor Pass (Targeted)

1. `readability-function-cognitive-complexity` and `readability-function-size`:
2. Split large evaluator branches in `PlanEvaluator::evaluateFull` into helper functions by opcode family (comparison, logical ops, string ops).
3. Keep behavior byte-for-byte equivalent for current execution plan semantics.
4. `cppcoreguidelines-pro-type-static-cast-downcast` in MP4 parsing:
5. Replace unsafe downcast sites with guarded typed-access helper(s) (e.g., checked cast or API adjustment reducing need for downcast).
6. `cppcoreguidelines-pro-type-member-init` (nested LMDB transaction ctor):
7. Refactor constructor to explicit member init state if check remains active.

Validation gate after this phase: full debug build + targeted media metadata tests.

## Phase 5: Boundary Suppression Tightening

1. Keep `reinterpret_cast`/`const_cast` suppressions only at hard boundaries.
2. Convert broad `NOLINTBEGIN/END` blocks to single-line `NOLINTNEXTLINE` where possible.
3. Add a short rationale comment on each remaining boundary suppression:
4. Why unavoidable.
5. What invariant keeps it safe.

Expected end state: very small, intentional suppression footprint.

## Verification Procedure

All steps should run inside Nix shell.

## Step 1: Environment And Build

```bash
nix-shell --run "cmake --preset linux-debug"
nix-shell --run "cmake --build --preset linux-debug --parallel"
```

If presets are not available in current environment, fallback:

```bash
nix-shell --run "cmake --build /tmp/build --parallel"
```

## Step 2: Suppression Inventory (Before/After Each Phase)

```bash
rg -n --hidden --glob '!.git' --glob '!report.txt' --glob '!report' "NOLINT|NOLINTNEXTLINE|NOLINTBEGIN|NOLINTEND" include src tool test
```

Optional rule-level count script:

```bash
python - <<'PY'
import pathlib,re,collections
root=pathlib.Path('.')
pat=re.compile(r'NOLINT(?:NEXTLINE|BEGIN|END)?\(([^)]*)\)')
c=collections.Counter()
for p in root.rglob('*'):
    if not p.is_file() or '.git' in p.parts or p.name in {'report','report.txt'}:
        continue
    try: lines=p.read_text().splitlines()
    except Exception: continue
    for line in lines:
        if 'NOLINT' not in line: continue
        for m in pat.finditer(line):
            for r in [x.strip() for x in m.group(1).split(',') if x.strip()]:
                c[r]+=1
for k,v in c.most_common():
    print(f"{v:3} {k}")
PY
```

## Step 3: Clang-Tidy Run

Run clang-tidy against changed files using `compile_commands.json`.

```bash
nix-shell --run "clang-tidy -p . <changed-file-1> <changed-file-2>"
```

For larger phase validation, run clang-tidy in batches by module (`src/expr`, `src/lmdb`, `src/tag`).

## Step 4: Tests

After behavior-touching phases (Phase 3/4), run tests:

```bash
nix-shell --run "ctest --test-dir /tmp/build --output-on-failure"
```

If test discovery is preset-based in your environment, use your existing CTest preset equivalent.

## Step 5: Acceptance Checks

A phase is accepted only if all are true:

1. Build succeeds in debug preset.
2. No increase in total `NOLINT*` markers.
3. No newly introduced suppression rules.
4. Rules designated for removal are removed from `.clang-tidy` and no longer need local suppressions.
5. Remaining suppressions are either temporary TODO-tagged or boundary-justified.

## Final Acceptance Criteria (Project-Level)

1. `.clang-tidy` no longer includes the 4 globally removed noisy rules.
2. Suppressions for “keep-and-fix” rules are reduced to near-zero or zero.
3. Boundary-only suppressions are explicit, minimal, and justified inline.
4. Debug build and tests are green.
5. A final summary table exists in PR description with per-rule: before count, after count, and rationale.

## Risks And Mitigations

1. Risk: behavior regression while refactoring evaluator complexity. Mitigation: refactor in small commits and run evaluator/parser tests after each commit.
2. Risk: false confidence from only spot-checking lint. Mitigation: phase-gated module batch runs and suppression count diffing.
3. Risk: boundary-cast cleanup overreach degrades performance/clarity. Mitigation: keep casts at API boundaries with strict invariants instead of forcing unnatural abstractions.

## Deliverables Checklist

1. Updated `.clang-tidy` (rule removals).
2. Code changes for keep-and-fix rules.
3. Reduced suppression inventory report (before/after counts).
4. Build + test + clang-tidy evidence attached in PR.
