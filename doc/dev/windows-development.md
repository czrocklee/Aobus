# Windows development

The native Windows build provides the FTXUI terminal application, the shared
core libraries, native tests, and a dedicated clang-tidy configuration. The GTK
and CLI frontends and the council tool are not enabled in the Windows presets.

## Prerequisites

- Visual Studio Build Tools with the **Desktop development with C++** workload
  and x64 compiler toolset.
- Python 3. The portal prefers the vcpkg-managed Python after dependencies have
  been restored, and otherwise uses `python.exe` from `PATH`.
- Git with long-path support is recommended for vcpkg build trees.

Visual Studio's bundled vcpkg is used by default. If a different vcpkg checkout
is required, set `VCPKG_ROOT` before invoking the portal.

## Portal commands

Run all commands from the repository root:

```bat
ao.bat build                 rem debug TUI build
ao.bat build release         rem release TUI build
ao.bat run tui               rem incrementally build and run the TUI
ao.bat test                  rem core and TUI tests
ao.bat test --all            rem every native Windows suite
ao.bat check                 rem full native Windows gate
ao.bat format --check        rem check changed C++/Python formatting
ao.bat tidy                  rem lint changed files with the native compile database
ao.bat hygiene               rem check formatting, audits, and lint
```

The portal initializes the Visual Studio x64 environment when a build-capable
command is selected. `start-msbuild-env.bat <command> [args...]` remains useful
when another development tool needs to run inside that environment.

Application-only commands use `out/build/windows-tui-debug` or
`out/build/windows-tui-release`. Test commands and `check` use the corresponding
`-tests` tree and CMake preset so they never depend on a prior application-only
configuration.

`tidy` uses the separate `out/build/windows-tidy` tree. On its first configure,
CMake downloads the official `clang+llvm` 22.1.8 Windows development archive,
verifies its pinned SHA-256, and extracts about 3.8 GiB under
`out/toolchains/`. Later configure runs reuse that shared cache, including after
the tidy build tree is cleaned. Concurrent configure runs serialize access to
the cache. CMake considers it complete only when all required SDK files exist
and its completion marker matches both the pinned version and SHA-256; an
incomplete or stale automatic cache is cleared and re-extracted under that
lock.

On an offline machine, extract the exact archive in advance. From an initialized
Visual Studio x64 developer prompt with `VCPKG_ROOT` set, configure the CMake
cache option explicitly (it is not read from an environment variable):

```bat
cmake --preset windows-tidy -DAOBUS_LLVM_SDK_ROOT=C:/toolchains/clang+llvm-22.1.8-x86_64-pc-windows-msvc
```

Use `start-msbuild-env.bat cmd` to open such a prompt from an ordinary terminal.
The configured value persists in `out/build/windows-tidy`. Run the same CMake
command with `-DAOBUS_LLVM_SDK_ROOT=` to return that build tree to the verified
automatic download. A pre-provisioned root is never modified; configuration
fails with the missing path when it is incomplete.

The official Windows `clang-tidy.exe` cannot load external C++ plugins. Aobus
therefore builds `tool/lint/AobusClangTidy.exe`, a self-contained executable
that statically links both the upstream checks and every `aobus-*` check from
the same SDK. The portal validates that the custom checks are registered before
scanning source files and never falls back to an unrelated tool from `PATH`.
`--no-build` requires this executable, the compile database, and the configured
SDK cache to exist already; it performs no download or configure step.

For analysis only, the portal passes `_USE_STD_VECTOR_ALGORITHMS=0` while Clang
parses the Visual Studio 18 standard-library headers. This isolates
[microsoft/STL#6294](https://github.com/microsoft/STL/issues/6294), an STL
vectorized-find bug for three-byte element types. LLVM 22 itself is supported;
normal MSVC builds keep their standard-library vectorization settings.

Clang-format is source based and can run on the same files on either host.
Windows `ao.bat format` resolves `clang-format.exe` from the same pinned LLVM
22.1.8 SDK and never falls back to Visual Studio or `PATH`. If the SDK is not
ready, the first C++ format run configures `windows-tidy` to provision it.
Linux continues to use the Nix-pinned formatter. Clang-tidy is compile-command
based, so complete C++ hygiene is a host matrix:

- Windows checks WASAPI, `AtomicFileWindows`, `SignalExitWatcherWindows`, and
  the shared translation units using the Windows compile database.
- Linux checks PipeWire, ALSA, POSIX, GTK, and the shared translation units
  using the Linux compile database.

For changed-file, folder, and `--all` scopes, the portal reports and defers a
translation unit that the current host does not build. Naming such a file
explicitly is an error instead of silently borrowing unrelated compiler flags.
Run hygiene on both hosts before treating cross-platform C++ coverage as
complete.

The default Windows test group is `core` and `tui`. The native `all` group adds
`integration`. Catch2 executables are resolved with the `.exe` suffix
automatically. The Python tooling gate uses the Ruff and mypy versions supplied
by the Linux Nix environment, so it remains part of the Linux `./ao check`
rather than the native Windows build gate. On Windows, `format`, `tidy`, and
`hygiene` still check Python files in their selected scope when `ruff` and
`mypy` are installed on `PATH`; a C++-only scope does not require them.
