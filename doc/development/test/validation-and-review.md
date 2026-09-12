---
id: development.test.validation-and-review
type: development
status: current
domain: development
summary: Defines test-file integration, regression expectations, validation gates, and review checks.
---
# Test validation and review

## Adding new files

When creating a new test file:

1. Place it under the matching existing `test/unit/...` module or layer directory, such as `runtime`, `uimodel`, `linux-gtk`, `library`, `query`, `tag`, `audio`, or `utility`.
2. Add it to the correct target in `test/CMakeLists.txt`.
3. Match namespace and include style of neighboring tests.
4. Include only headers that are used.
5. Check the focused `*TestSupport.h` files in the owning layer before creating a new shared helper; do not add an include-all umbrella.
6. Keep file-scope helpers local unless multiple files need them.
7. Do not create duplicate helper types and hide the conflict in a nested namespace; reuse or extend the existing helper instead.

## Regression tests

Regression tests are encouraged when they protect a real bug, especially for:

- Query optimizer correctness.
- Async cancellation/lifetime cleanup.
- GTK widget destruction order.
- Layout measurement stability.
- Import/export data preservation.
- Parser/serializer edge cases.

Name and tag them as regressions when appropriate:

```cpp
TEST_CASE("ImportExportCoordinator - cancellation after worker completion does not post error",
          "[gtk][regression][import-export]")
```

Add a short comment if the assertion is non-obvious or protects a fragile UI/layout lifecycle invariant.

## Large test files

When adding to an already large test file, prefer a new focused file if the behavior belongs to a separable contract area. Split by behavior domain, not by arbitrary line count.

