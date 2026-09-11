# Aobus Contributor Guide

This file is the human entry point for contribution rules. Detailed, numbered
rules live in focused docs so review references stay stable without making this
page hard to scan.

## Reference Documents

| Document | Scope |
|---|---|
| `doc/README.md` | Documentation tree organization and update expectations |
| `doc/development/coding-style.md` | C++ standard, formatting, includes, language idioms, class design, const/threading rules |
| `doc/development/naming-convention.md` | Identifier, type/contract, vocabulary, file, helper, and support naming |
| `doc/development/commit-message.md` | Commit message format, scopes, subject/body guidance, and examples |
| `doc/development/linting.md` | Lint policy: warning triage, suppression rules, cleanup playbook, Python hygiene, automatic-fix guidance |
| `doc/development/compiler-cache.md` | Compiler-cache setup, shared host state, capacity, overrides, and CI integration |
| `doc/development/macos.md` | Native macOS prerequisites, portal bootstrap, local state, supported commands, and current limitations |
| `doc/development/windows.md` | Native Windows portal, local state and tool bootstrap, mapped-source workflow, and migration guidance |
| `doc/architecture/failure-and-reporting.md` | Failure ownership, recovery, reporting, and presentation boundaries |
| `doc/development/test.md` | Testing policy and detailed test-writing references |

## Development Workflow

Run repository operations through `./ao` on Linux or macOS and `ao.bat` on
Windows from the project root. macOS and Windows source checkouts may be
network-backed, but generated state must remain on the native host's local
disk. Read `doc/development/macos.md` or `doc/development/windows.md` before
overriding platform state locations.

Select completion checks by the changed behavior using
[validation and review](doc/development/test/validation-and-review.md).

To enable the shared compiler cache, run `./ao setup compiler-cache` once for each native host user (`ao.bat setup compiler-cache` on Windows).
Builds also work without compiler caching.
Existing `CCACHE_DIR` and `CCACHE_MAXSIZE` exports override the shared-store and capacity defaults, including values inherited from an older shell session.
See [compiler-cache setup and migration](doc/development/compiler-cache.md) before adopting the managed defaults.

Install the repository's commit hooks explicitly with `./ao setup git-hooks`
(`ao.bat setup git-hooks` on Windows).
This assigns `core.hooksPath=script/git-hook` in the repository's local Git
configuration, replacing any previous hook path; linked worktrees share that
local configuration and resolve the relative hook path from their own checkout.
Ordinary portal commands leave hook configuration unchanged.
The Git environment must provide Python 3 to execute the commit-message hook.
Hook installation is optional local setup. Repository rules still apply when
hooks are absent; a local hook is not a server-side enforcement boundary.

## Coding Style Highlights

- Target `C++26` without modules.
- Use `clang-format`; do not hand-format against the formatter.
- Use `PascalCase` for types and classes, `camelCase` for functions and
  variables, `_camelCase` for non-static class data members, and `kCamelCase` for
  constants.
- Use `doc/development/naming-convention.md` for type/contract names, vocabulary,
  pointer/optional naming, file names, and helper/support allocation.
- Prefer modern C++ library facilities when they clarify intent; use ordinary
  loops when ranges obscure control flow, side effects, allocation, or debugging.
- Prefer `ao::Result<T>` for recoverable failures, exceptions for programmer
  errors or impossible states, and `std::optional<T>` for legitimate absence.
- See `doc/development/coding-style.md` and `doc/development/naming-convention.md` for the
  rules used in reviews.

## Commit Message Highlights

- Use Conventional Commits: `type(scope): imperative summary`.
- Prefer the narrowest useful scope, such as `docs`, `gtk`, `runtime`, `test`,
  `ao`, or the subsystem being changed.
- Keep the subject focused on the primary technical contribution.
- Use the body only when the motivation, tradeoff, or validation is not obvious
  from the diff.
- Do not mention AI tools, internal plans, or co-author signatures.
- See `doc/development/commit-message.md` for examples and review rules.

## Testing Highlights

- Tests are behavior contracts, not coverage probes.
- Prove observable behavior at the lowest layer that can express the contract:
  `lib` -> `runtime` -> `uimodel` -> `linux-gtk`.
- Name tests as `"Component - behavior under condition"` and tag them as
  `[layer][type][subsystem]`.
- Assert observable outcomes and postconditions, not just `called == true` or
  `has_value()`.
- See `doc/development/test.md` for testing policy, GTK guidance, coverage workflow,
  and suite organization.
