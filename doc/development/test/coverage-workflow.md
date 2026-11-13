---
id: development.test.coverage-workflow
---
# Coverage workflow

## Generating and inspecting coverage

Coverage measurement is supported on Linux only, in the Debug configuration
over the Bazel graph. Do not blindly guess which lines are uncovered, and do
not manually run the collector or build tools. Run the coverage command:

```bash
./ao coverage
```

Select `--core`, `--application`, `--backend`, `--cli`, `--tui`, or `--gtk` for
a single suite; the default runs core, application, and backend, and `--all`
adds TUI, CLI, and GTK. The system-media, perf, tooling, and lint suites are
excluded from coverage runs. Use repeatable `--scope <repository-relative-prefix>`
options to limit the reported source set — the denominator is the only thing
`--scope` changes; it never narrows instrumentation or the tests run. An
explicitly requested scope with no measurable lines fails rather than reporting
a vacuous percentage.

A Catch2 filter is not accepted: filters require genuine per-case Catch2 XML
proof, which this route does not collect, so a filter request is rejected
explicitly instead of silently narrowing the measurement. The former
renderer-facing `--summary-limit` option is retired.

Measurement contracts:

- Production `app/`, `lib/` and `include/` lines are reported. Test translation
  units are instrumented so authored headers instantiated only in tests
  contribute; line hits are summed across objects and tests, and virtual
  include sources are resolved through Bazel's reported-to-actual source
  metadata rather than guessed.
- A declared `--run_under` wrapper independently checks every observed
  gcno/gcda pair, reader exit and extraction report after running the test once.
  The stock collector and merger still run afterward; their production
  reports must agree with the retained strict measurements. The reader is
  `llvm-cov` from the same selected LLVM SDK, in gcov mode; an ambient `gcov`
  is rejected. Coverage-only repository `JAVA_HOME` selects the
  admitted canonical Nix JDK through the standard `@bazel_tools//tools/jdk:jdk`
  alias. Its `JavaRuntimeInfo.files` are merger runfiles and action inputs;
  the launcher resolves `JAVABIN` and `JAVA_HOME` there, never through an ambient
  fallback. Per-test, final and baseline reports run the original stock Main
  class from Bazel's pinned precompiled deploy JAR with unchanged report
  arguments. This keeps the native route usable with an empty action cache on
  NixOS without introducing a generic Java compiler toolchain or disabling
  header extraction.
- A narrow [rules_cc patch](../../../bazel/rules_cc_coverage_private_headers.patch)
  propagates implementation-dependency virtual-header mappings as coverage
  metadata only. Exported headers, include paths, defines and dependency
  compilation contexts stay private. The declared standard replacement file
  is the sole mapping input; no attempt-level action map or Python stock-merger
  augmentation is read. Strict collection receipts use schema 2 with the
  `declared-standard` contract, retain and hash that standard map, and its
  virtual aliases must agree with the controller's independent configured
  Symlink evidence. A missing virtual mapping fails rather than losing zero-hit
  lines. Strict, stock and combined reports must reconcile on source and
  freshness.
- Every test runs exactly once, unsharded, without flaky retries and without
  cached results; only the current attempt's fresh build-event artifacts can
  contribute. Each configured result's `execution_strategy` records the actual
  BEP spawn strategy (for example `linux-sandbox`, `processwrapper-sandbox` or
  `local`); absent metadata is explicitly `null`, not an inferred strategy.
  This is observability, not a requirement for a particular sandbox or evidence
  of hermeticity. The combined Bazel LCOV report must agree with the checked
  per-test measurements, and suites that contain application code require real
  application implementation counters in the report.
- Windows and macOS are rejected before tree or SDK work; there is no SDK
  install fallback, and `--vcpkg-prefix` is not accepted.

The command may still print a partial report after a test failure, but it
returns the first failing suite's non-zero status. A partial report is never a
green coverage result. Failed extraction, missing executable project data, or
any explicitly requested scope without measurable lines also fails the
command. An unavailable measurement is never reported as 100% coverage.

The route reports line coverage only, applies no percentage threshold, and
claims no function or branch metrics. Attempt receipts and the LCOV report live
under `<tree>/coverage/run-*/`. [Bazel development](../bazel.md#sanitizer-and-coverage-instrumentation)
records the command surface and profile boundaries.

Linux GCOV object compilation requires explicit `-fprofile-update=atomic`.
The independent configured-action check rejects missing or contradictory
counter-update options, including forwarded low-level overrides; exec tools and
static archives must not acquire profiling flags. This policy does not apply to
ordinary or sanitizer-only builds. The managed coverage command does not accept
raw compiler/feature overrides.

Earlier non-atomic application profiles contain inconsistent raw control-flow
counts and inferred error-path hits that did not establish actual product errors.
Do not reuse their percentages as validated counter-fidelity evidence or relabel
them as atomic runs. Fresh test passes are a separate result. Strict/stock report
agreement alone does not establish counter-update correctness, and legitimate
execution-path variation can still change covered-line totals across fresh runs.

The project goal is `> 95%` line coverage.

Line coverage is not evidence of thread safety; use
[concurrency and sanitizer validation](concurrency-and-sanitizer.md).

## Core philosophy

The goal is to **find bugs**, not to hit a KPI. Design test cases based on the public interface shape, not the current internal implementation.

- Do not blindly delete failing or "unreachable" tests. If an interface conceptually allows malformed input, test that input.
- Test boundaries and invalid inputs: corrupted data, invalid enums, out-of-bounds IDs, malformed structures.
- Avoid default/zero verification: populate mock objects with unique, non-trivial values (e.g., `rating = 5`, `codecId = 3`) to prove the code routes data correctly.
- Avoid over-mocking: prefer real public interface interactions over mocking internal collaborators.

## Common coverage gaps in Aobus

When you see `#####`, analyze the C++ source context. Common patterns:

### Async lambdas and executor dispatch

Code inside `executor.dispatch([this] { ... })` goes uncovered if the test executor does not execute deferred tasks or does not trigger the callback. Ensure you trigger the signal/callback on the mock so the lambda is invoked.

### Batch vs single mutations

Update functions may branch on `ids.size() == 1` vs `ids.size() > 1`. Write explicit tests for both the single-element and multi-element paths.

### Container set operations

`std::flat_set`, `std::ranges::unique`, and `std::ranges::set_difference` branches remain uncovered if batch operations do not contain overlapping elements. Create overlapping lists to force `inserted`, `updated`, and `removed` vectors to populate.

### YAML node edge cases

`ryml` serialization/deserialization has branches for default values, optional values, and `has_child()` checks. Inject edge-case data: missing string fields, `0` integers, or binary data.

## Execution workflow

When tasked to reach 95% coverage for a file or module:

1. **Locate and inspect**: run `./ao coverage` to get missing lines with context.
2. **Analyze**: read the context to understand the logical condition required to reach them.
3. **Test**: add a new `SECTION("...")` or `TEST_CASE` in the corresponding Catch2 test file.
4. **Verify**: re-run `./ao coverage`. Confirm the missing lines disappear and the percentage increases.
5. **Repeat** file-by-file until the module exceeds 95%.
