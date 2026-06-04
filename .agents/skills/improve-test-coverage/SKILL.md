---
name: improve-test-coverage
description: "Guides agents on how to systematically achieve >95% C++ unit test coverage using gcov, Catch2, and Aobus-specific conventions."
---

# improve-test-coverage

This skill provides a systematic workflow for agents to analyze and improve C++ unit test coverage in the Aobus project. 

It is designed to handle edge cases in coverage, find missing lines efficiently, and write Catch2 tests that simulate complex logic (e.g. batch operations, UI-driven data updates, lambda dispatching).

## Phase Contract — C2 delegation (machine path)

Implementing a decided test plan — adding boundary cases to an **existing, already-registered** Catch2
file — can be delegated to a mid-tier model (capability **C2**) via `script/agent/test_phase.sh`
(packet-driven). Deciding *what* to test and *which* boundaries is a **C3** judgement; C2 only
implements the plan. The rest of this document is the manual / frontier (C3) path for choosing targets.

- **Capability:** C2 (scoped implementation inside a fixed plan, validated by a real build + run).
- **Worker:** per `script/agent/routing.env` (`route_c2_worker`; pilot default: GPT-5.5 via codex).
- **Inputs (Phase Packet):** one existing test file (`inputs[0]`), the plan (packet body), and a
  Catch2 filter (`validation_args`) for the validation.
- **Validation:** an allowlisted build+run id — `test-core` / `test-gtk` (`script/agent/validation.env`);
  the filtered suite must build and pass. Re-run by the harness; never trusted from the model.
- **Iterate:** a failed build/test is fed back to the worker, up to a round budget, then escalate.
- **Isolation / harness-diff / guard / temporal isolation:** identical to the C1 lint phase — the worker
  edits a sandbox copy; the dispatcher takes the patch by diff, applies, validates, keeps or rolls back.
- **Scope limit (structural):** this **augments an existing, registered** test file only. A **new** test
  file needs a `test/CMakeLists.txt` registration (a guarded path), so new-file scaffolding is a **C3**
  task, not C2.
- **Escalate to C3 when:** the plan needs design judgement, a new file/registration, a public-API
  change, or the loop cannot produce a passing test.
- **Run:** via a Phase Packet through `script/agent/dispatch.sh <packet>` (routes
  `improve-test-coverage/C2` → `test_phase.sh`), or `script/agent/test_phase.sh <packet>` directly.

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

## 2. Core Philosophy: Test the Interface, Not the Implementation

Our ultimate goal is to **find bugs in the code**, not just to pass tests for the sake of a >95% KPI. Always design your test cases based on the public interface shape, rather than the current internal implementation.

- **Do not blindly delete failing or "unreachable" tests:** If an interface conceptually allows malformed input, test that input. Do not delete a test case just because you know the current internal implementation guards against it or skips it.
- **Test boundaries and invalid inputs:** Feed the public interface with intentionally corrupted data, invalid enums, out-of-bounds IDs, or malformed structures. Verify that it gracefully handles or rejects them without crashing (e.g., preventing buffer underflows or memory corruption).
- **Avoid Default/Zero Verification:** Never verify logic using default initializers (e.g., testing against `0`, `false`, or empty strings) unless you are explicitly testing empty/boundary behavior. If a getter always returns `0` due to a bug, testing it against `0` will falsely pass. Always populate your mock/spec objects with unique, non-trivial values (e.g., `rating = 5`, `codecId = 3`) to prove the code is actually routing the correct data.
- **Avoid over-mocking:** Do not mock or bypass components based on internal implementation details if you can test the real public interface interactions instead.

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

## 4. Execution Workflow

When tasked to reach 95% coverage for a file or module:
1. **Locate & Inspect:** Run `./script/run-coverage.sh <test_filter>` to get a report of all low-coverage files and their exact missing lines + context.
2. **Analyze:** Read the provided context in the terminal to understand the logical condition (e.g. an `if` branch or a `lambda`) required to reach them.
3. **Test:** Add a new `SECTION("...")` to the corresponding Catch2 test file (e.g. `test/unit/runtime/TargetSourceFileTest.cpp`).
4. **Verify:** Re-run the same `./script/run-coverage.sh` command. Verify that the missing lines disappear from the report and the percentage goes up.
5. **Repeat** file-by-file until the module hits > 95%.
