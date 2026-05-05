---
name: check-code-conformance
description: Audit and validate C++ code against Aobus's coding standards. Use when reviewing code, fixing linting issues, running clang-tidy-backed conformance checks, or ensuring new implementations follow project conventions for naming, member ordering, and modern C++ usage as defined in CONTRIBUTING.md.
---

# Aobus Code Conformance Audit

Use this skill to systematically audit C++ source files against the Aobus C++ Coding Guide.

Focus on concrete rule violations from [CONTRIBUTING.md](../../../CONTRIBUTING.md), not generic style commentary. Prefer reporting a short list of high-confidence violations with precise fixes over a long list of debatable nits.

Do not use this skill as a full correctness review. For regression, design-risk, or patch-level code review, load `reviewing-code` first and use this skill only for standards and tooling validation.

Use Aobus's built-in `clang-tidy` integration as part of conformance validation when the task involves changed C++ code, linting cleanup, or pre-commit verification. Combine automated findings with manual review; do not treat raw `clang-tidy` output as the final answer.

## Audit Workflow

1. **Read target files fully**. Do not audit from snippets.
2. **Load the standard** from [CONTRIBUTING.md](../../../CONTRIBUTING.md) and cite the specific rule being enforced.
3. **Run `clang-tidy` through the project build when it is relevant**.
   - Prefer `./build.sh debug --tidy` for normal validation because it uses the repository's configured checks and now automatically selects the Clang toolchain.
   - **For targeted audits**: Always `touch` the specific C++ source files (implementation files and/or headers) you wish to audit immediately before running the build. This forces the build system to re-analyze them even if they haven't changed.
   - **For full-project audits**: Use `./build.sh debug --tidy --clean`. A clean build is the only reliable way to ensure a comprehensive analysis of every file in the project.
   - Save reusable output to `/tmp`, for example `/tmp/ao-clang-tidy.log`, when you need to inspect or compare findings across iterations.
   - Treat diagnostics in untouched files as background noise unless the user asked for a broader cleanup.
4. **Prioritize repeated project violations first**, especially under `app/`, before hunting for marginal style issues.
5. **Separate real violations from framework constraints**. GTK, GLib, PipeWire, ALSA, FFmpeg, and CLI callback signatures often justify plain `int`, raw C pointers, or fixed callback spellings.
6. **Report only high-confidence findings** with file/line references and the concrete change needed.

## `clang-tidy` Guidance

- Use the repository entrypoint, not an ad-hoc one-off command, unless the user explicitly wants a custom `clang-tidy` invocation.
- Aobus wires `clang-tidy` through `AOBUS_ENABLE_CLANG_TIDY`; the standard path is [build.sh](../../../build.sh).
- The CMake setup already distinguishes strict checks for `lib/`, `tool/`, and `app/`, with relaxed checks for `test/`. Respect that split when interpreting findings.
- If a manual rule check and a `clang-tidy` suggestion disagree, follow [CONTRIBUTING.md](../../../CONTRIBUTING.md) and explain the discrepancy briefly.
- When a task is only a code review and not an implementation or validation pass, you may skip running `clang-tidy` if the likely signal is low; say that you skipped it.

## Priority Checks

Run the audit in this order.

1. **Rule 2.1.2: Missing blank lines around control blocks**
   - Must have blank lines before and after `if`, `for`, `while`, `switch`.
   - Top-level macros like `TEST_CASE` and `SECTION` must have blank lines between them.
   - Exception: Single-line if-statements that are early returns (though even these are preferred with a blank line if followed by logic).

2. **Rule 2.6.3: C functions/types missing `::` prefix**
   - Critical in playback and platform files using FFmpeg (`av_*`, `swr_*`), ALSA (`snd_*`), or PipeWire (`pw_*`, `spa_*`).
   - Example: `::avformat_open_input`, `::snd_pcm_open`.

3. **Rule 3.1.2 & 3.1.4: Pre-C++20 string/algorithm usage**
   - Prefer `std::format` over `std::to_string` or `printf`.
   - Prefer `std::ranges::sort` over `std::sort`.
   - Prefer `std::ranges::find_if` over `std::find_if`.

4. **Rule 4.3.1: Missing `const` on local variables**
   - Apply `const` (or `auto const`) to every local variable that is not reassigned.
   - Flag high-signal cases like complex GTK `RefPtr` or business logic results.

5. **Rule 4.2.1: Concrete classes missing `final`**
   - Recurring in UI classes: `StatusBar`, `MainWindow`, `TagPopover`.
   - Do not flag interfaces (`IAudioBackend`) or observer bases.

6. **Rule 3.4.5: Non-auto initialization for objects**
   - Prefer `auto x = T{...};` over `T x;` or `T x{...};` for all non-primitive types.
   - Exception: Primitive types where `auto` would reduce clarity or require a cast.
   - Exception: When `const` is needed but the object must be `move`d later (standard `unique_ptr` move pattern).

7. **Rule 3.1.7: Missing designated initializers**
   - Use `.member = value` syntax for struct initialization to improve clarity and safety.
   - Especially important for configuration objects like `TrackSpec`, `Device`, and test cases.

8. **Rule 2.5.3: Header member ordering**
   - Enforce `using` declarations -> non-static methods -> static functions -> data members.
   - Nested types/structs should appear at the top of their access section.

9. **Rule 3.3.1: RAII and mandatory collaborators**
   - Prefer references over nullable raw pointers for mandatory services.
   - Use `ao::utility::makeUniquePtr` for C resource RAII in implementations.

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

The project is migrating to **Uniform Initialization**. This is one of the most pervasive violations in legacy files.

- **Prefer Braces for Construction**: Replace `T x(a, b);` with `auto x = T{a, b};` or `T x{a, b};`.
- **Member Initializer Lists**: Modernize `Constructor() : member(val)` to `Constructor() : member{val}`.
- **Empty Initialization**: Use `auto s = std::string{};` instead of `std::string s;`.
- **The Vector Pitfall**:
    - `std::vector<int> v(10);` creates 10 elements (correct for size allocation).
    - `std::vector<int> v{10};` creates 1 element with value 10.
    - **Rule**: Keep `()` for size-based allocation; use `{}` for value lists or default construction.
- **Avoiding Most Vexing Parse**: Braces prevent the compiler from interpreting a variable declaration as a function declaration.

## Technical Lessons Learned (Summary)

- **C API User Data**: User data pointers (`void*`) in C callbacks MUST be passed from non-const local variables.
- **Move-from-Const**: Never mark `std::unique_ptr` as `const` if it is destined for `std::move`.
- **Descriptive Lambdas**: No one-letter variables in lambdas (e.g., `item` instead of `i`).
- **Include Hygiene**: `#include <ranges>` is mandatory for `std::views` and `std::ranges::to`.

## References

- **Full Standards**: [CONTRIBUTING.md](../../../CONTRIBUTING.md)
- **Build Entry Point**: [build.sh](../../../build.sh)
- **CMake clang-tidy setup**: [CMakeLists.txt](../../../CMakeLists.txt)
