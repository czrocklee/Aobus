# Aobus clang-tidy checks

This directory owns the `aobus-*` checks, their AST helpers, and registration.
Use `./ao check lint` or `ao.bat check lint`; the portal selects checks, admits tools, resolves configured compile actions, and handles diagnostics.

## Change a checker

- [`check/`](check/) contains implementations and local AST helpers.
- [`AobusLintModule.cpp`](AobusLintModule.cpp) owns checker aliases and registration; [`BUILD.bazel`](BUILD.bazel) owns source membership and native linkage.
- [Checker development](../../doc/development/lint/checker-development.md) explains symbol identity, macro safety, diagnostics, and fixture design.
- [Linting](../../doc/development/linting.md) is the starting point for investigating a finding rather than modifying a checker.

Fixtures live under `test/lint/fixture/`.
`bazel test //test/lint:lint_test --nocache_test_results` verifies diagnostics on native Linux, macOS, and Windows, including optional message assertions and mandatory compiler-error detection while Clang parses the original fixtures.
A focused `./ao check lint --tidy-check <alias> <owned-source>` invocation inspects compile-owned source diagnostics; it cannot select fixtures or replace those assertions.

## Native tool boundary

Linux and macOS build a module loaded by the matching native `clang-tidy` executable.
Windows builds `AobusClangTidy.exe`, linking the same checks with `clangTidyMain` from the pinned official development SDK rather than loading an external C++ DLL into the official executable.

The [Windows SDK guide](../../doc/development/windows.md#llvm-sdk-and-native-lint-tools) owns automatic cache validation and locking, offline provisioning, and the distinction between `AOBUS_LLVM_SDK_CACHE_ROOT` and `AOBUS_LLVM_SDK_ROOT`.
The pinned version, archive hash, and required files are owned by [`script/ao/windows-llvm-sdk.json`](../../script/ao/windows-llvm-sdk.json); [`windows_llvm_sdk.py`](../../script/ao/policy/windows_llvm_sdk.py) owns explicit provisioning and ready-only validation of that archive.
Use the [macOS guide](../../doc/development/macos.md) for its native LLVM environment.
