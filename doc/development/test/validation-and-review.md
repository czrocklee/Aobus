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
a build tree or check ownership; other applicable lint stages may need native tools.
The [ownership audit](../bazel.md#test-ownership) checks that each compatible source
that registers cases has a declared executable owner reachable from the appropriate
completion roots. Put `cc_test` owners in the actual completion test selection;
only building the owner does not exercise its tests. Manually run `cc_binary`
owners, such as benchmarks, only need to be built. Local and CI native
completion checks public BEP summaries for configured `cc_test` registration
owners, mapping native context wrappers through the graph. A cached pass is not
a fresh execution, and a target summary does not prove case-level execution;
the standalone ownership audit does not verify execution. Keep throwaway tests
outside the checkout.
When moving tests between binaries, compare runtime test lists on each affected
platform.

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

During implementation, debugging, and review-fix rounds, build or test the affected Bazel targets rather than running the completion gate after each turn. Check platform-specific changes early on their native host, such as WinUI on Windows.

Run the applicable checks below before opening a ready-for-review PR, marking a draft ready, or delivering a final local commit. Run full validation earlier on explicit request.
After review fixes settle, rerun any checks they invalidated before sign-off,
including full local completion where required.
Checkpoint commits and backup pushes alone do not require the native matrix. When handing back uncommitted work, report any required checks not run.

| Change or task                                                      | Completion evidence                                                                                                                                                                                                                                                                                                                                                                                                                                                                                          |
| ------------------------------------------------------------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------ |
| Read-only review or diagnosis                                       | Source evidence and the relevant existing validator or focused reproducer. No build is required solely to finish a review.                                                                                                                                                                                                                                                                                                                                                                                   |
| Documentation or agent instructions                                 | `./ao check docs` and `./ao check format --docs`, agreement with the owning code/policy, and representative task routing when skill behavior changes. Changes to skill scripts also take the tooling row.                                                                                                                                                                                                                                                                                                    |
| Python portal or automation scripts                                 | `./ao check --extend` on Linux, `ao.bat check --extend` on Windows for affected Windows behavior; these include the tooling targets. Use the [standalone tooling suite](test-suite.md#tooling-and-lint-fixtures) while iterating. Exercise changed shell/bootstrap/process boundaries on their native hosts. macOS runs its SDK and native environment, remote-cache, wheel-lock, and AppKit scenario supervisor modules; use native portal probes and scoped `check lint` for other host-specific behavior. |
| C++ implementation, headers, or tests                               | One full native `check --extend`, including its declared whole-graph lint test. Add affected native hosts when their implementation, toolchain, or frontend is involved.                                                                                                                                                                                                                                                                                                                                     |
| Build configuration, dependencies, native test orchestration, or CI | Tooling checks plus the affected native build/test matrix. Include Release or sanitizers when the changed configuration or behavior requires them.                                                                                                                                                                                                                                                                                                                                                           |
| Catalog-only wording in an existing maintained locale               | Build affected frontends to regenerate catalogs, run the catalog suite (`bazel test //test:i18n_test`) and any affected message-specific tests identified by the [text-catalog reference](../../system/presentation/text-catalog.md#compatibility-and-test-authority), and inspect the changed UI at normal and constrained widths. Native Windows UI evidence is needed when WinUI consumes the changed text; unchanged projection rules do not independently require the full Windows parity gate.         |
| UI, localization, or audio behavior                                 | The applicable implementation route plus focused visual, catalog, or audible evidence for the changed user-facing behavior. New locales or changes to locale selection, signatures, or WinUI projection rules include native Windows parity validation.                                                                                                                                                                                                                                                      |

Alongside the selected completion route, use [design review](../design-review.md) for structural acceptance and refactor evidence when work changes ownership or a public boundary, introduces an abstraction, or structurally refactors a subsystem.

Follow the [formatting order](#formatting-before-validation) before validation.
The full [completion gate](../bazel.md#completion-gate) is `./ao check --extend`
(`ao.bat check --extend` on Windows). Neither bare `check` with scoped
production lint nor standalone [`check lint`](../linting.md#scope-behavior)
replaces a required full gate. A sanitizer result cannot stand in for native
lint. See [CI and Release validation](../ci.md) for hosted checks and evidence.

For a C++ memory-safety fix, run [fresh native ASan](concurrency-and-sanitizer.md#sanitizer-findings)
on the affected tests and supported hosts. For a C++ concurrency-sensitive change,
follow [concurrency validation](concurrency-and-sanitizer.md#validation): exercise
the affected binary (for example, `bazel test //test:async_test --test_filter='[concurrency]~[.]'`)
and rerun the owning tests under TSan with `--nocache_test_results` on affected supported hosts. A compile-contract check
is not an instrumented execution.

Python subprocess or thread changes need native process-level regressions;
rebuilding unchanged C++ under TSan does not validate the Python runtime.

Use focused filters while implementing or diagnosing a concrete behavior:

```bash
bazel test //test:query_test --test_filter='Component - behavior'
bazel test //test:uimodel_test --test_filter='Component - behavior'
```

Filter the executable that owns the case. Even for a filtered run, the Linux
test wrapper starts only the [private fixtures](../bazel.md#direct-bazel-without-the-portal)
declared by the selected target. Do not start the services yourself.
A focused test or narrower CI job does not replace a required full gate.
Reuse a passing result while its source, tests, dependencies, configuration, platform, and scope remain unchanged.
After a correction, converge with the narrowest check that covers it (such as the owning `bazel test` target or `./ao check lint <paths>`) before rerunning a required full gate; broaden the scope only if another boundary is involved.

`check lint` is read-only, resolves its source scope once, and skips empty stage
subsets. It performs no formatting or global documentation check; use
`check format` and `check docs` separately.
Explicit file and directory scopes for `format`, `check format`, `check lint`,
and `check analyzer` must exist. Relative paths are resolved from the worktree root; a missing path fails even with `--report-only`.
Existing empty directories and scopes with no applicable files remain valid.
Use an explicit scope or `--commit <base>` for standalone lint on a subset of a
larger branch, or `--all` for whole-source standalone lint.

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

For read-only feedback on selected files, use `./ao check format` or `./ao check lint`. Finish edits, apply any needed formatting, and review the formatting diff before completion checks. Keep the rewrite separate from builds and tests. Re-read rewritten files before editing them again; rerun checks whose inputs changed.

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
- The affected C++ concurrency tests pass under TSan; Python process/thread changes use native regressions.
- Fixtures reduce noise without hiding the behavior under test.
- GTK tests do not duplicate policy better tested in `uimodel`.
- Testability seams follow [fixtures and helpers](fixture-and-helper.md).
- New test files are registered as described in [Adding new files](#adding-new-files).
- Focused validation has been run when practical, or skipped with an honest reason.
- The applicable completion route above passes, with its scope and any missing host evidence reported.
