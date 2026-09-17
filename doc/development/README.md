---
id: development.index
---
# Develop Aobus

Use the [contributor workflow](../../CONTRIBUTING.md) to begin a change, then choose only the references needed for the task.
Product structure and behavior live in the [system guide](../system/README.md), not in a second copy here.

## Build and setup

- [Build and run](build.md): Linux quick start, portal commands, build-tree selection, locks, and dependency inspection.
- [macOS](macos.md) and [Windows](windows.md): native bootstrap, toolchains, state, platform commands, and limitations. Use `ao.bat` on Windows.
- [Compiler cache](compiler-cache.md): optional setup, shared workspaces, and cache ownership.
- [Optimized builds](optimized-builds.md): Release, IPO/LTO, profiling, and performance investigation.

## Implement a change

| Task | Guidance |
|---|---|
| Write ordinary C++ | [Coding style](coding-style.md) and [naming](naming-convention.md) |
| Change ownership, a boundary, or an abstraction | [Design review](design-review.md) and the relevant [system topic](../system/README.md) |
| Place runtime, UIModel, or frontend behavior | [Application-layer review](application-layer-review.md) and [UIModel organization](uimodel-organization.md) |
| Change GTK signal wiring, binding, or teardown | [GTK lifetime](gtk-lifetime.md) |
| Change GTK appearance | [GTK style](gtk-style.md) |
| Add messages, translations, or a locale | [Localization workflow](localization.md) |
| Change a managed payload | [Managed-state schemas](managed-state-schemas.md) |
| Write or reorganize documentation | [Documentation maintenance](documentation.md) |

## Test and validate

The [testing guide](test.md) routes layer selection, fixtures, assertions, async, concurrency, UIModel/GTK, coverage, and performance tasks.
[Validation and review](test/validation-and-review.md) chooses completion checks and native hosts from the changed behavior; [test suites](test/test-suite.md) explains the available portal suites.
Do not read every testing topic before making an ordinary change.

## Diagnose or maintain tools

- [Linting](linting.md): investigate findings, scope checks, and decide whether a suppression is justified.
- [Naming checker semantics](lint/naming-checks.md): framework exceptions and the bounded automatic proofs behind diagnostics.
- [Checker development](lint/checker-development.md): AST identity, fixture design, registration, and fixes.
- [Dependency governance](dependency-governance.md) and [dependency upgrade](dependency-upgrade.md): locate existing dependencies or change their pins.
- [Concept metrics](concept-metrics.md): understand dependency measurements rather than treating one number as a verdict.
- [macOS portability compromises](macos-portability.md): current deviations and their removal conditions.

## Prepare a review

Use the change-specific validation above and the [commit-message guide](commit-message.md).
[Decisions](../decision/README.md) preserve important historical reasons; a normal change does not require a proposal or a new decision record.
