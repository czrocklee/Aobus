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

Initial Python setup, formatter setup, and explicit SDK provisioning need
network access unless their governed payloads are already cached. Ordinary Bazel builds consume a
ready SDK; they never install native dependencies.

Visual Studio's bundled vcpkg is the default. `VCPKG_ROOT` may select another
complete checkout for diagnosis, but the repository registry and manifest locks
still apply.

Do not install a separate Python for the normal path. `ao.bat` obtains the
NuGet tool through the Visual Studio vcpkg installation, uses NuGet's `python`
package for the exact CPython release from `script/ao/toolchain.json`. It does
not use the Windows Store alias or ambient `PATH` Python. The manifest's
`python3` host port serves dependency builds; it is not the portal interpreter
or the Ruff/mypy provider.

`AOBUS_PYTHON` is an advanced override for an explicit interpreter. It must
match the exact policy version. The governed base must provide `venv` and
`ensurepip` for wheel installation: the portal uses them for standalone
`check docs` and Python formatting/lint scopes, while Bazel prepares its own
checker payload without using the checkout's managed environment.

## Native Bazel and project commands

Run from the repository root. Prepare the pinned executable and SDK explicitly:

```bat
ao.bat setup bazel
ao.bat setup deps
```

Use the pinned executable printed by setup (or a Bazelisk pinned to
`.bazelversion`); it is not automatically on `PATH`. Before bare Bazel commands,
initialize the x64 Visual Studio environment, select the installed `x64-windows`
triplet with `AOBUS_BAZEL_VCPKG_PREFIX`, and set all three matching standard
selectors: `BAZEL_VC`, `BAZEL_VC_FULL_VERSION`, and
`BAZEL_WINSDK_FULL_VERSION`. An initialized shell alone is insufficient;
[native Bazel setup](bazel.md#direct-bazel-without-the-portal) supplies the
PowerShell selector recipe and admission rules. For shell-backed rules, put
Git for Windows' `usr\bin` on `PATH` or set `BAZEL_SH` in the developer shell
to the full path of `bash.exe`. Having `git.exe` alone, or setting only
`--repo_env=BAZEL_SH=…`, does not select Bash for the Bazel client. The portal
uses an explicit `BAZEL_SH` or discovers Git Bash for both the client and its
repository rules. Bare Bazel never installs Python, SDKs, ports, or WinUI runtime.

```bat
bazel build //app/cli:aobus //app/tui:aobus-tui
bazel build --config=release //app/tui:aobus-tui
bazel run //app/cli:aobus -- --help
bazel run //app/tui:aobus-tui

bazel test //:tests
bazel test --config=asan //test:utility_test

ao.bat setup llvm-sdk
ao.bat setup prettier
ao.bat check format
ao.bat check lint
ao.bat check deps

ao.bat check --extend
ao.bat check --extend --config release
```

Use
`ao.bat <command> --help` for current options rather than treating this list as
a complete CLI reference.

Build-capable project commands initialize Visual Studio x64 only when needed;
help and Python-only or authored-text source scopes skip native setup. Ordinary native Bazel
commands instead require the caller's initialized environment and selectors.
Prettier uses its own governed Node/tool bundle, not ambient Node or npm.
Markdown/JSON/YAML/CSS-only formatting needs neither a C++ SDK nor the managed
Ruff/mypy environment; [formatter setup](linting.md#authored-text-formatting)
prepares only that tool.
Running an existing binary directly is diagnostic reuse, not proof of source
provenance, SDK/DLL freshness, or post-build integrity.

The Windows bootstrap launches the selected interpreter directly; it does not
return interpreter paths through temporary files. Base Python parses the invocation
for tool admission and transfers any resolved source scope, including individual-file
provenance, through a private temporary record. The foreground selected interpreter
parses the same arguments and consumes that frozen scope rather than scanning again;
the bootstrap retains the record until the child exits. Ambient scope records are
not adopted. When native setup is needed, the portal invokes the Visual Studio
environment batch script separately, without application arguments, and captures
its environment through a private in-memory channel. Argument forwarding does
not use `CALL`. Parsing keeps the caller's working directory, while command
execution uses the repository root; UNC roots retain a `pushd` mapping until the
foreground portal exits. This does not replace build-tree or provisioning locks.

`script\windows\start-msbuild-env.bat <command> [args...]` runs another tool
from the repository root with an initialized x64 Visual Studio environment.
It preserves an explicit `VCPKG_ROOT` and otherwise selects Visual Studio's
bundled vcpkg. Use a local or mapped-drive repository path, not a UNC root.
Arguments are forwarded only until the first empty argument. Pass `cmd` as the
command for an interactive prompt; omitting the command prints usage and exits
without initializing Visual Studio.

`ao.bat check --extend` uses the ready native environment to run the full
[completion gate](bazel.md#completion-gate). Ordinary native Bazel builds and
runs do not repeat its repository-wide source scans.

### Dependency linkage

The governed target and host triplets are the standard `x64-windows` vcpkg
triplet: target dependencies and host tools resolve from the same reviewed
SDK with normal dynamic linkage. Recipe-backed static-only ports remain
static; this introduces no first-party DLL ABI. The CRT remains `/MDd` in
Debug, `/MD` in Release and fastbuild, supplied by the native toolchain. Windows system libraries, the
Windows App SDK/runtime and build-tool executables remain separate boundaries;
this is a dynamic development SDK, not a fully static executable.

Let `ao.bat setup deps` install the reviewed SDK closure into the
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

FLAC consumes Ogg from the SDK as a normal dynamic dependency. The CLI and TUI
use `mimalloc-new-delete.h` to define executable-global `operator new` and
`operator delete` overloads that forward to mimalloc. This does not intercept
CRT `malloc`/`free` calls broadly or redirect arbitrary cross-DLL allocations.
Allocation ownership must still match the allocating interface.

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

For managed project commands, without an explicit selection each configuration
gets its own default tree
named `bazel-<config>` under that checkout-isolated build root. An explicitly
selected tree (`-p`/`BUILD_DIR`) is mode-neutral: the `debug`/`release`/`asan`
profile is selected per invocation, not by the tree.

Overrides apply in this order:

1. command-line `-p <dir>` or `BUILD_DIR` selects one exact tree;
2. `AOBUS_BUILD_ROOT` replaces only the build base;
3. `AOBUS_STATE_ROOT` replaces the complete managed-state base;
4. otherwise `%LOCALAPPDATA%\Aobus` is used.

These overrides do not relocate the [Bazel action cache](bazel.md#shared-local-action-cache).
To share a different directory, set `$env:AOBUS_BAZEL_DISK_CACHE` to its
absolute local path for `ao.bat` and pass `"--disk_cache=$env:AOBUS_BAZEL_DISK_CACHE"`
to bare Bazel from the same PowerShell session. Omit both for the shared default.

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
needs a supported [metadata binding](build.md#shared-source-git-discovery).
Neither setting copies metadata, synchronizes source, or prepares another
root's tooling. Follow the [locked bootstrap](#prerequisites) for each checkout;
virtual environments contain checkout-specific paths and are not relocatable.

The checkout key's tolerance of mapped-drive, UNC and junction aliases is an
identity feature, not permission to build from a network root. Run the selected
local checkout's `ao.bat` from that root.

### Bind Git metadata on Windows

Follow [shared-source Git discovery](build.md#shared-source-git-discovery) to
verify the metadata. For the work tree, use Python's `PROJECT_ROOT`, derived
from the imported tooling module's path, not the current directory after
`ao.bat`'s `pushd`. The batch file sets
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
    .\ao.bat check format
} finally {
    $env:AOBUS_GIT_DIR = $previousGitDir
    $env:AOBUS_GIT_WORK_TREE = $previousWorkTree
}
```

## Build and run WinUI

Check prerequisites and install the governed runtime explicitly when needed:

```bat
ao.bat check winui
ao.bat check winui --build-only
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
bazel build //app/windows-winui:aobus-winui
bazel build --config=release //app/windows-winui:aobus-winui
bazel run //app/windows-winui:aobus-winui
bazel run --config=release //app/windows-winui:aobus-winui
```

The current app is unpackaged and framework-dependent; Developer Mode is not
required. Launch requires an interactive RDP/local desktop. An SSH service
session can build and verify but cannot display WinUI.

For offline deployment without SDKs, Python, or network access, see
[staging prerequisites](#stage-offline-prerequisites).

`bazel build //app/windows-winui:aobus-winui` stages only the shipping application, without
building or executing probes. `ao.bat check` builds the application and every
native probe, stages the probes'
XBF/PRI resources, then executes the track-item and localization console checks.
A default check does not launch GUI probes; add `--desktop-tests` for the
[UI Automation naming gate](#ui-automation-naming-gate). The configured graph
owns the boundary that neither the shipping nor default build transitively
depends on probes, and the completion selection includes the application and
every configured probe.

`ao.bat check --extend --config release` validates the managed Release IPO graph — MSVC `/GL` and
`/LTCG`, with `/GL-` only for the authored `app/windows-winui/MainWindow.xaml.cpp`
translation unit when the selected MSVC toolset is `14.51.*` — and the same WinUI
validation in Release.

The [shared worker baseline](build.md#default-parallelism) leaves one available
logical processor free locally for project commands; `--jobs` overrides it.
Native Bazel uses its own scheduling defaults and `--jobs=N`.

All first-party Windows executables verify their UTF-8 process manifest after
link. Narrow `argv` and audited narrow process boundaries are UTF-8, but
filesystem paths still require the project's explicit native/UTF-8 conversion
facades.

### Bazel product details

The [WinUI target](bazel.md#winui-product) stages the real
product as `//app/windows-winui:aobus-winui`. `ao.bat setup deps` installs
the tree's vcpkg manifest and restores the locked NuGet closure into the managed
cache; `packages.config`, the dependency
policy and the Windows SDK selection are the version authorities, and builds
themselves neither restore nor install packages. The build also
exposes explicit native probe targets and lint; development build acceptance
supplies no signing or installer workflow. The [WinUI product](bazel.md#winui-product)
owns compilation and executable-local staging. A successful build or bounded
startup does not replace the interactive GUI gates below or establish complete
WinUI GUI acceptance.

WinUI code that only needs plain resource lookup includes `StringResources.h`.
Formatting, MRT language configuration, and `winrt::hstring` lookup use
`StringResourceFormat.h`, `ResourceLanguage.h`, and `WinRtStringResources.h`,
respectively. Handwritten WinUI components and probes have no forced include
or shared `pch.h`. Include the C++/WinRT projection that **defines** a method
(e.g. `Controls.Primitives` for `ButtonBase::Click`), not just the namespace
of the derived control. Keep implementation-only dependencies out of public
headers. Where Win32 and XAML meet, resolve conflicting SDK macros at that
boundary rather than relying on a global include order.

The frontend uses classic COM for SMTC. Every translation unit linked with it
must see the real `<unknwn.h>` before its first C++/WinRT projection, including
before a generated `.g.h` that reads `winrt/base_macros.h` directly. Otherwise
the pinned projection defines different types and templates in the same program;
link success does not detect this. Keep this ordering at the owning include
boundary (and in the generated module wrapper), not in a forced prefix. When
changing the projection, check the first parse in the linked objects, not just
the macros still defined at the end of a source file.

MSBuild generates from `Project.idl`, `App.xaml`, `MainWindow.xaml`, and the
paired `.xaml.h` headers next to the private project. The paired `.xaml.cpp`
items identify the code-behind relationships without supplying files; Bazel
compiles the real sources. Add ordinary C++ members to their native Bazel owners,
not to the SDK project.

The four generated translation units compile the SDK's emitted C++ directly.
Authored implementations still include their own generated fragments; do not
compile those fragments as separate objects. If a page starts activating an
in-project control in XAML, check that the generated types unit can see the
control's **implementation** declaration, not just its WinRT projection. For
example, a native probe adding `local:AobusSoulControl` to `MainWindow.xaml`
required `#include "playback/AobusSoulControl.h"` alongside the local class
includes in the `XamlTypeInfo.g.cpp` translation unit, after its SDK preamble.
Supply the specific include at the types-unit integration point if needed;
adding an include search path or listing all native headers in MSBuild did not
supply that declaration. Do not edit generated files by hand.

### Stage offline prerequisites

For offline deployment without target-side SDKs, Python, or network access, use
the [operator instructions](../../app/windows-winui/deployment/README.md). This
helper prepares the machine CRT and application user's runtime registration; it
is not a complete installer.

From the repository root on the build host, stage the matching signed x64 VC
redistributable, the consumed
`VC\Tools\MSVC` directory version (not the `cl.exe` file version), and the
Windows App Runtime installer governed by
[dependency-contract.json](../../config/dependency-contract.json):

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

### Run manual WinUI probes

Use the [native Bazel setup](#native-bazel-and-project-commands) and a logged-in
local or RDP desktop with the [Windows App Runtime](#build-and-run-winui)
registered for that user. `ao.bat check` stages the manual probes below but
does not launch them. Their `bazel run` labels build and launch the staged
executables with executable-local resources and DLLs.

To run a staged probe directly, append `app\windows-winui\<probe>\<probe>.exe`
to the path from `bazel info bazel-bin`. Use the same output base, config,
and compilation mode as the build. For a portal-built probe, use the matching
prepared tree's rc (set `$tree` to the build tree in PowerShell):

```powershell
bazel --bazelrc="$tree\configuration\aobus.bazelrc" info bazel-bin
```

The rc selects the output base; matching just the config does not find a portal
build in a bare Bazel tree. See [trees and direct Bazel](bazel.md#trees-and-direct-bazel).
For a bare Bazel build, use bare `bazel info bazel-bin` with the same build
options. Keep the PRI, XBFs, assets, and DLLs alongside the executable and capture
its exit status and output. A dispatcher deadline is not an external process
watchdog.

The shipping executable has no probe switches or dependency on probe application
metadata. Close the application and probes before another build or lint refresh
can relink them.

### Soul lifecycle probe

```bat
bazel run //app/windows-winui:ao_winui_soul_probe
```

The native Soul control must settle both pause coast and aura fade on `Unloaded`
without replaying either on `Loaded`. Expect exit code 0 after both checks;
the dispatcher timeout is 20 seconds. No library, audio endpoint, or saved
settings are needed.

### Quality tooltip property probe

```bat
bazel run //app/windows-winui:ao_winui_quality_tooltip_probe -- "C:\scratch\quality-state" "C:\fixtures\track.flac"
```

Pass `STATE_ROOT` then `AUDIO_FIXTURE`, both absolute and canonically outside
the checkout used to build the probe. Build from the native-local checkout and
make sure the desktop user can access it: admission refuses an inaccessible
compiled checkout rather than skipping the check. The state path must end in
an unambiguous directory name, be absent (not even a symlink), and have an
existing directory as its parent, outside the fixture's directory tree.
The probe compares resolved ancestor identities, not spellings, including DOS
volume roots and real UNC shares but not virtual server roots. Canonical paths
must use DOS-drive or ordinary UNC-share syntax; extended input is allowed only
if it resolves to one of those forms.

Use an existing file with the lowercase `.flac` extension and keep the fixture
and paths stable. The decoder opens it read-only without requiring or changing
a read-only attribute. The probe neither copies media nor writes in the fixture's
directory; all database and workspace writes stay under `STATE_ROOT`. Keep that
directory as evidence; the probe does not clean it up.

Expect exit code 0 when the shipping adapter's tooltip text, HelpText, and
availability match shared Now Playing before and after the seeded track's real
decoder publication and across owner-window resize. The probe also checks that
the tooltip attachment and HelpText clear on teardown. The dispatcher deadline
is 90 seconds. The discard backend has no Sink or audio endpoint:
Unknown channel confidence does not mean the decoder failed to identify the
source layout. This is not evidence for opened-tooltip wrapping, clipping or
pixels, or physical speaker routing.

To check input rejection without a desktop, invoke the staged `.exe` directly
with invalid arguments. Require exit code 2, the diagnostic for the intended
rejection, and no fresh state directory; admission runs before WinRT/XAML.
Test one invalid condition at a time: a relative path, reused state, a missing
parent, a state path inside the build checkout (also through an alias), a
state path under the fixture's directory, or an ambiguous state name. For
this Windows-subsystem executable, wait for the process and redirect stderr
explicitly in PowerShell (set `$probe` to the staged executable's absolute path
and `$stderrFile` to an absolute scratch log path):

```powershell
$p = Start-Process -FilePath $probe -ArgumentList @('"C:\checkout\rejected-state"', '"C:\fixtures\track.flac"') -Wait -PassThru -RedirectStandardError $stderrFile
$p.ExitCode
Get-Content $stderrFile
```

Use the actual build checkout and a valid external fixture. For the example,
verify the `Both paths must be canonically outside the checkout` diagnostic
and that `rejected-state` was not created. An inaccessible compiled checkout
is not evidence that the outside-checkout or fixture-overlap check ran.
Accepted inputs still need the [interactive desktop](#run-manual-winui-probes).

### Track and navigation probes

The unattended `ao_winui_track_item_probe` checks the generated `TrackRowItem`
projection, lazy item-view behavior, duplicate-inclusive compact Credits counts,
and the projected text used for row automation names. A failing run fails
completion. To run it alone, no desktop or audio endpoint is needed:

```bat
bazel test //test:winui_track_item_probe_test
```

For the [manual desktop probes](#run-manual-winui-probes), use:

```bat
bazel run //app/windows-winui:ao_winui_track_table_probe
bazel run //app/windows-winui:ao_winui_track_table_probe -- --row-identity
bazel run //app/windows-winui:ao_winui_track_table_probe -- --fragmented-selection 1000
bazel run //app/windows-winui:ao_winui_activity_status_probe
bazel run //app/windows-winui:ao_winui_credits_probe
```

They use disposable runtime state, not personal settings or an audio endpoint.
Expect exit code 0; the probes fail at their dispatcher deadlines if the
required observations do not complete.

| Probe           | Dispatcher deadline | Contract                                                                                                                                                                                |
| --------------- | ------------------- | --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| Track table     | 90 s                | Stable-ID selection through filtering, scroll anchors and reveal priority, presentation history replay, themed transport, and navigation clearance at 720 and 1120 effective pixels.    |
| Activity status | 30 s                | Compiled `MainWindow` styles, theme updates, accessible names, keyboard and automation admission, and detail lifecycle.                                                                 |
| Credits         | 90 s                | Scoped native editing, automation and callback admission, plus single-track Properties staging without writes, composed Save and revision publication, discard, and stale-draft reload. |

In the activity strip, empty idle and ordinary Info expose no enabled action,
flyout, or tooltip; warnings and retained idle details stay keyboard-admissible
and automation-openable.

The table probe checks localized resize names from the compiled shipping header
template before and after sort and width changes, but the donor has no library
session: header click, drag, and context-menu routing are not tested. Column
changes use the controller API and the body uses representative row templates.
The separate `--row-identity` run reads 2,051 tracks from a disposable library. It checks that an unheld native row is evicted while a row held outside the strong cache still has the same COM identity on its live source.
Use `--fragmented-selection N` (1–10,000) to measure restoring alternating offscreen rows on a loaded 50,000-row list. Start with 1,000 before increasing the count: the restore runs synchronously on the UI thread. The probe reports elapsed restore time and row-provider calls, then checks the exact selected ranges. The time does not include subsequent layout or painting and is not a pass/fail threshold.

The column tests save and reload an Album visibility change on a fresh layout,
then switch the same List to Library and verify its authored header order. They
also check that changing visibility preserves saved positions and widths.
The File Path tests check both the reader's path demand for a titled track and
the projected cell through show/hide transitions.
The filter cases check that header-only gestures preserve partly or fully hidden
runtime selection without a new publication and that clearing the filter restores
the native rows. The table probe also queries the list's Selection provider after a sort and for an unrealized selected row; the query must leave the scroll offset unchanged and the row unrealized. History replay checks the presentation label before the table
controller adopts the replayed view. Transport checks observe a retained icon
across window-local theme and accent changes, programmatic button states, and
loss of runtime readiness; a silent provider enables those checks without
playing media.

The Credits probe checks native event delivery and reads committed values and
revisions back from the database after a composed Save. In batch Properties,
withdrawing a field edit must preserve each original value, even when other
edits are saved. The probe hosts the coordinator, not `MainWindow`'s modal
root. These programmatic observations do not certify physical input,
shipping-template paint, live shipping theme changes, or audible playback;
inspect the shipping window separately for those claims.

### UI Automation naming gate

Run the Windows-only automation probe explicitly after preparing its native
runtime and interactive session:

```bat
bazel run //app/windows-winui:ao_winui_automation_probe
```

This builds the probe and its resources before launching it. Use
`ao.bat check --desktop-tests` to run `//test:winui_uia_test` and
`//test:winui_uia_selection_test` inside completion. Without the desktop option,
`ao.bat check` builds the probe without running either test, and `//:tests`
does not select them.

Both run paths require the Windows App Runtime and an authenticated interactive
console. Outside the active console session, the probe fails with a diagnostic
rather than skipping. The gate does not create a session.

The default English run loads `MainWindow` with a disposable library and
private settings, without playing audio. It rescans a selected track to
check the summary in the live Modern shell and after retirement. It exercises both shells,
flat and grouped track rows, a narrowed Classic window, and the volume and
overflow popups. It also checks the live count and filter validity in New List.
The Modern track-list check queries the mounted peer's Selection pattern in process. For the OS event and query paths, run `bazel test --//bazel:desktop_tests=true //test:winui_uia_selection_test`. A separate UI Automation client subscribes to the loaded list, drives SelectionItem operations, and checks event types, senders, and the resulting selection. The fixture also makes one bulk range selection while the client is subscribed. A change to `IsSelected` is not a substitute for a SelectionItem event. To check provider identity across reordering in a shipping build, select two nonadjacent tracks, reverse their order, and verify the returned elements and their `SelectionContainer` before clearing.

In Modern, a committed List upsert rebuilds the native `NavigationView` while a
filtered view is active. The probe checks that the replacement item is selected
and that the exact workspace view, filter, and presentation survive. It does not
force or establish delayed `SelectionChanged` delivery.

In Classic, the probe clears and restores native tree selection to check that the
Rescan result survives, then selects a different List to check that navigation is
reported. It invokes **Move to Bottom** through the mounted shell's action
registry in a filtered Manual Order view. Once the committed List upsert has
replaced the selected tree node and the filtered row is selected, the probe
checks the saved order (including the hidden track), the exact view and
selection, and the localized Move result in the status line on a subsequent
render and dispatcher turn.
The probe sets up the filter through the workspace; it does not type the
filter, press a physical shortcut, or open the track context menu.

The audit requires a nonblank final `AutomationPeer.Name` on interactive and
item peers, including disabled controls. An ID or attached property alone is
not evidence of a name. The Soul and seek controls, title header and resize
grip, two named track rows, and grouped album heading must appear as on-screen
peers with the right roles and component scopes. Wide layouts must also expose
both pane resize grips and Classic's All Tracks item. The probe adjusts only
its own window's tracking ceiling and checks the effective client width after
DPI scaling; it does not change the desktop resolution. Intentionally hidden
narrow panes, decorative (`Raw`), text and layout roles, and `ScrollBar`
template internals are outside the audit.

To inspect the localized New List count, run:

```bat
bazel run //app/windows-winui:ao_winui_automation_probe -- --list-preview zh-Hans
```

Use `zh-Hant` for Traditional Chinese. This selects the probe's locale, not
Windows'. After `LIST_PREVIEW_READY`, inspect the count of two source tracks
under the saved parent `Bach` with a blank local filter, then choose Cancel.
This mode does not run the English naming audit.

Name presence is not semantic correctness: the probe does not certify dialogs
it never opens, rows it never materializes, names in other locales, or real
assistive technology such as Narrator. The [frontend accessibility contract](../system/frontend/windows.md#frontend-observations)
explains how controls receive names. The track-table and activity-status
probes are separate manual desktop checks.

### Album Columns restart probe

Use the [staged artifacts](#run-manual-winui-probes) for `aobus-winui` and
`ao_winui_automation_probe`, built from the same checkout and configuration.
Run this out-of-process UI Automation check on an English Windows desktop:

```powershell
powershell.exe -NoProfile -Mta -File .\test\winui\probe\WinUiAlbumColumns.ps1 -AppExe $app -SeedExe $probe
```

Set `$app` and `$probe` to the absolute staged executable paths. This check
starts with All Tracks in Albums, uses the actual **More → Columns...** menu,
checks **Album**, and verifies that the group heading and row cell are both
present. It visits Bach and returns to All Tracks, then uses normal app
restarts to verify the shown and subsequently hidden column states. The last
visible column must stay checked when the probe attempts to hide it. It then
uses a presentation switch to leave Albums with no visible columns and restarts
from that state. The probe checks that the message and opening its **Columns...**
button leave the saved layout unchanged, that the toggles are unchecked, and
that checking **Title** dismisses the message without removing the group or
track. This is UI Automation and persistence evidence, not physical pointer
input.

The driver uses `--seed-album-columns`, a headless mode of the automation
probe, to create a new temporary directory and print its absolute path in
UTF-8. The synthetic library is its `library` subdirectory. The driver sets
`LOCALAPPDATA` and `APPDATA` only in the shipping child environment. It reports
the fixture root and keeps it as evidence; remove only that run's reported root
if desired and only after all processes close. The seeder never accepts an
output path, refuses collisions, and removes its own partial fixture on a
handled failure. Never point the app at a personal library for this check.

### Output selection probe

Run the options below separately from the naming gate. Use a private, idle
active console and run as the signed-in test user without elevation. For a
scheduled launch, use an interactive token with run level `Limited`.

The default automation run restores a saved request for a missing WASAPI
endpoint. The Modern output button's tooltip explains the unavailable request;
the flyout shows a disabled advisory, leaves alternate devices unchecked, and
accepts an explicit choice of the discard-only endpoint. The probe refuses the
choice if a platform endpoint shares the discard device's name. To check the
same choice from Classic's Soul button, run without a media fixture:

```bat
bazel run //app/windows-winui:ao_winui_automation_probe -- --classic-output
```

Classic activates the frame's playback adapters, including SMTC, only while
idle with no playback subject and the missing request still pending. After
confirming the ready discard route by backend, device, and profile, the probe
reads the window's SMTC Play flag on selection, clearing, and reselection.
It checks that the disabled controls' Play flag stays unchanged on a selection
change after retirement. This is property readback, not OS media-button
delivery or a test of callbacks queued at retirement.

The default run does not activate those adapters; it writes a silent WAV for
Rescan. Neither mode issues Play or changes the Windows default device.

### Primary Play probe

To exercise Modern's primary Play button with a decoder, supply a private PCM
WAV of at least one second and at most 16 MiB. Use an absolute path to a
regular file, not a symlink, with the lowercase `.wav` extension:

```bat
bazel run //app/windows-winui:ao_winui_automation_probe -- --primary-play "C:\fixtures\private.wav"
```

The probe copies the fixture into its disposable library and requires a ready
discard-only route before invoking Play. A pass distinguishes starting a
selected row from resuming the same paused track at its saved position while
another List and row are selected. The probe also calls the production SMTC
Play handler directly: with no target the subject stays empty and notification
entries stay unchanged; after Stop, Play starts a new occurrence of the visible
selected row. This does not test OS button delivery, WASAPI rendering,
audibility, or physical input.

### Seek probes

Supply a private PCM WAV with a decoded duration longer than 11 seconds. Use
an absolute path to a regular file, not a symlink, with the lowercase `.wav`
extension and a size of at most 16 MiB. Run each command separately:

```bat
bazel run //app/windows-winui:ao_winui_automation_probe -- --seek-steps "C:\fixtures\private.wav"
bazel run //app/windows-winui:ao_winui_automation_probe -- --seek-range classic "C:\fixtures\private.wav"
bazel run //app/windows-winui:ao_winui_automation_probe -- --seek-range modern "C:\fixtures\private.wav"
```

Expect exit code 0. The probes require the ready discard-only route.
`--seek-steps` posts `WM_KEYDOWN` and `WM_KEYUP` for two Right arrows and one
Left arrow to its own focused Modern input window, not system-wide. It checks
1 → 11 → 6 seconds for the same paused occurrence.

The steps probe also checks the disabled slider and two `00:00` clocks before
playback. Restoring the saved six-second position for the track without a
catalog duration must yield a new deferred occurrence, elapsed `0:06`, total
`--:--`, and a slider that is disabled and refuses keyboard focus.

`--seek-range` reads the shipping slider's UI Automation `RangeValue` bounds
(zero to decoded duration, in milliseconds) and current position. Paired
`SetValue` calls coalesce to one committed seek at each of the minimum,
maximum, and a fresh interior target, updating the thumb and elapsed clock
without changing the paused occurrence. Neither path exercises physical drag,
pointer capture, or replacement or replay during a held gesture.

## Native tests and sanitizers

The ordinary `bazel test //:tests` selection covers Core, application, TUI,
and CLI contracts, including the native process tests. The per-test wrapper
for context-bearing C++ tests launches the wrapped test from runfiles without
starting private fixtures on Windows. The separate native process tests resolve
their executables from runfiles. The two console probes use their staged SDK
DLLs and do not need the caller's `PATH`. The UIA test inherits `PATH` for
PowerShell to check the current user's Windows App Runtime; it also requires
the interactive console before launching the probe.

Workflow lint and the compiler-driver diagnostic run only on Linux. The native
backend suite carries `manual`, but completion runs it through `//:check_tests`.
Both Windows cases are hidden `[.manual]`. When neither is selected, Bazel can
report the target `PASSED`; `ao.bat check` and the [CI summary](ci.md#validation-evidence)
report the hardware cases as **not run**, based on Catch2's XML. Choose the exact case
whose prerequisites you prepared; `[.manual]` selects both probes, not just the
provider example below. Concurrency filters should exclude hidden probes.

`bazel test //test/lint:lint_test --nocache_test_results` runs fresh result
assertions for fixtures produced with the governed LLVM SDK and Visual Studio
includes; the action cache can still supply Clang fixture outputs.
Compiler errors in those diagnostic invocations fail the fixtures; there is no second syntax-only pass.
Objective-C++ fixtures are explicitly deferred, not counted as Windows coverage.
Use `ao.bat check lint` for scoped source findings and `ao.bat check format` for
formatting. The portal uses its managed environment for scoped Python checks;
Bazel tests use the governed base interpreter and the
[separate repository tool inputs](dependency-governance.md#python-wheel-sources-and-fetches).

Run the affected native tests with `bazel test --config=asan` from the selected
VS/SDK environment. Use `//:asan_tests` for the broader instrumented suite.
Neither route runs normal completion or lint.
Third-party vcpkg libraries are not instrumented, so cross-boundary STL
container annotations are disabled. MSVC provides neither UBSan nor TSan, and
resumable coroutine instrumentation is incomplete; Linux ASan/UBSan and TSan
remain complementary. Bazel rejects `--config=tsan` on Windows. The portal also rejects an application `--compiler clang` selection before SDK work.

Source inspection and portal tests do not certify native builds, GUI behavior,
audio, or sanitizer results. Run the opt-in WASAPI endpoint and playback probe
on a host with an active render endpoint:

```bat
bazel test //test:backend_test --test_filter="WasapiProvider - enumerates and renders through real endpoints" --test_output=all
```

The exact case name selects the provider probe only; the hidden WASAPI
channel-map readback probe has different prerequisites and stays separate.
A mistyped filter can match nothing, and the provider probe skips without an
active endpoint. In either case Bazel can report `PASSED`. Check the Catch2
output or `test.xml` for the named case running without a skip. It checks
enumeration, frame advancement, and drain completion, not whether a listener
heard sound.
Use an interactive audio session and listen separately when audibility is part
of acceptance. Record the checks performed and remaining limits with the change.

## LLVM SDK and native lint tools

Windows formatting and C++ lint use the official LLVM development archive pinned by
[`script/ao/windows-llvm-sdk.json`](../../script/ao/windows-llvm-sdk.json);
explicit provisioning installs it into the shared
verified cache used by the [scoped lint
check](bazel.md#scoped-lint-and-tool-selection): `ao.bat setup deps` and
`ao.bat setup llvm-sdk` call the same native installer. Ordinary ready-only
build, lint and format commands never install it. The portal reads the manifest
lazily when Windows SDK code needs it, not while registering commands.
`check lint` checks for a ready LLVM SDK only when its resolved scope contains
C++ files; Python-only and empty scopes do not require it. Managed Python tooling
is prepared only for Python files; see the [tool-selection contract](bazel.md#scoped-lint-and-tool-selection).
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
and never falls back to Visual Studio or `PATH`. In LLVM 22.1.8, `Diagnostic.h`
returns a `unique_ptr<DiagnosticConsumer>` before the consumer type is complete;
MSVC 14.52 rejects the inline `takeClient()` definition. The Bazel SDK projection
moves only that definition below `DiagnosticConsumer`, without changing the
installed header, the LLVM libraries, or the checker flags. Retire the patch
when the unmodified pinned header builds with the supported MSVC toolset.

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

Validate a build or scoped lint
run from new state before manually removing old files.

## Troubleshooting

- **LLVM SDK provisioning failure**: `llvm-<version>.lock` is a coordination
  file used by `ao.bat setup deps`,
  not an existence-based stale-lock marker. If lock acquisition times out, check
  whether another provisioning process is still installing the SDK. Do not delete lock or cache files while
  a writer may be active. After interruption, wait for the portal's installer
  to settle before starting another writer, then rerun
  `ao.bat setup deps` as appropriate: it validates the SDK and completion
  marker and re-extracts an invalid automatic cache. For offline
  provisioning, use the complete extracted SDK and `AOBUS_LLVM_SDK_ROOT`
  procedure under [offline SDK setup](#offline-sdk-setup).
- **`vswhere` or toolset discovery failure**: install or repair Visual Studio
  Build Tools with the required `.vsconfig`, including
  `Microsoft.VisualStudio.Component.VC.Tools.x86.x64`. Both `ao.bat` and
  `script\windows\start-msbuild-env.bat` use the same discovery script, so
  opening the latter cannot repair missing discovery tools or components. Use
  `script\windows\start-msbuild-env.bat cmd` for a separate initialized x64
  prompt after discovery succeeds.
- **Windows Store Python interception**: running python commands directly outside
  `ao.bat` may trigger the Windows Store app execution alias. Disable the
  "App execution aliases" for `python.exe` and `python3.exe` in Windows Settings
  or use the governed interpreter prepared by `ao.bat`.
