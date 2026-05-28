---
name: use-clang-tidy
description: Runs Aobus clang-tidy checks through ./script/run-clang-tidy.sh and fixes reported C++ warnings. Use when linting C++ changes, resolving clang-tidy findings, or checking Aobus C++ style/conformance.
---

# Use clang-tidy

Use `./script/run-clang-tidy.sh` as the single entry point for clang-tidy in Aobus. Do not call `clang-tidy` directly or invent alternate check lists; the script owns file discovery, strict/relaxed modes, the custom plugin, include paths, and diagnostic de-duplication.

## Required Companion Skills

- Load `generate-cpp-code` before editing any `.cpp`, `.h`, or `.hpp` file.

## Default Workflow

Choose the smallest useful scope, then widen only when needed.

1. **Run the script for the relevant scope.** Use default for local changes, explicit files for focused work, `--folder` for a subtree, `--commit <ref>` for a base comparison, and `--all` only for whole-repo checks.
   ```bash
   ./script/run-clang-tidy.sh
   ./script/run-clang-tidy.sh lib/audio/Foo.cpp include/aobus/Foo.h
   ./script/run-clang-tidy.sh --folder app/linux-gtk
   ./script/run-clang-tidy.sh --commit HEAD~3 -o /tmp/aobus-clang-tidy.log
   ```

2. **Fix the warnings.** Prefer real code improvements: direct includes, clearer ownership, named constants, simpler control flow, safer APIs. Use `NOLINT` only for justified tool/API boundaries.

3. **Re-run narrowly after edits.**
   Recheck only modified files first. If the initial scope was a folder, commit range, or `--all`, re-run that broader scope only after the narrow checks are clean or when the fix could affect other files.
   ```bash
   ./script/run-clang-tidy.sh app/linux-gtk/modified_file.cpp
   ```

4. **Build/test when code changed.** Run the narrowest meaningful build or test; use the standard debug validation when in doubt.
   ```bash
   ./build.sh debug
   ```

## Script Usage

```bash
# Changed files (default — local main..HEAD + working tree + staged + untracked)
./script/run-clang-tidy.sh

# Specific files or folders
./script/run-clang-tidy.sh app/foo.cpp app/bar.h
./script/run-clang-tidy.sh --folder app/linux-gtk/app

# Changes since a specific base commit plus working tree and untracked files
./script/run-clang-tidy.sh --commit HEAD~3

# Full project
./script/run-clang-tidy.sh --all

# Full output to file
./script/run-clang-tidy.sh -o /tmp/tidy.log

# Automatic fixes, only when the diagnostics are straightforward and reviewable
./script/run-clang-tidy.sh --fix lib/audio/Foo.cpp

# Use an alternate build directory or job count
./script/run-clang-tidy.sh -p /tmp/build/debug-clang-tidy -j 8 --folder lib
```

Scope behavior from the script:

- `STRICT` checks apply to production code under `lib/`, `app/`, and `include/`.
- `RELAXED` checks apply to `test/` and suppress test-noisy checks such as magic numbers, cognitive complexity, identifier length, C arrays, varargs, `reinterpret_cast`, and optional access.
- The script prepares `/tmp/build/debug-clang-tidy`, builds `AobusLintPlugin` if needed, loads the plugin, and de-duplicates repeated diagnostics.

## Warning Policy

### Fix

- Treat `bugprone-*`, ownership, lifetime, optional access, and special-member warnings as real problems unless proven otherwise.
- For `misc-include-cleaner`, add the direct header that provides the symbol. Use `references/type-to-header-map.md` when the provider is not obvious.
- For readability warnings, prefer small local improvements: named `constexpr` values, clearer expressions, early returns, or a small helper when it makes the code easier to read.
- For RAII guards, explicitly delete copy/move or define the needed operations.

### NOLINT Cleanup Playbook

When reducing existing suppressions, prefer the smallest semantic-preserving edit and re-run clang-tidy on the touched files before continuing.

