---
id: development.windows
---
# Windows development

The native Windows profile builds the shared core, CLI, FTXUI terminal frontend,
WinUI 3 desktop, native tests, and a dedicated clang-tidy executable. GTK is not
available.

This page owns host setup, portal commands, local state, WinUI build/runtime
requirements, native lint provisioning, and offline LLVM SDK setup. Observable
WinUI behavior belongs to the [Windows frontend contract](../system/frontend/windows.md).

## Prerequisites

Install Visual Studio Build Tools with
`config/windows-build-tools.vsconfig`. It declares the required desktop C++,
Universal Windows, x64 toolset, NuGet, Windows SDK, and WinUI build components.
Install the optional **C++ AddressSanitizer** component before running the ASan
gate. The full IDE and Visual Studio Clang component are not required.

```cmd
vs_buildtools.exe --config config\windows-build-tools.vsconfig --passive
```

Or import `config/windows-build-tools.vsconfig` through the Visual Studio
Installer UI (**More** → **Import configuration**).

Git with long-path support is recommended for vcpkg build trees. Enable it for
the current user:

```cmd
git config --global core.longpaths true
```

Use `--system` instead only for a machine-wide setting from an elevated terminal.

The first portal invocation needs network access to provision managed Python and native
dependencies; valid local state is reused later.

Visual Studio's bundled vcpkg is the default. `VCPKG_ROOT` may select another
complete checkout for diagnosis, but the repository registry and manifest locks
still apply.

Do not install a separate Python for the normal path. `ao.bat` obtains NuGet
through the Visual Studio vcpkg installation, installs the exact CPython release
from `script/ao/toolchain.json`, and creates a checkout-specific environment
from the hashed Windows requirements lock. It does not use the Windows Store
alias or ambient `PATH` Python.

`AOBUS_PYTHON` is an advanced override for an explicit interpreter. It must
match the exact policy version and provide `venv` and `ensurepip`; the portal
still creates and validates the locked checkout environment.

## Use the portal

Run commands from the repository root:

```bat
ao.bat build
ao.bat build release
ao.bat build --target aobus-tui
ao.bat run cli
ao.bat run tui
ao.bat test
ao.bat test --all
ao.bat check
ao.bat check release
ao.bat check --asan
ao.bat format --check
ao.bat tidy
ao.bat hygiene
ao.bat deps report
ao.bat deps verify
```

Use
`ao.bat <command> --help` for current options rather than treating this list as
a complete CLI reference.

Build-capable commands initialize the Visual Studio x64 environment only when
the command needs it. Help, tooling-only tests, and Python-only source scopes
skip native setup. `ao.bat run <app> --no-build` launches an existing executable
without preparing a build environment.

`start-msbuild-env.bat <command> [args...]` is available when another tool needs
the same Visual Studio environment. It preserves an explicit `VCPKG_ROOT` and
otherwise selects Visual Studio's bundled vcpkg.

The completion `check` builds the `aobus_guardrails` aggregate. Incremental
`build` and `run` commands do not repeat repository-wide source scans.

## Local state and build trees

Managed state defaults to `%LOCALAPPDATA%\Aobus`:

```text
%LOCALAPPDATA%\Aobus\
  build\<checkout-key>\<preset>\
  n\<lock-id>\p\
  cache\llvm\
    toolchains\
    downloads\
  tools\
    python\<version>\
    venvs\<checkout-key>\<tool-fingerprint>\
```

The checkout key combines a canonicalized filesystem identity with an opaque ID
stored in the checkout's private Git directory. This isolates clones and linked
worktrees while allowing mapped-drive, UNC, and junction aliases for one
checkout to reuse state. If that private Git directory is unavailable or
read-only, set one stable, unique `AOBUS_CHECKOUT_ID` for the checkout.

Debug and Release use `windows-debug` and `windows-release`; MSVC ASan uses
`windows-debug-asan`; tidy uses `windows-tidy`; WinUI uses the separate
multi-config `windows-winui` Visual Studio tree. A Visual Studio tree cannot
share the Ninja build directory.

Overrides apply in this order:

