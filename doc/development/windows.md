---
id: development.windows
---
# Windows development

The native Windows profile builds the shared core, CLI, FTXUI terminal frontend,
WinUI 3 desktop, native tests, and a dedicated clang-tidy executable. GTK is not
available.

This page owns platform prerequisites, portal commands, local state, WinUI
build/runtime requirements, native lint provisioning, and offline LLVM SDK setup.
Machine access, source synchronization, account settings, and operator-selected
paths belong in machine-local guidance, not this project contract. Observable
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
ao.bat setup llvm-sdk
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
skip native setup. `ao.bat run <app> --no-build` still prepares the native
environment for positive SDK and configured-artifact analysis admission, but
never builds missing outputs. Existing binary source provenance remains unverified.
It admits declared outputs and runfiles, but performs no dedicated comparison
between adjacent DLLs and their original SDK sources. With `--no-build`, SDK/DLL
freshness and post-build integrity are not guaranteed.

The Windows bootstrap launches the selected interpreter directly; it does not
return interpreter paths through temporary files. The selected Python process
parses once, retains any resolved source scope, and dispatches the same invocation.
Visual Studio's batch setup runs separately with no application arguments; its
environment returns through a private in-memory channel. Argument forwarding does
not use `CALL`. Parsing keeps the caller's working directory, while command
execution uses the repository root; UNC roots retain a `pushd` mapping until the
foreground portal exits. This does not replace build-tree or provisioning locks.

`start-msbuild-env.bat <command> [args...]` is available when another tool needs
the same Visual Studio environment. It preserves an explicit `VCPKG_ROOT` and
otherwise selects Visual Studio's bundled vcpkg.

The completion `check` runs the shared completion audits. Incremental
`build` and `run` commands do not repeat repository-wide source scans.

### Dependency linkage

The governed target and host triplets are the standard `x64-windows` vcpkg
triplet: target dependencies and host tools resolve from the same reviewed
SDK with normal dynamic linkage. Recipe-backed static-only ports remain
static; this introduces no first-party DLL ABI. The CRT remains `/MDd` in
Debug and `/MD` in Release, supplied by the native toolchain. Windows system libraries, the
Windows App SDK/runtime and build-tool executables remain separate boundaries;
this is a dynamic development SDK, not a fully static executable.

