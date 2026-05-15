---
name: check-code-conformance
description: Audit and validate C++ code against Aobus's coding standards. Use when reviewing code, fixing linting issues, running clang-tidy-backed conformance checks, or ensuring new implementations follow project conventions for naming, member ordering, and modern C++ usage as defined in CONTRIBUTING.md.
---

# Aobus Code Conformance Audit

Use this skill to systematically audit C++ source files against the Aobus C++ Coding Guide.

Focus on concrete rule violations from [CONTRIBUTING.md](../../../CONTRIBUTING.md), not generic style commentary. Prefer reporting a short list of high-confidence violations with precise fixes over a long list of debatable nits.

Use Aobus's built-in `clang-tidy` integration as the primary conformance signal when the task involves changed C++ code, linting cleanup, or pre-commit verification. Combine automated findings with a small residual manual review; do not treat raw `clang-tidy` output as the final answer.

## Scope Boundary

This skill owns **style, coding conventions, and tooling gates only**.

- In scope: formatting/spacing, naming and member order, `const`, `final`, `auto`/braced initialization, designated initializers, C API `::` prefixes, modern standard-library conventions, `clang-format`, and conformance-owned `clang-tidy` warnings.
- Out of scope: feature correctness, behavior regressions, performance, architecture, API design, test adequacy, and semantic ownership/lifetime review.
- If a `clang-tidy` warning is bug-risk, performance, security, concurrency, or lifetime related, report it separately as **route to code review** instead of mixing it with style findings or auto-fixing it here.
- If the user explicitly asks for formal code review, keep this skill read-only and subordinate to the builtin `code-review` workflow; `reviewing-code` only supplies Aobus-specific review guidance.

## Default Execution Strategy

Use the smallest execution mode that can reliably finish the task. Prefer automation first: run the project `clang-tidy` gate when it is relevant, triage diagnostics, then manually audit only rules that are not reliably covered by tooling.

- **Narrow local change**: run this skill in a single agent, audit only the touched files, fix issues directly, and run targeted verification. If C++ files changed, use the targeted `touch` + `./build.sh debug --tidy` path unless the change is clearly too small to benefit from it.
- **Broad conformance audit or weak-model environment**: use the multi-agent workflow below. Parallel agents should discover findings, not edit files.
- **Formal code review**: keep this skill read-only and subordinate to the builtin `code-review` workflow; run only the requested standards or tooling checks.
- **Pre-commit validation**: run the repository `clang-tidy` build path first, then perform the residual manual checks below and report only actionable findings.

When multiple agents are available, prefer **tooling verifier plus residual rule-sliced read-only auditors plus one fixer** over giving every agent the full standards checklist. Weak models perform better when each agent checks one uncovered rule group with a narrow scope.

## Automation Coverage Map

Treat these checks as tooling-owned before spending manual review time on them. Manual auditors should only inspect these areas when `clang-tidy` is unavailable, a diagnostic looks wrong, or a local exception needs judgment.

- `aobus-readability-control-block-spacing`: Rule 2.1.2 blank lines around control blocks and top-level test macros.
- `readability-identifier-naming` plus `aobus-readability-identifier-naming-extensions`: Rules 2.2.1-2.2.6 naming conventions, including data-member prefix extensions.
- `aobus-modernize-concrete-final`: Rule 4.2.1 concrete classes/structs that should be `final`.
- `aobus-readability-c-api-global-qualification`: Rule 2.6.3 external C-library functions and types needing `::`.
- `aobus-readability-std-c-library-qualification`: Rule 2.6.4 C functions/types from C++ standard headers needing `std::`.
- Built-in `modernize-concat-nested-namespaces`: Rule 2.6.1 nested namespace syntax.
- Built-in `modernize-use-using`: Rule 2.7.4 `using` over `typedef`.
- Built-in `cppcoreguidelines-pro-type-cstyle-cast`: Rule 2.8.1 no C-style casts.
- Built-in `performance-avoid-endl`: Rule 2.9.1 prefer `'\n'` over `std::endl`.
- Built-in modernize/performance formatting diagnostics where available: Rule 3.1.2 preference for modern formatting; confirm fixes manually when formatting semantics are non-trivial.
- Built-in `modernize-use-ranges`: Rule 3.1.4 ranges algorithms where the replacement is clear.
- Built-in `modernize-use-starts-ends-with`: Rule 3.1.6 prefix/suffix checks.
- `aobus-modernize-forbid-nodiscard`: Rule 3.2.7 no `[[nodiscard]]`.
- `aobus-readability-unused-suppression-style`: Rule 3.2.8 unused parameter/variable suppression style.
- `aobus-readability-optional-naming-and-usage`: Rule 3.2.1.1 `std::optional` `opt` prefix and no `.has_value()` checks.
- `aobus-modernize-member-initializer-braces`: Rule 3.4.3 brace initialization in member initializer lists.
- `aobus-modernize-local-initialization-style`: Rule 3.4.5 local initialization style. Treat diagnostics around containers, C APIs, and primitives carefully.
- `aobus-modernize-forbid-trailing-return`: Rule 3.4.6 traditional return syntax for non-lambda functions.
- `aobus-modernize-lambda-params`: Rule 3.4.7 omit empty lambda parameter lists.
- `aobus-threading-policy`: Rule 4.4 mechanical threading-policy checks. Route semantic concurrency risk to `reviewing-code`.
- Built-in `readability-function-cognitive-complexity`: Hotspot signal only; refactoring decisions remain human-reviewed.

