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

## Include-cleaner Triage

For paired-header types, add direct includes to the `.cpp` rather than relying on the header's transitive includes. Suppress only when the tool genuinely cannot resolve the header, such as some GLib C macros.

## `--fix` Policy

Use `--fix` only for mechanical, reviewable diagnostics in a focused file set. After `--fix`, inspect the changed code before continuing, because generated edits can alter formatting or choose a less idiomatic local pattern. Do not use `--fix --all` unless the user explicitly asked for broad automatic cleanup.

## Verification

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
