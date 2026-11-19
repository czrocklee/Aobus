---
id: development.test.validation-and-review
---

# Test validation and review

## Adding new files

When creating a new test file:

1. Place it under the matching existing `test/...` module or layer directory, such as `runtime`, `uimodel`, `linux-gtk`, `library`, `query`, `audio`, `utility`, `cli`, `tui`, or `winui`.
2. Register it with its owning executable in `test/BUILD.bazel`. Declare a platform- or frontend-conditional test once, with an explicit disabled declaration and reason when it is not selected on a host.
3. Match namespace and include style of neighboring tests.
4. Include only headers that are used.
5. Check the focused `*TestSupport.h` files in the owning layer before creating a new shared helper; do not add an include-all umbrella.
6. Keep file-scope helpers local unless multiple files need them.
7. Do not create duplicate helper types and hide the conflict in a nested namespace; reuse or extend the existing helper instead.

Registrations live only
in `*Test.cpp`/`*Test.mm` files (`*Benchmark.cpp` for benchmark cases);
[naming and assertions](naming-and-assertion.md#tags) owns the discovery and
placement contract. The source audit in scoped `check lint` does not require
a build tree or check ownership; other applicable lint stages may need native tools. The completion gate's ownership check
joins registration discovery with the Bazel graph — a declaring `cc_test` the
gate executes, or a `cc_binary` it builds — so an unowned or never-run test
source fails the gate; keep throwaway tests outside the checkout. When moving tests between binaries, compare
runtime test lists on each affected platform.

## Regression tests

Protect a real bug with a test named for the behavior, placed with related cases
(see [fixtures and helpers](fixture-and-helper.md#before-adding-or-restructuring-tests)).
Keep it beside related cases and add no marker tag. Add a short comment
when the assertion guards a non-obvious lifecycle or layout invariant.

## Reviewing existing tests

A review of test quality or tags reads each complete case, every `SECTION`, and
the helpers needed to explain its assertions. Names and keyword searches
are triage only. Judge scope and `[concurrency]` from the asserted contract, not
from the fixture's threads or binary. For an exhaustive review, keep a per-case
record of reviewed and pending cases outside the repository and report gaps; a
clean test audit or passing run does not show the review is complete.

## Large test files

When adding to an already large test file, prefer a new focused file if the behavior belongs to a separable contract area. Split by behavior domain, not by arbitrary line count.

Prefer standalone `TEST_CASE`s for independent behavior contracts; keep `SECTION` for
variants of one contract that share an arrange (see _SECTION vs TEST_CASE_ in
[test naming and assertions](naming-and-assertion.md)). Splitting duplicates the arrange, so route genuinely shared
setup into an existing `*TestSupport.h` rather than copying it — and do not hide a
duplicate helper behind a nested namespace to dodge a collision.
Keep its concrete implementation in the paired `.cpp` unless it must remain
visible for template or compile-time semantics.

Good split boundaries include:

- Query evaluator scalar, dictionary, tag, bloom-filter, range/list, and load-mode behavior.
- TrackStore raw layout and malformed buffer behavior vs create/read/update/delete behavior through fixtures.
- Activity status compact, detail, local hiding, and task-progress policy.
- Import/export round-trip correctness vs coordinator dialog/lifecycle glue.

Do not perform a broad test-file split as drive-by cleanup unless it is necessary for the current change or explicitly requested.

## Validation

This document owns completion validation for contributors and agent skills.
Choose the route from the behavior and execution boundary being changed, and
combine routes when a change spans several rows.

| Change or task                                                      | Completion evidence                                                                                                                                                                                                                                                                                                                                                                                                                                                                                  |
| ------------------------------------------------------------------- | ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| Read-only review or diagnosis                                       | Source evidence and the relevant existing validator or focused reproducer. No build is required solely to finish a review.                                                                                                                                                                                                                                                                                                                                                                           |
| Documentation or agent instructions                                 | `./ao check docs` and `./ao check format --docs`, agreement with the owning code/policy, and representative task routing when skill behavior changes. Changes to skill scripts also take the tooling row.                                                                                                                                                                                                                                                                                            |
| Python portal or automation scripts                                 | `./ao check` on Linux, `ao.bat check` on Windows for affected Windows behavior; these include the tooling targets. Use the [standalone tooling suite](test-suite.md#tooling-and-lint-fixtures) while iterating. Exercise changed shell/bootstrap/process boundaries on their native hosts. macOS runs its SDK selection and remote-cache modules; use native portal probes and scoped `check lint` for other host-specific behavior.                                                                 |
| C++ implementation, headers, or tests                               | One full native `check`, including its declared whole-graph lint test. Add affected native hosts when their implementation, toolchain, or frontend is involved.                                                                                                                                                                                                                                                                                                                                      |
| Build configuration, dependencies, native test orchestration, or CI | Tooling checks plus the affected native build/test matrix. Include Release or sanitizers when the changed configuration or behavior requires them.                                                                                                                                                                                                                                                                                                                                                   |
| Catalog-only wording in an existing maintained locale               | Build affected frontends to regenerate catalogs, run the catalog suite (`bazel test //test:i18n_test`) and any affected message-specific tests identified by the [text-catalog reference](../../system/presentation/text-catalog.md#compatibility-and-test-authority), and inspect the changed UI at normal and constrained widths. Native Windows UI evidence is needed when WinUI consumes the changed text; unchanged projection rules do not independently require the full Windows parity gate. |
| UI, localization, or audio behavior                                 | The applicable implementation route plus focused visual, catalog, or audible evidence for the changed user-facing behavior. New locales or changes to locale selection, signatures, or WinUI projection rules include native Windows parity validation.                                                                                                                                                                                                                                              |

Alongside the selected completion route, use [design review](../design-review.md) for structural acceptance and refactor evidence when work changes ownership or a public boundary, introduces an abstraction, or structurally refactors a subsystem.

Follow the [formatting order](#formatting-before-validation) before completion checks.
Run from the project root. Ordinary build/test/run uses pinned native Bazel;
project-specific validation uses `./ao`, or `ao.bat` on Windows.
For changes requiring complete native validation, run:

```bash
./ao check
# CI supplies its own configuration:
./ao check --ci
```

`check` prepares the ready native environment and runs one `bazel test //...`
([completion gate](../bazel.md#completion-gate)), so the graph itself owns live
documentation, formatting, lexical source guardrails, test and class naming
audits, workflow lint, Ruff/mypy, one target per tooling module, native tests
and the whole-graph lint test. `--ci` adds the repository CI configuration;
`check` has no `--commit` selector and always checks the complete source
snapshot. After Bazel succeeds, `check` verifies BEP passing summaries for its
required platform-selected checks; cached passes are accepted unless `--fresh`
is selected, and optional test semantics are unchanged. Standalone `ao check docs`,
`ao check format`, and `ao check lint` remain task-specific entries; the docs semantics check is always global. An explicitly invoked `./ao check` is complete for every change,
including documentation-only edits: the whole-graph lint test runs regardless of
the diff, and a cold cache pays that analysis cost — cache reuse never makes the
gate free. Docs-only local validation uses `./ao check docs` plus
`./ao check format --docs`, while docs-only CI events route to the documentation job
([continuous integration](#continuous-integration)). CI uses the same command
for native changes, and its separate documentation job remains responsible for
docs-only routing. The tooling targets also test the document checker against
isolated fixtures.

`check lint` remains a useful standalone semantic check: source audits and
Python checks for files in scope, plus native clang-tidy coverage for selected
C++ files and the Linux-owned workflow-lint stage over direct `.github/workflows` YAML
([workflow lint](../linting.md#github-actions-workflow-lint)). Its C++ stage
stays diff-scoped; `check` does not invoke it, and the gate's whole-graph lint
coverage is the declared `//:production_lint` test
([lint reports](../bazel.md#lint-reports)).
It never rewrites source. Sanitizer execution remains separate from the normal
lint configuration; a sanitizer result cannot stand in for native lint.

`./ao check` is the completion route
([completion gate](../bazel.md#completion-gate)): it builds the platform
products and the manual GUI/performance harnesses without running them, runs
the portable suites, the native process tests, the GTK, backend and lint-fixture
tests listed by `//:check_tests`, the root `//:native_policy` audit, and on
Linux the `//:native_driver_policy` diagnostic and
[instrumentation analysis tests](../bazel.md#native-instrumentation-tests). On
Windows it runs the two unattended native console probes plus, under
`--config=ci`, the declared WinUI automation test. By default it admits passing test summaries from Bazel's
result cache or new executions; `--fresh` adds `--nocache_test_results` so every
selected test executes again
([test-result cache admission](../bazel.md#test-result-cache-admission)).
Documentation, formatting, source and naming audits, workflow lint, the native
policy tests and the tooling targets are graph tests inside the same check;
standalone `check docs`, `check format`, and `check lint` remain available for
task-specific validation.

C++ concurrency-sensitive changes additionally follow
[concurrency and sanitizer validation](concurrency-and-sanitizer.md) and run:

```bash
# Focus a binary that owns the changed concurrency contract:
bazel test //test:async_test --test_filter='[concurrency]~[.]'
./ao check --tsan --fresh
```

`./ao check --asan` and `./ao check --tsan` run the full normal fastbuild
completion first, then a separate managed sanitizer profile in the same
selected tree; a Debug result is never relabeled as a sanitized
one ([sanitizer profiles](../bazel.md#sanitizer-profiles)). The normal
completion already includes the Linux-only instrumentation analysis tests,
which prove configured flags and providers rather than executing a sanitizer
suite. Both admit cached
test results unless `--fresh` requires new executions; a memory-safety or
concurrency fix asks for `--fresh`, because rerunning an unchanged binary is
what samples another interleaving.

Python subprocess or thread changes need native process-level regressions;
rebuilding unchanged C++ under TSan does not validate the Python runtime.

Use focused filters while implementing or diagnosing a concrete behavior:

```bash
bazel test //test:query_test --test_filter='Component - behavior'
bazel test //test:uimodel_test --test_filter='Component - behavior'
```

Use the actual owning executable: a selected binary with no matching case
fails. On Linux, GTK and backend tests declare their own context, and the declared
context-specific wrapper starts only the private fixtures that context
requests — display, session bus, GTK environment profile and private audio
for `gtk`; a private session bus with private PipeWire services and no display
for `audio` — so an explicit label needs only the ready native tools and SDK.
Focused results do not replace a full gate required by the table.
Reuse a successful check when the relevant source, tests, dependencies,
configuration, platform, and selected scope are unchanged.
Do not repeat it merely because a skill hands off to another skill, a commit is
created, or unrelated prose changes.
Re-run affected checks after corrections; broaden only when new evidence shows
another execution boundary is involved.

`check lint` is read-only, resolves its source scope once, and skips empty stage
subsets. It performs no formatting or global documentation check; use
`check format` and `check docs` separately.
Explicit file and directory scopes for `format`, `check format`, `check lint`,
and `check analyzer` must exist; a missing path fails even with `--report-only`.
Existing empty directories and scopes with no applicable files remain valid.
Use an explicit scope or `--commit <base>` when validating a subset of a larger
branch. `--all` requests whole-source lint; `check` independently owns the
repository-wide `guardrails` target and the `//:production_lint` test.

Required `check lint`, the graph's Ruff/mypy targets, and scoped
formatting corrections needed to complete an authorized edit do not require a
separate lint request. Review any formatting diff and preserve unrelated work.
Standalone lint campaigns or cleanup beyond the authorized change require an
explicit request. Such a request uses:

```bash
./ao check lint
```

If the task is explicitly about coverage percentage or missing lines, use `coverage-workflow.md` instead of guessing from source files.

### Formatting before validation

Finish source edits and any needed `./ao format` before the required completion
checks. Run formatting as a separate step, review its diff, and re-read rewritten
files before relying on earlier file text. Do not chain source-rewriting format
with builds or tests, or repeat it merely because another build or test starts.
For a route requiring full validation, the order is source edits and formatting,
then `./ao check`, whose graph includes the formatting and guardrail targets.
Standalone scoped `./ao check format` and `./ao check lint` can be used while
iterating without repeating native completion.

If a later correction rewrites validated inputs, rerun the affected checks under
the reuse rules above. A passing check from before that rewrite is not evidence
for the new inputs.

### Continuous integration

The existing Linux, macOS Intel/ARM64, and Windows check identities are
retained. A separate unconditional documentation job resolves the event's
verified comparison base once, then runs `ao check docs` and
`ao check format --docs` for
documentation-only, code-only, and mixed changes before native build work.
A successful, nonempty Markdown-only change in the recognized root-document,
`doc/`, `.agents/skills/`, `tool/`, `app/`, or `asset/` paths skips native jobs.
The documentation gate includes naming vocabulary and referenced lint-check
contracts, so prose changes cannot bypass their semantic validation.
The ignored local `doc/plan/` tree remains outside documentation discovery;
its path classification does not make it a supported tracked-document location.
The documentation job prepares the Python version from
`script/ao/toolchain.json`, installs the hash-locked documentation parser from
`script/ao/docs-requirements.txt` with `--require-hashes`,
`--only-binary=:all:`, and `--no-deps`, then prepares the hash-pinned Node/Prettier
closure with `python -m ao setup prettier`. It invokes
`python -m ao check docs` and `python -m ao check format --docs` with
`PYTHONPATH=script`; setup and both checks use host Python, outside Nix and
without preparing a native SDK.
Unknown paths, script or workflow changes, empty comparisons, and manual
`workflow_dispatch` runs select the full native matrix.
Renames are compared as deletion plus addition so moving code into a Markdown
path cannot bypass native validation.
If classification or documentation validation fails, the existing native jobs
are still scheduled and run a failing prerequisite step before native setup or
compilation, rather than appearing successfully skipped.
This changes workflow routing without changing repository branch-protection rules.
The native jobs run `ao check --ci` in fastbuild: the same graph covers live
documentation, host-compatible tooling, native tests, the native-policy audits
and the whole-graph lint test. Linux and Windows run the full tooling suite. macOS
uses the existing jobs and `bazel test //...`, not ordinary `//:tests` and not a
new job. From the tooling modules it selects `test_bazel_remote_cache`,
`test_bazel_appkit_deps`, and `test_bazel_environment` only.
`test_bazel_appkit_deps` reads real host contract
runfiles and projects a fixture SDK; that is not a native Debug build. macOS does
not test exact Homebrew Python patch parity or the full tooling suite. Ordinary CI admits matching passing Bazel test
results from cache; use
`--fresh` when new executions are required for diagnosis ([CI product
gates](../bazel.md#ci-product-gates-and-r2-validation)). Daily CI is fastbuild
only. Linux adds a separate bounded bare Bazel entry without duplicating the
whole-graph lint check. Release verification is deferred to its own job. The Linux job's graph includes the
[instrumentation tests](../bazel.md#native-instrumentation-tests) inside the same
check. Those tests require the native SDK; they do not expand the documentation
job or certify sanitizer or coverage runtime execution. CI runs the same Bazel completion route as the
local commands above.

## Common smells to fix while writing tests

- The test name only says the class name.
- A mutation test has no read-back assertion.
- The test checks only `has_value()` or `count` when content matters.
- A GTK test duplicates detailed policy already covered in `uimodel`.
- A pure rule is tested through a full widget tree.
- A full production import/export/scan workflow is marked unit merely because it lives in the core target.
- The test uses sleep/yield to make async behavior pass.
- The same fixture setup is copied into many cases.
- The expected value is computed by duplicating the production algorithm.
- The test relies on incidental widget hierarchy or display text when a stable accessor would be better.
- The test violates the testability seam order in
  [fixtures and helpers](fixture-and-helper.md).
- Commented-out assertions or stale TODO comments remain in the test.
- A new case is appended to a giant test file even though it has a clear separate contract area.

## Final review checklist

Before finishing, confirm:

- The test or check meets the [admission rule](scope.md#the-admission-rule).
- The test lives at the lowest layer that proves the behavior.
- The name explains behavior and condition.
- Cases carry no tag except a retained `[concurrency]` or `[.manual]` marker.
- The test asserts observable outcomes, not implementation details.
- Mutations have postconditions.
- Async/GTK behavior is deterministic.
- Concurrent cancellation and teardown cover the applicable race matrix.
- C++ concurrency changes pass the applicable TSan gate; Python process/thread changes use native regressions.
- Fixtures reduce noise without hiding the behavior under test.
- GTK tests do not duplicate policy better tested in `uimodel`.
- Testability seams follow [fixtures and helpers](fixture-and-helper.md).
- New test files are registered as described in [Adding new files](#adding-new-files).
- Focused validation has been run when practical, or skipped with an honest reason.
- The applicable completion route above passes, with its scope and any missing host evidence reported.
