---
id: development.test.coverage-workflow
---
# Coverage workflow

## Generating and inspecting coverage

Coverage measurement is supported on Linux only because the portal uses GCC and gcov. Do not blindly guess which lines are uncovered, and do not manually run gcov or CMake. Run the coverage command with an optional Catch2 filter:

```bash
./ao coverage "SmartListEvaluator*"
```

This script will automatically:

1. Ensure the coverage build tree is initialized (normally `/tmp/build/<project-directory>/coverage`, or under `AOBUS_BUILD_ROOT` when overridden).
2. Build the tests.
3. Run the coverage test target with the provided filter.
4. Process all `.gcda` files and print out any files that have missing coverage (`#####:`).
5. Output the exact missing lines with 6 lines of surrounding context.

Select `--cli`, `--tui`, or `--gtk` for a frontend suite; the default is core, and `--all` runs core, TUI, CLI, and GTK. Use repeatable `--scope <repository-relative-prefix>` options to limit the reported source set without changing which tests run. An explicitly requested scope with no measurable lines fails rather than reporting a vacuous percentage.

Coverage extraction includes test objects so production headers instantiated only in tests contribute; reports retain only `app/`, `lib/`, and `include/` sources. Coverage runs are deliberately unsharded because multiple test processes would write the same gcov counters.

The command may still print a partial report after a test failure, but it
returns the first failing suite's non-zero status. A partial report is never a
green coverage result.
Failed gcov extraction, missing executable project data, or any explicitly
requested scope without measurable lines also fails the command.
An unavailable measurement is never reported as 100% coverage.

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

1. **Locate and inspect**: run `./ao coverage <test_filter>` to get missing lines with context.
2. **Analyze**: read the context to understand the logical condition required to reach them.
3. **Test**: add a new `SECTION("...")` or `TEST_CASE` in the corresponding Catch2 test file.
4. **Verify**: re-run `./ao coverage`. Confirm the missing lines disappear and the percentage increases.
5. **Repeat** file-by-file until the module exceeds 95%.