- Skip `misc-include-cleaner` suppressions when the task explicitly excludes `-include`/include-cleaner work; do not mix include cleanup with behavioral lint cleanup.
- Remove stale suppressions first: if the line no longer triggers after deleting `NOLINT`, just delete the suppression and run clang-tidy.
- Replace bare `NOLINT` with code that avoids the warning when the fix is local. Good examples:
  - Use `.at()`/`.back()` for fixed-size array access instead of suppressing constant-index or magic-number warnings.
  - Use named `constexpr` values for protocol versions, byte counts, enum sequence numbers, and switch sectors.
  - For binary-layout `static_assert(sizeof(T) == N)`, put `static constexpr std::size_t kByteCount = N;` on the layout type and assert against that name.
  - For unused overload parameters, use comment names such as `Type& /*value*/` instead of `NOLINT(readability-named-parameter)`.
  - Prefer `std::from_chars` over `strtoul` when strict full-string unsigned parsing is wanted; it avoids C `char**` output-parameter const-correctness suppressions and preserves no-leading-space behavior.
  - For SPA/PipeWire test structs, prefer `std::to_array<::spa_dict_item>` plus a tiny local `makeDict(std::span<...>)`, or a local designated initializer helper for `spa_pod_builder`, instead of raw C arrays and macro initializers.
  - At C API pointer boundaries in tests, use existing `utility::layout::asLegacyPtr<T>(ptr)` rather than suppressing array-to-pointer decay.
- If a suppression is repeated because of the same framework shape, consider a small local helper only when it stays simple and removes several suppressions. Do not add a cross-cutting helper when protected constructors, ownership contracts, or friend access would create churn.
- Iterator trait aliases (`value_type`, `difference_type`, `reference`, `pointer`, `iterator_category`) are often required by STL concepts. Prefer a lint-rule allowlist for these names over touching every iterator or keeping bare `NOLINT` long term.
- GTKmm/glibmm ownership boundaries (`Glib::make_refptr_for_instance(new T)`, `set_data(..., destroy)`) are usually acceptable suppressions. Do not hide them behind helpers unless the local class design already supports it cleanly.
- Binary/protocol magic numbers and layout sizes can be cleaned with named constants, but if doing so makes the code less readable than a documented format literal, propose disabling or narrowing that readability rule instead.

### Suppress

Use `NOLINT` only when the warning is caused by an external API shape, a clear false positive, or a test-only pattern where the fix would be worse than the warning.

- Use `NOLINTNEXTLINE(check-name)` or inline `NOLINT(check-name)` at the exact expression.
- Include the specific check name; avoid bare `NOLINT`.
- Add a short English comment only when the reason is not obvious.
- `NOLINTBEGIN/END` is only for a compact, contiguous region that cannot be made clearer.

Common acceptable cases: GTKmm `make_refptr_for_instance(new T)`, GLib/GTK macros, C varargs/C arrays at an API boundary, unavoidable `reinterpret_cast` in tests, and clang-tidy false positives around framework code.

### Avoid

- Do not disable checks directory-wide or file-wide.
- Do not add global constants for one-use magic numbers.
- Do not add umbrella includes to appease include-cleaner.
- Do not wrap C APIs in one-off abstractions that only hide a warning.
- Do not split clear local logic into many single-use functions just to reduce a metric.

- **Include-cleaner Triage**: For paired-header types, add direct includes to the `.cpp` rather than relying on the header's transitive includes. Suppress only when the tool genuinely cannot resolve the header, such as some GLib C macros.
- **Finding Missing Headers**: If `misc-include-cleaner` flags a symbol but you don't know the header:
  - For standard libs, use `pkg-config --cflags <name>`.
  - For Clang/LLVM internals, grep `/tmp/build/debug-clang-tidy/compile_commands.json` for `-isystem` paths, or use `llvm-config --cxxflags`.

## Verification

Use `--fix` only for mechanical, reviewable diagnostics in a focused file set. After `--fix`, inspect the changed code before continuing, because generated edits can alter formatting or choose a less idiomatic local pattern. Do not use `--fix --all` unless the user explicitly asked for broad automatic cleanup.

After fixing warnings, run the narrowest clang-tidy check that covers the modified C++ files:

```bash
./script/run-clang-tidy.sh path/to/changed_file.cpp
```

Then build/test at a scope appropriate to the change. For normal C++ edits, the default safe check is:

```bash
./build.sh debug
```

Report the initial scope, warning count or notable diagnostics, what was fixed, any remaining suppressions and why, and verification commands with pass/fail results.

## References

- **`references/type-to-header-map.md`** — exact header for every GTKmm, GLib, STL, and Aobus type. Consult before adding/removing any `#include`.
- **`./script/run-clang-tidy.sh`** — authoritative repository script, contains file discovery, strict/relaxed check configuration, plugin loading, and diagnostic de-duplication.
