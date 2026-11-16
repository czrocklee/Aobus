# Aobus Agent Guide

Work from the repository root. Use pinned native `bazel build`, `bazel test`,
and `bazel run`; `bazel test //:tests` is the default test selection.
Use `./ao` (`ao.bat` on Windows) for project tooling; `./ao check` is the full
validation gate. Linux tools come from the pinned `nix-shell`.
Follow [native setup](doc/development/bazel.md#direct-bazel-without-the-portal)
for host prerequisites.

## Repository rules

- Use English for code comments, commit messages, and documentation.
- Put throwaway artifacts outside the checkout, in the platform temporary directory.
- Preserve build trees when diagnosing failures.
- Never silently drop review feedback, including non-blocking items: apply valid in-scope improvements or state why not.

## Guidance and authorization

- [Development guide](doc/development/README.md): task-specific contributor policy.
- [System guide](doc/system/README.md): product contracts and subsystem code.
- [Validation and review](doc/development/test/validation-and-review.md): select
  checks by changed behavior, including formatting order and result reuse.

Run required checks and fix failures within the authorized scope without
per-step approval; report remaining failures or unvalidated boundaries.
Before Git or PR mutations, load [manage-git-flow](.agents/skills/manage-git-flow/SKILL.md).
