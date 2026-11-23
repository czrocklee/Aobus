# Aobus Agent Guide

## Development workflow

- **Iterate:** Use pinned `bazel build`, `bazel test`, or `bazel run` on affected targets. For the default test suite, run `bazel test //:tests`.
- **Format:** Do not reformat a file another agent is editing in the same worktree: its pending patch may no longer match.
- **Validate:** `./ao check` (`ao.bat check` on Windows) includes whole-graph lint. Run it on the final change when the [validation guide](doc/development/test/validation-and-review.md#validation) requires it, not after each edit.

## Repository rules

- Use English for code comments, commit messages, and documentation.
- Put throwaway artifacts outside the checkout, in the platform temporary directory.
- Preserve build trees when diagnosing failures.
- Never silently drop review feedback, including non-blocking items: apply valid in-scope improvements or state why not.

## Guidance and authorization

- [Native setup](doc/development/bazel.md#direct-bazel-without-the-portal): platform prerequisites and Linux private test fixtures.
- [Development guide](doc/development/README.md): task-specific contributor policy.
- [System guide](doc/system/README.md): product contracts and subsystem code.

Run required checks and fix failures within the authorized scope without
per-step approval; report remaining failures or unvalidated boundaries.
Before Git or PR mutations, load [manage-git-flow](.agents/skills/manage-git-flow/SKILL.md).
