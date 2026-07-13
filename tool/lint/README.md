# Aobus clang-tidy checks

This directory owns the `aobus-*` clang-tidy checks and their registration
module. Use `./ao tidy` or `ao.bat tidy`; the portal owns check selection,
compile databases, tool discovery, and diagnostic handling.

Linux builds `libAobusLintPlugin.so` against the Nix-provided LLVM development
packages and loads it into the matching `clang-tidy` process.

Official Windows LLVM binaries do not export the symbols needed by an
out-of-tree DLL plugin. The `windows-tidy` preset therefore downloads the pinned
official development archive and builds `AobusClangTidy.exe`, linking the same
check sources directly with `clangTidyMain`. This keeps upstream and Aobus check
registries in one process without relying on an unstable cross-package C++ ABI.

CMake serializes access to the shared SDK below the local
`AOBUS_LLVM_SDK_CACHE_ROOT` and reuses it only when its required files and
version-plus-SHA completion marker validate. The default cache is
`%LOCALAPPDATA%\Aobus\cache\llvm`, independent of the source checkout and shared
by its Windows build trees. See
[Windows development](../../doc/development/windows.md) for the full state
layout, mapped-source rules, overrides, and migration guidance.

For an offline configure, extract the exact pinned archive first and run the
following from an initialized Visual Studio x64 developer prompt with
`VCPKG_ROOT` set (`start-msbuild-env.bat cmd` opens one):

```bat
cmake -S . --preset windows-tidy -B C:\local\aobus-build\windows-tidy ^
  -DAOBUS_LLVM_SDK_ROOT=C:/path/to/clang+llvm-22.1.8-x86_64-pc-windows-msvc
```

`AOBUS_LLVM_SDK_CACHE_ROOT` relocates the automatically managed cache.
`AOBUS_LLVM_SDK_ROOT` instead names one complete pre-provisioned SDK; it is a
CMake cache option, is validated, and is never modified.

Checker behavior is covered by fixtures under
`test/integration/lint/fixture/`; run them with `./ao test --lint` on Linux.
