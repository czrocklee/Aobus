# Aobus clang-tidy checks

This directory owns the `aobus-*` checks, their AST helpers, and registration.
Use `./ao tidy` or `ao.bat tidy`; the portal selects checks, discovers tools, prepares compile commands, and handles diagnostics.

## Change a checker

- [`check/`](check/) contains implementations and local AST helpers.
- [`AobusLintModule.cpp`](AobusLintModule.cpp) owns command aliases and registration; [`CMakeLists.txt`](CMakeLists.txt) owns source membership and native linkage.
- [Checker development](../../doc/development/lint/checker-development.md) explains symbol identity, macro safety, diagnostics, FixIts, and fixture design.
- [Linting](../../doc/development/linting.md) is the starting point for investigating a finding rather than modifying a checker.

Fixtures live under `test/integration/lint/fixture/`.
`./ao test --lint` runs diagnostic, FixIt, and fixed-output compilation assertions on Linux and macOS.
A focused `tidy --check` invocation inspects diagnostics; it does not replace those assertions.

## Native tool boundary

Linux and macOS build a module loaded by the matching native `clang-tidy` executable.
Windows builds `AobusClangTidy.exe`, linking the same checks with `clangTidyMain` from the pinned official development SDK rather than loading an external C++ DLL into the official executable.

The [Windows SDK guide](../../doc/development/windows.md#llvm-sdk-and-native-lint-tools) owns automatic cache validation and locking, offline provisioning, and the distinction between `AOBUS_LLVM_SDK_CACHE_ROOT` and `AOBUS_LLVM_SDK_ROOT`.
The version, archive, hash, and required-file checks are defined in [`LlvmSdk.cmake`](../../cmake/LlvmSdk.cmake).
Use the [macOS guide](../../doc/development/macos.md) for its native LLVM environment.
