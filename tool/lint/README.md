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

CMake serializes access to the shared SDK under `out/toolchains/` and reuses it
only when its required files and version-plus-SHA completion marker validate.
An invalid automatic cache is removed and re-extracted from the SHA-verified
archive. For an offline configure, extract the exact pinned archive first and
run the following from an initialized Visual Studio x64 developer prompt with
`VCPKG_ROOT` set (`start-msbuild-env.bat cmd` opens one):

```bat
cmake --preset windows-tidy -DAOBUS_LLVM_SDK_ROOT=C:/path/to/clang+llvm-22.1.8-x86_64-pc-windows-msvc
```

`AOBUS_LLVM_SDK_ROOT` is a CMake cache option, not an environment variable, and
the pre-provisioned directory is validated but never modified.

Checker behavior is covered by fixtures under
`test/integration/lint/fixture/`; run them with `./ao test --lint` on Linux.
