# Contribute to Aobus

Work from the repository root through `./ao` on Linux/macOS or `ao.bat` on Windows.
Start with [build and setup](doc/development/build.md); read the [macOS](doc/development/macos.md) or [Windows](doc/development/windows.md) guide before native work.

## Make a change

1. Find the affected topic in the [system guide](doc/system/README.md), or use the task links in [development](doc/development/README.md).
2. Follow [coding style](doc/development/coding-style.md) and [naming](doc/development/naming-convention.md) for code you touch. Do not perform unrelated cleanup.
3. Add tests at the lowest layer that proves the changed behavior; the [testing guide](doc/development/test.md) routes fixtures and task-specific advice.
4. Update the current contract or user task when it changes. See [documentation maintenance](doc/development/documentation.md) when reorganizing knowledge; ordinary changes require neither an RFC nor an ADR.
5. Select completion checks and native hosts from [validation and review](doc/development/test/validation-and-review.md). Include remaining gaps in the review description.

[Design review](doc/development/design-review.md) helps when changing ownership or public boundaries.
The [linting guide](doc/development/linting.md) explains diagnosis and justified suppressions; checker implementation details are not prerequisite reading for ordinary feature work.

## Prepare a review

Use the [commit-message guide](doc/development/commit-message.md) for the project's Conventional Commit format and scope selection.
Explain motivation, consequential tradeoffs, and validation rather than duplicating the diff.
Keep a proposal only when unresolved design choices benefit from written comparison, and retain a separate decision only when its reasons are worth revisiting.

## Optional local setup

`./ao setup compiler-cache` enables the shared compiler cache for the host user; builds also work without it.
Use `--shared-workspaces` only after reading the [cache setup and migration guide](doc/development/compiler-cache.md).
Existing `CCACHE_DIR` and `CCACHE_MAXSIZE` environment overrides take precedence.

`./ao setup git-hooks` explicitly installs the repository's commit hooks (`ao.bat setup git-hooks` on Windows).
It replaces the repository's `core.hooksPath` with `script/git-hook`; linked worktrees share that configuration and resolve the relative path from their own checkout.
The Git environment must provide Python 3. Hook installation is optional local setup, not a server-side enforcement boundary; ordinary portal commands do not change hook configuration.

AI agents also follow [AGENTS.md](AGENTS.md).