## Residual Manual Review Map

After the tooling pass, focus manual conformance review on rules that are still hard to automate or require project judgment:

- Rule 2.3.1: headers use `#pragma once`.
- Rule 2.4.1: include groups are separated and ordered as paired/project, third-party, then standard library.
- Rule 2.5.1: `.cpp` member definitions follow the header declaration order.
- Rule 2.5.2: header access sections are ordered `public` -> `protected` -> `private`.
- Rule 2.7.1: plain `int`/`unsigned` are used only for external APIs, toolkit callbacks, or other justified API-shaped boundaries.
- Rule 2.7.2 and 2.7.3: owning strings and fixed buffers use C++ library types unless an external C API requires otherwise.
- Rule 2.9.2: runtime diagnostics use the project logging facility rather than `std::cout`/`std::cerr`.
- Rules 3.1.1, 3.1.3, 3.1.5, 3.1.7, 3.1.8, 3.2.1, 3.2.2-3.2.6, 3.3.1, 3.4.1, 3.4.2, and 3.4.4: review when touched code makes the coding-guide preference directly applicable; avoid speculative rewrites.
- Rules 4.2.2-4.2.3, 4.3.1, and 5.x: review for clear conformance problems, but route behavior, ownership, error-model semantics, and architecture trade-offs to `reviewing-code` when they are not mechanical.

## Execution Modes

### Coordinator Mode

Use this mode when the task is broad, the file set is unclear, or multiple agents are available.

Responsibilities:

1. Determine the target files. Prefer changed C++ files unless the user requested a full-project audit.
2. Run or assign the tooling verifier first when C++ files changed.
3. Split residual read-only audit work by uncovered rule group, not by asking every agent to enforce every rule.
4. Require structured findings from auditors using the schema below.
5. Merge, deduplicate, and discard low-confidence findings.
6. Apply accepted fixes yourself or assign exactly one fixer.
7. Run the narrowest useful verification and report failures honestly.

### Rule Auditor Mode

Use this mode for parallel subagents.

Rules:

- Read target files fully before reporting findings.
- Audit exactly the assigned rule group.
- Do not edit files, run formatters, stage files, or perform broad cleanup.
- Return only high-confidence findings in the structured schema.
- If a finding may be a framework/API constraint, mark it as uncertain or omit it.

### Fixer Mode

Use this mode after findings are accepted.

Rules:

- Be the only writer unless the coordinator explicitly assigned disjoint file ownership.
- Apply only accepted findings in the assigned files.
- Do not opportunistically fix unrelated issues.
- Preserve behavior and ownership/lifetime semantics.
- Batch fixes by risk: mechanical spacing and namespace fixes first, semantic const/RAII/header changes later.

### Verifier Mode

Use this mode after fixes or for validation-only tasks.

Responsibilities:

- Run targeted formatting, `clang-tidy`, build, or tests based on the touched files and risk.
- Save reusable long output under `/tmp` when useful.
- Summarize commands, pass/fail status, and relevant diagnostics only.

## Multi-Agent Workflow

Default to this workflow for broad audits or weak models:

1. **Scope**: identify target C++ files (`.cpp`, `.h`, `.hpp`) and any relevant generated or test files.
2. **Tooling pass**: run targeted `clang-tidy` when relevant and turn in-scope diagnostics into structured findings.
3. **Dispatch read-only auditors** for residual manual rules only. Do not allow audit agents to modify files.
4. **Collect findings** using the schema below.
5. **Merge findings** by `(file, line, rule)` and keep only high-confidence, actionable items.
6. **Fix serially** with one fixer, or in parallel only when each fixer owns a disjoint file set.
7. **Verify** with the narrowest command that would increase confidence; broaden only if needed.
8. **Iterate once** on remaining high-signal issues instead of trying to solve every marginal nit in one pass.

Concurrency safety rules:

- Parallel agents may read the same files, but only one agent should write to a file.
- If parallel fixing is unavoidable, assign explicit file ownership and forbid cross-file edits.
- If a fix requires another agent's file, report it to the coordinator instead of editing.
- Never let a formatting pass race with active edits.

## `clang-tidy` Scheduling

Treat `clang-tidy` as a verifier-owned gate, not as a free-running parallel auditor.

Ownership rules:

- Only the coordinator or verifier should run `clang-tidy`.
- Read-only auditors should not run it; they should perform manual rule checks only.
- Fixers should not run it while other agents are editing. If a fixer is asked to run it, all edits must be complete and no formatter may be running concurrently.
- Do not run multiple `clang-tidy` builds against the same build tree at the same time.

Use three checkpoints when the goal is to fix all warnings in scope:

1. **Baseline capture** before fixing, especially for broad audits or unknown warning state.
   - Targeted scope: `touch` the relevant C++ source/header files, then run `./build.sh debug --tidy` and save output such as `/tmp/ao-clang-tidy-baseline.log`.
   - Full-project scope: run `./build.sh debug --tidy --clean` and save the output.
   - Convert every in-scope diagnostic into structured findings. Treat out-of-scope diagnostics as background unless the user requested full-project cleanup.
2. **Post-fix targeted gate** after the single fixer has applied accepted findings and formatting is complete.
   - Re-`touch` the same target files before `./build.sh debug --tidy` so cached objects are re-analyzed.
   - If warnings remain, do not hand-wave them away. Convert them to findings, fix them, and rerun this checkpoint.
3. **Final zero-warning gate** at the end.
   - For changed-file assurance: `touch` all touched C++ files and run `./build.sh debug --tidy`.
   - For a full-project guarantee: run `./build.sh debug --tidy --clean`.
   - Done means the command succeeds and there are no remaining in-scope `clang-tidy` warnings.

Warning triage rules:

- In-scope conformance `clang-tidy` warnings are blockers for conformance completion.
- Non-conformance `clang-tidy` warnings still block a final zero-warning gate, but route them to code review instead of fixing them in a style pass.
- Prefer real code fixes over suppressions.
- Use `NOLINT` only for a true false positive or required framework/API shape, keep it as narrow as possible, and include a short rationale.
- If a warning comes from generated code, vendored code, or an untouched out-of-scope file, report it separately instead of mixing it with the fix batch.
- When many warnings share one check, group them by check name and fix one pattern at a time, rerunning targeted `clang-tidy` after each batch.

## Structured Finding Schema

Rule auditors should return findings in this shape so the coordinator can merge and filter them:

```json
[
  {
    "file": "app/example.cpp",
    "line": 123,
    "rule": "2.1.2",
    "confidence": "high",
    "problem": "Missing blank line after an if block before the next statement.",
    "suggested_fix": "Insert one blank line after the closing brace.",
    "needs_human_judgment": false
  }
]
```

Omit low-confidence items. Use `needs_human_judgment: true` only when the issue is likely real but depends on framework constraints, ABI requirements, or ownership semantics.

## Residual Rule-Sliced Auditor Assignments

For multi-agent audits, prefer these independent passes after the tooling pass. Do not manually duplicate diagnostics that are already covered by the automation map unless the tool is unavailable or the result needs exception triage.