1. command-line `-p <dir>` or `BUILD_DIR` selects one exact primary tree;
2. `AOBUS_BUILD_ROOT` replaces only the build base;
3. `AOBUS_STATE_ROOT` replaces the complete managed-state base;
4. otherwise `%LOCALAPPDATA%\Aobus` is used.

A composite `ao.bat check` derives `<primary>-winui` when the primary path is
explicit. Keep build, tool, and cache overrides on a local Windows disk; mapped
drives are not reliably distinguishable from local disks and drive mappings are
login-session scoped.

Portal build-tree writers use a persistent lock beside the tree. This prevents
concurrent mutation but does not make a stable snapshot for a simultaneously
running application, test, or analyzer.

## Build and run WinUI

Check prerequisites and install the governed runtime explicitly when needed:

```bat
ao.bat doctor winui
ao.bat doctor winui --build-only
ao.bat setup winui-runtime
```

The Windows App SDK development closure is restored from
`app/windows-winui/packages.config` and `NuGet.Config`. The Windows App Runtime
is separate user/host state. `setup winui-runtime` verifies the governed SHA-256
and Microsoft Authenticode signature before installation; normal build and
doctor commands never install it. A healthy runtime registered for the current
user in the governed Microsoft package family and architecture satisfies the
requirement when its four-part version is at least the contract's runtime
version. Doctor and setup accept newer serviced versions rather than requiring
a downgrade; setup reports the selected installed version. The NuGet development
closure remains exactly pinned.

Build or launch the dedicated tree with:

```bat
ao.bat build --target winui
ao.bat build release --target winui
ao.bat run winui
ao.bat run winui release
```

The current app is unpackaged and framework-dependent; Developer Mode is not
required. Launch requires an interactive RDP/local desktop. An SSH service
session can build and verify but cannot display WinUI.

`ao.bat check` builds Debug WinUI after the native Debug graph except under
MSVC ASan. `ao.bat check release` validates the native IPO graph and builds the
WinUI Release configuration with LTCG.

The portal uses one concurrency limit for both CMake project scheduling and
MSBuild's cross-project C++ compiler scheduling. By default it leaves one
logical processor available. Set `CMAKE_BUILD_PARALLEL_LEVEL` to a positive
integer to override both limits:

```powershell
$env:CMAKE_BUILD_PARALLEL_LEVEL = 12
ao.bat build --target winui
```

The WinUI build enables MSBuild MultiToolTask with a process-count semaphore,
so this single limit controls concurrent `cl.exe` work across generated
projects instead of multiplying project-level and translation-unit-level
parallelism.

A current CMake/MSBuild limitation means WinUI may not relink when only a
linked library changed. If shared code outside `app/windows-winui/` changed,
delete the WinUI executable before rebuilding or pass `--clean`; otherwise a
successful library build can leave stale executable contents.

All first-party Windows executables verify their UTF-8 process manifest after
link. Narrow `argv` and audited narrow process boundaries are UTF-8, but
filesystem paths still require the project's explicit native/UTF-8 conversion
facades.

### Soul lifecycle probe

The WinUI build also produces `ao_winui_soul_probe`, a separate native GUI
executable built by `ao.bat check` but not run by unattended suites. From a
logged-in desktop, run
`<reported-build-dir>\app\windows-winui\soul-probe\Debug\ao_winui_soul_probe.exe`.
It removes and reinserts the same Soul in a real XAML tree, checking that both a
pause coast and an aura fade settle on `Unloaded` and do not replay on `Loaded`.
Exit code 0 requires both cases to complete; a 20-second dispatcher timeout
fails the probe. It uses the control's normal presentation API and no library,
audio endpoint, or saved application settings. The shipping executable has no
probe switches.

### Track and navigation probes

The WinUI build also executes `ao_winui_track_item_probe`, a console check of the
real generated TrackRowItem projection and lazy item view. Its eight scenarios
cover bounded materialization, null retry, cache identity and eviction, IndexOf,
GetMany, iterator lifetime, oversized-count rejection, and duplicate-inclusive
compact Credits counts. Failure stops the
build; no interactive desktop or audio endpoint is required.

