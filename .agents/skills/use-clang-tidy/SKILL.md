---
name: use-clang-tidy
description: Run or review requested Aobus C++ clang-tidy checks, and fix findings when asked. Ordinary completion hygiene does not activate this skill.
---

# Use clang-tidy

A findings review is read-only unless fixes are requested. Use `./ao tidy`
(`ao.bat tidy` on Windows); the portal owns scope, native compile flags,
custom checks, and diagnostic filtering. Do not invoke clang-tidy directly.

Use `doc/development/linting.md` for the affected triage or suppression policy.
Agents make explicit edits; do not run `tidy --fix` or apply exported replacements.
The lint suite's temporary fixture fixes are a separate validation mechanism.

Use explicit paths, `--folder`, or `--commit` for a limited request. `--all` is
for requested whole-repository linting. `--no-build --check <name>` reuses a
prepared compile database and plugin for checker/fixture diagnosis.

Validate the requested scope and report remaining diagnostics or suppressions.
Completion gates and result reuse are owned by
`doc/development/test/validation-and-review.md`.
