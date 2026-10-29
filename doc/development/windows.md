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

Do not install a separate Python for the normal path. `ao.bat` obtains the
NuGet tool through the Visual Studio vcpkg installation, uses NuGet's `python`
package for the exact CPython release from `script/ao/toolchain.json`, and
creates a checkout-specific environment from the hashed Windows requirements lock. It does not use the Windows Store
alias or ambient `PATH` Python. The manifest's `python3` host port serves
dependency builds; it is not the portal interpreter or the Ruff/mypy provider.

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
login-session scoped. This is a storage requirement, not automatic local-volume
enforcement; verify the actual backing storage for overrides.

Ruff check/format and mypy use `.ruff_cache` and `.mypy_cache` under the
managed environment's `sys.prefix`, including for local source checkouts.
These explicit cache paths take precedence over inherited tool cache settings.
Check-only commands may write caches without rewriting source. This is not a
write-free checkout guarantee: ordinary Python `__pycache__` and explicit
checkout-ID creation remain separate writes. Moving build trees alone does not
move Git metadata or Python bytecode.

Portal build-tree writers use a persistent lock beside the tree. This prevents
concurrent mutation but does not make a stable snapshot for a simultaneously
running application, test, or analyzer.

### Shared-source Git discovery

A linked worktree created on another host may have a `.git` pointer that native
Git cannot use. For Git-backed source discovery, pass `AOBUS_GIT_DIR` (the
intended private Git directory) and `AOBUS_GIT_WORK_TREE` (the source root) to
the portal process. Both must be present, nonempty native absolute paths;
malformed pairs fail when discovery runs. Explicit files, `--folder`, and
`--all` bypass this discovery and its binding validation. A complete pair for
another normalized root is ignored; a discovery failure reports the mismatch.

Use Python's `PROJECT_ROOT`, derived from the imported tooling module's path,
not the current directory after `ao.bat`'s `pushd`. The batch file sets
`PYTHONPATH` from its own directory before `pushd`: a UNC module path remains
UNC even when the working directory becomes a temporary drive letter. UNC,
mapped-drive, and junction spellings are not guaranteed to match each other.

For example, set `$verifiedPrivateGitDir` and `$verifiedPortalRoot` to the
verified native absolute paths, using the portal's actual root spelling.
Preserve any existing process-scoped values in the PowerShell session:

```powershell
$previousGitDir = $env:AOBUS_GIT_DIR
$previousWorkTree = $env:AOBUS_GIT_WORK_TREE
try {
    $env:AOBUS_GIT_DIR = $verifiedPrivateGitDir
    $env:AOBUS_GIT_WORK_TREE = $verifiedPortalRoot
    .\ao.bat format --check
} finally {
    $env:AOBUS_GIT_DIR = $previousGitDir
    $env:AOBUS_GIT_WORK_TREE = $previousWorkTree
}
```

Do not create or copy metadata for this example. Before accepting a
mapping, compare its actual private/common/index paths, exact HEAD, and semantic
staged index with the intended checkout. A plausible `--show-toplevel` is not
association proof: this trusted binding does not verify that metadata belongs
to the source root.

Only Git-backed source discovery calls use the pair. Discovery also uses
process-local `--no-optional-locks` and `-c diff.autoRefreshIndex=false`:
optional-lock suppression alone does not
prevent `git diff` from writing stat-only index refreshes.
Working-tree discovery also compares content, so stat-only differences do not
expand the selected source scope.
The binding does not redirect `setup git-hooks`,
performance revision commands, or arbitrary Git operations. It neither rewrites
`.git` nor redirects checkout-ID creation. If the original private Git directory
is inaccessible, supply a separate stable, unique `AOBUS_CHECKOUT_ID` as
described above. Do not export standard `GIT_DIR` or `GIT_WORK_TREE` across
the portal: dependencies and disposable test repositories must remain independent.
The binding and these discovery options do not prove that Git or the entire
checkout is write-free.

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
a downgrade; setup reports the selected installed version. These checks inventory
framework registration, not bootstrap execution or successful application startup.
Validate the actual app on its [supported systems](../user/use-windows-desktop.md);
package requirements depend on the consumed SDK, bootstrap path and features.
The NuGet development closure remains exactly pinned.

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