1. **Header/Include Auditor**: Rules 2.3.1 and 2.4.1. Check `#pragma once` and include grouping/order.
2. **Definition Order Auditor**: Rule 2.5.1. Check `.cpp` member definitions against the header order when the touched class has several methods.
3. **Access Section Auditor**: Rule 2.5.2. Check `public` -> `protected` -> `private` ordering in touched headers.
4. **Integer/API Boundary Auditor**: Rule 2.7.1. Check plain `int`/`unsigned` only where not dictated by external APIs, toolkit callbacks, or C interop.
5. **Resource/Error Policy Auditor**: Rules 3.3.1, 3.4.1, and 5.x only when there is a direct coding-guide violation. Route semantic ownership/lifetime and error-model correctness concerns to `reviewing-code`.
6. **Logging/Output Auditor**: Rule 2.9.2. Check runtime diagnostics for project logging instead of `std::cout`/`std::cerr`.
7. **Tooling Verifier**: Run the repository `clang-tidy` path at the scheduled checkpoints above. Summarize actionable diagnostics and feed them back as structured findings; do not treat raw output as the final answer.

Useful candidate searches before manual judgment:

```bash
rg '^#pragma once|^#include|^\s*(public|protected|private):' include app lib tool test
rg '\b(unsigned|int)\b' include app lib tool test
rg '\bstd::(cout|cerr)\b' include app lib tool test
```

## Subagent Prompt Templates

Use compact prompts like these when spawning weaker subagents. Fill in the rule group and file list explicitly.

Read-only auditor:

```text
You are a read-only Aobus conformance auditor.

Scope: [explicit file list]
Rule group: [one rule-sliced assignment]

Instructions:
- Read each target file fully before reporting.
- Audit only the assigned rule group.
- Do not edit files, run formatters, stage files, or perform cleanup.
- Return only high-confidence findings using the structured finding schema from check-code-conformance.
- Omit uncertain framework/API constraints unless they clearly violate CONTRIBUTING.md.
```

Single fixer:

```text
You are the sole fixer for accepted Aobus conformance findings.

Editable files: [explicit file list]
Accepted findings: [paste filtered findings]

Instructions:
- Apply only the accepted findings.
- Do not edit files outside the editable list.
- Do not opportunistically fix unrelated issues.
- Preserve behavior, ownership, and lifetime semantics.
- Report any finding that requires a cross-file edit outside your ownership.
```

Verifier:

```text
You are the verifier for Aobus conformance fixes.

Touched files: [explicit file list]

Instructions:
- Run the narrowest useful formatting, clang-tidy, build, or test command.
- For targeted clang-tidy, touch the relevant C++ files before ./build.sh debug --tidy.
- Save long reusable output to /tmp when useful.
- Report command, pass/fail status, and relevant diagnostics only.
```

## Audit Workflow

1. **Read target files fully**. Do not audit from snippets.
2. **Load the standard** from [CONTRIBUTING.md](../../../CONTRIBUTING.md) and cite the specific rule being enforced.
3. **Run `clang-tidy` through the project build when it is relevant; this is the default first pass for changed C++ files**.
   - Prefer `./build.sh debug --tidy` for normal validation because it uses the repository's configured checks and now automatically selects the Clang toolchain.
   - **For targeted audits**: Always `touch` the specific C++ source files (implementation files and/or headers) you wish to audit immediately before running the build. This forces the build system to re-analyze them even if they haven't changed.
   - **For full-project audits**: Use `./build.sh debug --tidy --clean`. A clean build is the only reliable way to ensure a comprehensive analysis of every file in the project.
   - Save reusable output to `/tmp`, for example `/tmp/ao-clang-tidy.log`, when you need to inspect or compare findings across iterations.
   - Treat diagnostics in untouched files as background noise unless the user asked for a broader cleanup.
4. **Triage tooling results before manual auditing**. Convert in-scope diagnostics into structured findings, discard true false positives, and route semantic bug/performance/lifetime diagnostics to `reviewing-code`.
5. **Prioritize residual manual rules and repeated project violations first**, especially under `app/`, before hunting for marginal style issues.
6. **Separate real violations from framework constraints**. GTK, GLib, PipeWire, ALSA, FFmpeg, and CLI callback signatures often justify plain `int`, raw C pointers, or fixed callback spellings.
7. **Report only high-confidence findings** with file/line references and the concrete change needed.

## `clang-tidy` Guidance

