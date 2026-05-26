---
name: improve-test-coverage
description: "Guides agents on how to systematically achieve >95% C++ unit test coverage using gcov, Catch2, and Aobus-specific conventions."
---

# improve-test-coverage

This skill provides a systematic workflow for agents to analyze and improve C++ unit test coverage in the Aobus project. 

It is designed to handle edge cases in coverage, find missing lines efficiently, and write Catch2 tests that simulate complex logic (e.g. batch operations, UI-driven data updates, lambda dispatching).

## 1. Generating and Inspecting Coverage

Do not blindly guess which lines are uncovered, and do not manually run gcov or cmake. A dedicated script exists to fully automate configuring the coverage build, compiling tests, running them, and parsing the missing lines.

Run the coverage script with an optional test filter (to save time):
```bash
./script/run-coverage.sh "rt::SmartListEvaluator"
```

This script will automatically:
1. Ensure the coverage build tree (`/tmp/build/coverage`) is initialized.
2. Build the tests.
3. Run `ao_test` with the provided filter.
4. Process all `.gcda` files and print out any files that have missing coverage (`#####:`).
5. Output the exact missing lines with 6 lines of surrounding context so you can immediately see what logic wasn't covered.

The output will clearly state the coverage percentage. The project goal is `> 95%`.

## 3. Common Coverage Gaps in Aobus & How to Fix Them

When you see `#####`, analyze the C++ source file context. Here are common Aobus-specific patterns that often lack coverage:

### A. Async Lambdas & Executor Dispatch
**Problem:** Code inside `executor.dispatch([this] { ... })` often goes uncovered if the test's mock executor doesn't execute deferred tasks, or if the mock doesn't trigger the callback.
**Solution:** 
- Inspect how the test implements `IControlExecutor`. Often, tests use a `NullExecutor` that immediately runs `task()`. 
- Ensure you actually trigger the signal/callback on the mock (e.g. triggering an audio player state change) so the lambda is invoked.

### B. Batch vs. Single Mutations
**Problem:** Update functions might have branches for single elements vs. batches (e.g. `ids.size() == 1` vs `ids.size() > 1`). 
**Solution:** 
- If a test uses `std::array{id}` to insert a single track, it hits the `size() == 1` optimization branch. 
- Write an explicit test passing an array of `>1` elements to cover the batch code path, and vice versa.

### C. Container Set Operations (flat_set, unique, set_difference)
**Problem:** Custom UI projections or library models use `std::flat_set`, `std::ranges::unique`, and `std::ranges::set_difference` for deduplication and delta tracking. If a batch operation does not contain overlapping elements, or does not result in a state transition, the inner loop bodies remain uncovered.
**Solution:**
- Create overlapping lists in your test cases (e.g. `batchRemove({t1, t2})` where `t1` matches the condition but `t2` doesn't) to force `inserted`, `updated`, and `removed` vectors to populate.

### D. YAML Node Edge Cases (RapidYaml)
**Problem:** `ryml` (RapidYAML) node serialization/deserialization has specific branches for default values, optional values, or `has_child()` checks.
**Solution:**
- Ensure tests inject edge-case data (e.g. missing string fields, `0` integers, or binary data like `cover_art`).
- Use YAML anchors/references in the test payload to hit reference resolution logic if applicable.

## 3. Execution Workflow

When tasked to reach 95% coverage for a file or module:
1. **Locate & Inspect:** Run `./script/run-coverage.sh <test_filter>` to get a report of all low-coverage files and their exact missing lines + context.
2. **Analyze:** Read the provided context in the terminal to understand the logical condition (e.g. an `if` branch or a `lambda`) required to reach them.
3. **Test:** Add a new `SECTION("...")` to the corresponding Catch2 test file (e.g. `test/unit/runtime/TargetSourceFileTest.cpp`).
4. **Verify:** Re-run the same `./script/run-coverage.sh` command. Verify that the missing lines disappear from the report and the percentage goes up.
5. **Repeat** file-by-file until the module hits > 95%.