Prefer standalone `TEST_CASE`s for independent behavior contracts; keep `SECTION` for
variants of one contract that share an arrange (see *SECTION vs TEST_CASE* in
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

| Change or task | Completion evidence |
|---|---|
| Read-only review or diagnosis | Source evidence and the relevant existing validator or focused reproducer. No build is required solely to finish a review. |
| Documentation or agent instructions | `./ao docs check`, agreement with the owning code/policy, and representative task routing when skill behavior changes. Changes to skill scripts also take the tooling row. |
| Python portal or automation scripts | `./ao test --tooling` on Linux, `ao.bat test --tooling` on Windows for affected Windows behavior, and scoped `hygiene`. Exercise changed shell/bootstrap/process boundaries on their native hosts. macOS has no tooling suite; use native portal probes and scoped hygiene there. |
| C++ implementation, headers, or tests | One full native `check`, then scoped `hygiene`. Add affected native hosts when their implementation, toolchain, or frontend is involved. |
| Build configuration, dependencies, native test orchestration, or CI | Tooling checks plus the affected native build/test matrix. Include Release or sanitizers when the changed configuration or behavior requires them. |
| Catalog-only wording in an existing maintained locale | Build affected frontends to regenerate catalogs, run `./ao test --core "[catalog]"` and any affected message-specific tests identified by the [text-catalog reference](../../reference/presentation/text-catalog.md#test-authority), and inspect the changed UI at normal and constrained widths. Native Windows UI evidence is needed when WinUI consumes the changed text; unchanged projection rules do not independently require the full Windows parity gate. |
| UI, localization, or audio behavior | The applicable implementation route plus focused visual, catalog, or audible evidence for the changed user-facing behavior. New locales or changes to locale selection, signatures, or WinUI projection rules include native Windows parity validation. |

Alongside the selected completion route, use [design review](../design-review.md) for structural acceptance and refactor evidence when work changes ownership or a public boundary, introduces an abstraction, or structurally refactors a subsystem.

Run commands from the project root through `./ao`, or `ao.bat` on Windows.
For changes requiring the full native gate, run it in this order:

```bash
./ao check
./ao hygiene
```

`check` builds the enabled graph, verifies dependency resolution, and runs
every suite in the native `all` group. `hygiene` is check-only and validates
formatting, source audits for files in scope, Python files in scope, and native
clang-tidy coverage for changed files. Keeping the stages explicit lets
sanitizer and release checks retain their own build trees without implicitly
provisioning a second tidy tree.

C++ concurrency-sensitive changes additionally follow
[concurrency and sanitizer validation](concurrency-and-sanitizer.md) and run:

```bash
./ao test --concurrency
./ao check --tsan
```

Python subprocess or thread changes need native process-level regressions;
rebuilding unchanged C++ under TSan does not validate the Python runtime.

Use focused filters while implementing or diagnosing a concrete behavior:

```bash
./ao test --core "Component - behavior"
./ao test --gtk "Component - behavior"
./ao test --integration "Component - behavior"
```

Focused results do not replace a full gate required by the table.
Reuse a successful check when the relevant source, tests, dependencies,
configuration, platform, and selected scope are unchanged.
Do not repeat it merely because a skill hands off to another skill, a commit is
created, or unrelated prose changes.
Re-run affected checks after corrections; broaden only when new evidence shows
another execution boundary is involved.

`hygiene` is check-only, resolves its source scope once, skips empty stage
subsets, and stops after a formatting failure before running audits or tidy.
Explicit file and directory scopes for `format`, `hygiene`, `test-audit`, and
`name-audit` must exist; a missing path fails even when an audit is advisory.
Existing empty directories and scopes with no applicable files remain valid.
Use an explicit scope or `--commit <base>` when validating a subset of a larger
branch. `--all` requests whole-source hygiene; the full `check` independently
owns repository-wide guardrails.

Required `hygiene`, the Ruff/mypy checks bundled with tooling tests, and scoped
formatting corrections needed to complete an authorized edit do not require a
separate lint request. Review any formatting diff and preserve unrelated work.
Standalone lint campaigns or cleanup beyond the authorized change require an
explicit request. Such a request uses:

```bash
./ao tidy
```

If the task is explicitly about coverage percentage or missing lines, use `coverage-workflow.md` instead of guessing from source files.

### Continuous integration

The existing Linux, macOS Intel/ARM64, and Windows check identities are retained.
CI resolves the event's verified comparison base once, then validates a nonempty
Markdown-only change in the repository documentation or Skills paths with the
`ao docs check` command before skipping native jobs. That gate includes the
documented naming vocabulary and referenced lint-check contracts, so prose
changes cannot bypass their semantic validation.
The documentation job prepares only the Python version from
`script/ao/toolchain.json` and invokes `python -m ao docs check` with
`PYTHONPATH=script`; it does not enter the C++ Nix development environment.
Unknown paths, script or workflow changes, empty comparisons, and manual
`workflow_dispatch` runs select the full native matrix.
Renames are compared as deletion plus addition so moving code into a Markdown
path cannot bypass native validation.
If classification or documentation validation fails, the existing native jobs
run a failing prerequisite step rather than appearing successfully skipped.
This changes workflow routing without changing repository branch-protection rules.

## Common smells to fix while writing tests

- The test name only says the class name.
- A mutation test has no read-back assertion.
- The test checks only `has_value()` or `count` when content matters.
- A GTK test duplicates detailed policy already covered in `uimodel`.
- A pure rule is tested through a full widget tree.
- A runtime unit test performs a full filesystem workflow without being marked workflow/integration.
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

- The test lives at the lowest layer that proves the behavior.
- The name explains behavior and condition.
- Tags identify layer, type, and component.
- The test asserts observable outcomes, not implementation details.
- Mutations have postconditions.
- Async/GTK behavior is deterministic.
- Concurrent cancellation and teardown cover the applicable race matrix.
- C++ concurrency changes pass the applicable TSan gate; Python process/thread changes use native regressions.
- Fixtures reduce noise without hiding the behavior under test.
- GTK tests do not duplicate policy better tested in `uimodel`.
- Testability seams follow [fixtures and helpers](fixture-and-helper.md).
- New files are listed in `test/CMakeLists.txt`.
- Focused validation has been run when practical, or skipped with an honest reason.
- The applicable completion route above passes, with its scope and any missing host evidence reported.

`ao check` rejects unregistered C++ and Objective-C++ test sources before building on every native profile, including macOS. This lightweight invariant does not depend on running the Python tooling suite.