- Use the repository entrypoint, not an ad-hoc one-off command, unless the user explicitly wants a custom `clang-tidy` invocation.
- Aobus wires `clang-tidy` through `AOBUS_ENABLE_CLANG_TIDY`; the standard path is [build.sh](../../../build.sh).
- The CMake setup already distinguishes strict checks for `lib/`, `tool/`, and `app/`, with relaxed checks for `test/`. Respect that split when interpreting findings.
- If a manual rule check and a `clang-tidy` suggestion disagree, follow [CONTRIBUTING.md](../../../CONTRIBUTING.md) and explain the discrepancy briefly.
- When a task is only a code review and not an implementation or validation pass, you may skip running `clang-tidy` if the likely signal is low; say that you skipped it.

## Priority Checks

Run the audit in this order.

1. **Tooling-owned rules from the automation map**
   - Run the project `clang-tidy` path first for changed C++ files.
   - Fix or triage in-scope diagnostics before starting manual nit-picking.

2. **Rules 2.3.1 and 2.4.1: Header and include hygiene**
   - Check `#pragma once`.
   - Check include groups: paired/project, third-party, standard library, separated by blank lines.

3. **Rules 2.5.1 and 2.5.2: Ordering that needs context**
   - Check `.cpp` member definition order against the header.
   - Check header access section order: `public`, `protected`, `private`.

4. **Rule 2.7.1: Plain integer types**
   - Prefer `std::int32_t`, `std::uint64_t`, etc.
   - Do not flag external API, toolkit callback, or C interop signatures.

5. **Rules 3.3.1, 3.4.1, and 5.x: Error/resource policy**
   - Check only direct coding-guide violations such as new `bool` + `lastError()` patterns, `std::optional` used to represent failure, or missing RAII wrappers for owned C resources.
   - Route semantic ownership, lifetime, or recoverability concerns to `reviewing-code`.

6. **Rule 2.9.2: Runtime diagnostics**
   - Prefer project logging macros/facilities over `std::cout`/`std::cerr` outside CLI output paths and tests.

## Common Audit Gotchas

These are subtle patterns that are frequently missed but are required for full conformance:

- **The Move-from-Const Trap**: Rule 4.3.1 (`const` everywhere) has a vital exception: never make a `std::unique_ptr` `const` if it is intended to be `std::move`d later. `std::move` on a `const` object does not move; it triggers a copy (which unique_ptr prohibits).
- **C API Interop & Const**: Many C-based APIs (GLib, ALSA, FFmpeg) take a `void*` (gpointer) for user data. In C++, you cannot implicitly convert a `const T*` to `void*` as it drops const. For objects passed to C callbacks, keep the local variable non-const.
- **Efficient String Building**: While `std::format` is preferred for simple formatting (Rule 3.1.2), building complex multi-line strings in a loop should use `auto ss = std::stringstream{}` as a builder, with `ss << std::format(...)` for content. This avoids O(N²) allocation overhead of naive `s += a + b`.
- **Modern Ranges Hygiene**: The use of `std::ranges::to` and `std::views` requires an explicit `#include <ranges>` even if other range-like headers are present.
- **Lambda Parameter Naming**: Strict `readability-identifier-length` checks are enforced in the `app/` and `lib/` modules. Avoid one-letter names like `s`, `i`, or `it` in lambdas; prefer `value`, `index`, or `item`.
- **Functional Casts vs. Braced Init**: Always prefer `std::string{view}` or `T{args}` over functional style `std::string(view)` or `T(args)` (Rule 3.4.5).
- **Const Pointers to Managed Objects**: When initializing managed widgets with `Gtk::make_managed<T>()`, the pointer itself is typically immutable. Use `auto* const p = ...` instead of `auto* p = ...`.
- **Spacing After Block Closure**: Rule 2.1.2 requires a blank line *after* a closing brace `}` if another statement follows in the same scope.

## `app/` False Positives To Avoid

- `main(int argc, char* argv[])` and toolkit entrypoints are API-shaped and should not be flagged.
- Toolkit callback signatures often require exact C-compatible types (`int`, `void*`).
- Stored raw widget pointers are valid when toolkit-owned or optional UI state.
- Do not flag "could be more modern" unless the coding guide explicitly requires the alternative.

## Reporting Expectations

- Lead with findings, ordered by severity or confidence.
- Distinguish manual standards violations from `clang-tidy` findings when that helps clarity.
- For each finding, include the file, line, violated rule, why it is a violation, and the smallest conforming fix.
- If no violations are found, say so explicitly.