`ao_winui_track_table_probe` is built by native checks but runs only from a
logged-in desktop, like the Soul probe:
`<reported-build-dir>\app\windows-winui\track-table-probe\Debug\ao_winui_track_table_probe.exe`.
Its temporary offline library and real application runtime exercise registered
track-table selection, stable-id restoration, scroll anchoring and reveal
priority. Sparse selection and interleaved headers distinguish selected-collection
positions from display indices. Filtering checks retain partially or fully hidden
Runtime selection through header-only gestures without publication, then restore
native rows when the filter is cleared. Presentation checks include the intermediate
public history-replay notification before controller adoption. Transport checks
retain a loaded icon across window-local Dark/Light transitions and an ancestor
accent override, comparing it with a separately theme-bound reference. They
observe programmatic PointerOver/Pressed states and the native disabled foreground
after runtime readiness is lost. A local silent provider supplies real runtime
enablement without an audio endpoint or media playback. Native navigation chrome
clearance covers the 720/1120 effective-width boundaries, including window-local
Light. A 90-second dispatcher deadline bounds execution; failure reports a
nonzero exit rather than a dialog.

`ao_winui_credits_probe` likewise requires a logged-in desktop:
`<reported-build-dir>\app\windows-winui\credits-probe\Debug\ao_winui_credits_probe.exe`.
It hosts the shipping Credits controls at a constrained width and exercises empty,
single, and multiple entries, duplicates, roles, reordering, locked/full kind scope,
mixed replacement/clear intent, validation, focus, cancellation, and disabled
callback admission. Steps await native Loaded/template readiness and observed
TextChanged delivery; a 90-second deadline fails rather than guessing completion
from dispatcher turns. A disposable application runtime also exercises the shipping
Properties coordinator's composed Save, durable readback and revision publication,
staging without writes, discard confirmation, and stale draft reload. It does not
cover MainWindow modal-root admission or multi-target coordinator workflows.

These are programmatic native XAML observations. The table uses representative
row templates, and transport checks inspect native properties: passing does not
certify physical input, shipping-template paint, live shipping theme changes or
audible playback. Inspect the actual shipping window separately for those
claims. The shipping executable has no probe switches; probe application
metadata stays outside its WinMD/PRI dependency closure. Close the application
and probes before another portal build or hygiene refresh can relink them.

## Compiler cache

Run `ao.bat setup compiler-cache` to install and verify governed ccache and
activate the host-local shared store. The Ninja trees use CMake compiler
launchers. The Visual Studio generator uses the managed host-local `cl.exe`
wrapper selected by `AOBUS_MSBUILD_CL_TOOL_EXE`; its directory is not added to
`PATH`.

See [compiler cache](compiler-cache.md) for override precedence, file-tracking
safety, and the optional fixed `S:`/`B:` compiler views available only in
isolated Windows SSH logons.

## Native tests and sanitizers

The default test group is core and TUI. Windows `all` and `ao.bat check` add
CLI, integration, and tooling. The `--lint` suite is Linux and macOS only;
Windows runs native lint verification via `ao.bat tidy` and `ao.bat format --check`.
The managed environment owns Ruff and mypy; ambient tools are not used.

`ao.bat check --asan` runs the native suite group with MSVC AddressSanitizer.
Third-party vcpkg libraries are not instrumented, so cross-boundary STL
container annotations are disabled. MSVC provides neither UBSan nor TSan, and
resumable coroutine instrumentation is incomplete; Linux ASan/UBSan and TSan
remain complementary. Windows `--tsan` and application `--clang` selections
fail before configuration.

Source inspection and portal tests do not certify native builds, GUI behavior,
audio, or sanitizer results. Run the opt-in WASAPI endpoint and playback probe
on a host with an active render endpoint:

```bat
ao.bat test --integration "[wasapi][.manual]"
```

The probe skips when there are no active endpoints; inspect the test summary
rather than treating a skipped run as playback evidence. It checks enumeration,
frame advancement, and drain completion, not whether a listener heard sound.
Use an interactive audio session and listen separately when audibility is part
of acceptance. Record the checks performed and remaining limits with the change.

## LLVM SDK and native lint tools

