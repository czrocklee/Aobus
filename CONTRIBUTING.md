# Aobus Contributor Guide

This file is the human entry point for contribution rules. Detailed, numbered
rules live in focused docs so review references stay stable without making this
page hard to scan.

## Reference Documents

| Document | Scope |
|---|---|
| `doc/README.md` | Documentation tree organization and update expectations |
| `doc/dev/coding-style.md` | C++ standard, formatting, naming, includes, language idioms, class design, const/threading rules |
| `doc/dev/commit-messages.md` | Commit message format, scopes, subject/body guidance, and examples |
| `doc/dev/linting.md` | Lint policy: warning triage, suppression rules, cleanup playbook, Python hygiene, automatic-fix guidance |
| `doc/design/error-model.md` | Error contracts by layer and subsystem |
| `doc/dev/testing.md` | Testing policy and detailed test-writing references |

## Coding Style Highlights

- Target `C++26` without modules.
- Use `clang-format`; do not hand-format against the formatter.
- Use `PascalCase` for types and classes, `camelCase` for functions and
  variables, `_camelCase` for non-static class data members, and `kCamelCase` for
  constants.
- Prefer modern C++ library facilities when they clarify intent; use ordinary
  loops when ranges obscure control flow, side effects, allocation, or debugging.
- Prefer `ao::Result<T>` for recoverable failures, exceptions for programmer
  errors or impossible states, and `std::optional<T>` for legitimate absence.
- See `doc/dev/coding-style.md` for the numbered rules used in reviews.

## Commit Message Highlights

- Use Conventional Commits: `type(scope): imperative summary`.
- Prefer the narrowest useful scope, such as `docs`, `gtk`, `runtime`, `test`,
  `ao`, or the subsystem being changed.
- Keep the subject focused on the primary technical contribution.
- Use the body only when the motivation, tradeoff, or validation is not obvious
  from the diff.
- Do not mention AI tools, internal plans, or co-author signatures.
- See `doc/dev/commit-messages.md` for examples and review rules.

## Testing Highlights

- Tests are behavior contracts, not coverage probes.
- Prove observable behavior at the lowest layer that can express the contract:
  `lib` -> `runtime` -> `uimodel` -> `linux-gtk`.
- Name tests as `"Component - behavior under condition"` and tag them as
  `[layer][type][subsystem]`.
- Assert observable outcomes and postconditions, not just `called == true` or
  `has_value()`.
- See `doc/dev/testing.md` for testing policy, GTK guidance, coverage workflow,
  and suite organization.