## The Braced Initialization ({}) Audit (Rule 3.4.5)

The project uses a custom `clang-tidy` check for initialization style. Use this section for triage and manual exception judgment, not as a reason to redo a full manual initialization audit after a clean tooling pass.

- **Prefer Braces for Construction**: Replace `T x(a, b);` with `auto x = T{a, b};` when the type is non-primitive and not a container-size/initializer-list ambiguity.
- **Member Initializer Lists**: Modernize `Constructor() : member(val)` to `Constructor() : member{val}`.
- **Empty Initialization**: Use `auto s = std::string{};` instead of `std::string s;` when an explicit string object is needed; prefer string literals for constants per CONTRIBUTING.md.
- **The Vector Pitfall**:
    - `std::vector<int> v(10);` creates 10 elements (correct for size allocation).
    - `std::vector<int> v{10};` creates 1 element with value 10.
    - **Rule**: Keep `()` for size-based allocation; use `{}` for value lists or default construction.
- **Avoiding Most Vexing Parse**: Braces prevent the compiler from interpreting a variable declaration as a function declaration.

## Audit Best Practices

- **Constraint Precedence**: Explicit user hints override any skill default. If a user preference (e.g. "explicit types for primitives") conflicts with Rule 3.4.5, immediately treat it as the highest priority rule.
- **Large File Strategy**: For files >500 lines or structural refactors, read the complete file and make edits with broad, unambiguous context to preserve syntax tree integrity and prevent hallucinated edits.
- **Pattern Remediation**: Identify and fix recurring violation patterns (e.g. all YAML decoders missing `const`) across all scoped files simultaneously for higher throughput.
- **Explicit Conditionals**: Prioritize `if (ptr != nullptr)` over `if (ptr)` whenever raw and smart pointers coexist to ensure explicit intent.

## Technical Lessons Learned (Summary)

- **C API User Data**: User data pointers (`void*`) in C callbacks MUST be passed from non-const local variables.
- **Move-from-Const**: Never mark `std::unique_ptr` as `const` if it is destined for `std::move`.
- **Descriptive Lambdas**: No one-letter variables in lambdas (e.g., `item` instead of `i`).
- **Include Hygiene**: `#include <ranges>` is mandatory for `std::views`; `#include <format>` for `std::format`.
- **Struct Hardening**: Resolve `missing-field-initializer` warnings by adding inline default values in the struct definition.
- **Logging over Silence**: Replace empty `catch` blocks with appropriate logging (e.g. `APP_LOG_TRACE`) to satisfy linters and maintain observability.
- **Primitive Vigilance**: Strictly preserve explicit types for primitives (`int`, `bool`) as per Rule 3.4.5; prevent automated over-application of `auto`.
- **misc-include-cleaner resolution**: fix-first, suppress-last. See dedicated section below.

## `misc-include-cleaner` Resolution Guide

**Always try to fix the code before suppressing the warning.**

1. **"No header providing X" — find the actual header:**
   ```bash
   rg -l 'X' $(echo $NIX_CFLAGS_COMPILE | grep -oP '\-I\K\S+' | head -3)
   ```
   Search nix-store include trees. Don't guess — a wrong guess creates cascading noise. PipeWire and ALSA C headers resolve fine through this path.

2. **Prefer the most specific header** that provides the symbol directly. Umbrella headers are a last resort.

3. **Recognize unfixable system-header cases.** Some glibc/glib C headers (notably `poll.h`, `gtypes.h`, `gtkenums.h`) fail to resolve even when correctly included. This is header-specific — PipeWire and ALSA C headers work correctly. When you hit one of these, switch to fixing the code to eliminate the C symbol reference (e.g., use a C++ equivalent or a local constant). Only if that's impossible, add a NOLINT.

4. **Scope suppressions tightly:** `NOLINT` at usage site > `NOLINT` on include > `NOLINTBEGIN`/`NOLINTEND` for a contiguous block > `IgnoreHeaders` for cross-cutting patterns.

5. **Workflow:** `touch` changed files → `./build.sh debug --tidy` → grep `build.log` for `warning:`.

## References

- **Full Standards**: [CONTRIBUTING.md](../../../CONTRIBUTING.md)
- **Build Entry Point**: [build.sh](../../../build.sh)
- **CMake clang-tidy setup**: [CMakeLists.txt](../../../CMakeLists.txt)
