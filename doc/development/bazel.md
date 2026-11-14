---
id: development.bazel
---
# Bazel build system

Bazel owns the complete native product build: Core, CLI, TUI, Linux GTK,
incremental macOS AppKit and the bounded native Windows WinUI product. The
portal commands — `./ao build`, `./ao test`, `./ao check`, `./ao run`,
`./ao tidy`, `./ao hygiene` and `./ao coverage` — are Bazel-backed and are
the sole entrypoints; there is no second `ao bazel` command tree or build
backend. Raw Bazel queries, fetches and extra flags use
[direct Bazel](#direct-bazel-without-the-portal). Nix/vcpkg remain the dependency version and
installation authorities; Bazel consumes their artifacts without configuring
an Aobus CMake graph or parsing installed CMake exports — vcpkg package
builds may still use CMake internally, and the repository retains the
CMake-language provider assets those ports need. BCR supplies build rules, not
application dependencies.

The cacheable native C++ lint reports come from the Bazel lint report actions;
[lint reports](#lint-reports) defines their workflow, cache and failure contracts,
and the `./ao tidy`/`./ao hygiene` commands consume those actions with the same
STRICT/RELAXED policy. Explicit scoped sanitizer and
coverage instrumentation routes exist beside the ordinary Debug/Release
modes; [sanitizer and coverage
instrumentation](#sanitizer-and-coverage-instrumentation) owns their commands,
profile design and boundaries. Linux CI runs their analysis-only smoke, not
instrumentation runtime acceptance.

## Setup and commands

Run from the repository root through `./ao`; Windows uses `ao.bat`.
Linux uses Bazel from the pinned Nix shell. On native macOS/Windows,
`./ao setup bazel` verifies or downloads the official pinned executable into
managed state with SHA-256 verification. `AOBUS_BAZEL` overrides the executable for diagnostics,
but still requires the pinned version.
The pins are owned by [bazel-toolchain.json](../../script/ao/bazel-toolchain.json),
[.bazelversion](../../.bazelversion) and [MODULE.bazel](../../MODULE.bazel).

```bash
./ao setup bazel
./ao deps provision --path /tmp/build/aobus-bazel-debug  # native macOS/Windows only
./ao test --path /tmp/build/aobus-bazel-debug
./ao check release --path /tmp/build/aobus-bazel-release
./ao verify --config release --path /tmp/build/aobus-bazel-release --jobs 2
```

On native macOS and Windows, `./ao deps provision` installs the shared vcpkg manifest
into the selected tree's `vcpkg_installed`
with the pinned vcpkg and the governed triplets, overlays and binary cache,
without a CMake configure, so unchanged package ABIs restore from that cache.
On Windows, `ao.bat setup llvm-sdk` is the second explicit provisioning
spelling: it calls the same native LLVM SDK installer as `./ao deps provision`. Ordinary
ready-only build, lint and format commands never install an SDK.
Windows target libraries
use the standard `x64-windows` triplet: reviewed dynamic portable
dependencies with the dynamic CRT. The same triplet supplies the host tools.
Windows `./ao deps provision` also
restores the locked [WinUI NuGet closure](#winui-product) and installs the
pinned [LLVM SDK](windows.md#llvm-sdk-and-native-lint-tools) for
[lint](#lint-reports) into the verified shared cache, under the
same lock and completion marker. Linux needs no
provisioning. Other actions use the tree's provisioned triplet; a ready
installed SDK is consumed, never installed implicitly, and
`--vcpkg-prefix <installed/triplet>` replaces the provisioned prefix with an
existing installed target-triplet prefix. Rerun `./ao deps provision` after changing the vcpkg
manifest, configuration or overlays: Bazel itself never restores missing ports
or substitutes an ambient installation. An interrupted `./ao deps provision` waits for
the running installer process to exit before propagating the interruption and
releasing tree or shared-cache locks. Cancellation can wait for the tool's natural
completion; it does not contain detached descendants a tool leaves behind. The
[process runner](../../script/ao/core/proc.py) uses owned-child POSIX status polling
to avoid an interrupt-sensitive Python wait lock; an unavailable exit status is
an error, never a successful installation.
Follow [macOS setup](macos.md) or [Windows setup](windows.md) for native SDK
prerequisites and the [native source and storage requirements](#native-hosts-and-caching).
Windows Bazel commands use governed base Python without probing or provisioning
Ruff, mypy or documentation packages; explicit tooling/hygiene/docs commands keep
their managed checks.

The portal commands are the complete surface over this graph: `./ao
build`, `./ao test`, `./ao check`, `./ao run`, `./ao tidy`, `./ao hygiene` and
`./ao coverage` own the ordinary lifecycle. Where a command takes a flavor,
the positional `debug|release|profile` selector picks it (`./ao check` keeps
`debug|release`); `--asan`/`--tsan` request the governed sanitizer
configurations beside the Debug flavor.
`./ao build` additionally accepts repeatable `--target LABEL` selectors
(including the public product names) and a scoped `--event-log PATH` that
retains the build's BEP artifact; the build owns `--lockfile_mode=error` for
reproducible CI requests, not arbitrary forwarded arguments. `./ao run`'s
positional application selector also accepts an explicit first-party
executable label
(`./ao run //app/windows-winui:ao_winui_automation_probe -- ARGS`); canonical
application names and labels retain their application-only checks, and `run`
inherits the
caller's standard input, output and error, preserving terminal detection for
interactive frontends, without overwriting an existing build log.
`./ao setup bazel` is the pinned-executable setup, `./ao deps provision` the
explicit vcpkg/NuGet/LLVM provisioner, and `./ao verify` the
[compile-contract analysis](#compile-contract-verification) gate. Common
options include `--path`, `--vcpkg-prefix`, `--jobs` and `--remote-cache`;
availability depends on the command. `AOBUS_BAZEL_REMOTE_CACHE`, when set, is
`--remote-cache`'s default. Raw Bazel `query`/`cquery`/`aquery`/`fetch`
invocations and extra Bazel flags are not portal options: use
[direct Bazel without the portal](#direct-bazel-without-the-portal).

`./ao test` passes a Catch2 filter as its last argument. Catch2 receives the
filter as a test spec through Bazel's `TESTBRIDGE_TEST_ONLY`; its Bazel support
reads that variable because the Linux/macOS test setup script exports
`BAZEL_TEST=1`. A selected test binary with no matching case fails, so filter
one owning suite rather than an aggregate. Build and clean output goes to the
selected tree's `build.log`, printed once on failure.

### Direct Bazel without the portal

The checkout also accepts bare native Bazel commands run from the repository
root. No pre-entered Nix shell and no generated `aobus.bazelrc` are needed on
Linux: the tracked [repository rc](../../.bazelrc) supplies the default Debug
mode, host toolchain selection and repository-channel clearing, and the
[`aobus_environment` repository](#architecture) prepares the pinned Nix SDK by
entering `nix-shell --pure` itself.

```bash
bazel build --jobs=8 //app/cli:aobus //app/tui:aobus-tui
bazel run //app/tui:aobus-tui -- --help
bazel test --jobs=8 //:tests
bazel build --config=release --jobs=8 //app/cli:aobus //app/tui:aobus-tui
```

Bazel 9.2 ([`.bazelversion`](../../.bazelversion)) with a JVM, and Nix with
`nix-shell` on `PATH` on Linux, are installed prerequisites; a bare invocation
never auto-installs Bazel or a JDK. The pinned Nix environment's `bazel` wrapper
selects `.bazelversion` and its system rc supplies the distribution JDK, so an
independently installed Bazel or Bazelisk must be pinned to the same version.
Bare commands default to Debug; `--config=release` selects `opt`. The explicit
`asan`/`tsan` instrumentation profiles are also available to bare commands
([sanitizer profiles](#sanitizer-profiles)); they are named rc configurations,
not Bazel compilation modes. Imported SDK
interfaces follow each consumer's actual compilation mode, so optimized
execution tools resolve their Release interfaces beside a Debug target, and
`fastbuild` has no accepted native SDK mapping: analysis fails rather than
falling back.

On native macOS and Windows, bare commands still require the existing explicit
setup and an already provisioned SDK ([macOS setup](macos.md) or
[Windows setup](windows.md), or `./ao deps provision`); repository fetch never
installs Python, SDKs, ports or the WinUI runtime. The managed Bazel executable
is not automatically on `PATH`: use the pinned executable that
`./ao setup bazel` prints, or a Bazelisk pinned to `.bazelversion`; there is no
new installer.
`AOBUS_BAZEL_VCPKG_PREFIX` must explicitly select the installed target-triplet
prefix — `x64-windows` on Windows, whose host tools come from that same
triplet, or `x64-aobus-osx`/`arm64-aobus-osx` on macOS —
including its host tools and both Debug and Release artifacts. An invalid
explicit prefix fails; there is no fallback. Export it or pass it through a
standard local Bazel `--repo_env` configuration; the repository tracks no
absolute-path configuration.

On Windows, bare commands resolve C++ through rules_cc's official default MSVC
auto-configuration ([Windows compiler
configuration](#windows-compiler-configuration)). Initialize the selected x64
Visual Studio environment before starting Bazel, then supply all three standard
selectors: `BAZEL_VC`, `BAZEL_VC_FULL_VERSION` and `BAZEL_WINSDK_FULL_VERSION`.
They must match the initialized VS installation, toolset, SDK and compiler paths.
For example, in an already initialized x64 developer PowerShell:

```powershell
$env:BAZEL_VC = Join-Path $env:VSINSTALLDIR 'VC'
$env:BAZEL_VC_FULL_VERSION = $env:VCToolsVersion.TrimEnd('\', '/')
$env:BAZEL_WINSDK_FULL_VERSION = $env:WindowsSDKVersion.TrimEnd('\', '/')
```

Bare SDK admission rejects missing, partial or conflicting selections instead of
independently choosing the current compiler or latest VS. This deliberately
narrows the former automatic-discovery contract: an initialized developer shell
alone, or selectors without its matching compiler environment, is insufficient.
Upstream auto-configuration still owns compilation; exports made inside a
repository-rule child cannot select another repository's toolchain.

Changing the host installation selection — Visual Studio, Xcode or SDK
selection — may require `bazel fetch --force --configure` before repository
rules observe it; automatic selection is watched host state, not a complete
registry-drift guarantee. A warm output base reuses the fetched environment
instead of re-entering Nix on every invocation, but that is not a performance
guarantee, and private experiment metrics are not production acceptance. A
bare build or test success is not a lint, hygiene or completion gate; the
explicit actions above retain those roles.

## Scope

The graph covers static Core utility, LMDB/library, query, coroutine primitives,
encoded-media readers/containers, complete host-selected Core audio,
CoreRuntime/AppRuntime, UIModel, the real CLI/TUI entrypoints, and the complete
Linux GTK application and existing incremental macOS AppKit product with shared
desktop-launch composition. This does not establish GTK/WinUI feature parity.
The bounded native Windows slice additionally covers the real WinUI desktop
product ([WinUI product](#winui-product)); installation, signing, notarization
and complete release packaging remain outside this build scope. The complete
`//app/runtime:runtime` library directly owns its implementation sources,
including `library/MediaTrack.cpp`. Media fixture tests use the regular runtime
test-header interface and link that actual runtime as `//test:media_fixture_test`.
The separate pure `//test:media_test` binary retains its media-only dependency
boundary; production media has no application-runtime dependency.

The ordinary `//lib/utility:utility` library owns `Base64.cpp` alongside the
other utility implementations. `//lib/utility:path` remains a narrow library for
the catalog execution tool, and `//lib/utility:unicode_text` keeps ICU behind its
facade. `//test:utility_test`, exposed by `//:utility_tests`, links the normal
utility and Unicode-text libraries and reuses the existing algorithm, lifetime,
path/Unicode, YAML and controlled temporary-file cases. Filesystem fixtures use
`//test:common_test_support`; the test does not compile product implementation
sources directly. Existing assertions and the callback-scope concurrency marker
are unchanged.

Utility coverage also includes the existing atomic-file transaction, platform-directory,
compatibility, fatal-contract, fixture-self-test and performance-report cases.
The source-private transaction header has a test-only interface; fatal scenarios
retain their independent sibling probe rather than entering the test main.

The complete `//app/uimodel:uimodel` library owns all UIModel implementation
sources, including playback-position interpolation, the subscribed view model,
time formatting and seek interaction. Runtime and UIModel headers use the regular
header interfaces. `//test:uimodel_test`, exposed by `//:uimodel_tests`, links
that full library and reuses the complete UIModel case membership,
including layout, authoring, presentation, preferences, activity and playback policy.
Runtime fixtures share the complete `//test:runtime_test_support` owner; async and
audio support retain their layer owners. Catalogs use the existing message-catalog
fixture, and decoder/media inputs are declared audio runfiles. On macOS, the UIModel
test child uses `TZ=:/etc/localtime` so its independent C-library expectation reads
the same OS zone as the formatter's [date fallback](macos-portability.md#10-date-time-zone-database),
rather than an isolated test-runner zone.
The private runtime/audio header interfaces and Linux SPA requirements remain
explicit on their compiling consumers.

These cases use controlled output providers and include real decoder input; they
do not establish physical or audible playback. Assertions and case markers are
unchanged. Test-source ownership completeness is not a line-coverage claim.

Build or run the actual frontends explicitly:

```bash
./ao build --target //app/cli:aobus --target //app/tui:aobus-tui
./ao run cli -- --help
./ao run tui -- --help
./ao test cli tui
# Linux only; native GUI execution is explicitly selected.
./ao build --target //app/linux-gtk:aobus-gtk
./ao run gtk
```

The Linux GTK library owns the existing frontend implementations and links the
regular desktop-launch, runtime, UIModel, catalog and Linux MPRIS owners.
The [resource action](../../app/linux-gtk/gresource.bzl) compiles the existing
[GResource XML](../../app/linux-gtk/aobus.gresource.xml), preserving its CSS,
layout, image and license aliases with declared inputs. The generated C bundle
has its own standard library target and retained resource anchor; it is not a
source-tree runtime lookup. GTKmm, Boost.Process and the resource compiler come
from the pinned Nix provider, not a second installer. Cairo FT/FC metadata supplies
the font-backend headers exposed by Cairomm, without ambient Nix include flags.
GTK widget tests are not included by this product target, and a headless window
smoke does not establish Wayland, GPU, physical audio, desktop integration or
release acceptance.

On native macOS, explicitly build or run the existing AppKit product:

```bash
./ao build --target //app/macos-appkit:aobus-appkit
./ao run appkit -- \
  --state-root /tmp/aobus-bazel-appkit-state --library /tmp/aobus-bazel-appkit-music
```

The [AppKit target](../../app/macos-appkit/BUILD.bazel) produces `Aobus.app`,
not a replacement smoke executable. Use separate disposable state and library
paths for development launches; `--library` opens and scans that directory.
Omitting isolation can use personal application state and library data.
The GUI product is outside the default test aggregate.
The shared desktop-launch owner supports Linux and macOS; macOS Boost.Process
and Boost.Filesystem use static artifacts from the existing vcpkg SDK, without a
new installer.

The standard rules_cc `objc_library` owns the mixed C++/Objective-C++
implementation, separate entry unit and smoke sources. Its ARC feature selection
uses the [governed Homebrew toolchain](#macos-toolchain-composition), which retains
mode, warning and SDK policy; no default Apple/Xcode compiler is introduced.
`alwayslink` preserves the complete object-library membership.
The local [bundle rule](../../app/macos-appkit/bundle.bzl) declares the original
Info.plist template, icon, brand license, catalog inventory and minimum-OS
metadata as inputs. Its native Python action retains the catalog SDK's declared
host-absolute interpreter boundary, not a new hermetic Python toolchain.
The native product/release gates and the existing
[AppKit GUI smoke scenarios](macos.md#appkit-smoke-scenarios) are owned by the
same Bazel graph and the native test routes. This bundle adds no installation,
signing or notarization workflow.
Product build/run support alone does not establish GUI or ARM64 acceptance.

### WinUI product

On native Windows, explicitly build or run the real WinUI product:

```bat
ao.bat build --target //app/windows-winui:aobus-winui
ao.bat run winui
```

The [WinUI target](../../app/windows-winui/BUILD.bazel) stages a real
executable-local layout — `Aobus.exe`, `Aobus.pri`, both XBFs, `Assets` and the
SDK's selected runtime DLLs — not a launcher around another build. DLL inputs
come from the linked `cc_binary`'s `runtime_dynamic_libraries` output group plus
the WinAppSDK runtime filegroup. Assembly preserves executable-adjacent MRT,
XBF and `Assets` lookup; propagating runfiles does not replace that layout.
Generated XAML/MIDL output follows the selected `--config`'s Debug/Release mapping.
The shared Windows stream adapter
([`//app/windows:windows_platform`](../../app/windows/BUILD.bazel)) consumes
that same generated projection through its `CcInfo` instead of a second
C++/WinRT projection pin, so the adapter and the frontend share one projection
ABI from the installed SDK. `run`
launches the normal no-arg shipping application, so a direct `run` uses the
ordinary user's application state; the AppKit `--state-root`/`--library`
isolation flags do not exist here, `AOBUS_STATE_ROOT` remains a
[portal state override](windows.md#local-state-and-build-trees), and the
[interactive desktop requirement](windows.md#build-and-run-winui) remains a
runtime prerequisite.

The lazy `@aobus_winui_sdk` repository is evaluated only when this consumer
requests the installed SDK. It reads the NuGet cache under managed state's
`n/<lock-id>/p`, which `ao.bat deps provision` restores
under its lock and completion marker; preparation selects it once complete,
and `AOBUS_NUGET_PACKAGES_DIR` selects another restored cache. The repository
rule consumes the existing
[packages.config](../../app/windows-winui/packages.config) and dependency
policy and never restores, installs, or registers versions. The selected
[Windows source requirements](windows.md#source-checkout-routes), host-local
outputs and portal-selected vcpkg provider modes apply.
Its `sdk.json` carries only the execution metadata consumed by codegen and PRI actions; `sdk-diagnostics.json` retains the
full SDK observation for inspection, without becoming an action input. Both are
projections of the same SDK query, not additional dependency authorities.

The query and action projects share the
[native VC project spine](../../app/windows-winui/msbuild_project.py), while
each caller retains its own properties, targets and forbidden operations. Private
projects suppress Common user/machine wildcard imports, project extensions,
Directory.Build customization and forced C++ imports; declared installed VC SDK
imports remain enabled. A bounded inline task in the selected MSBuild executable
records actual evaluated imports through its object model, not the incomplete
`MSBuildAllProjects` property. This uses the executable's own assembly-binding
configuration rather than a parallel CLR host. Actual loaded host assemblies
may reside in the selected `Bin/amd64` directory or its parent `Bin`; admission
does not assume every assembly is adjacent to the executable. Imported files,
the selected binding configuration and loaded dependencies are observed and
declared as action tool inputs. The query project retains identity evidence;
resolver sources retain identity evidence and repository watches. Neither
becomes an SDK action tool.
Consequently, a .NET patch that changes loaded assemblies can invalidate these
native actions; this is intentional, not a hermetic or image-independent cache
claim. The nested import evaluation
runs no targets; the existing interface query still runs its SDK initializers
and dependency-presence checks. Directory observations supplement successful
imports, but do not establish every missing candidate, empty wildcard or
evaluation-time file read.
Query and action commands disable automatic MSBuild response files.
The actual action helpers, `build_winui.py` and `msbuild_project.py`, remain
declared codegen/PRI inputs; final executable staging does not load the project
builder. Generation tools
also declare the selected VC preprocessor/runtime, VC headers (including
extensionless headers) and SDK UCRT/MIDL inputs, independently of C++ toolchain
inputs.

Native SDK MSBuild owns metadata, MIDL, C++/WinRT, XAML and PRI generation only;
ordinary Bazel MSVC actions compile all C++, create archives and perform the PE
link. SDK actions keep their private `TEMP`/`TMP` at a short path inside the
action's compact `p` tree because full-CLR MSBuild `Exec` appends GUID-named paths
against the legacy Windows path limit; the public headers/src/XBF paths are
unchanged. The selected tree-path geometry remains
a constraint, and no automatic long-path fallback is provided. Generated C++
text has its own consumer tree; XBFs are separate declared outputs. MSBuild
projects, logs and intermediate metadata stay in the private diagnostics output
group, outside the compilation context and PRI inputs. SDK import
libraries remain DLL-interface imports beside the reviewed dynamic vcpkg
dependencies, so this is not a DLL-free executable. The default completion and
GUI-scenario orchestration are owned by the Bazel completion gate and the
explicit desktop suites. Bazel also exposes the native
checker, lint and explicit probe targets, but build/run availability is neither
WinUI functional acceptance nor complete GUI acceptance. This route adds no
signing or install workflow.

CLI retains the complete-runtime/Unicode-only link boundary: no interactive
catalog or ICU i18n library. TUI embeds the full maintained/pseudo catalog and
uses the existing UIModel, FTXUI and platform signal/terminal adapters. Linux
TUI includes the real shared MPRIS adapter; `--system-media=off` is a runtime
choice, not a reduced build. Linux `//test:systemmedia_test` owns the adapter
contracts and real-product PTY/private-bus cases together, with declared product
runfiles and the governed D-Bus daemon. It joins the Linux test aggregate;
`//:tui_system_media_tests` remains a convenience selection. Benchmarks stay
outside ordinary tests. Help/version smokes alone do not prove an interactive
terminal session or audible playback.

The [root BUILD](../../BUILD.bazel) owns the `//:tests` aggregate — the
default test closure that [compile-contract
verification](#compile-contract-verification) expands — and the existing
subsystem convenience suites. Its membership is non-configurable; platform
compatibility belongs to the member executables.
[test/BUILD.bazel](../../test/BUILD.bazel) owns complete case membership at
module boundaries, not migration milestones. An unlabelled `./ao build`
builds the default completion roots `//:check_products`, `//:check_tests` and
`//:check_native_tests` ([`build.py`](../../script/ao/command/build.py)); an
unlabelled `./ao test` runs the native platform profile's registered default
suites ([`builddir.py`](../../script/ao/core/builddir.py)). Runtime keeps its
fatal self-reentering main; catalog and shared desktop contracts have their
own module tests. The portable WinUI model is shared by product and
cross-host tests, while native WinRT cases join that frontend owner only on
Windows.

GTK and native-backend tests remain explicit service/hardware boundaries:

```bash
./ao test gtk
./ao build --target //test:backend_test
./ao build release --target //test:ao_perf_baseline --target //test:ao_tui_bench
```

On Linux the test portal owns one Xvfb display and a private, no-activation
session bus per GTK test process — sharing a display across shards is
unreliable, and one shared bus would make independent MPRIS tests compete for
a canonical name — and sets the complete headless profile on the child
process before GTK selects its accessibility and input-method backends. It
never adopts personal desktop endpoints or the caller's desktop bus.
GTK retains its original main, accessibility/rendering profile and GApplication
probe. Native backend/hardware filters require the same admission as any native
test route;
compilation does not authorize real playback. Performance executables stay manual,
with independent entry points; plain `opt` compilation is not the managed
Release IPO performance baseline. AppKit smoke and WinUI probes likewise retain their existing independent
native entry points and bundle/resource identities, outside unattended selection.

Library/doc/WinUI source inputs use exact declared runfiles, including manifest-only
Windows execution. The WinUI source-inspection case receives a manifest of authored
inputs instead of recursively reading an undeclared checkout.
Explicit labels retain normal Bazel meaning: `//...` is not rewritten and may
be expensive on a shared source filesystem.

Audio includes every Core implementation for the selected host, not just
NullBackend. Common and bounded native SDK tests retain existing assertions
and case markers. Core discovery and provider contracts join
`//test:audio_platform_test`; the native backend cases have the explicit
`//test:backend_test` owner. Ordinary Core coverage includes
ALSA null/missing-device calls, synthetic Core Audio UID lookup and native
provider construction, not only pure fakes; live endpoint and service requirements
remain explicit on the backend suite.

## Trees and direct Bazel

`--path`, then `BUILD_DIR`, selects one exact external tree; otherwise the
platform build root supplies the per-configuration default tree named
`bazel-<config>`. Source-contained trees and ancestors
of the checkout are rejected. An explicitly selected tree is mode-neutral: the
selected profile, not the tree, picks the compilation mode, and the composite
sanitizer check runs its normal Debug completion and the managed sanitizer
profile in that one selected tree.
Do not reconfigure a tree during another invocation or remove an active tree or
lock: the portal serializes writers and Bazel owns its server/output-base lock.

The [workspace launcher](../../script/ao/core/bazel_workspace.py) writes:

- `<tree>/configuration/request.json`: mode-neutral native host, selected
  provider prefix and the original checkout source;
- `<tree>/configuration/aobus.bazelrc`: launch environment, output/cache selection
  and startup `--max_idle_secs=900`;
- `<tree>/build.log`: noninteractive invocation output.

Each tree has an explicit `--output_base=<tree>/state/output`; its server,
analysis and materialized repositories stay separate. A shared native
`--output_user_root=<managed-state>/bazel` lets Bazel select installations by its
embedded key, not just the version string. Repository downloads use Bazel's
default content-addressed cache under that shared root, and action disk caching
remains shared; the repository rc keeps the repository contents cache disabled.
Install-base GC is disabled as a retention choice: Bazel re-extracts a missing
installation for an existing tree. The portal does not migrate or clean up
legacy installations or former per-tree `state/cache/repos` downloads.
Native invalid-install repair remains Bazel behavior. Convenience links use
`<tree>/bazel-`.

Repreparing an older tree selects the new `state/output` child and starts new
server/analysis state without moving or deleting its old hashed output base or
installation. It does not preserve the old JVM's warm analysis state. Repository
state remains in Bazel's external repository area, not a Python-refreshed tree.
New servers use the 900-second idle timeout on all hosts/profiles. Existing
servers retain their original timeout until exit; the portal does not force a
restart. Action/disk caches survive exit, but the next invocation pays startup
and analysis warm-up costs.

After preparation, use the pinned executable with the generated rc from the
same checkout for IDE or diagnostic work:

```bash
bazel --bazelrc=/tmp/build/aobus-bazel-debug/configuration/aobus.bazelrc test //:audio_tests
```

No hidden artifact refresh is required. Missing configuration fails with setup
instructions. Reprepare when changing provider/profile; an old rc does not
certify another host environment or replace native prerequisites. Bare
commands that skip the generated rc entirely are covered in
[Direct Bazel without the portal](#direct-bazel-without-the-portal).
The portal pairs Debug/Release providers with Bazel `compilation_mode=dbg|opt`;
imported APIs use select guards requiring the matching mode, while host execution
tools are selected independently.

The portal reserves configuration, provider, environment, startup/output-root
and unsafe CRT/GSL overrides. This pairing is not proof that arbitrary user
copts preserve ABI/assertion semantics. The rc is configuration, not a security
boundary against rewritten inputs or arbitrary direct Bazel overrides.

## Architecture

```text
bazel, or ./ao for managed launch/output selection
  -> persistent Bazel server
     -> shared governed host environment, refreshed when watched inputs change
     -> repository rules invoke the governed Python resolver
     -> imported interfaces, native tools and project BUILD graph
     -> generation, compilation, linking, tests and cached actions
```

One [`aobus_environment` repository](../../bazel/environment.bzl) is shared
within each output base and refreshed when its watched inputs change.
It runs the [environment helper](../../script/ao/core/bazel_environment.py)
and publishes a mode-neutral host environment. Linux enters the pinned
`nix-shell --pure` itself, independent of the caller's Nix environment;
Nix may download or build missing pinned store outputs. macOS reuses the existing
Homebrew Python/LLVM/pkgconf and the selected SDK; Windows reuses the governed
base Python and captures the Visual Studio environment through the existing
batch. Native macOS/Windows discovery never installs SDKs, ports, Python or
runtimes. The Python discovery helper itself only validates ready inputs;
an invalid explicit selection fails without fallback. Every projection repository
— dependencies, tools, the Unix compiler toolchain and the lazy lint SDK/policy —
consumes that shared `environment.json` and its recorded interpreter instead of
per-invocation portal state.

The portal is a launcher, not an outer dependency freshness engine.
[Repository rules](../../bazel/dependencies.bzl) invoke the
[resolver](../../script/ao/core/bazel_resolver.py), which composes independent
native observations without importing the launcher. Responsibilities follow the
input contract:

- [Native inputs](../../script/ao/core/bazel_native.py) own host/path admission,
  sanitized child environments and scoped probe observation; observers are
  restored on exit, not assigned through launcher globals.
- [Providers](../../script/ao/core/bazel_providers.py) admit Nix report identity
  and installed vcpkg status/features; [library interfaces](../../script/ao/core/bazel_dependencies.py)
  apply reviewed metadata repairs and ABI checks through the existing artifact
  adapter and header-ownership owner.
- [Host tools](../../script/ao/core/bazel_host_tools.py) observe selected
  executables and their declared ICU/Python/runtime inputs.
- [Unix compiler probes](../../script/ao/core/bazel_compiler_probe.py) admit
  exact installed tool/include interfaces and raw host/Nix metadata. Linux keeps
  its aggregate Nix byte ledger; macOS admits selected tool readiness and small
  selection metadata under the installed-SDK trust boundary
  ([macOS toolchain composition](#macos-toolchain-composition)). They do not
  return Bazel labels, CPU constraints, projection destinations or per-mode
  action environments. The Windows manifest-tool helper is owned by host tools
  below.
- [WinUI SDK admission](../../script/ao/core/bazel_winui.py) validates the installed
  NuGet/MSBuild interface, native tool closure and COFF roles; it reports provider
  membership and files, not Bazel destinations or declarations.
- [Lint SDK admission](../../script/ao/core/bazel_lint_sdk.py) validates the ready
  LLVM tools, archive closure and ABI interface, independently of product SDK imports.
- The [launcher](../../script/ao/core/bazel_workspace.py) owns managed Bazel,
  launch/output/cache selection, the governed `build`/`clean` invocations and
  logs.

Native observers return data directly, without temporary BUILD/native-path
programs for the resolver to reread. The
[library projection](../../bazel/import_repository.bzl),
[host-tool projection](../../bazel/host_tools_repository.bzl),
[compiler projection](../../bazel/toolchain_repository.bzl) and
[WinUI projection](../../bazel/winui_repository.bzl) materialize selected artifacts,
data-only values and fixed BUILD entrypoints. The Unix compiler projection owns
tool labels, collision checks and native-machine-to-Bazel CPU constraints;
Windows has no local compiler projection (see
[Windows compiler configuration](#windows-compiler-configuration)). Checked-in
[import declarations](../../bazel/imports.bzl),
[host-tool declarations](../../bazel/host_tools.bzl),
[WinUI declarations](../../bazel/winui_sdk.bzl) and compiler macros own the graph.
Declaration-only edits do not load back into native provider discovery.
Repository requests are mode-neutral: the resolver reports Debug and Release
observations keyed by configuration, not by Bazel package paths. Starlark owns
native/test subpackages, the catalog SDK's package layout and root mode aliases;
it requires both configurations before materialization. Fixed single-mode root
SDK repositories are not supported. Per-variant native queries still receive
their own Debug or Release configuration.
WinUI remains independent and lazy, using the same Starlark mode selection.
The resolver returns no generated-file, destination-link or alias program.

Native discovery never installs/builds dependencies. Bazel owns materialization,
watched-input invalidation and repository failure handling; the launcher does
not stage and compare whole Python-owned artifact trees.

The read inventory includes source/policy inputs, provider metadata, directory
membership, negative probes and declared tools. Python audit events supplement
it but are neither a complete trace of pkgconf/native reads nor an OS sandbox.
Repository subprocesses receive a closed environment: an empty `ctx.execute`
environment alone would inherit client variables. Isolated Python uses `-B` and
a directory `pycache_prefix` under the existing repository-owned `_tmp`.
Bazel clears that root before each new fetch, so source-backed imports do not
consume source-tree bytecode caches. Source/directory and actual dynamic-input
watches remain; explicit sourceless bytecode is not disabled or silently ignored.
Portal Python inputs are the resolver and the modules and package directories
its imports actually read, not every module beside it.

Executable selection uses the single actual `shutil.which` result. Watches keep
negative candidates before the winner and all candidates in its directory;
only an exact lexical candidate match permits dropping later PATH directories.
Windows includes PATHEXT variants and conservative current-directory probes;
an implicit current-directory winner may retain the full PATH inventory.

Nix reports each selected package's evaluated derivation and named output paths.
Resolution validates membership in that package's existing output directories,
including split-output gperf links; it does not invoke ambient `nix-store`.
Extra output metadata does not add those outputs to the report's realization
closure. This trusts Nix report membership rather than independently comparing
local store-DB deriver records. Nix outputs remain trusted immutable identities.
Mutable vcpkg metadata is watched,
and materialized payloads remain Bazel inputs. Bazel owns and watches Windows
file-copy fallback without requiring symlink privileges. Concurrent provider
mutation during resolution/build is unsupported.

## Imported interfaces and compilation

The governed repositories provide:

- `@aobus_environment`: the shared mode-neutral host environment and selected
  interpreter consumed by every other repository here;
- `@aobus_native_deps`: Core libraries/codecs, Linux ALSA/PipeWire/SPA/udev,
  GTKmm, and host-selected Boost.Process/Boost.Filesystem interfaces;
- `@aobus_test_deps`: static Catch2/Catch2Main and reviewed FakeIt Catch integration;
- `@aobus_native_tools`: governed gperf, catalog ICU/Python SDK, Linux resource
  compiler and test daemon, and Windows manifest tools; catalog compiler ICU
  imports cover Debug/Release
  independently of the target provider mode;
- `@aobus_toolchain`: the governed Unix compiler identity and the selected Nix or
  macOS LLVM SDK; Windows omits this repository and uses rules_cc's default MSVC
  auto-configuration;
- `@aobus_winui_sdk`: the installed WinUI NuGet SDK's headers, bootstrap sources,
  import libraries, runtime payload and generation tools, projected lazily from
  the restored NuGet cache for the [WinUI product](#winui-product) and its shared projection.

Native, test and WinUI SDK repositories expose both Debug and Release
interfaces and alias them to each consumer's actual `compilation_mode`
(`dbg`/`opt`); `fastbuild` has no accepted SDK mapping and fails analysis
instead of falling back. Execution tools resolve their own optimized
interfaces independently of the target mode.

[Header ownership](../../script/ao/core/bazel_headers.py) comes from installed
per-port vcpkg `.list` files. Sparse imports project only reviewed owned closures,
preserving include order and topology such as RapidYaml's `../c4` wrappers.
FakeIt's child-only interface excludes standalone/sibling integrations. No whole-triplet include directory is linked.
Header and library labels omit absolute provider prefixes for action-path stability,
not guaranteed cache hits. Linux shared Nix imports retain linker-only search hints
from their declared ELF runtime search directories. Indirect dependency lookup
therefore prefers those provider directories over command-line runtime paths that
might contain incompatible same-SONAME libraries. These hints do not inventory the
complete recursive runtime closure.

The [adapter](../../script/ao/core/bazel_deps.py) and
[library interface resolver](../../script/ao/core/bazel_dependencies.py) prefer metadata.
Missing module groups permit only complete reviewed fallbacks; partial groups
fail. Windows uses the standard dynamic `x64-windows` SDK with reviewed import
library/DLL pairs; macOS defaults to static imports, and Linux defaults to shared
imports with reviewed per-package static interfaces. Header-only or recipe-backed
static consumers include CLI11, FTXUI, ALAC, RapidYaml and Catch2. Static interfaces
retain reviewed archive names, installed headers and applicable consumer
definitions; shared-package headers are not repaired by inventing a blanket
consumer macro.
A `.lib` suffix or absent DLL alone is not static-link evidence;
artifacts from another triplet, configuration or obsolete SDK layout are not a fallback.
Provider versions, raw port revisions, artifact roles, configuration roots, expected shape and applicable
header evidence remain guards. Installed identity does not certify original
registry/build provenance.

Version claims in this projection have distinct owners. The shared
`dependency-contract.json` and the resolver inputs (`nixpkgs.json`,
`vcpkg-configuration.json`, `vcpkg.json`) remain the only version-selection
authorities; Bazel adds no installer pin, and
[config/bazel/dependencies.json](../../config/bazel/dependencies.json) holds no
accepted-version table. [Dependency governance](dependency-governance.md) owns
what a version change requires. Installed Nix reports and triplet-filtered
vcpkg status are actual observations, matched against the installed headers,
pkg-config metadata and tools together with the recorded ownership, root and
configuration data; raw `version#port` revisions are parsed and validated but
not compared to a Bazel allowlist.
`config/bazel/dependencies.json` therefore holds structural contracts only:
exact archive closures, library spellings and include order,
metadata-token preconditions, ABI defines, and native repair entries. Narrow
producer-format rules derive the expected metadata version shape from the
observed provider — mimalloc's major/minor, the Nix ALAC release part, udev's
major number — and a mismatch or unrecognized format fails. Catalog ICU takes
its version from the same existing contract entry and the actual provider,
with header macros and the genrb report required to match; the core/tools
split and static tool adjacency remain. A coherent SDK upgrade accepted by the
shared policy consequently passes these unchanged structural checks without a
Bazel-side version edit — this is intentional. It is not a claim that the
structure certifies any future version's API/ABI compatibility: governed
upgrades still run the affected native validation. Structural preconditions
fail closed; missing or unexpected fields are not guessed, repaired with
blanket macros, or silently ignored. The stb overlay's fixed source remains
owned by its producer pins ([temporary
overlay ports](dependency-governance.md#temporary-overlay-ports)); the Bazel
header API checks are not proof of the undefined-behavior fix or of upstream
provenance.

Non-definition compile flags are closed requirements, not generic `PACKAGE_COPTS`
for all consumers. Windows `/permissive-` is satisfied globally; reviewed PipeWire
flags belong to its compiling consumer closure. GTK's compiling consumers apply
the admitted x86_64 SSE requirements explicitly, with no ISA flags on other CPUs.
New unsatisfied thread/PIC/ISA
requirements fail rather than disappear. Defines and link requirements retain
their own transitive semantics.

## Compiler policy ownership

The governed Unix toolchains own their compiler parameters, not rc files,
Python mode branches or launch JSON. The [repository rc](../../.bazelrc) selects
modes/toolchains, clears ambient repository/action channels, enables strict
action environments and requires Windows manifest validation; it contains no
raw Unix compiler/linker parameters, and supplies the Windows project profile
only through the upstream auto-configuration inlets of
[Windows compiler configuration](#windows-compiler-configuration).
Launch/server/cache configuration belongs to the launcher.
Repository clearing is separate from action clearing: `--repo_env==CPLUS_INCLUDE_PATH`
unsets the variable, rather than assigning an empty path entry.

The rc sets the upstream repository guards
`BAZEL_DO_NOT_DETECT_CPP_TOOLCHAIN=1` and `BAZEL_NO_APPLE_CPP_TOOLCHAIN=1` through
`common --repo_env`, disabling both generic and Apple default C++ toolchains for
the governed Linux/macOS registrations. With the current
[module graph](../../MODULE.bazel), Unix resolution must use a registered
governed native toolchain; no compatible governed toolchain means failure, not
ambient fallback. The rc's Windows configuration overrides the generic guard
with `0`, so rules_cc's official default MSVC auto-configuration registers
there instead ([Windows compiler
configuration](#windows-compiler-configuration)). `APPLE_SUPPORT_LAYERING_CHECK_BETA=0` additionally
disables upstream's automatic developer-tree module-map scan, which is not an
admitted SDK input producer. These guards do not replace repository/action
environment clearing or control warning-feature selection.

[REPO.bazel](../../REPO.bazel) requests `aobus_warnings` and the standard
`treat_warnings_as_errors` feature for standard rules_cc compiling targets in
the main repository. The Unix platform configurations disable the standard
feature for both target and host consumers, preserving the existing
`aobus_warnings`-owned Unix warning policy; on Windows only the standard
feature supplies `/WX`, for main-repository compiling targets only, and
fixture targets disable both feature names. Ownership follows the compiling
target, not the source filename, suffix, generator or macro origin. Generated
C++ and external sources compiled by a main-repository target receive them;
external compiling targets, including rules_cc runfiles, do not inherit them.
Features do not propagate through `CcInfo` or `compile_contract`. No wrapper,
macro, target registry, source regex or repeated per-target feature attribute
is needed; custom compile rules must honor feature selection themselves.

Native Bazel feature merging applies:

- Package/rule settings can override repository defaults; a rule can re-enable a
  package-disabled feature.
- An empty rule feature list does not clear inherited defaults.
- Global `--features=-aobus_warnings` disables even local requests; global
  `--features=aobus_warnings` can enable external targets too.

Unix main-repository Starlark definitions are instantiated by a thin
host-selected BUILD in `@aobus_toolchain`, retaining external target names and
caller-relative tool inputs; Windows has no local compiler projection.
Compiler policy edits follow Bazel load/analysis edges rather than
rerunning native provider discovery. Python returns Unix compiler facts and
identity;
the checked-in repository projection creates tool links, data and the fixed call,
not copies of policy templates. Unselected hosts are not eagerly
loaded through separate main BUILD packages.

[Shared Unix warning values](../../bazel/compiler_policy.bzl) precede ordinary
user copts in the [Linux toolchain](../../bazel/linux_toolchain.bzl),
which adds Linux diagnostics, and the
[macOS upstream composition](../../bazel/macos_cc_config.bzl).
Windows warnings come from the upstream inlets and standard features of the
[auto-configuration](#windows-compiler-configuration), not a local bridge.
The default-policy checker requires the full warning
contract; intentional opt-outs are not expected to pass it.

External-header classification, language/ABI defaults and hardening are
independent of warnings. The Windows configuration enables the standard
`external_include_paths` feature for both target and host consumers.
Linux/macOS default-enable their corresponding feature. MSVC represents Bazel's
quote include contexts with `/I`: checks recognize exact declared per-action
contexts, including private dependencies, separately from provider `/external:I`
interfaces, not arbitrary prefix exemptions.

The headerless [`//bazel:compile_contract`](../../bazel/BUILD.bazel) always enables
GSL checking for utility headers and pure probe support. Actual `compilation_mode`
selects `TERMINATES` for `opt`, `ASSERTS` for `dbg`/`fastbuild`, independently of
the prepared provider. The selected Unix compiler/runtime/SDK/shim identity is
recorded in `identity.json` and declared directly through toolchain tool/policy
inputs; it does not rely on a consumer macro or library dependency. Windows
records no local compiler identity ledger.

Consumer BUILD owners retain SPA flags, library link requirements and system
frameworks. Windows manifest linker options/inputs in
[test/BUILD.bazel](../../test/BUILD.bazel) are an executable artifact contract,
separate from compiler `/utf-8`.
The graph is primarily C++, with Linux GTK's generated `resources.c` compiled
through a standard C action in
[`//app/linux-gtk:resources`](../../app/linux-gtk/BUILD.bazel), and the native
macOS AppKit product compiled with Objective-C++ actions. These bounded consumers
do not certify general C, Objective-C, Objective-C++, assembly or module support.
The macOS toolchain covers C++ compile/header-parsing and AppKit Objective-C++
actions; another language must add its action coverage in the same change.
Policy changes require native compiler-action evidence for ownership and
external-target exclusion.

## Linux toolchain and modes

The [rule-based Linux toolchain](../../bazel/linux_toolchain.bzl) uses the
pinned rules_cc, with no additional compiler manager/pin, rules_nixpkgs or
toolchains_llvm. Bazel selects Nix Clang 22 from the
[pinned shell's report](../../shell.nix). A `--clang` request selects the
governed Clang that the toolchain already uses; there is no GCC switch, and
Windows rejects the option. Preparation does not probe
`CC`/`CXX`: SDK tools, standard-library/builtin roots and Nix wrapper hardening
metadata come from that report.
The builtin include allowlist contains compiler, standard C++ library and sysroot
include directories, never the whole Nix store or application dependency roots.

Actual Bazel mode features select arguments and salted
`NIX_HARDENING_ENABLE_<suffix>` action environments. The compiler probe supplies
only the report's raw suffix and enabled flag list; the Linux Starlark macro
constructs the salted environment and applies the mode-specific fortify rule.
The compile-contract checker derives its hardening expectations independently
from the raw report; its verification oracle is never used to generate build
configuration. The shared Unix warning list is narrower: the checker reads the
same `UNIX_WARNINGS` literal in
[compiler_policy.bzl](../../bazel/compiler_policy.bzl) that both Unix toolchains
load, so it proves each captured first-party action carries that declared list
effectively, without wholesale suppression, not that the list itself is right.
Only the Linux-only additions remain spelled independently in the checker.
Toolchain identity is declared data of
tools and policy arguments, including mode-specific hardening, so compilation
and link action keys include it.

| Mode | Compiler policy | Nix hardening policy |
|---|---|---|
| `dbg` | `-O0 -g`, no `NDEBUG` | Supported policy except `fortify`/`fortify3` |
| `opt` | `-O2 -g0 -DNDEBUG`, function/data sections | Full supported policy |
| `fastbuild` | `-O0 -g0`, no `NDEBUG` | Supported policy except `fortify`/`fortify3` |

Only optimization-dependent fortify is excluded in Debug/fastbuild; other
supported hardening remains. The Clang wrapper filters unsupported `fortify3`
to level 2 in optimized mode.
The portal exposes only Debug/Release, with imported providers prepared and
guarded for `dbg` or `opt`; the mode table does not establish whole-provider
`fastbuild` acceptance.

## Release and sampling profiles

Release and `profile` are named managed profiles over the ordinary `dbg`/`opt`
modes; they are selected with the `release`/`profile` flavors
(`--config release|profile` for `./ao verify`), never through
Bazel's generic `thin_lto`/`full_lto` features, and the repository rc selects
them with appendable `--features` entries so user and baseline features are
never silently replaced. Unix profile requests require the selected feature;
the boolean admission flag alone does not enable compiler options. Windows
Release instead uses its governed platform flags.
[Optimized builds](optimized-builds.md) owns the contributor-facing flavor contract.

- Unix Release enables interprocedural optimization for C++ translation units
  and final links through the direct driver `-flto=thin`, including the
  Objective-C++-exclusive scope rules: `cpp_compile` and `linkstamp_compile`
  actions carry the Release C++-only policy, while Objective-C++ translation
  units retain normal Release optimization. Final link actions exclude archive
  actions from the IPO policy. Apple uses the existing `lto_object_path`
  mechanism; the generic Apple `thin_lto`/`full_lto` indexing is not used.
- Windows Release applies MSVC `/GL` at compile and `/LTCG` at link through
  target-platform flags, with the exact per-file `/GL-` exception for the
  authored `app/windows-winui/MainWindow.xaml.cpp` on the selected MSVC 14.51
  toolset only. Standard `lib.exe` archives are not given `--linkopt`; LIB
  restarts in LTCG mode on `/GL` input. Because Bazel 9.2 platform flags
  replace all previous values of repeatable flags, the portal explicitly
  rejects forwarded `--cxxopt`, `--linkopt` and `--per_file_copt` in Windows
  Release before SDK or tree preparation, with an honest managed-IPO
  diagnostic. Ordinary rule copts and the standard baseline flags remain
  intact; Debug/Unix forwarding is not narrowed.
- The Unix `profile` flavor is a sampling configuration: `-O2` with debug
  information and frame pointers, no IPO. It is not PGO, and no PGO workflow
  exists.
- Execution tools resolve their own ordinary unprofiled optimized interfaces
  independently of a Release or profile target; imported SDK interfaces
  follow the consumer's actual compilation mode.

The compile-contract verification checks effective
compile, link and archive arguments — including frame-pointer and no-LTO
sampling policy and the Windows per-file exception — rather than flag
membership alone. Native acceptance remains profile- and platform-specific:
analysis verifies the requested policy but does not replace the affected build,
link and runtime gates. Release evidence does not qualify the sampling profile.

## macOS toolchain composition

The [module graph](../../MODULE.bazel) fetches the pinned `apple_support`
implementation from BCR. The [local composition](../../bazel/macos_cc_config.bzl)
loads its public `cc_toolchain` macro rather than copying Apple action/argument
implementations. Existing Homebrew LLVM, SDK admission, deployment and expected-header
shim remain authoritative; this is not registration of an automatically selected
Apple Clang toolchain. The existing `registered_llvm22` label and default-toolchain
rejection guards are unchanged.

The [small external patch](../../bazel/apple_support_llvm.patch) has two purposes:
select an explicit installed LLVM compiler while preserving its full child
`argv[0]`, and let the caller replace automatic platform include/environment
arguments. The wrapper rejects a present `TOOLCHAINS` selector rather than
falling back, and retains upstream response-file, path-placeholder, dSYM and strip
protocols. The platform-input seam keeps only the admitted include roots; upstream's
whole `/Applications` and `/Library` allowlists and automatic SDK selection do not
become Aobus policy. Retire the patch when the upstream public API supports both
installed-compiler selection and caller-owned platform inputs.

A local instance of the public `apple_genrule` bootstraps the external wrapper
source with the selected LLVM in C++17 mode, without resolving the C++ toolchain
it constructs. Its explicit command preserves the admitted developer, SDK,
linker and host target selection. Original compiler/tool bytes and the admitted
SDK selection metadata are declared inputs, not compiler source arguments. The wrapper is
not compiled during repository fetch.
The repository also projects the selected Apple linker and wrapper postprocessors;
`xcrun`, `dsymutil` and Apple `strip` remain declared tool inputs. Ordinary
archives and explicit strip actions retain the selected LLVM tools. These are
host-installed interfaces, not a hermetic SDK or remote-execution guarantee.

The generated native platform projects the admitted deployment value into Bazel's
standard `macos_minimum_os` and `host_macos_minimum_os` options. The repository rc
selects this platform for portal and bare entry; Bazel's execution transition
carries the host minimum into compiled execution tools. Upstream derives the
product compiler target from that configuration, without another local cc-action
`-target`; the bootstrap command separately selects the wrapper's host target.
A no-fallback configuration check rejects effective target/host minima that do
not match the admitted SDK metadata.

Compiler projection and identity reuse one set of admitted SDK/tool
observations. macOS follows the installed-SDK trust boundary shared with
Windows: `identity.json` carries small schema-2 selection metadata — the
selected SDK path with its `xcrun`-reported version and build, LLVM root and
version, deployment target, and the selected tool, linker and developer-directory
paths. Bounded SDKSettings watches refresh those SDK version/build observations.
It contains no SDK, libc++ or compiler-resource byte inventory. The
projection binds the selected tools as tool inputs and the linker plugin and
governed shim header as explicit file inputs; the aggregate identity input
groups that small metadata with those two files. Implicit installed SDK content
is trusted: a same-path, same-version rewrite of its headers is not guaranteed
to invalidate actions automatically. A governed SDK
upgrade must change the admitted identity and the affected cache inputs;
refetching unchanged metadata or selecting a new output base over the same disk
cache is not a blanket freshness cure. The Linux Nix toolchain retains its byte
ledger, and the resolver watches only those Linux members. The
`ao deps report --concepts` consumer guards the identity file and actually
observed inputs without rehashing installed SDK members; ordinary
`./ao verify` checks compiler actions, not the trusted SDK content.

Upstream owns ordinary modes, ARC argument selection, archive/link expansion,
external-header handling and debug-artifact mechanics. Standard `objc_library`
supplies the ARC variable; the toolchain does not force ARC on arbitrary custom
consumers. Local policy retains C++26, libc++, the expected-header shim and explicit
SDK/linker arguments. Upstream baseline diagnostics remain; the owned Unix warning
feature follows upstream defaults before user copts and is gated by the compiling
repository. Disabling `aobus_warnings` does not disable upstream diagnostics.
Empty ambient include/link channels remain cleared by the rc and launcher.

## Windows compiler configuration

Windows resolves C++ through the pinned rules_cc's official default MSVC
auto-configuration. There is no local Windows compiler definition, probe, SDK
identity ledger or materialization; the governed Linux and macOS toolchains
retain their own identity records. The [repository rc](../../.bazelrc) overrides the generic
`BAZEL_DO_NOT_DETECT_CPP_TOOLCHAIN=1` guard with `0` in its Windows
configuration so rules_cc registers the default MSVC toolchain, and it no
longer registers `@aobus_toolchain//:registered_msvc`.

This follows the installed-SDK trust boundary that the macOS toolchain also
uses; only the Linux Nix toolchain retains a byte ledger. Selected versions,
tool/include paths, generated configuration and declared artifacts affect
action keys, but absolute builtin VC/Windows SDK headers are excluded from
ordinary discovered header artifacts. Same-path, same-version SDK rewrites
therefore have no whole-SDK byte guarantee.
The [WinUI generation inputs](#winui-product) are a separate declared
closure, not protection for every ordinary MSVC action.

Project compiler policy reaches the auto-configuration only through its
documented repository-environment inlets: `BAZEL_COPTS=/W4`, the complete
current language/ABI defaults (effective standard, UTF-8, permissiveness, `Zc`
conformance, Unicode/lean-and-mean and WinVer definitions, `/external:W0`)
through `BAZEL_CXXOPTS` — colon-separated with upstream's `%` escaping for
flag-internal colons such as `/Zc:__cplusplus` — and
`BAZEL_WIN32_WINNT=/D_WIN32_WINNT=0x0A00` as the complete flag, not a bare
version token. Effective standard selection remains `/std:c++latest` after
the upstream `/std:c++17` default, retaining the known D9025 override
diagnostic; upstream supplies `NOMINMAX` and `_CRT_SECURE_NO_WARNINGS`.
Native dynamic CRT defaults stay upstream: `/MDd` in `dbg`, `/MD` in `opt`.
Actual compilation mode selects these for each consumer. The catalog compiler
is a real compiled execution tool: its execution transition must use optimized
imports and `/MD`, independently of a Debug target's `/MDd`. Target-only
evidence cannot establish that host-tool contract.

Warning policy stays feature-owned, not global: REPO.bazel requests the
standard `treat_warnings_as_errors` beside `aobus_warnings`, the Unix platform
configurations disable the standard feature for both target and host
consumers, and on Windows only the standard feature supplies `/WX`, for
main-repository compiling targets only; fixture targets disable both feature
names. There is no source-regex exemption and no global `/WX`. The global
`/W4` inlet can surface nonfatal warnings from external sources compiled for
those targets; external headers keep their `/external:W0` classification. The
standard `external_include_paths` feature is enabled for both target and host
consumers. The [compile-contract checker](#compile-contract-verification)
still requires effective `/W4`/`/WX` for first-party contexts; intentional
opt-outs are not expected to pass it.

The portal derives the exact selection from the active captured Visual Studio
environment: `BAZEL_VC` from `VSINSTALLDIR` plus `VC`, `BAZEL_VC_FULL_VERSION`
from `VCToolsVersion` and `BAZEL_WINSDK_FULL_VERSION` from `WindowsSDKVersion`.
Missing captured metadata, or an explicit caller selector conflicting with
that environment, fails preparation; it does not guess or silently reselect.
Existing Visual Studio bootstrap/SDK installation remain authoritative. Bare
Windows callers must supply the complete standard selector tuple and a matching
initialized x64 compiler environment, as shown in
[direct entry](#direct-bazel-without-the-portal). SDK admission checks the original
client selection before relying on upstream auto-configuration; it does not
attempt to influence that independent repository by exporting child variables.

The existing `compiler_environment` request capture (original `INCLUDE`,
`LIB`, `PATH`, `TMP` and VC/SDK selectors) stays for WinUI and dependency
admission; it is not a compiler-policy channel, and no whole-SDK byte ledger
or hermetic SDK claim is made. The Windows `mt.exe` manifest-tool helper is
owned by governed host-tools discovery; manifest tools and validation
contracts are unchanged. Repository fetch omits the Unix compiler repository
on Windows, fetches the remaining SDK repositories and may reevaluate them, so
use ordinary build/test invocations to assess warm repository reuse.

## Compile-contract verification

Run the explicit native Linux, macOS or Windows analysis gate separately from
build/test. For Linux/macOS:

```bash
./ao verify --config debug --jobs 2
./ao verify --config release --jobs 2
```

On Windows use `ao.bat verify` with the same options and a supported local
source checkout. UNC/mapped-network sources remain rejected, including by this
analysis entry. `--path` selects an external host-local Bazel tree as for other
Bazel commands, including its provisioned vcpkg triplet.

`verify` accepts the `--config` flavors — `debug`, `release`, `profile` and
the `asan`/`tsan` analysis profiles — plus path, provider prefix and jobs,
not labels or forwarded Bazel options. After provider preparation, the
[shared checker](../../script/ao/core/bazel_contract.py) expands the default test
aggregate with `bazel query 'tests(...)'`, then analyzes its concrete targets.
This keeps test-suite membership owned by the BUILD graph; querying the suite
node itself yields no compile actions. The unconfigured selection may include
foreign-platform tests, so analysis uses `--skip_incompatible_explicit_targets`
to retain Bazel's native compatibility filtering.

- Every host checks flags/environments for the default test aggregate. Official
  aquery configuration IDs distinguish target and tool actions; native exec tools
  require opt/GSL-TERMINATES even beside Debug targets. Linux additionally requires
  their own full Nix hardening policy. Linux/macOS omit the artifact graph here.
- Windows includes artifacts and analysis-only FileWrite contents from the
  [compile-context aspect](../../bazel/compile_context.bzl). It combines the real
  public compilation context with immediate private dependency contexts, then
  joins the declared quote roots to compile actions by target and configuration
  IDs. MSVC compile actions always pass their arguments through a param file,
  whose contents aquery omits; the aspect records each action's expanded
  Starlark command line, joined additionally by its outputs. The FileWrite
  actions are inspected, not executed. Missing or ambiguous context fails; `/I`
  arguments cannot grant themselves exemptions.
- Native link/archive analysis also checks Objective-C++ actions. On macOS it
  includes `//:check_products`, because AppKit is outside the default tests.
  Missing target Objective-C++ evidence fails; this analysis does not run GUI code.
- Linux alone performs a second analysis of potential SPA consumers in the two
  audio binaries, including artifacts and pruned inputs: conservative
  declared-input checking, not textual header use.

Checks cover effective C++ standard (`-std=c++26` or `/std:c++latest`), mode,
warnings, GSL, imported system includes and local SPA requirements. Windows
checks also cover UTF-8, critical language/ABI defaults and Win10 definitions
as known switches/macros, not preprocessor execution. All hosts require nine
explicitly cleared target include/link channels; Linux also requires its salted
Nix policy. Strict exec actions may leave those channels absent instead of
assigning empty values; nonempty values still fail. Aquery omits inherited-only settings, so
this fixed-environment check does not replace the portal's closed launch
environment, strict action configuration or override restrictions. Arbitrary
system/home rc overrides remain outside that managed guarantee.
Zero first-party or required SPA matches fail.

On Linux, between analyses, one captured target-configuration, checkout-source
Clang command/environment is expanded with `-###` from the checkout. Only a relative external compiler path is resolved,
using `query --output=location`, not execution-root discovery/tool staging.
This works on fresh analysis trees without C++ build actions and checks effective
optimization, fortify and stack protection without executing `cc1`. It is a
representative wrapper diagnostic, not literal or whole-graph action execution,
linking or runtime certification.

Each attempt retains `<tree>/contracts/<config>/verify-*/receipt.json`, raw test
selection and native flag queries with stderr, plus Linux SPA and
driver/source-location streams when applicable. Receipts record the native
platform, concrete selection, commands, exits, counts and failed/unrun stages.
macOS/Windows mark the Linux Nix driver/source-location diagnostic inapplicable,
not passed. The first failure stops later stages. This covers captured compiler,
link and archive policy, not native product execution or AppKit GUI acceptance.
Ordinary builds do not implicitly run this policy checker.

## Sanitizer and coverage instrumentation

Bazel exposes explicit, scoped sanitizer and coverage instrumentation beside
the ordinary Debug/Release modes. `./ao tidy`, `./ao hygiene` and
`./ao deps provision` keep `debug`/`release` only, and an instrumentation run never substitutes
for the ordinary completion a change still requires: it is not a default or
the whole-project ownership gate. Linux CI separately runs
[instrumentation smoke](#ci-instrumentation-smoke), not instrumentation runtime
acceptance. Task-level validation
guidance lives in [concurrency and sanitizer
validation](test/concurrency-and-sanitizer.md) and the [coverage
workflow](test/coverage-workflow.md). Sanitizer and coverage profiles use the
same `--path`/`BUILD_DIR` selection; without an explicit selection the
per-configuration default tree names apply, and the composite sanitizer check
uses one selected tree for its normal Debug completion and the managed profile.

### Sanitizer profiles

`./ao build`, `./ao run` and `./ao check` request a profile with `--asan` or
`--tsan` beside the Debug flavor, and `./ao verify` accepts
`--config asan|tsan`. The managed test is a fixed graph-owned scope:

```bash
./ao test --asan
./ao test --tsan
./ao verify --config asan --jobs 2
./ao build --tsan --target //app/tui:aobus-tui  # explicit target, caller-owned runtime
```

Managed `./ao test --asan/--tsan` accepts no labels or forwarded Bazel
arguments: narrowing a scoped instrumentation run would silently change what
was measured. The [root
BUILD](../../BUILD.bazel) owns the suites: `//:asan_tests` covers Core, the
application layer, CLI, TUI, Linux GTK and Linux system media; `//:tsan_tests`
covers Core, the application layer and Linux GTK. The managed test adds the
`//:check_native_tests` backend selection through `cquery`, so every host adds
the native backend suite; a raw `//:asan_tests` invocation alone therefore
excludes the backend. Manual GUI and performance
targets and hidden hardware cases stay outside the sanitizer scope, and the existing C++
fatal-contract sibling probes remain in the transitive test dependencies.
Under the ASan profile the TUI cover-art target sets `STBIR_NO_SIMD`, a
retained local parity decision.

Bare commands select a profile through the repository rc, not a compilation
mode: `-c asan` is invalid because Bazel accepts only `dbg|opt|fastbuild`.

```bash
bazel build -c dbg --config=asan //:asan_tests
bazel test -c dbg --config=tsan //:tsan_tests
```

A bare sanitizer run gets none of the managed runtime admission below — no
frozen rc, forced report status, private service context or suppression merge;
the caller owns that runtime, and a bare success is not a managed gate result.

The [repository rc](../../.bazelrc) turns each profile into the
`//bazel:asan_enabled`/`//bazel:tsan_enabled` bool build settings, a sanitizer
target platform and the root [admission
aspect](../../bazel/sanitizers.bzl). The aspect rejects combined profiles,
ThreadSanitizer on Windows, any effective compilation mode other than `dbg`,
and the uninstrumented lint SDK host targets — explicit sanitizer lint is
therefore impossible, and the normal full check retains lint. The profile
never sets `--compilation_mode` itself: the global bare Debug default stays, so
an explicit `-c opt` beside `--config=asan` fails admission instead of being
hidden by rc ordering. The [platforms](../../bazel/BUILD.bazel) derive from
the standard host constraints; the macOS profiles parent the existing
deployment platform. Unix profiles select the standard upstream Apple
`asan`/`ubsan` features on macOS and the local Linux toolchain's
`asan`/`ubsan`/`tsan` features, preserving the Unix
`treat_warnings_as_errors` disablement; the Windows ASan profile selects MSVC
`/fsanitize=address` with `_DISABLE_STL_ANNOTATION`, matching the
uninstrumented vcpkg SDK's STL annotation boundary. Platform flags replace,
not append, the caller's repeatable `--features`/`--copt` lists.
Instrumentation is target-configuration-wide — external source runfiles
compiled for the sanitizer platform are instrumented as well — while imported
SDK binaries are not recompiled, execution tools keep the normal optimized
uninstrumented execution platform, and no global `PATH` or `BAZEL_CXXOPTS`
sanitizer injection, compiler or provider change exists. On Linux, Clang's
observed resource `share/` directory remains a compiler-owned dependency
root, preserving its standard sanitizer ignorelists. The TSan compile profile
also explicitly admits the pinned compiler-rt header interface used by
libstdc++'s atomic-shared-pointer mutex annotations; clearing ambient Nix flags
does not remove those annotations. Neither mechanism adds a project report
suppression or substitutes the production atomic implementation.

The managed test runtime is owned by
[bazel_sanitizers.py](../../script/ao/core/bazel_sanitizers.py). It freezes
one per-attempt rc — the private Linux Xvfb/D-Bus/PipeWire service context
and the explicit sanitizer option variables — before the configured queries,
and the build and test phases reuse the same rc. It forces
`halt_on_error=1` and positive report exit codes, adds UBSan stack traces, and
preserves other caller-supplied options. Caller TSan suppression files are
merged with the governed project rules into one immutable file without adding
suppressions; Linux LSAN keeps the existing replace contract. Caller ASan,
UBSan and macOS LSan suppression files are each frozen in place as separate
copies, preserving option order. Every frozen copy is content-addressed under
`<tree>/sanitizers/<profile>/runtime/<sha256>/`: Bazel hashes the path in
`--test_env`, not the bytes behind it, so unchanged rules keep one path and
changed rules always miss the test-result cache.
Tests use `--test_strategy=standalone` with `--strategy=TestRunner=local`:
the standalone runner alone can still delegate to a sandboxed spawn. Local
execution preserves absolute suppression paths and native fatal-child
contracts; a trusted remote action cache stays optional for the build phase. Linux and macOS select the existing governed LLVM symbolizer; macOS
adds no service context and keeps the existing console-user audio admission;
Windows freezes the already-selected MSVC runtime `PATH` and provisions no new
DLL. Attempt receipts live under
`<tree>/sanitizers/<profile>/run-*/receipt.json`.

The `./ao check --asan` and `./ao check --tsan` commands run the full
normal Debug completion gate first — including its completion products — then
a separate managed sanitizer profile in the same selected tree. Both use the
[test-result cache admission](#test-result-cache-admission) of the normal
gate: a cached sanitizer PASS belongs to the sanitizer-configured binary and
runtime environment Bazel hashed, so a Debug result is never relabeled as a
sanitized one. `--fresh` requires new executions in both; the receipts record
executed and cached runs. Rerunning an unchanged binary only samples another
interleaving, random test order or heap layout; ask for that with `--fresh`.
Windows rejects `--tsan` before any tree or SDK work, exactly like the managed
route.

`verify --config asan|tsan` runs only the ordinary Debug compile-contract
oracle over the sanitizer-configured graph, admitting the selected compiler-rt
interface root as a compiler include root under Linux TSan; the managed test
runs the same phase before building. It performs no compilation or linking and
does not examine sanitizer flags.

Building the [sanitizer profile canary](../../test/core/SanitizerProfileTest.cpp)
checks compiler sanitizer selection; runtime behavior requires executing native
tests. The canary is part of
`//test:utility_test`, which the ordinary and ASan test selections compile on
every host and the TSan selection on Linux and macOS. [Test BUILD](../../test/BUILD.bazel) passes the
selected profile as local defines — AddressSanitizer for `//bazel:asan`,
UndefinedBehaviorSanitizer for its Linux and macOS variants, ThreadSanitizer
for `//bazel:tsan` — and the canary's `STATIC_REQUIRE`s compare them with the
compiler's own facts: Clang's `__has_feature(address_sanitizer)`,
`undefined_behavior_sanitizer` and `thread_sanitizer`, and MSVC's
`__SANITIZE_ADDRESS__`. A profile whose compiler did not instrument that
translation unit — for example because a rule-based toolchain silently ignores
an unknown feature name — fails to build, as does a normal profile that is
unexpectedly instrumented; linking the instrumented object also requires the
sanitizer runtime.

This replaced an action-level `aquery` oracle and deliberately narrows the
guarantee. The canary proves one first-party target-configuration translation
unit per profile, not every action. These per-action properties are no longer
independently verified: frame pointers and non-recovering UBSan
(`-fno-sanitize-recover`) on instrumented compiles, the Linux TSan `-isystem`
interface root on every target C++ compile, MSVC `/Z7`, `/MDd`,
`/INCREMENTAL:NO` and `_DISABLE_STL_ANNOTATION`, and the absence of sanitizer
flags on execution-tool and archive command lines. The Debug compile-contract
oracle still requires no optimization, debug information and no `NDEBUG` on
first-party target compiles of the default test closure, and optimized
execution-tool compiles; the managed runtime still forces UBSan
`halt_on_error=1`. Non-recovering UBSan code generation has no compiler fact
the canary could observe without executing undefined behavior.

These are implemented contracts, not native acceptance: sanitizer runtime
compatibility and symbolization still require the real native validation gates
described under [Completion](#completion), and the dependency-closure limits
apply unchanged — the Linux GTK closure is not
TSan-instrumented and Windows dependencies stay uninstrumented.

### Coverage

`./ao coverage` is the Linux-only Debug line-coverage route over the same
graph. Its suites, scoping, strict per-test collection and report
contracts are defined in the [coverage
workflow](test/coverage-workflow.md).

The pinned rules_cc has one [local coverage-metadata
patch](../../bazel/rules_cc_coverage_private_headers.patch): private implementation
header aliases must reach the final binary's standard source replacement file
without exporting the private C++ compilation interface. Retire it when upstream
preserves that mapping closure; keep the private-interface and zero-hit-line
regressions. The coverage merger uses the existing selected JDK's declared
runtime files and Bazel's pinned precompiled stock deploy JAR. It does not build
the source-less Java launcher or its compiler/bootclasspath intermediates: those
upstream generic-Linux SDK executables cannot run directly on NixOS. This is a
consumer-specific native cold-build route, not support for arbitrary Java source
builds or a repair of the generic Java SDK. No Java header-extraction flag or
compiler toolchain is overridden.

### CI instrumentation smoke

After Nix, cache and tooling preparation, the existing Linux job runs these
checks before ordinary product completion, in separate trees:

```bash
./ao verify --config asan --path /tmp/build/aobus-bazel-asan-analysis --jobs 16
./ao build --asan --path /tmp/build/aobus-bazel-asan-analysis --jobs 16 --target //test:utility_test
./ao verify --config tsan --path /tmp/build/aobus-bazel-tsan-analysis --jobs 16
./ao build --tsan --path /tmp/build/aobus-bazel-tsan-analysis --jobs 16 --target //test:utility_test
./ao coverage --analysis-only --path /tmp/build/aobus-bazel-coverage-analysis --jobs 16
```

Each sanitizer analysis runs the Debug compile-contract oracle, then the utility
build compiles and links the profile canary. This checks one translation unit's
sanitizer selection and runtime linkability, not test execution or instrumentation
of every source. Build logs and each canary's BEP — retained by the build's
scoped `--event-log` — remain in the always-uploaded
completion evidence. Coverage analysis reuses the normal suite selection and action oracle:
coverage flags and atomic counters must appear on measured target actions,
while execution tools remain unprofiled. Each selected configured test must
consume its own standard metadata outputs. Queries of the actual C++ compilation
context and `InstrumentedFilesInfo` verify private-header mapping propagation,
instrumented source declarations and declared counter-note dependencies: the
existing LMDB private aliases must reach a selected consumer's context when the
library closure is present. Independent public JDK and stock-payload queries
also constrain that consumer's merger runfiles.

Bazel 9.2 does not export the generated contents of its native coverage-map and
instrumentation-manifest actions through analysis, despite their `FileWrite`
mnemonic. Coverage analysis therefore checks provider declarations and context
mapping propagation, not those generated file contents, complete host SDK identity,
materialization, counter values or report fidelity. Ordinary coverage retains
the independent standard-map and runtime/report cross-checks.

`coverage --analysis-only` accepts the ordinary suite selectors but rejects
report-only `--scope` and test filters. It issues no Bazel `build`, `test` or
`coverage` command, enters no private audio/display service context, and produces
no compilation, test execution or coverage report. Its unique receipt and query
evidence remain under `<tree>/coverage/run-*`, explicitly marked as
analysis-only. Ordinary coverage
retains its fresh tests and strict runtime/report validation unchanged.

The smoke requires the prepared native toolchain: it does not belong to
the lightweight documentation prerequisite job. A failure is fatal to the Linux
job and prevents disk-cache publication; attempt evidence is retained even when
the first failed command leaves later profiles unrun. The product, Release and
lint gates remain independent. This is Linux analysis plus bounded sanitizer
canary builds, not an all-platform sanitizer/coverage build or runtime matrix.

## Lint reports

The Bazel lint report actions cache native C++ analysis reports and gate the
selected findings; `./ao tidy` and `./ao hygiene` consume them. They use
actual Bazel `CppCompile`/`ObjcCompile` contexts, not a separate compilation
database. The [aspect](../../bazel/lint/lint.bzl),
[launcher](../../script/ao/core/bazel_lint_action.py) and
[current-report gate](../../script/ao/core/bazel_lint_reports.py) implement the
contracts below.

```bash
./ao tidy --target //lib/utility:utility
./ao tidy lib/utility/Base64.cpp
./ao tidy --folder lib/utility
./ao tidy --all
./ao tidy --commit origin/main
```

The C++ stage accepts the common scope selectors (`files`, `--folder`,
`--commit`, `--all`), `--config`, `--path`, `--vcpkg-prefix`, `--check`,
`--header-filter`, `--output`, `--debug`, `--jobs`, `--tidy-arg` and
`--remote-cache`. `--target` is repeatable and selects explicit native compile
owners. Without another scope selector, the public selection covers every actual
translation unit and paired header main in those owners, not unpaired declared
headers. Without `--target`,
a file/folder/commit/default-changed selection maps selected paths to the actual
query `srcs` owners (expanding filegroups and aliases); with `--target`, those
owners restrict the request and the selected scope gates the reports. The portal
then checks configured compatibility with `cquery`; an owner root never implies
its dependency closure. Explicit owners also bound the rule-kind query to those
roots, while native action/report inventories establish configured source coverage.
Every compatible explicit root must produce a TU report even if another root
covers the selected file. Compatibility uses the top-level target configuration,
not an exec-configuration row for the same owner built as a tool.
Automatic discovery without `--target` queries the whole project graph.

Each report records the checkout files its analysis included. A selected header
without an unambiguous same-component companion source, and a selected include
fragment, are analyzed through a translation unit that included them. The portal
picks one from the first report pass, preferring the same component and then the
longest shared directory prefix, and a second pass replays that consumer's native
context: a header becomes the main file, while a fragment stays inside the
consumer translation unit. Fragment warning blocks are selected by normalized
file identity; consumer compiler errors and unlocated diagnostics remain, and
scoping never hides an incomplete attempt. Without
`--target`, the first pass builds only the possible consumers nearest the path;
the other owners whose dependencies declare it build only when none of those
reports included it. When every nearest consumer is platform-incompatible, the
held owners are queried for compatibility before the path can defer or fail: an
incompatible nearest group cannot prove farther consumers unavailable. The
selection reaches the aspect as one canonical JSON string parameter, outside
the policy and inputs that every report action reads. The portal writes that
option into a private named build configuration in its existing generated rc,
under the tree lock, and selects it only for report builds. Query/cquery do not
select it; no consumer repository, environment selector or pre-query reset is
needed. Identical parameter bytes avoid rewrites between report passes; the next
workspace preparation regenerates the baseline rc without that private line.
Large maps stay out of the Windows client argv. This uses the normal
Bazel server mode: batch mode can re-expand rc options into an oversized Java
command line and is not supported for large maps. Each pass records its literal
parameter and exact parameterized aspect identity in the receipt. Naming a new
consumer adds its reports without changing existing report actions. The Linux
CI smoke includes an unpaired public header to exercise this rc-backed replay,
in addition to the analysis-only parameter probe.

Deferral is limited to files that are incompatible with the current platform:
such a path that no report analyzes is deferred. A compatible header or fragment
that no analyzed translation unit includes, or a compatible selected path with
no owner, fails the operation; it is not a quiet partial pass. `--all` selects
the project C++ scope and uses the same rules. The portal prints every deferred
owner or path.

The report aspect is nonpropagating; its required provider-only aspect follows
dependencies solely to map virtual include headers. It captures actual native
argv and declared inputs, adds reports for paired headers analyzed as main
files, and retains
AppKit Objective-C++/ARC and WinUI generated-header/forced-include semantics.
Unsupported compiler views, modules, PCH/header-unit contexts and unresolved
response files fail rather than being stripped. The portal reads only the
current invocation's BEP and local report artifacts, binding each owner to the
fixed production aspect and that pass's exact consumer parameter. A foreign
aspect or different parameterization cannot stand in. It does not scan stale
output directories or join `aquery` data. A clean selected set returns 0,
selected warnings or ordinary compiler errors return 1, and incomplete
coverage, infrastructure failure or an invalid report returns 2. Cached warnings still fail. `--debug` displays per-run evidence paths
and publishes all action completions in the BEP for diagnosis; those events are
not the report inventory or proof that a disk-cached action executed anew. The
tree keeps the eight most recent run evidence directories.

`--check` and `--header-filter` change report-action keys. Consumer parameters
change aspect analysis identity, but unchanged report argv, environment and
inputs retain their action keys; the selected file set is not a report input.
Raw `--tidy-arg` values are limited to diagnostic-only
presentation options (`--quiet`, `--system-headers`, `--system-headers=true`,
`--system-headers=false`, `--enable-check-profile`, `--use-color=false`); other
raw options, including fix/export, overlay/config, extra-arg and
warnings-as-errors forms, are rejected before workspace or native preparation.
The existing ownership/suppression policy applies, including stale `NOLINT`
detection inside cached analysis. The route never exports or applies fixes.

### Native lint contexts and cache boundary

The aspect preserves ordered native argv, configured owner identity and
legitimate source/PIC variants; matching paths alone cannot merge or substitute
configured contexts. It must account for every eligible configured
source, not merely find a nonempty action list. Declared inputs include the
native action's File/tree artifacts, CcInfo headers, immediate `deps` and
`implementation_deps` contexts, own header/additional inputs and the analysis
tools/policy. An implementation dependency's compiled sources do not become
this owner's lint scope. Generated WinUI trees retain their producers rather
than being reconstructed from path strings.

`Action.env` alone is not the compile environment. The capture combines native
feature and action environments in producer precedence order. For native compile
actions it also mirrors Bazel 9.2's `PWD` injection unless the toolchain execution
requirements contain `requires-darwin`, before applying the toolchain environment;
this is not a host-OS condition. Its pinned
feature admission accepts `aobus_warnings`, `treat_warnings_as_errors`,
`external_include_paths`, `static_link_msvcrt`, `dynamic_link_msvcrt` and
`default_compile_flags` for the actual report context; unsupported requested or
disabled features fail rather than receiving an approximate replay. Linux
clang-tidy does not execute the compiler wrapper from its database, so the
launcher projects the governed Nix wrapper's ordered before/original/after
arguments through `NIX_DEBUG=1` and a driver-only `-###` probe. It preserves raw
capture separately, executes no `cc1` or linker and writes no product object.
Report/verification actions use a stdlib-only module closure and omit Windows
`USERPROFILE` from their cache-keyed environment. Only the full-portal fixture
replay receives the observed profile from governed Python SDK metadata.

Lint reports do not execute native `CppCompileAction.validateInclusions()`.
Their declared input closure therefore does not replace product compilation's
include-validation gate or promise detection of every newly undeclared include.
Report actions may have a conservative input superset; their keys are not the
native object action keys. They share the governed provider/SDK trust boundary,
not a lint-only VFS, filesystem snapshot or RBE contract. Strengthening mutable
provider identity belongs to the shared Unix toolchain and the Windows
auto-configuration inputs, not a second lint policy.

One cacheable verification action checks the host version and complete
fixture-derived `aobus-*` registry; reports consume that result. Absolute checkout
roots remain repository-time discovery inputs, not published policy fields.
Actions derive their execution-root and physical-checkout aliases from the
already-declared helper source, preserving source-scope checks without binding
policy identity to the producer's directory. This removes one cache-key path
dependency, not the governed host SDK boundary or all cross-machine differences.
The helper's declared import closure is tested, so unrelated portal edits need
not invalidate analysis. Temporary compilation databases are action-local. Logs normalize
execution-root paths and map virtual headers to their declared checkout origin;
generated/external locations retain the producing execution root.

The public gate reconciles exactly one current owner/aspect completion per
requested owner, recursive named file sets, the context/report index and locally
materialized artifacts. It requires successful native completion and a complete
BEP. Report builds explicitly select local BEP artifact URIs even with a remote
action cache; `--remote_download_outputs=all` does not permit a remote URI to
stand in for a missing local report. Warnings and ordinary compiler errors are cacheable
negative reports; crashes, missing tools/context, missing or skipped compile
commands and malformed or incomplete attempts must fail instead of becoming
clean reports. Exit zero and an empty log
do not alone establish coverage. Failed actions print the analyzer output tail because
Bazel can discard their action log. A raw report build can succeed with findings,
but the public gate still fails them. A selected path is covered when any
requested owner reports it as a main; another owner's uncovered listing does
not negate that coverage. Whole main/context diagnostics and related
notes remain together; infrastructure failure in an owner-wide report build
cannot be hidden by selecting fewer mains. The report build alone disables
product validations to avoid pulling PE/link actions into lint; normal
build/test/run validations remain unchanged. STRICT/RELAXED mode, tools, checks,
filters and compiler policy remain action inputs even though selected scope is
not. The selected tree lock covers preparation through report consumption and
the final gate decision; it is not a source-filesystem snapshot. The portal
launches no detached lint workers and does not mutate source-checkout links.

### Physical header mains

A paired header borrows its actual companion command but remains the physical
main file. Derivation replaces only the native source argument, adds the explicit
header language and reserved `AOBUS_LINT_HEADER_MAIN=1` definition, and preserves
semantic forced includes, ARC and SDK settings. A native argument referencing
that reserved mode is rejected. There is no wrapper, VFS overlay, remapped file
or rewritten source buffer: main-file-sensitive checks and suppression scans
must see the original header.

A producer such as WinUI `pch.h` may use the reserved mode to retain framework
includes and macro cleanup while skipping implementation-header aggregation.
This is a framework-only header-analysis prefix, **not identical full native
preinclude semantics**. Normal product compilation does not enable the mode;
Bazel's `/FIpch.h` remains text inclusion, not a compiled PCH speedup.

Default source filters admit project headers under the context's STRICT or
RELAXED folders, including native execution-relative, absolute and virtual
spellings. They are POSIX extended expressions, not Python regexes. Virtual
origins come from each native `Symlink` action's single declared source input;
missing, unsupported or ambiguous producers fail instead of using suffix
inference. Generated originals remain unmapped and do not qualify through the
default project-folder filter. A header-main report selects
only that header. The [shared lint policy](linting.md) still owns frontend
exceptions, compiler-warning promotion and stale-suppression behavior.

### Checker, fixtures and validation scope

The [checker build](../../tool/lint/build.bzl) consumes a ready installed LLVM
SDK without CMake configuration, new dependency pins or installed CMake-export
parsing. Explicit Windows provisioning installs that governed SDK —
`ao.bat deps provision` or `ao.bat setup llvm-sdk`, which call the same
installer; ordinary ready-only lint does not. The lazy [lint SDK](../../bazel/lint/sdk.bzl) and
[policy](../../bazel/lint/policy.bzl) repositories consume the same shared
`@aobus_environment` host metadata instead of a portal-selected interpreter;
they import a ready SDK and never install one. Broad queries such as `//...`
load lint packages, unlike a bounded product query such as `//:tests`. If an
initial explicitly/root-selected tool or a Windows selected-root required file
is genuinely absent, the lint repository exposes a query-only unavailable graph.
Actual SDK, checker, verification, fixture and report targets still fail analysis
with the native diagnostic; `metadata.json` is a failing rule's generated output,
not fake admitted metadata. The exact missing path is watched, so completing the
selected SDK retries admission without cleaning or changing selectors. This does
not defer malformed selectors, wrong file types, inaccessible paths, missing
selections containing `..`, PATH lookup failure, bad markers, tool-query failures
or later ABI/input rejection; shared
native environment and product SDK prerequisites still apply. Both checker forms
use C++23 and the selected SDK ABI.
Unix builds the existing plugin against its externally supplied host, excluding
product sanitizer instrumentation from compilation and linking. Windows builds
the linked `AobusClangTidy` host optimized with `/MT`, independently of product
`/MDd` or `/MD`. Plugin/host ABI and sanitizer compatibility must be preserved;
macOS SDK C include ordering stays driver-owned. Registry objects and required upstream modules must survive linking. The
[SDK admission](../../script/ao/core/bazel_lint_sdk.py) derives the Windows
initializer closure from selected archive references rather than linking all
LLVM. The [repository projection](../../bazel/lint/sdk.bzl) and checked-in
[import declarations](../../bazel/lint/imports.bzl) own SDK file destinations,
metadata/flag exports and the `cc_import`/`CcInfo` graph; Python returns native
file and ABI observations, not BUILD source. Required system libraries, including DIA when enabled by the SDK, remain
part of that closure; a required library must exist. XML omission is allowed
only when the selected archives have no corresponding references.

The explicit/manual entry `./ao test lint` reuses real
fixture discovery, marker/message assertions, source-policy smokes and compiler
error canaries. Its direct verified action starts no nested project Bazel or
`ao tidy` preparation. Manual runs execute fresh: the actual configured
fixture producer action (FixtureTools) is replayed in a new attempt directory
with its marker/message assertions, source-policy smokes and compiler-error
canaries, and cached lint results are never
replayed as fixture evidence. It stays outside the default test aggregate.

Target-only scope does not imply unpaired-header coverage. Selected-check
samples, source parity and cached action completion do not establish every
header, complete scopes, other native profiles, GUI or hardware acceptance.
Record actual invocation scope, deferrals and fresh execution versus cache reuse
with PR/CI evidence, not as a second changing acceptance ledger here. The
`./ao tidy` and `./ao hygiene` commands consume these same report actions and
keep the contributor/completion role.

### Hygiene gate

`./ao hygiene` is the check-only commit gate: format check, test and name
audits, Python checks, and
[Buildifier formatting/lint](linting.md#bazelstarlark-hygiene) for Bazel/Starlark
files. The C++ stage uses the Bazel lint report actions, the same actions
`./ao tidy` consumes. It accepts hygiene's scope selectors
(`files`, `--folder`, `--commit`, `--all`) plus `--config`, `--path`,
`--vcpkg-prefix`, `--jobs` and `--remote-cache`. For C++ scope, Windows formats
with the `clang-format` of the LLVM SDK that `ao.bat deps provision` installs
and requires that SDK to be ready before any stage. It never configures a lint
tree; ordinary `ao.bat format` uses the same ready provisioned SDK directly,
with no CMake, no provisioning and no Visual Studio environment needed for
formatting. Python-only, Bazel-only and empty scopes need no LLVM SDK lookup. The
managed Python tooling environment is provisioned only when the scope contains
Python files; C++-only, Bazel-only and empty scopes do not prepare it. A Bazel-only
scope selects just the governed Buildifier tool; `./ao setup buildifier`
(`ao.bat setup buildifier`) can prepare it explicitly.
On macOS and Windows the gate itself stays on the base Python that Bazel
records as its interpreter, like every other Bazel command, and runs only Ruff and
mypy with the managed tooling environment, which it provisions; alternating it
with other Bazel commands therefore keeps their repository and action caches.
Its stage boundaries and completion role are owned by
[validation](test/validation-and-review.md).

## Completion gate

`./ao check` is the bounded Bazel completion route for a prepared tree and the
default local completion route; the positional `release` flavor selects the
Release configuration. It
accepts only the common options — the `debug|release` flavor,
`--path`, `--jobs`, `--vcpkg-prefix` and `--remote-cache` — plus the explicit
`--fresh` test-admission selector
([test-result cache admission](#test-result-cache-admission)), and accepts no
targets: a completion gate covers the whole project, so there is no partial
or label-scoped selection, and neither mode accepts forwarded Bazel
arguments or a test filter. Completion ignores system and personal Bazel rc
files, loading only the checked-in repository rc and prepared launch rc.
Isolated Bazel clients also remove the `BAZELRC` environment entry, including
Windows key-case variants; the no-system/home/workspace startup flags alone
would not disable that separate rc channel. Compile-contract analysis inside
this gate, sanitizer admission and coverage use the same environment isolation.
Ordinary build/test invocations retain their personal-rc behavior; a
personal test filter cannot silently narrow completion.

```bash
./ao check
./ao check release --path /tmp/build/aobus-bazel-release
```

The [root BUILD](../../BUILD.bazel) owns four completion roots.
`//:check_tests` executes the portable default test aggregate, the Linux GTK
test under the portal's owned headless display and private session bus, and
the lint fixtures. `//:check_native_tests` is a filegroup that adds the
native backend suite on every host, selected with `cquery labels(srcs, ...)`.
Catch2 skips its hidden `[.manual]` hardware cases, so a backend case without
that marker always runs; every current Windows case is hidden, and that target
passes `--allow-running-no-tests` there only. `//:check_products` builds the
CLI/TUI entrypoints, the host's GTK, AppKit or WinUI product, and the
explicitly manual GUI harnesses and performance benchmarks — built, never
run, so no interactive GUI session and no performance execution happens.
On Windows, `//:check_runs` additionally selects the two
unattended native console probes (track item and localization) with `cquery`
and executes them, preserving the unattended native scope. The
gate also runs the existing
[compile-contract checker](#compile-contract-verification), including macOS product
Objective-C++ actions alongside the default-test and Linux SPA analysis scopes.
Interactive GUI scenarios, physical playback and hardware admission stay outside the gate.

On Linux the gate owns its audio admission context before configured
analysis. The bounded [service
fixture](../../script/ao/core/bazel_test_services.py) starts only owned test
processes under private XDG directories and the already-owned private D-Bus
session, requires daemon readiness with `AOBUS_REQUIRE_PIPEWIRE=1`, and
adopts no personal runtime, service configuration, default audio or
installation — the same private PipeWire policy family the CI harness already
uses. Per-service streams and lifecycle state are retained beside the check
receipt. Cleanup only signals admitted owned children; uncertain spawn or stop
keeps the runtime in place and fails.

One per-attempt prepared rc freezes the runtime environment options —
including the private-context `--test_env` —
before configured analysis, because `--test_env` changes the full
configuration checksum; analysis, build, tests and the compile-contract
verification share that one frozen rc, and a `--fresh` attempt adds
`--nocache_test_results` to it. The gate uses `--lockfile_mode=error`
instead of rewriting the shared checkout's module lock. On Linux the isolated server JVM comes
from `AOBUS_BAZEL_SERVER_JAVABASE`, derived from the JDK already selected by
the pinned Bazel package in the Nix shell; no JDK is installed or upgraded.

Each attempt holds the tree lock and writes a unique receipt under
`<tree>/checks/<config>/check-*/receipt.json`, with per-phase stdout/stderr
logs, the build and test BEP files, `guardrails.json` and `test-ownership.json`
beside it. The gate reconciles passing BEP summaries
against the exact configured test selection under the
[test-result cache admission](#test-result-cache-admission) contract. Service
startup failures retain terminal receipts and diagnostics; an interrupted controller does
not establish that native writers have stopped. The receipt records the excluded scope explicitly:
hygiene, documentation, interactive GUI runs, performance runs, sanitizers,
coverage and distribution.

### Test-result cache admission

By default the gate uses Bazel's ordinary test-result caching: a passing
BEP summary counts as completion evidence whether Bazel executed the test
anew on this host or restored it from the result cache, under the same
strict membership — exact configured labels, the full configuration and a
complete BEP — as before. A cached PASS is valid for the declared inputs,
environment and configuration Bazel hashed; it is not a claim of a new host
execution, and cache authority follows the existing trusted-cache
boundaries ([native hosts and caching](#native-hosts-and-caching)). The
`external`-tagged native audio, platform and Linux system-media tests stay
outside that caching and execute on every attempt, the two unattended native
WinUI console probes remain actual runs, and the frozen private Linux
service environment is part of the hashed test configuration: changing
those owned runtime inputs invalidates cached results even when the source
is unchanged. The gates therefore name the private session bus and PipeWire
roots after the build tree, not the attempt (`/tmp/aobus-gtk-bus-<digest>`
and `/tmp/aobus-pipewire-<digest>`, recreated by each attempt under the tree
lock), and export the bus address without dbus-daemon's per-start guid.
A different Xvfb display number still changes the hash, which only costs
a cache miss.

The check controller
([bazel_check.py](../../script/ao/core/bazel_check.py)) records each test
result's run, cached and executed counts in the receipt, and the receipt's
mode together with the CLI summary states whether the attempt was fresh,
cached or mixed. `--fresh` forces `--nocache_test_results` and rejects any cached result: missing
suites, cached results or mismatched configurations fail even if the Bazel
process exits zero. It keeps a fresh-execution admission for the required
CI jobs without changing the gate's scope, forwarded arguments or filters.
Lint-producer and other actions may still be legitimately cached in either
mode, so the gate makes no fresh-producer claim beyond test results, and
hygiene, audits and documentation stay outside this caching claim. The
sanitizer route follows the same `--fresh` choice, and coverage keeps
its own fresh proof ([sanitizer and coverage
instrumentation](#sanitizer-and-coverage-instrumentation)).

### Test ownership

There are no handwritten source inventories. Test registrations come from the
existing lexical test-audit detector. The gate requires two things of every
registered source, checked by the
[ownership module](../../script/ao/core/bazel_test_ownership.py):

- an unconfigured `bazel query` shows a `cc_test` or `cc_binary` that declares
  it, directly or through linked libraries;
- unless a separate `cquery` reports it incompatible with this host, each
  such `cc_test` is in the test selection the gate executes, and each such
  `cc_binary` is below the completion roots in one configured `cquery`. A
  `cc_test` that a root only builds, for example through `//:check_products`,
  fails the gate. A `cc_binary` holds registrations that run only on request,
  such as benchmarks, so being built is its whole guarantee.

The result is recorded in `test-ownership.json`. The check works on labels:
it does not tell whether a compatible owner drops a source through `select()`,
and it reports no per-source configuration detail.

### Shared audits and the root version

The lexical completion audits — forbidden scans, boundary and naming rules —
are implemented once in [guardrails.py](../../script/ao/core/guardrails.py);
the gate writes their per-run `guardrails.json` receipt, and the standalone
[shared CLI](../../script/ao/guardrails.py) exposes the same inventory for
ad-hoc use, so there is one audit inventory, not one per route. The
application version is owned by the
plaintext root [VERSION.txt](../../VERSION.txt) file: the build reads those bytes
directly and never parses CMake syntax.
The `.txt` suffix avoids shadowing the standard C++
`<version>` header on case-insensitive filesystems.

### Scope boundaries

The completion gate implies no
sanitizer, GUI-performance, release, cache-portability or distribution
acceptance; those boundaries remain owned by their own gates and routes.
Formatting, hygiene and documentation validation stay independent gates.

## Generated files and runtime data

The [gperf rule](../../bazel/gperf.bzl) uses the governed executable in execution
configuration, `LC_ALL=C`, declared grammar/tool/runtime/output and direct
arguments. Resolution executes `gperf --version` and rejects an unexecutable or
mismatched tool. It generates query's unit dispatch and four media dispatch
headers without pre-generated headers or shell redirection.

The [catalog rule](../../bazel/catalog.bzl) declares the existing authored
inventory, text/templates, native ICU/Python SDK inputs and the real compiled
[`ao_catalog_compiler`](../../tool/catalog/BUILD.bazel) in exec configuration.
One [action helper](../../tool/catalog/build_catalog.py) follows the existing
strict genrb/validation/pseudo/pkgdata-common pipeline and emits aligned embedded
C++ data plus maintained locale tags. A private tree owns all resource/RESW/temp
side outputs; package.lst remains the sole locale inventory. No external configure,
prebuilt project tool or pre-generated resource is consumed. Common-data
packaging does not compile C, and this does not establish C/Objective-C support.
The action launches Python at its selected host-absolute path; the interpreter
bytes and selected runtime files are already declared through `catalog_tools`.
This is a native SDK boundary, not a relocatable/hermetic Python toolchain.
Switching to a repository-local executable requires separate native validation
of DLL and standard-library lookup, particularly on Windows.
The application version header expands the existing template from the project's
version declaration without a CMake configure.

[Audio fixtures](../../test/data/audio/BUILD.bazel) are checked-in files;
generation scripts are not run. The shared helper uses official rules_cc runfiles
and a declared marker for manifest/directory runfiles. There is no checkout fallback or fixture copy.
Windows FLAC consumes Ogg from the reviewed SDK as a normal dynamic
dependency; recipe-backed static-only ports remain static, which introduces no
first-party DLL ABI. Windows linked imports declare both the import library and
its DLL. Indirect DLLs such as Ogg, ICU data and mimalloc's redirector use private
`cc_import` targets with only `shared_library`, reached through `deps`. On the
MSVC toolchain these contribute adjacent runtime files without adding DLL link
arguments. These imports are generated only for an admitted Windows SDK; Unix
shared-library imports have different link semantics. The standard rules_cc
`copy_dynamic_libraries_to_binary` mechanism owns deployment for binaries, tests
and compiled catalog tools; `data` alone does not establish adjacency. Each
configured SDK projection rejects case-insensitive DLL basename collisions
between distinct sources, and WinUI checks the combined SDK payload again.
There are no executable deployment companions.

CLI/TUI retain the existing executable-local `mimalloc-new-delete.h` integration.
This preserves that C++ allocation interface, not broad CRT `malloc`/`free` interception or arbitrary cross-DLL
allocation redirection. Ordinary tests do not link mimalloc. Catalog tools are
a separate execution boundary: the compiled catalog compiler and actual
`genrb`/`pkgdata` tools retain their declared inputs and runtime guards,
including the declared DLL runtime closure of the same `x64-windows`
triplet's host tools. Catalog generation and the compiled execution tool still
need native validation.

The [Windows manifest aspect](../../bazel/windows_utf8_manifest.bzl) checks
first-party C++ executables through `deps`/`data` and declared exec `tools`,
including the default module tests, their sibling probes, explicit performance
executables, product frontends and the execution-configuration catalog compiler. `_validation` actions declare the EXE, verifier script and selected
SDK/system executable bytes, producing extracted XML and a success artifact;
unchanged inputs reuse Bazel's cache. Request the checks explicitly with:

```bat
ao.bat build --target //test:windows_utf8_manifest_checks
```

This target also materializes the declared XML/stamp outputs for inspection.
A cached `_validation` result on an ordinary test need not leave those files
materialized in the output tree; absence there alone is not a validation failure.

Ordinary managed Windows build/test/run includes validation and rejects disabling
it. Failure fails the command and prevents `run` launching, but validation need
not precede individual test execution. Direct custom overrides are outside the
managed contract. Installed `mt.exe`/PowerShell runtime locations remain in use;
`PATHEXT=.EXE` enables PowerShell's native invocation. Resource verification is
separate from compiler `/utf-8` and observed effective runtime code pages.

## Native hosts and caching

macOS uses governed Homebrew LLVM 22, the SDK, deployment target and libc++
expected shim with the tree's provisioned vcpkg artifacts; the launcher, not Bazel,
bootstraps the pinned vcpkg, and Bazel does not bootstrap CMake/Ninja. Windows uses Visual Studio, its selected SDK, reviewed dynamic portable target
dependencies and the existing dynamic CRT. No global `/MT` or replacement CRT
flag is added; upstream toolchain mode features still select `/MDd` and `/MD`.
Native input identity follows each toolchain's trust boundary. Linux hashes
its selected Nix compiler inputs. macOS declares the selected LLVM tools, the real
`xcrun`-selected linker, the governed libc++ shim and the driver's explicit
LTO plugin as bounded inputs; the installed SDK, libc++ and compiler-resource
content is trusted state behind small selection metadata, not a byte ledger.
Link actions select that linker rather than relying on the
`/usr/bin/ld` dispatcher; compiler
and link actions disable implicit Clang configuration files. On Windows the
explicit auto-configuration selection ([Windows compiler
configuration](#windows-compiler-configuration)) owns compiler admission; the
captured `compiler_environment` request still serves WinUI and dependency
admission, and no whole-SDK byte ledger is required. Directory membership
changes also invalidate resolution.
Shell-backed rules need `BAZEL_SH`; the Windows portal selects the
`usr/bin/bash.exe` that Git for Windows installs beside `git`, unless the caller
sets `BAZEL_SH`. Bare Windows callers must make Git Bash discoverable by Bazel
or set `BAZEL_SH` explicitly.

Keep selected SDK/tool installations stable while resolution and actions run;
admission is not an atomic installation snapshot. These are native
installation inputs, not a packaged relocatable SDK or a complete
syscall/dynamic-loader closure; the project does not emulate loader search paths.
Runtime libraries, .NET/registry state and other inputs outside the enumerated
families remain trusted host state; changing
those is not certified by the selected SDK identity. Linux similarly trusts
immutable native Nix SDK state. None is an RBE contract. Changing the host's
installation selection — Visual Studio, Xcode or SDK selection — can require
`bazel fetch --force --configure` before repository rules observe the new
selection; the bootstrap treats it as watched host state, not a complete
registry-drift guarantee.

Windows Bazel commands require a local source root and host-local NTFS output;
UNC and mapped-network working roots are rejected without a network-build
fallback. [Windows source routes](windows.md#source-checkout-routes) defines this
portable path contract and the independent checkout-identity and Git-discovery
interfaces. Selecting an editing host, source transport, synchronization method,
or machine-specific storage layout is an operator concern, not Bazel or portal
provisioning. A readable share or successful dependency inspection does not
establish support for network-source compilation.

[.bazelignore](../../.bazelignore) excludes local state, outputs and tool caches
such as `.aobus`, `.cache` and `build`. They contain no packages, and without
`--watchfs` each invocation re-checks every directory a `//...` pattern has
listed. `REPO.bazel` `ignore_directories` patterns would only filter packages:
Bazel still lists the directories beneath them. A tooling test keeps tracked
files outside every ignored prefix.

Bazel retains per-output action state and a shared disk cache at the managed
state root's `cache/bazel`. Generated launch rc files set
`--experimental_disk_cache_gc_max_size=50G`. This target applies to the shared
cache directory across workspaces/trees, not separately to each tree.
It is an idle-time GC target, not a synchronous disk quota: the cache may exceed
it during active builds or before Bazel's default five-minute idle delay elapses.
The portal does not manually purge the shared cache. See [Bazel disk-cache GC](https://bazel.build/remote/caching#garbage-collection).
A trusted remote cache is optional; forward
`--noremote_upload_local_results` for read-only remote use. Cross-tree/worktree
hits still require matching inputs, toolchain, environment and paths.
`//test:audio_test`, `//test:audio_platform_test` and, on Linux,
`//test:systemmedia_test` carry Bazel's `external` tag and execute on warm
invocations of the default aggregate. Catch case tags are unchanged; cached results
from other tests are not fresh binary executions.

The [Linux workflow](../../.github/workflows/ci.yml) runs
`ao check --fresh` (Debug) as its product gate. The separate required `ao hygiene` gate and the explicit
portal tooling run stay fatal independently of the check.

The same workflow transports only
`${{ runner.temp }}/aobus-state/cache/bazel` using pinned cache restore/save
actions. The Bazel steps explicitly set `AOBUS_STATE_ROOT`.
The versioned OS/architecture/pin-contract/commit key and same-contract fallback
select candidate archives, not ABI compatibility; Bazel action keys decide reuse.
Only successful pushes to `refs/heads/main` may save, and only after the
Debug check, the separate Release compile verification, the Linux
instrumentation analysis, the mandatory production lint outcomes and the
required hygiene and portal-tooling gates
all succeed. PRs and manual dispatches never publish; GitHub's branch
restore rules may let untrusted PRs reuse main's cache. After restore and before Bazel runs, CI creates a marker outside the cache.
Before saving, it removes only cache files whose mtime is not newer than that
marker, retaining the current run's used/produced working set rather than
accumulating old blobs through the fallback archive. This uses the pinned
Bazel's cache-access mtime behavior; it is not a fixed archive-size quota.
CI does not wait for idle GC. Failed trimming prevents publication, without
bypassing the required gates. Transport failure is optional, not failure of
those gates.
The archive excludes servers, install bases, output trees, repositories, provider
installations and Nix state; it adds no remote endpoint or dependency manager.

### CI product gates and R2 validation

The [CI workflow](../../.github/workflows/ci.yml) owns product compilation,
tests and check work through the Bazel completion gate. The required job
identities are Linux / check, macOS Intel/ARM64 / check and Windows / check;
a separate unconditional documentation job keeps `ao docs check`, the
docs-only routing and the failed-prerequisite behavior. Each native job runs
`ao check --fresh` in Debug — the explicit fresh admission, not a
narrowed scope — and a separate required `ao hygiene`; Linux
and Windows additionally run the explicit portal tooling suite. The former
advisory/manual `*_bazel` product jobs and the dispatch `mode` selector are
removed; only the explicit `r2` boolean remains. Native SDK provisioning
keeps the existing `ao setup bazel`/`ao deps provision` routes. Package providers and
isolated tooling fixtures may still use CMake internally; CI configures or
builds no Aobus CMake product graph. This implies no
sanitizer or coverage runtime, GUI-performance, release or full Release matrix
acceptance. Linux additionally runs the
[instrumentation smoke](#ci-instrumentation-smoke); the documentation
job remains SDK-free.

The check keeps its bounded completion scope
([Completion gate](#completion-gate)): portable tests, the native backend
suite, Linux headless GTK and the lint fixtures run; manual GUI
and performance harnesses are built, never run; Windows executes only the two
unattended native console probes, and no new AppKit GUI smoke executes.
Linux retains its separate Release compile verification, and every host
retains the bare direct-entry acceptance: a direct Debug TUI run plus a
Release TUI and canary build through the pinned executable in a fresh output
root. The macOS Release build also links the AppKit bundle, covering the mixed
C++ ThinLTO and native Objective-C++ product without launching its GUI.
The Linux production lint responsibilities — the checked production
source, the unpaired public header and the Release cache-clean report — and
the real Release lint-analysis probe stay independent of the check gate. The
Windows WinUI naming gate migrates the exact same real automation probe: CI
runs it through Bazel after the check, still mandatory, with the existing
runtime and interactive-console admission and no login or system bypass.

CI retains immutable per-attempt check and compile-contract verification
receipts, phase logs and BEP files beside the lint, analysis and direct-entry
receipts on every attempt; Windows keeps its separate artifact roots. The required macOS and Windows
jobs save their existing dependency and managed-tool caches under the
preserved restore/seal/key policies. The local
[phase-capture contract](build.md#measure-native-validation) is unchanged.

The Bazel disk archive keeps its trusted-main key, fallback, marker, trim and
save contract ([native hosts and caching](#native-hosts-and-caching)): only
trusted main pushes publish, after the check, Release verification,
instrumentation analysis, mandatory production lint and required
hygiene/tooling outcomes. Automatic R2 use
remains restricted to trusted main pushes gated by
`AOBUS_BAZEL_R2_CACHE_AUTHORIZED=true`; routine PR runs get no R2 binding. A
manual `r2=true` dispatch explicitly trusts the selected checkout: native
Linux/macOS/Windows jobs use run/attempt-isolated `bazel-ci-validation/` prefixes, separate from
the automatic main cache's `bazel-v1/` prefix, require a strictly nonempty
proxy URL and a successful drain, and fail rather than silently passing with
local cache only. This reuses the existing `R2_SCCACHE_ENDPOINT`,
`R2_SCCACHE_ACCESS_KEY_ID` and `R2_SCCACHE_SECRET_ACCESS_KEY` secrets and the
`aobus-sccache` bucket; no new bucket or credential is required.

This manual selection explicitly trusts the chosen workflow and checkout.
Reused bucket-level writer credentials retain their write rights, including in
an upload-disabled consumer. Prefix separation, a YAML condition and disabling
upload workers are **not backend authorization**. Repository-level secrets
remain addressable by a malicious same-repository workflow edit; this route
does not claim to isolate that threat. Fork/secretless runs cannot perform the
requested live validation.

The [remote-cache helper](../../script/ao/core/bazel_remote_cache.py) downloads
the version and SHA-256 that
[`bazel-remote.json`](../../script/ao/bazel-remote.json) pins. Each cached Bazel
step selects its loopback URL explicitly. Startup
rejects occupied fixed ports and checks that its own child is live even when
HTTP status answers. Shutdown first waits for uploads with a bounded drain,
matching acknowledged writes and backend completions by object kind/hash;
timeout or stalled progress does not imply completion. An optional structured
stop receipt records prefix, traffic, failures and unfinished uploads without
credentials. Expired backend blobs make bazel-remote's gRPC dependency check
miss; a lifecycle rule is a storage/retention choice, not build correctness.

bazel-remote refuses Windows builds, so the Windows job hosts the pinned
Linux binary in WSL2. The helper imports a throwaway distribution from the
base rootfs that the same manifest pins, copies in the verified binary and
starts it detached; WSL forwards its loopback listeners, so Windows Bazel
steps use the same `grpc://127.0.0.1:9092` URL. The base rootfs has no trust
store: the runner image's Git for Windows CA bundle supplies it through
`SSL_CERT_FILE`. Only the three backend variables named in `WSLENV` enter the
distribution, and the call that carries them never echoes shell output.
`stop` reads the access log from the distribution, drains, signals the Linux
process and unregisters the distribution. This depends on the hosted
`windows-2025-vs2026` image's WSL2; where the import or start fails, an
automatic run keeps its local Bazel cache and a manual `r2=true` run fails.
The Windows job publishes no Bazel disk archive.

## Completion

`./ao build` does not run formatting, hygiene, Ruff, mypy, docs checks or
`./ao verify`. Required executable validation is an artifact contract, not
a general quality gate. Direct bare `bazel build`/`test` success carries the
same boundary: a raw successful build is not a lint, hygiene or completion
result. `./ao test` does not replace `./ao check`,
tooling, lint, hygiene, physical-output or product validation; its explicit
`asan`/`tsan` profiles are scoped instrumentation runs
([sanitizer and coverage instrumentation](#sanitizer-and-coverage-instrumentation)),
not whole-completion substitutes, and coverage keeps its separate explicit
route. Select checks through
[validation and review](test/validation-and-review.md). `./ao check`
is the default local completion route ([Completion gate](#completion-gate)),
and hygiene and documentation validation stay independent gates.
Action checks do not transfer the graph's full ownership or replace native
tests.
Tests of the Bazel support layer follow the
[Bazel support test boundary](test.md#bazel-support-tests): generators, integration
mechanisms and specific failure contracts, not a second representation of the
complete product build. C++ tests executed by Bazel retain their own behavior
contracts.

Historical selected-graph tests and compiler-action evidence cover native
Debug/Release on Linux, macOS and Windows, not subsequent policy changes. Rerun
affected native gates after changes; Windows validation must cover the
selected SDK's builds, product startup, decoder workflows, allocator integration
and catalog pipeline, not merely archive inventory or receipts from a former
linkage.
The Windows auto-configuration migration supersedes prior Windows compiler-action
receipts; no fresh native validation of it is claimed until the affected gates
rerun.
Older macOS receipts predate repaired mode flags and do not by themselves prove
optimized Release or debug information. No physical playback, complete GUI,
ARM64, shipping/release, RBE or cold-build performance acceptance is implied.
Development build/run support does not establish distribution readiness; see
[binary distribution](dependency-governance.md#binary-distribution).
