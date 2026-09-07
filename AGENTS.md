# Aobus Agent Guide

Aobus is a C++26 music application with GTK4, TUI, and CLI frontends over a
shared core. Work from the repository root through `./ao`; on Windows use
`ao.bat`. The portal owns CMake, tools, dependencies, and build-tree selection.
`./ao help` and `./ao <command> --help` describe the available native commands.

## Project constraints

- Use English for code comments, commit messages, and documentation.
- Put throwaway artifacts in `/tmp`, outside the repository.
- Preserve build trees when diagnosing failures; build/check output is in the
  selected build tree's `build.log`.
- Keep abstractions tied to current consumers and an independent contract.
  Keep one-consumer helpers local; do not add speculative registries or policies.

## Read by task

These are authority pointers, not a prerequisite reading list. Read the
sections relevant to the change; reuse material already read in this session.

| Task | Authority |
|---|---|
| Contributor or Git workflow | `CONTRIBUTING.md`; `doc/development/commit-message.md` before committing |
| C++ implementation | `doc/development/coding-style.md`; `doc/development/naming-convention.md` |
| Documentation ownership, templates, or migration | `doc/README.md` |
| Test design and fixtures | `doc/development/test.md` and the linked task-specific reference |
| Lint findings or suppressions | `doc/development/linting.md` |
| Concurrency contracts | `doc/development/test/concurrency-and-sanitizer.md` |
| Native macOS work | `doc/development/macos.md` before using the native environment |
| Native Windows work | `doc/development/windows.md` before using the native environment |

Linux dependencies come from the pinned `nix-shell`; use it for manual compiler
or dependency inspection. Native macOS and Windows use the shared vcpkg
manifest and their documented bootstrap paths.

## Completion and authorization

Use `doc/development/test/validation-and-review.md` to select validation for the
changed behavior. It owns check/hygiene scope, lint boundaries, and result reuse.
Run local checks, fix failures caused by the authorized change, and rerun affected
checks without asking for approval at each step. Continue through the requested
implementation and validation; report any remaining failure or unvalidated boundary.

Git history and PR mutations use the `manage-git-flow` skill. Existing user
authorization persists; publishing a branch does not authorize merging its PR.