Let `ao.bat deps provision` install the reviewed SDK closure into the
selected tree; do not rename old import libraries, mix installations from
different triplets, or copy old caches into it. A stale
`x64-windows-static-md` installation is not this SDK. The build consumes that
installed SDK without restoring it; see
[Bazel setup](bazel.md#setup-and-commands).

Dynamic linkage makes the declared DLL runtime closure part of the SDK
contract: the installed inventory owns the DLLs the reviewed binaries import,
and import/runtime-closure checks reject unowned dependencies. Standard rules_cc
places linked and runtime-only `cc_import` DLLs beside binaries, tests and the
compiled catalog tool. WinUI assembles that output group together with its
WinAppSDK runtime payload in the executable's resource directory. These are
graph-owned outputs, not launch-time copies or `PATH` discovery; `data` alone
does not place a DLL beside an executable. Actual Debug/Release deployment,
CRT-flavor and loading validation remain separate from SDK admission.

FLAC consumes Ogg from the SDK as a normal dynamic dependency. CLI/TUI keep
the existing executable C++ global `new`/`delete` forwarding operators from
`mimalloc-new-delete.h`. This does not preserve or claim broad CRT
`malloc`/`free` or arbitrary cross-DLL allocation interception. Allocation
ownership must still match the allocating interface.

Validate a changed SDK in both Debug and Release, including product startup,
allocation behavior, decoder workflows and the catalog build-tool closure.
An installed archive inventory is not product acceptance, and results from a
former linkage do not validate a changed SDK. Select the affected native
gates through [validation and review](test/validation-and-review.md).

### Distribution and storage boundaries

[mpg123](https://www.mpg123.de/) is licensed under LGPL-2.1. Before distributing
Windows binaries that link it, choose and validate a compliance plan
for [LGPL-2.1 section 6](https://www.gnu.org/licenses/old-licenses/lgpl-2.1.html),
including recipients' ability to modify the library and relink the application.
That may require application object code or source, library source, and usable
relink instructions, alongside the required license notices and other terms.
The application's MIT license does not waive those obligations; the SDK's
dynamic linkage alone would not establish complete compliance either. There is
currently no Windows binary release/installer workflow in this repository. Any
future one must resolve this distribution boundary before publishing binaries.
The shared
[binary-distribution guidance](dependency-governance.md#binary-distribution)
covers other dependencies, including FDK-AAC source/notice requirements, and
the actual SDK/runtime payloads and assets that a release includes — with the
dynamic SDK, those payloads include the declared DLL closure a release actually
ships and loads.

Budget for installed SDK trees, intermediate archives, Debug symbols and
Release LTCG scratch space when sizing build trees and hosted runners.
Measure the selected tree's actual size and free-space headroom before
release/CI capacity decisions.

## Local state and build trees

Managed state defaults to `%LOCALAPPDATA%\Aobus`:

```text
%LOCALAPPDATA%\Aobus\
  build\<checkout-key>\
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

Without an explicit selection, each configuration gets its own default tree
named `bazel-<config>` under that checkout-isolated build root. An explicitly
selected tree (`-p`/`BUILD_DIR`) is mode-neutral: the `debug`/`release`/`asan`
profile is selected per invocation, not by the tree, and the composite
sanitizer check uses one selected tree for its normal Debug completion and the
managed sanitizer profile.

Overrides apply in this order:

1. command-line `-p <dir>` or `BUILD_DIR` selects one exact tree;
2. `AOBUS_BUILD_ROOT` replaces only the build base;
3. `AOBUS_STATE_ROOT` replaces the complete managed-state base;
4. otherwise `%LOCALAPPDATA%\Aobus` is used.

Keep build, tool, and cache overrides on a local Windows disk; mapped
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

### Source checkout routes

Use a Windows-local NTFS source root and host-local tools, SDKs, caches and
output ([native hosts and caching](bazel.md#native-hosts-and-caching)). The portal
rejects UNC and mapped-network Bazel working roots; it does not mount shares,
synchronize source, or fall back to a network build. The project does not
prescribe an editing host, transfer protocol, or source-mirror layout.

Keep the selected source stable while native commands or applications use it,
and preserve exact path spelling and repository line endings. Windows
case-insensitive lookup does not prove exact governed source spelling.

A source root without usable private Git metadata needs a stable
`AOBUS_CHECKOUT_ID` for managed-tool identity. Git-backed discovery independently
needs a supported [metadata binding](#shared-source-git-discovery).
Neither setting copies metadata, synchronizes source, or prepares another
root's tooling. Follow the [locked bootstrap](#prerequisites) for each checkout;
virtual environments contain checkout-specific paths and are not relocatable.

The checkout key's tolerance of mapped-drive, UNC and junction aliases is an
identity feature, not permission to build from a network root. Run the selected
local checkout's `ao.bat` from that root.

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

`build --target winui` builds and stages only the shipping application, without
building or executing probes. `ao.bat check` builds the application and every
native probe, stages the probes'
XBF/PRI resources, then executes the track-item and localization console checks.
It never launches GUI probes. The configured graph owns the boundary that
neither the shipping nor default build transitively depends on probes, and the
completion selection includes the application and every configured probe.

`ao.bat check release` validates the managed Release IPO graph — MSVC `/GL` and
`/LTCG` with the exact MainWindow per-file exception — and the same WinUI
validation in Release.

The [shared worker baseline](build.md#default-parallelism) leaves one available
logical processor free locally; `--jobs` overrides the concurrency limit.

All first-party Windows executables verify their UTF-8 process manifest after
link. Narrow `argv` and audited narrow process boundaries are UTF-8, but
filesystem paths still require the project's explicit native/UTF-8 conversion
facades.

### Bazel product details

The [WinUI target](bazel.md#winui-product) stages the real
product as `//app/windows-winui:aobus-winui`. `ao.bat deps provision` installs
the tree's vcpkg manifest and restores the locked NuGet closure into the managed
cache; the existing `packages.config`, dependency
policy and Windows SDK selection remain the version authorities, and builds
themselves neither restore nor install packages. The build also
exposes explicit native probe targets and lint; development build acceptance
supplies no signing or installer workflow. The [WinUI product](bazel.md#winui-product)
owns compilation and executable-local staging. A successful build or bounded
startup does not replace the interactive GUI gates below or establish complete
WinUI GUI acceptance.

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

`ao.bat check` also produces `ao_winui_soul_probe`, a separate native GUI
executable not run by unattended suites. From a
logged-in desktop, run
`<printed staged probe path>\ao_winui_soul_probe.exe`.
It removes and reinserts the same Soul in a real XAML tree, checking that both a
pause coast and an aura fade settle on `Unloaded` and do not replay on `Loaded`.
Exit code 0 requires both cases to complete; a 20-second dispatcher timeout
fails the probe. It uses the control's normal presentation API and no library,
audio endpoint, or saved application settings. The shipping executable has no
probe switches.

### Quality tooltip property probe

`ao_winui_quality_tooltip_probe` is built by native checks but runs only from a
logged-in desktop:
`<printed staged probe path>\ao_winui_quality_tooltip_probe.exe STATE_ROOT AUDIO_FIXTURE`.
Both arguments must be absolute paths canonically outside the checkout used
at build time. The probe resolves and reports its compiled checkout root;
verify that it is the actual native-local checkout path, not an alias or a
mapped view. The desktop logon must have access to that same checkout.
Admission fails rather
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

The WinUI validation aggregate executes `ao_winui_track_item_probe`, a console check of the
real generated TrackRowItem projection and lazy item view. Its nine scenarios
cover bounded materialization, null retry, cache identity and eviction, IndexOf,
GetMany, iterator lifetime, oversized-count rejection, duplicate-inclusive
compact Credits counts, and the row names UI Automation reads. Failure stops the
build; no interactive desktop or audio endpoint is required.

`ao_winui_track_table_probe` is built by native checks but runs only from a
logged-in desktop, like the Soul probe:
`<printed staged probe path>\ao_winui_track_table_probe.exe`.
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
`<printed staged probe path>\ao_winui_activity_status_probe.exe`.
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
`<printed staged probe path>\ao_winui_credits_probe.exe`.
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

`ao.bat test --winui` is a Windows-only explicit suite: it builds the Debug
WinUI validation selection, then runs `ao_winui_automation_probe` on a desktop.
`-p` selects the tree. Reuse requires a recent validation build in that tree:
the shipping-only `build --target winui` does not build or refresh the
automation probe. Missing probes fail. Ordinary `check` builds the WinUI
validation graph but never executes GUI probes; the `all` test group does not
include this explicit desktop suite.

The probe requires an authenticated interactive console: it exits with a diagnostic instead of starting a GUI whenever the process is not in the active console session (no session 0, no silent skip, no fallback), so an unattended host fails rather than reports false success. CI runs the same real probe as a separate required Windows gate after its completion check, through Bazel, with the same runtime and interactive-console admission; the runner must provide that session. The gate does not provision or bypass desktop login.

The probe loads the real `MainWindow` against an isolated `LibrarySession` with two metadata-only tracks in one album (a disposable scratch root, no playback or personal saved settings). It drives the true Modern and Classic shells, flat and album-grouped rows, a narrowed Classic window, and the volume and overflow popups. It walks the automation tree — peers and their children, including disabled controls — and requires a nonblank final `AutomationPeer.Name` from the audited interactive and item roles; an attached property or id stamp alone is never accepted as a name. Decorative (`Raw` view), text, and layout roles are out of scope, and `ScrollBar` template internals are explicitly excluded.

Positive landmarks use those same audited observations, not visual existence or a minimum control count. Native roles and authored component scopes identify the on-screen Soul and seek controls, title header and resize grip, both track rows, and the album heading when grouped. Wide layouts additionally require the navigation and inspector resize grips, plus Classic's All Tracks tree item. The probe requests DPI-scaled client sizes, overrides only its own window's native tracking ceiling (including measured chrome), and checks the effective DIP width before applying wide or narrow expectations; intentionally hidden narrow panes are not required. The sizing fixture verifies both a constrained width and a client request beyond the system's default tracking ceiling, without changing the desktop resolution. Visual ancestors supply scope metadata only for already-audited peers. Canaries reject missing or whitespace-only names, wrong-role and wrong-scope landmarks, a named control moved to Raw view, and a removed control; factory-named controls (`makeNamed`), text content, and `LabeledBy` sources pass.

Name presence is not semantic correctness: dialogs the probe never opens, rows it never materializes, names in other locales, and real assistive technology such as Narrator remain uncertified. The [frontend accessibility contract](../system/frontend/windows.md#frontend-observations) owns how controls receive names; the existing track-table and activity-status probes stay separate, manual desktop contracts.

## Native tests and sanitizers

The default test group is core, application, and TUI. Windows `all` and `ao.bat check` add
CLI, tooling, and lint. The backend suite is explicit-only for `ao.bat test`.
Both of its
cases are hidden `[.manual]`, so `ao.bat test --backend` without a filter selects
no case and fails (non-zero). `ao.bat check` still runs the backend executable,
as on every host; it selects none of those cases and passes
([completion gate](bazel.md#completion-gate)). Use the exact case name to select the probe whose
hardware prerequisites you prepared, as the provider example below shows;
`[.manual]` selects both cases.
Automatic `all` and concurrency selections exclude them. `ao.bat test --lint` runs the
custom-check fixture diagnostics from the Bazel lint graph with the governed
LLVM SDK and Visual Studio includes, replaying the actual configured fixture
producer action (FixtureTools) fresh in a new attempt directory; cached lint
results are never replayed.
Compiler errors in those diagnostic invocations fail the fixtures; there is no second syntax-only pass.
Objective-C++ fixtures are explicitly deferred, not counted as Windows coverage.
Use `ao.bat tidy` for native source findings and `ao.bat format --check` for
formatting. The managed environment owns Ruff and mypy; ambient tools are not used.

`ao.bat check --asan` runs the full normal Debug completion first, then a
separate managed MSVC AddressSanitizer profile in the same selected
tree.
Third-party vcpkg libraries are not instrumented, so cross-boundary STL
container annotations are disabled. MSVC provides neither UBSan nor TSan, and
resumable coroutine instrumentation is incomplete; Linux ASan/UBSan and TSan
remain complementary. Windows `--tsan` and application `--clang` selections
fail before tree or SDK work.

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
explicit provisioning installs it into the shared
verified cache used by lint and its [hygiene
gate](bazel.md#hygiene-gate): `ao.bat deps provision` and
`ao.bat setup llvm-sdk` call the same native installer. Ordinary ready-only
build, lint and format commands never install it. The portal reads the manifest
lazily when Windows SDK code needs it, not while registering commands.
Hygiene checks for a ready LLVM SDK only when its resolved scope contains
C++ files; Python-only and empty scopes do not require it. Managed Python tooling
is prepared only for Python files; see the [gate's tool-selection contract](bazel.md#hygiene-gate).
Required-file entries use portable forward-slash-separated components containing
ASCII letters, digits, `_`, `.`, `+`, or `-`; `.` and `..` components are rejected.
This keeps the same relative-path interpretation on every host.
Provisioning downloads the archive, verifies SHA-256, and extracts it below
`%LOCALAPPDATA%\Aobus\cache\llvm` by default. Later use reuses the verified
cache. Concurrent provisioning is locked, and incomplete or stale SDKs are not
accepted. Ready-only validation also rejects malformed completion
markers, including non-UTF-8 text; explicit provisioning repairs an invalid
automatic cache under its shared lock. Access errors propagate rather than being
treated as proof that an existing cache is corrupt.

`ao.bat format` uses the ready provisioned SDK directly: no CMake, no
provisioning, and no Visual Studio environment are needed for formatting.

`AOBUS_LLVM_SDK_CACHE_ROOT` relocates the managed cache.
`AOBUS_LLVM_SDK_ROOT` instead names one complete pre-extracted SDK; it is
validated and never modified.

### Offline SDK setup

On an offline machine, extract the exact pinned archive in advance and place it
as `AOBUS_LLVM_SDK_ROOT`, or populate the managed cache with a complete
previously verified SDK including its `.aobus-llvm-sdk-complete` marker.
A pre-provisioned root is never repaired in place; admission names any missing
required path and fails.

The official `clang-tidy.exe` cannot load Aobus's out-of-tree C++ plugin.
Therefore the project builds `tool/lint/AobusClangTidy.exe`, statically linking
upstream and `aobus-*` checks from the same SDK. The portal verifies registration
and never falls back to Visual Studio or `PATH`.

Windows tidy replays exact Windows compile actions, including WinUI generated
contexts when needed. Tidy reports and defers platform-incompatible files
for batch scopes; an explicitly named uncovered file fails. Complete shared C++
coverage still requires the affected Linux, macOS, and Windows native tidy
passes. See [checker development](lint/checker-development.md#native-replay-and-header-diagnostics)
for replay mechanics.

## Migrating a repository-local `out` tree

The portal does not move or delete an old `out` directory. Run a normal portal
command to create fresh local state. Do not copy build trees,
`vcpkg_installed`, or Python virtual environments; they contain absolute or
checkout-specific paths.

Validate a build or hygiene
run from new state before manually removing old files.

## Troubleshooting

- **LLVM SDK provisioning failure**: `llvm-<version>.lock` is a coordination
  file used by `ao.bat deps provision`,
  not an existence-based stale-lock marker. If lock acquisition times out, check
  whether another provisioning process is still installing the SDK. Do not delete lock or cache files while
  a writer may be active. After interruption, wait for the portal's installer
  to settle before starting another writer, then rerun
  `ao.bat deps provision` as appropriate: it validates the SDK and completion
  marker and re-extracts an invalid automatic cache. For offline
  provisioning, use the complete extracted SDK and `AOBUS_LLVM_SDK_ROOT`
  procedure under [offline SDK setup](#offline-sdk-setup).
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
