---
id: development.windows
type: development
status: current
domain: development
summary: Defines the native Windows setup, build, test, and local-state workflow.
---
# Windows development

The native Windows build provides the CLI and FTXUI terminal applications, the
shared core libraries, native tests, and a dedicated clang-tidy configuration.
The GTK frontend and council tool are not available in the Windows development
profile.

The source checkout may live on a local disk, a mapped drive such as `Y:`, or a
network-backed workspace. Build trees and managed development tools are kept on
the local Windows disk by default. This avoids putting vcpkg, Ninja/MSVC output,
Python virtual environments, or the multi-gigabyte LLVM SDK on a mapped
filesystem.

## Prerequisites

- Visual Studio Build Tools with the **Desktop development with C++** workload,
  x64 compiler toolset, and **C++ AddressSanitizer** component. The Visual
  Studio Clang component is not required.
- Git with long-path support, recommended for vcpkg build trees.
- Network access for the first managed Python and dependency bootstrap. A valid
  local state directory is reused on later runs.

Visual Studio's bundled vcpkg is used by default. If a different vcpkg checkout
is required, set `VCPKG_ROOT` before invoking the portal.

A separate Python installation is not required for the normal portal path. On
first use, `ao.bat` uses the Visual Studio vcpkg installation to obtain NuGet,
installs the official CPython distribution pinned in
`script/ao/toolchain.json` under the local Aobus state directory, and creates a
checkout-specific virtual environment. The pinned Ruff and mypy releases and
their locked dependencies are installed in that environment. The base Python is
shared across checkouts; the virtual environment is not. Neither the Windows
Store `python.exe` alias nor an unrelated Python from `PATH` is used.

The exact Python, Ruff, and mypy policy is shared with Linux in
`script/ao/toolchain.json`. `script/ao/windows-requirements.txt` separately
locks the accepted Windows artifacts and hashes; tooling tests require the two
files to agree.

`AOBUS_PYTHON` is an advanced escape hatch for supplying an explicit Python
executable. It must match the pinned Python patch release and support
`venv` and `ensurepip`; the portal validates it before use. It does not disable
the locked checkout-specific tooling environment.

## Local state and build trees

The default state root is:

```text
%LOCALAPPDATA%\Aobus
```

Its relevant layout is:

```text
%LOCALAPPDATA%\Aobus\
  build\<checkout-key>\<preset>\
  cache\llvm\
    toolchains\
    downloads\
  tools\
    python\<version>\
    venvs\<checkout-key>\<tool-fingerprint>\
```

The managed base interpreter is
`%LOCALAPPDATA%\Aobus\tools\python\<version>\python.exe` with the default state
root, where `<version>` is the pinned release from `script/ao/toolchain.json`.

The checkout key is stable for one checkout and keeps two clones or linked
worktrees from sharing CMake and vcpkg state. LLVM downloads and verified SDKs
are intentionally shared. The tool fingerprint changes when the managed Python
or locked requirements change, allowing a new virtual environment to be built
before the portal switches to it. Build, run, test, and check commands share the
`windows-debug` or `windows-release` flavor directory. MSVC AddressSanitizer
uses the separate `windows-debug-asan` directory. Tests and their vcpkg
dependencies are part of the normal development graph; selecting a CMake target
limits what is compiled without changing presets. `tidy` uses the separate
`windows-tidy` directory.

The portal normally stores an opaque ID in the checkout's private Git directory.
For a read-only checkout or a linked worktree whose Git directory is not visible
from Windows, set a stable, unique `AOBUS_CHECKOUT_ID` for that checkout before
running `ao.bat`.

The following overrides have distinct scopes:

| Setting | Meaning |
|---|---|
| `AOBUS_STATE_ROOT` | Replaces `%LOCALAPPDATA%\Aobus` for managed Windows state, including default builds, tools, and caches. |
| `AOBUS_BUILD_ROOT` | Replaces only the `build` base; checkout and preset directories are still appended. |
| `BUILD_DIR` | Selects one exact build tree. This is useful for a one-off command but should not be reused across incompatible presets. |
| `AOBUS_LLVM_SDK_CACHE_ROOT` | Relocates the automatically managed LLVM cache containing `toolchains`, `downloads`, and its lock. It is available as both an environment setting and a CMake cache option. |
| `AOBUS_LLVM_SDK_ROOT` | CMake cache option naming one complete, pre-extracted LLVM SDK. It is validated and never modified; it is not the automatic cache root. |

