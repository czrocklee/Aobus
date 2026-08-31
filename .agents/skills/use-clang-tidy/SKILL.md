---
name: use-clang-tidy
description: Runs or reviews Aobus clang-tidy checks through the project portal and, when explicitly requested, fixes reported C++ warnings. Use only when the user asks for linting, clang-tidy, lint cleanup, or clang-tidy findings in the current session.
---

# Use clang-tidy

## Boundary

Do not activate this skill or run tidy unless the user explicitly asks for linting, clang-tidy,
lint cleanup, or clang-tidy findings in the current session. A request to inspect findings is
read-only; edit code only when the user also asks to fix them.

Use `./ao tidy` as the single clang-tidy entry point on Linux and macOS, and `ao.bat tidy` on
Windows. The portal owns file discovery, strict/relaxed modes, the custom plugin, include paths,
exported fixes, and diagnostic de-duplication. Do not call `clang-tidy` directly or invent another
check list.

Never run `tidy --fix` or apply exported replacements. Make requested fixes explicitly and review
their diff.

## Policy

Read `doc/development/linting.md` before fixing or suppressing a finding. It owns warning policy,
the `NOLINT` cleanup playbook, include-cleaner triage, and automatic-fix guidance. C++ edits also
follow `doc/development/coding-style.md` and `doc/development/naming-convention.md`.

## Workflow

1. Choose the smallest scope that answers the request:

   ```bash
   ./ao tidy
   ./ao tidy lib/audio/Foo.cpp include/aobus/Foo.h
   ./ao tidy --folder app/linux-gtk
   ./ao tidy --commit HEAD~3 -o /tmp/aobus-clang-tidy.log
   ./ao tidy --all
   ```

   The default covers changed and untracked files. Use `--all` only for an explicit whole-repo
   check. On Windows substitute `ao.bat` for `./ao`.

2. Triage findings under the lint policy. When fixes were requested, prefer a real code correction;
   use `NOLINT` only at a documented tool or API boundary.
3. Re-run tidy on the changed files first, then repeat any broader requested scope. Use
   `--no-build --check <name>` only when reusing a prepared compile database and plugin for checker
   development or fixture diagnosis.
4. If code changed, run the narrowest useful build/test while iterating, then follow the completion
   policy in `doc/development/test/validation-and-review.md`.

Useful diagnostic overrides are `-p <build-dir>`, `-j <jobs>`, `--debug`, and `-o <log>`; their
authoritative behavior lives in `script/ao/command/tidy.py`.

## Handoff

Report the scope, notable diagnostics, requested fixes, justified remaining suppressions, and the
verification commands with their results.