Windows format/tidy uses the official LLVM development archive pinned by
`cmake/LlvmSdk.cmake`. On first native C++ format or tidy configure, CMake
downloads it, verifies SHA-256, and extracts it below
`%LOCALAPPDATA%\Aobus\cache\llvm` by default. Later trees reuse the verified
cache. Concurrent provisioning is locked, and incomplete or stale SDKs are not
accepted.

`AOBUS_LLVM_SDK_CACHE_ROOT` relocates the managed cache. For an existing CMake
tree, reconfigure with the corresponding CMake option or create a new tree.
`AOBUS_LLVM_SDK_ROOT` instead names one complete pre-extracted SDK; it is
validated and never modified.

### Offline SDK setup

On an offline machine, extract the exact pinned archive in advance. From an
initialized Visual Studio x64 developer prompt with `VCPKG_ROOT` set, configure
a local tidy tree explicitly:

```bat
cmake -S . --preset windows-tidy -B C:\local\aobus-build\windows-tidy ^
  -DAOBUS_LLVM_SDK_ROOT=C:/toolchains/clang+llvm-<version>-x86_64-pc-windows-msvc
```

Use `start-msbuild-env.bat cmd` to open such a prompt from an ordinary terminal.
The value persists in that build tree. Reconfigure with
`-DAOBUS_LLVM_SDK_ROOT=` to return to the automatic verified cache. A
pre-provisioned root is never repaired in place; configuration names any missing
required path and fails.

The official `clang-tidy.exe` cannot load Aobus's out-of-tree C++ plugin.
Therefore the project builds `tool/lint/AobusClangTidy.exe`, statically linking
upstream and `aobus-*` checks from the same SDK. The portal verifies registration
and never falls back to Visual Studio or `PATH`. `--no-build` requires the
executable, compile database, and configured SDK to exist already.

Windows tidy replays exact Windows compile commands, including a companion WinUI
command database when needed. It reports and defers platform-incompatible files
for batch scopes; an explicitly named uncovered file fails. Complete shared C++
coverage still requires the affected Linux, macOS, and Windows native tidy
passes. See [checker development](lint/checker-development.md#native-replay-and-header-diagnostics)
for replay mechanics.

## Migrating a repository-local `out` tree

The portal does not move or delete an old `out` directory. Run a normal portal
command to create fresh local state. Do not copy CMake caches, build trees,
`vcpkg_installed`, or Python virtual environments; they contain absolute or
checkout-specific paths.

A complete previously verified LLVM SDK may be copied into the automatic cache
only with its `.aobus-llvm-sdk-complete` marker. Without that marker, select the
old directory through `AOBUS_LLVM_SDK_ROOT` instead. Validate a build or hygiene
run from new state before manually removing old files.

## Troubleshooting

- **LLVM SDK provisioning failure**: `llvm-<version>.lock` is a coordination
  file used by CMake's `file(LOCK ... GUARD FUNCTION)`, not an existence-based
  stale-lock marker. If lock acquisition times out, check whether another CMake
  process is still provisioning the SDK. Do not delete lock or cache files while
  a writer may be active. After interruption, rerun configuration: it validates
  the SDK and completion marker and re-extracts an invalid cache. For offline
  provisioning, use the complete extracted SDK and `AOBUS_LLVM_SDK_ROOT`
  procedure under [offline SDK setup](#offline-sdk-setup).
- **WinUI relinking staleness**: repeating an ordinary target build does not
  force a missed link. Follow the executable-removal or `--clean` procedure in
  [Build and run WinUI](#build-and-run-winui), selecting the same configuration
  and build tree as the application being tested.
- **`vswhere` or toolset discovery failure**: install or repair Visual Studio
  Build Tools with the required `.vsconfig`, including
  `Microsoft.VisualStudio.Component.VC.Tools.x86.x64`. Both `ao.bat` and
  `start-msbuild-env.bat` use the same discovery script, so opening the latter
  cannot repair missing discovery tools or components. Use
  `start-msbuild-env.bat cmd` when a separate tool needs an initialized x64
  environment after discovery succeeds.
- **Windows Store Python interception**: running python commands directly outside
  `ao.bat` may trigger the Windows Store app execution alias. Disable the
  "App execution aliases" for `python.exe` and `python3.exe` in Windows Settings
  or run all commands through `ao.bat`.