An explicit command-line `-p <dir>` selects that command's exact build tree.
Otherwise `BUILD_DIR`, `AOBUS_BUILD_ROOT`, and `AOBUS_STATE_ROOT` are applied in
that order before the `%LOCALAPPDATA%` default.

Those build overrides are portal settings. A direct `cmake --preset` invocation
uses a local, name-based fallback under `%LOCALAPPDATA%`; it does not have the
portal's checkout key. Use `ao.bat`, or pass an explicit local `-B` directory as
shown in the offline example, when more than one checkout is present.

Keep all override paths for builds, tools, and caches on a local Windows disk.
Windows can report some mapped drives as fixed disks, so the portal cannot
reliably reject every remote override. Drive-letter mappings are also scoped to
the Windows login session; make sure the source drive is visible in the shell
or SSH session that invokes `ao.bat`.

## Portal commands

Run all commands from the repository root, including when that root is mapped:

```bat
ao.bat build                 rem debug build of all enabled targets
ao.bat build release         rem release build of all enabled targets
ao.bat build --target aobus-tui  rem build only the TUI target
ao.bat run cli               rem incrementally build and run the CLI
ao.bat run tui               rem incrementally build and run the TUI
ao.bat test                  rem core and TUI tests
ao.bat test --all            rem all Windows suites, including tooling
ao.bat check                 rem full Windows gate
ao.bat check --asan          rem full Windows gate with MSVC AddressSanitizer
ao.bat deps report           rem show governed versions and vcpkg identities
ao.bat deps verify           rem reject stale or mismatched dependency evidence
ao.bat format --check        rem check changed C++/Python formatting
ao.bat tidy                  rem lint changed files with the native compile database
ao.bat hygiene               rem check formatting, audits, and lint
```

The portal initializes the Visual Studio x64 environment when a build-capable
command is selected; each command declares that need in its module under
`script/ao/command/`, and Visual Studio discovery is shared with
`start-msbuild-env.bat` through `script/ao/windows-vsenv.bat`.
`start-msbuild-env.bat <command> [args...]` remains useful when another
development tool needs to run inside that environment; it honors a preset
`VCPKG_ROOT` and otherwise defaults to the Visual Studio bundled vcpkg.

The repository's `vcpkg-configuration.json` selects both the default registry
snapshot and the Boost-scoped snapshot. `dependency-contract.json` owns the
accepted upstream versions. Do not edit one resolver input in isolation; use
the procedure in `doc/development/dependency-upgrade.md` and validate a new local build
tree so an old `vcpkg_installed` directory cannot mask the selection change.

The default Windows test group is `core` and `tui`. The Windows `all` group and
`ao.bat check` add CLI, integration, and the Python `tooling` suite. Catch2
executables are resolved with the `.exe` suffix automatically. The managed
checkout environment supplies the pinned Ruff and mypy tools used by formatting,
tidy, hygiene, and tooling tests; these commands do not depend on ambient
`PATH` tools.

`ao.bat check --asan` builds and runs the same native suite group under MSVC
AddressSanitizer. It instruments Aobus translation units; dependencies from the
normal vcpkg triplet remain uninstrumented, so MSVC STL container annotations
are disabled across that binary boundary. MSVC provides neither
UndefinedBehaviorSanitizer nor ThreadSanitizer, and resumable coroutine bodies
are not fully instrumented; the Linux ASan/UBSan and TSan gates remain
complementary coverage. Windows `--tsan` and application-build `--clang`
selections fail before configuration instead of depending on an ambient Visual
Studio component. The independently managed LLVM SDK described below remains
available for format and tidy.

## LLVM SDK and native lint tools

On the first C++ format or tidy configure, CMake downloads the official
`clang+llvm` 22.1.8 Windows development archive (version and SHA-256 pinned in
`cmake/LlvmSdk.cmake`), verifies the hash, and extracts about 3.8 GiB below
`%LOCALAPPDATA%\Aobus\cache\llvm\toolchains` by default. Downloads live in the
sibling `downloads` directory. Later configure runs and other checkouts reuse
the shared cache, including after a tidy build tree is cleaned. Concurrent
configure runs serialize access to it.

