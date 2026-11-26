# Aobus Agent Guide

## Development workflow

- **Iterate:** Use pinned `bazel build`, `bazel test`, or `bazel run` on affected targets (`bazel test //:tests` for the default suite) throughout coding and review rounds.
- **Format:** Once edits are complete, run `./ao format <changed-paths>` (`ao.bat` on Windows) [before completion checks](doc/development/test/validation-and-review.md#formatting-before-validation). Never reformat files another agent is editing in the same worktree.
- **Validate:** When the [validation guide](doc/development/test/validation-and-review.md#validation) requires `./ao check --extend`, run it before opening a ready-for-review PR, marking a draft ready, or delivering a final local commit. Run it earlier on explicit request. Verify lint fixes with `./ao check lint <paths>` on the scope that reproduced the diagnostic. Once review fixes settle, rerun any invalidated full check before sign-off.

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
