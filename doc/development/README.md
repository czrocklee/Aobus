---
id: development.index
type: index
status: current
domain: documentation
summary: Routes contributor setup, standards, testing, tooling, and repository workflow.
---
# Development documentation

This area tells contributors how to work in the Aobus repository.
It owns platform setup, coding and naming standards, testing, validation, linting, dependency governance, application-layer review, and project tooling.

Product behavior does not belong here even when contributors are the primary readers.
Link to architecture, specifications, and reference for the contract being implemented.

Use the [development guide template](../template/development.md) for a new contributor policy or workflow.

## Repository workflow

- [C++ coding style](coding-style.md) defines language, formatting, include, class-design, const, and threading rules.
- [Naming convention](naming-convention.md) owns identifiers, type and contract roles, vocabulary, files, and support-code allocation.
- [Commit message](commit-message.md) defines commit structure, scopes, subjects, bodies, and review expectations.
- [Linting](linting.md) defines warning triage, suppression, cleanup, Python hygiene, and automatic-fix policy.
- [Dependency governance](dependency-governance.md) owns cross-platform dependency policy, reproducibility, and exceptions.
- [Dependency upgrade](dependency-upgrade.md) gives the supported procedure for changing dependency and tool pins.
- [Windows development](windows.md) covers the native Windows portal, local state, tool bootstrap, and mapped-source workflow.
- [Agent council](agent-council.md) defines the advisory multi-model review workflow and its evidence boundary.

## Testing

- [Testing policy](test.md) is the contributor entry point and default test design contract.
- [Layer selection](test/layer-selection.md) chooses the lowest test layer that can prove a behavior.
- [Naming and assertions](test/naming-and-assertion.md) defines Catch2 names, tags, structure, and assertion quality.
- [Fixtures and helpers](test/fixture-and-helper.md) defines fixtures, fakes, test data, testability seams, and filesystem setup.
- [Runtime and asynchronous testing](test/runtime-and-async.md) defines deterministic executors, callbacks, coroutines, and lifetime checks.
- [Concurrency and sanitizer validation](test/concurrency-and-sanitizer.md) defines race matrices and sanitizer gates.
- [UIModel and GTK testing](test/uimodel-and-gtk.md) defines policy-versus-adapter placement, fixtures, lifecycle, and geometry checks.
- [Coverage workflow](test/coverage-workflow.md) defines measurement, analysis, test selection, and verification.
- [Validation and review](test/validation-and-review.md) defines file integration, regression expectations, gates, smells, and review checks.
- [Test suites](test/test-suite.md) enumerates the `./ao test` suites and suite groups.

## Application layers

- [Application-layer review](application-layer-review.md) turns runtime, UIModel, and frontend ownership into a contributor review workflow.
- [UIModel organization](uimodel-organization.md) defines namespaces, feature capsules, role names, dependencies, and mirrored tests.

## Linux GTK

- [GTK style](gtk-style.md) defines theme tokens, structural variants, shared component CSS, motion, and visual-complexity policy.