CMake considers an automatically managed SDK complete only when all required
files exist and its completion marker matches both the pinned version and
SHA-256. An incomplete or stale SDK at the selected cache location is not
reused. Changing the cache location does not move or delete a repository's old
`out` directory.

The `AOBUS_LLVM_SDK_CACHE_ROOT` environment setting initializes a new CMake
tree. To redirect an existing tree, reconfigure it with
`-DAOBUS_LLVM_SDK_CACHE_ROOT=<local-path>` or create a fresh build tree.

On an offline machine, extract the exact archive in advance. From an initialized
Visual Studio x64 developer prompt with `VCPKG_ROOT` set, configure a local tidy
tree explicitly:

```bat
cmake -S . --preset windows-tidy -B C:\local\aobus-build\windows-tidy ^
  -DAOBUS_LLVM_SDK_ROOT=C:/toolchains/clang+llvm-<version>-x86_64-pc-windows-msvc
```

Use `start-msbuild-env.bat cmd` to open such a prompt from an ordinary terminal.
The configured value persists in that build tree. Configure it again with
`-DAOBUS_LLVM_SDK_ROOT=` to return the tree to the verified automatic cache. A
pre-provisioned root is never modified; configuration fails with the missing
path when it is incomplete.

The official Windows `clang-tidy.exe` cannot load external C++ plugins. Aobus
therefore builds `tool/lint/AobusClangTidy.exe`, a self-contained executable
that statically links both the upstream checks and every `aobus-*` check from
the same SDK. The portal validates that the custom checks are registered before
scanning source files and never falls back to an unrelated tool from `PATH`.
`--no-build` requires this executable, the compile database, and the configured
SDK to exist already; it performs no download or configure step.

For analysis only, the portal passes `_USE_STD_VECTOR_ALGORITHMS=0` while Clang
parses the Visual Studio 18 standard-library headers. This isolates
[microsoft/STL#6294](https://github.com/microsoft/STL/issues/6294), an STL
vectorized-find bug for three-byte element types. LLVM 22 itself is supported;
normal MSVC builds keep their standard-library vectorization settings.

Clang-format is source based and can run on the same files on either host.
Windows resolves `clang-format.exe` from the same pinned LLVM SDK and never
falls back to Visual Studio or `PATH`. Clang-tidy is compile-command based, so
complete C++ hygiene is a host matrix:

- Windows checks WASAPI, `AtomicFileWindows`, `SignalExitWatcherWindows`, and
  shared translation units using the Windows compile database.
- Linux checks PipeWire, ALSA, POSIX, GTK, and shared translation units using
  the Linux compile database.

For changed-file, folder, and `--all` scopes, the portal reports and defers a
translation unit that the current host does not build. Naming such a file
explicitly is an error instead of silently borrowing unrelated compiler flags.
Run hygiene on both hosts before treating cross-platform C++ coverage as
complete.

## Migrating an existing `out` directory

The portal does not move or remove an existing repository-local `out`
directory. Start a normal portal command to create fresh local build and Python
state. Do not copy any of these directories into the new state root:

- `out/build` or any `CMakeCache.txt`;
- a build tree's `vcpkg_installed` directory;
- an old Python virtual environment.

They contain absolute paths or checkout-specific state and must be configured
again. To avoid downloading and extracting LLVM again, copy only a complete,
previously verified SDK directory, including its
`.aobus-llvm-sdk-complete` marker, into the new automatic cache. For example:

```bat
set "OLD_SDK=%CD%\out\toolchains\llvm-<version>-x86_64-windows-msvc"
set "NEW_SDK=%LOCALAPPDATA%\Aobus\cache\llvm\toolchains\llvm-<version>-x86_64-windows-msvc"
robocopy "%OLD_SDK%" "%NEW_SDK%" /E /COPY:DAT /DCOPY:DAT /R:2 /W:1
ao.bat tidy
```

`robocopy` return codes from 0 through 7 indicate success or copied differences.
If the old directory is complete but has no automatic-cache marker, use it as
`AOBUS_LLVM_SDK_ROOT` instead of placing it in the automatic cache. Verify a
build or hygiene run from the new local state before deciding whether to remove
the old `out` directory manually.