For offline deployment without SDKs, Python, or network access, see
[staging prerequisites](#stage-offline-prerequisites).

`ao.bat check` builds Debug WinUI after the native Debug graph except under
MSVC ASan. `ao.bat check release` validates the native IPO graph and builds the
WinUI Release configuration with LTCG.

The portal uses one concurrency limit for both CMake project scheduling and
MSBuild's cross-project C++ compiler scheduling. The
[shared worker baseline](build.md#default-parallelism) leaves one available
logical processor free locally. Set `CMAKE_BUILD_PARALLEL_LEVEL` to a positive
integer to override both limits; CI sets it to the full detected logical processor count:

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

### Stage offline prerequisites

For offline deployment without target-side SDKs, Python, or network access, use
the [operator instructions](../../app/windows-winui/deployment/README.md). This
helper prepares the machine CRT and application user's runtime registration; it
is not a complete installer.

From the repository root on the build host, stage the matching signed x64 VC
redistributable, the consumed
`VC\Tools\MSVC` directory version (not the `cl.exe` file version), and the
Windows App Runtime installer governed by
[dependency-contract.json](../../dependency-contract.json):

```powershell
.\script\windows\stage-prerequisites.ps1 `
  -VcRedistPath $vcRedistPath `
  -VcToolsVersion $vcToolsVersion `
  -WindowsAppRuntimeInstallerPath $runtimeInstallerPath `
  -Destination 'C:\package\Prerequisites'
```

Set the input variables from the matching build, not an unrelated installation.
The [staging script](../../script/windows/stage-prerequisites.ps1) never installs
anything and refuses an existing destination. It verifies Microsoft signatures,
the governed runtime hash, x64 VC identity/version, and copied payload hashes.
The output includes `README.md`, both installers, `prerequisites.json`,
`Install-Prerequisites.ps1`, and `Prerequisites.cmd`. The manifest records the
bundled CRT floor/hash and governed runtime identity/minimum/hash; it is an
artifact, not another dependency policy. Keep the complete package together,
include applicable notices, and verify its hash after transfer.

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

### Quality tooltip property probe

`ao_winui_quality_tooltip_probe` is built by native checks but runs only from a
logged-in desktop:
`<reported-build-dir>\app\windows-winui\quality-tooltip-probe\Debug\ao_winui_quality_tooltip_probe.exe STATE_ROOT AUDIO_FIXTURE`.
Both arguments must be absolute paths canonically outside the checkout used
at build time. CMake resolves and reports `Quality tooltip probe checkout root`
when configuring the probe. For an SMB checkout, verify that this is the UNC
checkout path, not an SSH session's temporary `pushd` drive or `S:` compiler
view. The desktop logon must have access to that same checkout; resolving the
path does not transfer SMB credentials between logons. Admission fails rather
than skipping checkout checks if the compiled root is inaccessible.
The state directory must not exist (even as a symlink), its parent must exist,
and it must be outside the fixture's directory tree. The probe resolves existing
ancestors and compares filesystem identities, including Windows path aliases,
before creating state. Ancestor checks include DOS volume roots and stop at
real UNC share roots, never the virtual server root. Canonical paths must use
ordinary DOS-drive or UNC-share syntax; extended spellings are accepted only
when canonicalization normalizes them to those forms. Other namespaces are
rejected rather than given guessed containment rules.
Use an existing `.flac` fixture. The decoder opens it
read-only; no filesystem read-only attribute is required or changed. Keep the
fixture and paths unchanged during the run. No media copying, fixture-directory
writes, or cleanup is performed; database and workspace configuration writes
stay under the fresh state root.

The probe uses a real decoder and application runtime with a discard backend,
not an audio endpoint. Its graph has no Sink, so Unknown channel confidence
here does not establish that the decoder failed to identify its source layout.
It checks real seeded-track publication and the shipping tooltip adapter's
text, HelpText, availability, owner-window resize, and teardown against shared
Now Playing presentation.
These are native-property observations, not an opened tooltip's wrapping,
clipping, pixels, or physical speaker-routing evidence. Keep the state directory
as evidence and close the probe before another build or hygiene refresh.

Input rejection runs before WinRT/XAML startup and can be checked without a
desktop. Invoke the same executable with invalid arguments and require exit
code 2, the expected input/startup diagnostic, and no state-directory creation.
Useful cases include relative paths, reused state, a missing parent, a fresh
state path inside the checkout (including aliases), state under the fixture's
directory, and malformed state names. Check the diagnostic as well as the exit
code: an inaccessible compiled checkout is not evidence that the intended
externality or overlap check ran. Accepted inputs still require a desktop.

### Track and navigation probes

The WinUI build also executes `ao_winui_track_item_probe`, a console check of the
real generated TrackRowItem projection and lazy item view. Its nine scenarios
cover bounded materialization, null retry, cache identity and eviction, IndexOf,
GetMany, iterator lifetime, oversized-count rejection, duplicate-inclusive
compact Credits counts, and the row names UI Automation reads. Failure stops the
build; no interactive desktop or audio endpoint is required.

`ao_winui_track_table_probe` is built by native checks but runs only from a
logged-in desktop, like the Soul probe:
`<reported-build-dir>\app\windows-winui\track-table-probe\Debug\ao_winui_track_table_probe.exe`.
Its temporary offline library and real application runtime exercise registered
track-table selection, stable-id restoration, scroll anchoring and reveal
priority. The frame's compiled header template verifies localized column-resize
names before and after sort and width changes; the probe does not duplicate
that binding in test XAML. The template donor has no initialized library session,
so header click, drag, and context-menu routing are not tested: column changes
use the controller's public API. Sparse selection and interleaved headers distinguish selected-collection
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

`ao_winui_activity_status_probe` also runs only from a logged-in desktop:
`<reported-build-dir>\app\windows-winui\activity-status-probe\Debug\ao_winui_activity_status_probe.exe`.
It builds the registered `status.activity` component — real chrome, adapter, and
runtime — in its own window, taking the strip's styles from the actual compiled
`MainWindow` resources through an uninitialized donor window, and invokes the
component's native automation providers; a separately theme-bound reference
provides the normal foreground. Empty idle and ordinary Info states have no
enabled action, attached flyout, or tooltip; warnings and retained idle details
remain keyboard- and automation-openable. It checks the real compact and
detail-dismiss names, null inactive tooltips, Light/Dark foreground updates, and
native Invoke and lifecycle transitions, without physical input,
playback, or user state. A 30-second dispatcher deadline bounds the probe. Native
checks build it but do not execute it unattended.

`ao_winui_credits_probe` likewise requires a logged-in desktop:
`<reported-build-dir>\app\windows-winui\credits-probe\Debug\ao_winui_credits_probe.exe`.
It hosts the shipping Credits controls at a constrained width and exercises empty,
single, and multiple entries, duplicates, roles, reordering, locked/full kind scope,
automation-driven kind dropdown choice, default Add kind, row command names,
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

### UI Automation naming gate

`ao.bat test --winui` is a Windows-only explicit suite: it builds the Debug configuration of the dedicated WinUI tree plus the `ao_winui_automation_probe` executable (CMake target `ao_winui_automation_probe`), then runs that probe on a desktop. `-n/--no-build` reuses an existing dedicated WinUI tree, and `-p` selects that dedicated tree. Ordinary `check` and `all` runs build the WinUI graph but never execute GUI probes.

The probe requires an authenticated interactive console: it exits with a diagnostic instead of starting a GUI whenever the process is not in the active console session (no session 0, no silent skip, no fallback), so an unattended host fails rather than reports false success. CI runs it as a separate required Windows gate after `check`; the runner must provide that session. The gate does not provision or bypass desktop login.

The probe loads the real `MainWindow` against an isolated `LibrarySession` with two metadata-only tracks in one album (a disposable scratch root, no playback or personal saved settings). It drives the true Modern and Classic shells, flat and album-grouped rows, a narrowed Classic window, and the volume and overflow popups. It walks the automation tree — peers and their children, including disabled controls — and requires a nonblank final `AutomationPeer.Name` from the audited interactive and item roles; an attached property or id stamp alone is never accepted as a name. Decorative (`Raw` view), text, and layout roles are out of scope, and `ScrollBar` template internals are explicitly excluded.

Positive landmarks use those same audited observations, not visual existence or a minimum control count. Native roles and authored component scopes identify the on-screen Soul and seek controls, title header and resize grip, both track rows, and the album heading when grouped. Wide layouts additionally require the navigation and inspector resize grips, plus Classic's All Tracks tree item. The probe requests DPI-scaled client sizes, overrides only its own window's native tracking ceiling (including measured chrome), and checks the effective DIP width before applying wide or narrow expectations; intentionally hidden narrow panes are not required. The sizing fixture verifies both a constrained width and a client request beyond the system's default tracking ceiling, without changing the desktop resolution. Visual ancestors supply scope metadata only for already-audited peers. Canaries reject missing or whitespace-only names, wrong-role and wrong-scope landmarks, a named control moved to Raw view, and a removed control; factory-named controls (`makeNamed`), text content, and `LabeledBy` sources pass.

Name presence is not semantic correctness: dialogs the probe never opens, rows it never materializes, names in other locales, and real assistive technology such as Narrator remain uncertified. The [frontend accessibility contract](../system/frontend/windows.md#frontend-observations) owns how controls receive names; the existing track-table and activity-status probes stay separate, manual desktop contracts.

### Capture MSBuild incrementality

Use the build-only `--msbuild-capture <directory>` option for private diagnosis
of a native WinUI build:

```powershell
.\ao.bat build --target winui --msbuild-capture "$env:LOCALAPPDATA\Aobus\diagnostics\first"
.\ao.bat build --target winui --msbuild-capture "$env:LOCALAPPDATA\Aobus\diagnostics\second"
```

Prepare the private parent directory first. Each capture destination must be a
new absolute guest-local directory, separate from the source and selected build
trees. Existing destinations, remote storage, whitespace, quotes, and
logger-delimiter characters are rejected; use a plain directory spelling for
the initial capture interface. The option is unavailable for Ninja targets, other platforms, and
`check`; it does not expose arbitrary native build arguments.

The normal portal still configures and builds the real WinUI target under its
build-tree lock. Capture adds a binary log with `ProjectImports=None`, private
tracking/state snapshots around configuration/build, and selected identity
metadata. It never forces tracking on, substitutes a cache provider, cleans a tree, or skips
an ordinary target build. Keep the source, generated inputs, configuration,
launcher and backend unchanged when comparing consecutive invocations. A local
managed-ccache or uncached result is not evidence for CI's sccache fallback.
The captured tracking policy flag is not the evaluated `TrackFileAccess`
property: for example, an uncached tree has no compiler-cache props import even
when the policy flag is `0`. Check the selected wrapper and evaluated MSBuild
evidence before drawing a tracking conclusion.

**Keep the whole capture private.** Binary logs and tlogs can contain compiler
arguments, paths and environment-derived properties. `ProjectImports=None`
omits imported project contents; it does not redact secrets. Choose a parent
with suitable native filesystem permissions, do not blindly upload captures as
CI artifacts, and publish only reviewed derived facts. Public
[phase summaries](build.md#measure-native-validation) never include this raw
capture. No diagnostic-verbosity console or full-environment dump is enabled.

Snapshots privately preserve tree-relative raw `.tlog` and `.lastbuildstate`
files, the bare `unsuccessfulbuild` marker, and suffix-shaped
`*.unsuccessfulbuild` project markers; basename matching is case-insensitive.
The per-stage `project_state` list and `project-state` copies distinguish a
zero-byte marker from absence; compare lists across stages to identify removal.
These are selected diagnostic files, not a complete incremental filesystem or
replayable build state. Selected `.exe`, `.dll`, and `.lib` outputs have
size/mtime/SHA-256 identities only; their payloads and object files are not
copied, and object files are not hashed. Each stage independently hashes its
selected files. Only the build-tree-root `vcpkg_installed` and `third-party_deps`
subtrees (including nested dependencies/tools) are pruned from traversal;
identically named directories deeper in first-party output/tool trees remain
eligible. Native project tracking files, nested WinUI outputs and other selected
output identities outside those dependency roots remain eligible. Selected
compiler/wrapper tools are hashed separately even if located in a pruned tree.
This selection does not bound file count, bytes read, or elapsed time; other
configurations and unexcluded nested output/tool trees can still add work.

The private `capture.json` lifecycle receipt is separate from each snapshot's
`complete` flag, which describes only that snapshot. Its fixed fields are
`stage` (`setup`, `before-configure`, `configure`, `before-build`, `build`, or
`after-build`), `outcome` (`aborted`, `failed`, or `ok`), and `build_status`
(`not-run`, `failed`, or `ok`, describing the native build only). The initial
receipt is `setup`/`aborted`/`not-run`; finalization records the original failing
stage, or `after-build` for final evidence and success. Setup failures do not fabricate
pre-build snapshots. Missing/empty binlog evidence after native success fails
capture even though the receipt's native `build_status` is `ok`. Receipt updates
replace atomically, retaining the initial classification if a later write
fails; persistent storage errors can prevent even the initial receipt. Evidence
I/O failures never replace an existing build/setup exception. Leaves are never
deleted or reused, including failed attempts. These fields/lists are a private
diagnostic contract, not additions to the public phase schema or a replay API;
they contain no exception text, argv, or environment dump.

Capture itself adds logging, copying and hashing work. Use it to explain dirty
compile/generate/link tasks, not as a matched end-to-end speed benchmark; run
performance comparisons separately without diagnostic capture. No native
performance improvement follows from the narrowed selection alone. Preserve
failed attempts and their trees rather than cleaning away the evidence.

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
CLI, tooling, and lint. The backend suite is compiled but explicit-only. Both
cases are hidden `[.manual]`, so `ao.bat test --backend` without a filter selects
no case and fails (non-zero). Use the exact case name to select the probe whose
hardware prerequisites you prepared, as the provider example below shows;
`[.manual]` selects both cases.
Automatic `all` and concurrency selections exclude them. `ao.bat test --lint` prepares the separate
`windows-tidy` host and runs the custom-check fixture diagnostics with the governed LLVM SDK and Visual Studio includes.
Compiler errors in those diagnostic invocations fail the fixtures; there is no second syntax-only pass.
Objective-C++ fixtures are explicitly deferred, not counted as Windows coverage.
Use `ao.bat tidy` for native source findings and `ao.bat format --check` for
formatting. The managed environment owns Ruff and mypy; ambient tools are not used.

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
ao.bat test --backend "WasapiProvider - enumerates and renders through real endpoints"
```

The exact case name selects the provider probe only; the hidden WASAPI
channel-map readback probe has different prerequisites and stays separate.
The probe skips when there are no active endpoints; inspect the test summary
rather than treating a skipped run as playback evidence. It checks enumeration,
frame advancement, and drain completion, not whether a listener heard sound.
Use an interactive audio session and listen separately when audibility is part
of acceptance. Record the checks performed and remaining limits with the change.

## LLVM SDK and native lint tools

Windows format/tidy uses the official LLVM development archive pinned by
[`script/ao/windows-llvm-sdk.json`](../../script/ao/windows-llvm-sdk.json);
`cmake/LlvmSdk.cmake` provisions that archive. The portal reads the manifest
lazily when Windows SDK code needs it, not while registering commands.
Required-file entries use portable forward-slash-separated components containing
ASCII letters, digits, `_`, `.`, `+`, or `-`; `.` and `..` components are rejected.
This keeps the same relative-path interpretation in CMake and Python on every host.
On first native C++ format or tidy configure, CMake
downloads it, verifies SHA-256, and extracts it below
`%LOCALAPPDATA%\Aobus\cache\llvm` by default. Later trees reuse the verified
cache. Concurrent provisioning is locked, and incomplete or stale SDKs are not
accepted.

Prepare the same governed SDK explicitly before native tooling controls with
`ao.bat setup llvm-sdk`. This initializes the required Visual Studio environment
and provisions the SDK in its dedicated lint-host context, without formatting
an unrelated source file or building the custom checker. For SDK setup and
product-test groups, `BUILD_DIR` names a primary product tree and the checker
host is its `<primary>-tidy` sibling. In contrast, direct `tidy` and `format`
commands treat `BUILD_DIR` as the exact tool tree. To reuse the host prepared
by setup, pass that sibling explicitly, for example
`ao.bat tidy -p C:\build\primary-tidy <source>`. Prefer `AOBUS_BUILD_ROOT` when
you only want to relocate the base while retaining separate product and tool
trees.

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
command database when needed. WinUI lint preparation configures that tree and
runs only `aobus-winui-lint-context`: native IDL/C++/WinRT and XAML generation,
not application compilation, linking, probe execution, or resource deployment.
`ao.bat check` still owns the full product build and tests. To diagnose generation
separately, run `ao.bat build --target aobus-winui-lint-context`; tidy consumes the
Debug context, even after a Release product build. Private `--msbuild-capture`
remains restricted to full WinUI builds, not this nested generation target.
Tidy reports and defers platform-incompatible files
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
