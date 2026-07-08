---
name: use-clang-tidy
description: Runs Aobus clang-tidy checks through ./ao tidy and fixes reported C++ warnings. Use only when the user explicitly asks for linting, clang-tidy, lint cleanup, or resolving clang-tidy findings in the current session.
---

# Use clang-tidy

## Session Opt-In

Do not activate this skill or run `./ao tidy` unless the user explicitly asks for linting,
clang-tidy, lint cleanup, or resolving clang-tidy findings in the current session. Build and
targeted tests are the default validation path for ordinary C++ changes.

Use `./ao tidy` as the single entry point for clang-tidy in Aobus. Do not call `clang-tidy`
directly or invent alternate check lists; the command owns file discovery, strict/relaxed modes,
the custom plugin, include paths, exported fixes, and diagnostic de-duplication. Python files in
scope are checked by Ruff and mypy instead (see the use-python-lint skill); when a scope contains
only Python files, `./ao tidy` does not prepare the clang-tidy plugin.

Do not run `./ao tidy --fix` or otherwise apply exported clang-tidy replacements. Automatic fixes
can create broad, hard-to-review worktree churn; agents must make explicit, reviewable edits by hand
and then verify them with read-only tidy runs.

## Policy

The project lint policy — which warnings to fix, when `NOLINT` is acceptable, the NOLINT cleanup
playbook, include-cleaner triage, and automatic-fix guidance — lives in `doc/dev/linting.md`. Read
it before fixing or suppressing any finding. When editing C++ to fix warnings, follow the
conventions in `doc/dev/coding-style.md` and `doc/dev/naming-conventions.md` (the `aobus-*` checks
enforce them).

## Default Workflow

Choose the smallest useful scope, then widen only when needed.

1. **Run the command for the relevant scope.** Use default for local changes, explicit files for focused work, `--folder` for a subtree, `--commit <ref>` for a base comparison, and `--all` only for whole-repo checks.
   ```bash
   ./ao tidy
   ./ao tidy lib/audio/Foo.cpp include/aobus/Foo.h
   ./ao tidy --folder app/linux-gtk
   ./ao tidy --commit HEAD~3 -o /tmp/aobus-clang-tidy.log
   ```

2. **Fix the warnings** per `doc/dev/linting.md`: prefer real code improvements; use `NOLINT` only for justified tool/API boundaries.

3. **Re-run narrowly after edits.**
   Recheck only modified files first. If the initial scope was a folder, commit range, or `--all`, re-run that broader scope only after the narrow checks are clean or when the fix could affect other files.
   ```bash
   ./ao tidy app/linux-gtk/modified_file.cpp
   ```

4. **Build/test when code changed.** Run the narrowest meaningful build or test. Use `./ao check` only when the user asked for the full gate or the change affects broad tooling behavior; it now runs the lint integration suite as part of `--all`.
   ```bash
   ./ao build --target ao_core_test
   ./ao test --core
   ```

## Command Usage

```bash
# Changed files (default — local main..HEAD + working tree + staged + untracked)
./ao tidy

# Specific files or folders
./ao tidy app/foo.cpp app/bar.h
./ao tidy --folder app/linux-gtk/app

# Changes since a specific base commit plus working tree and untracked files
./ao tidy --commit HEAD~3

# Full project
./ao tidy --all

# Full output to file
./ao tidy -o /tmp/tidy.log

# Use an alternate build directory or job count
./ao tidy -p /tmp/build/debug-clang-tidy -j 8 --folder lib

# Reuse an existing compile database and plugin; useful for lint integration and quick reruns.
./ao tidy --no-build --check aobus-include-convention path/to/file.cpp
```

Scope behavior (STRICT vs RELAXED, fixture handling) is documented in `doc/dev/linting.md`. For C++ files, the command prepares `/tmp/build/debug-clang-tidy`, builds `AobusLintPlugin` if needed, loads the plugin, and de-duplicates repeated diagnostics.

## Verification

After fixing warnings, run the narrowest clang-tidy check that covers the modified C++ files:

```bash
./ao tidy path/to/changed_file.cpp
```

Then build/test at a scope appropriate to the change. Examples:

```bash
./ao build --target ao_core_test
./ao test --core
./ao test --lint
```

Report the initial scope, warning count or notable diagnostics, what was fixed, any remaining suppressions and why, and verification commands with pass/fail results.

## References

- **`doc/dev/linting.md`** — project lint policy: warning fix/suppress/avoid rules, NOLINT cleanup playbook, include-cleaner triage, automatic-fix guidance.
- **`references/type-to-header-map.md`** — exact header for every GTKmm, GLib, STL, and Aobus type. Consult before adding/removing any `#include`.
- **`./ao tidy`** (implemented in `script/ao/command/tidy.py`) — authoritative entry point; owns file discovery, strict/relaxed check configuration, plugin loading, and diagnostic de-duplication.
