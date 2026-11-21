---
id: decision.0022.single-native-build-backend
status: accepted
---

# Decision 0022: use Bazel as the single native build backend

## Decision

Use one first-party Bazel graph for native products, resources, and tests. [PR #151](https://github.com/czrocklee/Aobus/pull/151) made Bazel the sole backend, with fastbuild as the ordinary development and CI mode. Nix on Linux and vcpkg on macOS and Windows remain responsible for dependencies; choosing a build graph does not require replacing those resolvers.

## Why Bazel

The deciding question was how it feels to change one component, rebuild it, and run its tests throughout the day. The maintainers preferred Bazel's incremental development loop to CMake's. They also found Starlark's syntax clearer and its programmable rules a better fit for expressing and customizing project-specific build behavior. This was a judgment about working on Aobus, not a claim that every Bazel build is faster.

## Alternatives considered

Keeping CMake alone would avoid a migration, but would keep the incremental workflow and customization language the maintainers wanted to change. Keeping both backends would preserve that option at the cost of declaring and checking product inputs, platform branches, and test ownership twice. The project chose to migrate those declarations once and maintain Bazel rules rather than keep the graphs in agreement.

## Test ownership

Bazel's configured targets declare executable owners for sources that register test cases. The project audits that ownership against the completion graph rather than maintaining a separate CMake configured manifest. Graph membership alone is not evidence that a test ran. This replaces the mechanism in [Decision 0021](0021-own-test-executables-by-runtime-needs.md#supersession), not its criteria for grouping cases by runtime and link needs.

## Current authorities

- [Native build graph and profiles](../development/bazel.md)
- [Test ownership](../development/bazel.md#test-ownership)
- [Test suites](../development/test/test-suite.md)
