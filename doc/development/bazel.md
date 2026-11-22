---
id: development.bazel
---

# Bazel build system

Bazel owns the complete native product build: Core, CLI, TUI, Linux GTK,
incremental macOS AppKit and the bounded native Windows WinUI product. The
ordinary workflow uses pinned native `bazel build`, `bazel test`, and
`bazel run` from the checkout root; see [native setup](#direct-bazel-without-the-portal).
Run `./ao check` for the full local [completion gate](#completion-gate). See
[validation and review](test/validation-and-review.md) to choose checks and
understand [CI routing](test/validation-and-review.md#continuous-integration).
The public portal has four commands: `setup` explicitly prepares components,
`format` rewrites source, `check` validates, and `measure` collects coverage,
performance, or concept metrics. There are no
ordinary lifecycle aliases or `ao bazel` command tree. Nix/vcpkg remain the
dependency version and
installation authorities; Bazel consumes their artifacts without configuring
an Aobus CMake graph or parsing installed CMake exports — vcpkg package
builds may still use CMake internally, and the repository retains the
CMake-language provider assets those ports need. BCR supplies build rules, not
application dependencies.

The cacheable native C++ lint reports come from the Bazel lint report actions;
[lint reports](#lint-reports) defines their workflow, cache and failure contracts,
`./ao check lint` consumes those actions for a selected scope, and
the completion gate consumes them through the declared `//:production_lint`
test, with the same STRICT/RELAXED policy. Explicit scoped sanitizer and
coverage instrumentation routes exist beside the ordinary fastbuild/Release
modes and explicit Debug instrumentation; [sanitizer and coverage
instrumentation](#sanitizer-and-coverage-instrumentation) owns their commands,
profile design and boundaries.

## Setup and commands

Run from the repository root. Ordinary build/test/run uses pinned native
Bazel; project commands use `./ao` (`ao.bat` on Windows).
Linux can use Bazel from the pinned Nix shell or a separately installed pinned
executable with a JVM and `nix-shell` available. On native macOS/Windows,
`./ao setup bazel` verifies or downloads the official pinned executable into
managed state with SHA-256 verification. `AOBUS_BAZEL` overrides the executable for diagnostics,
but still requires the pinned version.
The pins are owned by [bazel-toolchain.json](../../script/ao/bazel-toolchain.json),
[.bazelversion](../../.bazelversion) and [MODULE.bazel](../../MODULE.bazel).

```bash
./ao setup bazel
./ao setup deps --path /tmp/build/aobus-native-sdk  # native macOS/Windows only
# After native setup below, ordinary tests use the pinned executable:
bazel test //:tests
./ao check --config release --path /tmp/build/aobus-native-sdk
./ao check contracts --config release --path /tmp/build/aobus-native-sdk --jobs 2
```

On native macOS and Windows, `./ao setup deps` installs the shared vcpkg manifest
into the selected tree's `vcpkg_installed`
with the pinned vcpkg and the governed triplets, overlays and binary cache,
without a CMake configure, so unchanged package ABIs restore from that cache.
On Windows, `ao.bat setup llvm-sdk` is the second explicit provisioning
spelling: it calls the same native LLVM SDK installer as `./ao setup deps`. Ordinary
ready-only build, lint and format commands never install an SDK.
Windows target libraries
use the standard `x64-windows` triplet: reviewed dynamic portable
dependencies with the dynamic CRT. The same triplet supplies the host tools.
Windows `./ao setup deps` also
restores the locked [WinUI NuGet closure](#winui-product) and installs the
pinned [LLVM SDK](windows.md#llvm-sdk-and-native-lint-tools) for
[lint](#lint-reports) into the verified shared cache, under the
same lock and completion marker. Linux needs no
provisioning. Other actions use the tree's provisioned triplet; a ready
installed SDK is consumed, never installed implicitly, and
`--vcpkg-prefix <installed/triplet>` replaces the provisioned prefix with an
existing installed target-triplet prefix. A prepared portal request owns that selection;
`AOBUS_BAZEL_VCPKG_PREFIX` is the selector for bare Bazel commands. Debug library
and runtime directories must not resolve into the Release installation.
Rerun `./ao setup deps` after changing the vcpkg
manifest, configuration or overlays: Bazel itself never restores missing ports
or substitutes an ambient installation. An interrupted `./ao setup deps` waits for
the running installer process to exit before propagating the interruption and
releasing tree or shared-cache locks. Cancellation can wait for the tool's natural
completion; it does not contain detached descendants a tool leaves behind. The
[process runner](../../script/ao/core/proc.py) uses owned-child POSIX status polling
to avoid an interrupt-sensitive Python wait lock; an unavailable exit status is
an error, never a successful installation.
Follow [macOS setup](macos.md) or [Windows setup](windows.md) for native SDK
prerequisites and the [native source and storage requirements](#native-hosts-and-caching).
Native Windows Bazel uses governed base Python without provisioning Ruff,
mypy, or documentation packages. `ao check` requires the ready admitted Python
tools on every host; no graph target installs or repairs one, and standalone
scoped checks retain their own managed-tool admission.

`./ao setup bazel` prepares the pinned executable, `./ao setup deps`
explicitly provisions vcpkg/NuGet/LLVM, and `./ao check contracts` runs
[compile-contract analysis](#compile-contract-verification). `./ao check`
accepts `--config fastbuild|debug|release` and
optional `--asan`/`--tsan`, whose normal completion defaults to fastbuild.
See [completion](#completion-gate) for CI selections and evidence.
Common project-command options include `--path`, `--vcpkg-prefix`, `--jobs`,
`--remote-cache`, and the Linux target-compiler selector `--compiler=gcc|clang`
(no `--clang` alias); availability depends on the command.
`AOBUS_BAZEL_REMOTE_CACHE`, when set, supplies their remote-cache default.
Native Bazel instead uses standard labels and options: `--config=release`,
`--jobs=N`, `--remote_cache=URL`, and startup output-base selection. It does
not interpret portal tree or cache environment variables as command options.
Use native `query`/`cquery`/`aquery`/`fetch` for graph inspection.

Catch2 receives native Bazel `--test_filter` as a test spec through
`TESTBRIDGE_TEST_ONLY`; its support reads that variable because the native
test setup exports `BAZEL_TEST=1`. Filter the executable that owns the case,
not an aggregate.
See [test suites](test/test-suite.md) for native filtering, repetition, and
caller-owned runtime prerequisites.

### Python package architecture

The `script/ao` Python packages split by role, all at one package level, with
root-level assets (`bazel-toolchain.json`, `windows-llvm-sdk.json`,
`bazel-remote.json`, `guardrails.py`) keeping their place. The roles are
semantic, not filename prefixes:

- [`core`](../../script/ao/core/) holds the eight shared primitives — paths,
  the process runner, jobs, phases, the build lock, the command registry,
  native-input admission ([bazel_native.py](../../script/ao/core/bazel_native.py))
  and the package init — with no Bazel launch or provisioning logic.
- [`policy`](../../script/ao/policy/) holds static declared source policy:
  compiler/tidy/lint policy and configuration readers, the native contract's
  pure oracles ([bazel_contract.py](../../script/ao/policy/bazel_contract.py)),
  mode buckets, dependency and concept scope, the lint fixture inventory and
  the Windows LLVM SDK declaration. Policy modules read declarations; they
  import no Portal module and no process/network helper.
- [`graph`](../../script/ao/graph/) holds declared-execution helpers: the lint
  action/report/result modules, lint fixture execution, the test
  runner/services/context/unittest helpers, runfiles-backed fixtures and the
  AppKit process supervisor. Graph modules import no Portal module and no
  command module.
- [`portal`](../../script/ao/portal/) is the composition root the `ao`
  commands build on: host providers and the resolver, SDK preparation (vcpkg,
  NuGet/WinUI, lint SDK), live Git state, the Bazel client/launcher, and the
  native-facts live checks ([bazel_verify](../../script/ao/portal/bazel_verify.py),
  [repository checks](../../script/ao/portal/bazel_repository_checks.py),
  [native smoke](../../script/ao/portal/bazel_native_smoke.py)). Bazel-launched
  live checks belong here — not to every `bazel_*` module — because they
  observe external state.

Concrete cuts behind the split: the native contract's pure oracles live in
policy while the Portal [bazel_verify](../../script/ao/portal/bazel_verify.py)
driver builds the folded facts;
[compiler_policy.py](../../script/ao/policy/compiler_policy.py) reads
declarations while the Portal [compiler_rc](../../script/ao/portal/compiler_rc.py)
module writes rc files; the fixture runner in
[graph/bazel_lint_fixture.py](../../script/ao/graph/bazel_lint_fixture.py)
uses the policy-owned toolchain DTO and carries no Portal engine; and the Portal
[bazel_lint](../../script/ao/portal/bazel_lint.py) entry does not call back
into `ao check lint` command code.

Public command parsers adapt to shared Portal execution services rather than
calling other command modules: [completion](../../script/ao/portal/completion.py)
owns managed test selections and evidence admission, [source_format](../../script/ao/portal/source_format.py)
owns formatting, [source_lint](../../script/ao/portal/source_lint.py) owns scoped
diagnostics, audits, and workflow lint, and
[source_analyzer](../../script/ao/portal/source_analyzer.py) owns path-sensitive analysis.
[documentation_checks](../../script/ao/portal/documentation_checks.py) owns global
document semantics, [dependency_checks](../../script/ao/portal/dependency_checks.py)
owns SDK checking, [contract_checks](../../script/ao/portal/contract_checks.py)
owns standalone compile-contract analysis, and
[setup_actions](../../script/ao/portal/setup_actions.py) owns explicit setup.
[measurements](../../script/ao/portal/measurements.py) dispatches coverage and
concept operations; [performance_review](../../script/ao/portal/performance_review.py)
owns performance execution and provenance.

Import direction is one-way — core ← policy ← graph ← portal, with core also
usable alone — and the AST-based package-boundary test
([test_ao_package_boundaries.py](../../test/script/test_ao_package_boundaries.py))
verifies it over every declared source file. Lint fixture inputs are declared
through the `//:ao_lint_fixture_sources` filegroup (`LINT_FIXTURE_SOURCES`),
not the broad `ao_python_sources` closure. Cacheable fixture and replay actions
use their declared dependencies. Shared native and live-checkout test helpers
use official runfiles resolution for declared files, not a checkout-marker
search. On Windows, the Python unit and repository-check entry points try to
replace Bazel's deep `TEST_TMPDIR` with a short, private temporary directory.

### Direct Bazel without the portal

Use bare native Bazel commands from the repository root for ordinary
builds, tests, and launches. On Linux, SDK resolution needs neither a pre-entered
Nix shell nor a generated `aobus.bazelrc`: the tracked [repository rc](../../.bazelrc) supplies the default fastbuild
mode, the GCC default target compiler and both registered native Linux
toolchains, host toolchain selection and repository-channel clearing, and the
[`aobus_environment` repository](#architecture) prepares the pinned Nix SDK by
entering `nix-shell --pure` itself. Ordinary Linux `bazel build`, `bazel test`,
and `bazel run` must not require the caller to enter a Nix shell first.

**Linux test limitation:** SDK resolution does not populate the test `PATH`
with host fixture tools. On Linux the native audio and GTK test targets inherit
`PATH`. Their private audio fixture requires `pipewire`, `wireplumber`, and
`pw-cli` from the Nix store, not arbitrary host installations. The `//:tests` members
`//test:audio_test` and `//test:audio_platform_test` also look up `dbus-daemon`
on `PATH`, as does `//test:backend_test`. `//test:systemmedia_test` and
`//test:gtk_test` instead declare the governed D-Bus daemon. Only the GTK
fixture needs `Xvfb` on `PATH`; ordinary `//:tests` does not select it.
If tools are missing, run the affected target from the pinned shell, for
example `nix-shell --run 'bazel test //:tests'`. Let the wrapper start the
fixtures; do not substitute personal services.

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
Bare commands default to fastbuild with GCC as the Linux target compiler;
`--compiler=gcc|clang` selects the actual target compiler
([Linux toolchain and modes](#linux-toolchain-and-modes)); `--config=release`
selects `opt`. The explicit
`asan`/`tsan` instrumentation profiles are also available to bare commands
([sanitizer profiles](#sanitizer-profiles)); they are named rc configurations,
not Bazel compilation modes. Imported SDK
interfaces follow each consumer's actual compilation mode, so optimized
execution tools resolve their own Release interfaces, and fastbuild target
consumers alias the same Release SDK instead of falling back.

On native macOS and Windows, complete the [macOS host setup](macos.md#host-setup)
or [Windows prerequisites](windows.md#prerequisites) and provision the SDK
with `./ao setup deps` (`ao.bat setup deps` on Windows), or select an already
provisioned SDK. Bare commands need the governed base Python; repository fetch
never installs Python, SDKs, ports or the WinUI runtime. The managed Bazel
executable is not automatically on `PATH`:
use the pinned executable that `./ao setup bazel` prints, or a Bazelisk pinned
to `.bazelversion`.
`AOBUS_BAZEL_VCPKG_PREFIX` must explicitly select the installed target-triplet
prefix — `x64-windows` on Windows, whose host tools come from that same
triplet, or `x64-aobus-osx`/`arm64-aobus-osx` on macOS —
including its host tools and both Debug and Release artifacts. An invalid
explicit prefix fails; there is no fallback. Export it or pass it through a
standard local Bazel `--repo_env` configuration; the repository tracks no
absolute-path configuration.

On macOS, bare `bazel test //test/script:format_docs` and `//test/script:format`
need the state root where `./ao setup prettier` installed the formatter. For
the default location, run `export AOBUS_STATE_ROOT="$HOME/Library/Caches/Aobus"`
in the invoking shell; use your configured location otherwise. `--repo_env`
alone does not reach tests. Managed `./ao check` sets the state root for you.

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

Bare SDK admission requires both the complete selector tuple and its matching
initialized compiler environment. It rejects missing, partial or conflicting
selections rather than choosing a compiler or Visual Studio installation.
Upstream auto-configuration owns compilation; exports made inside a
repository-rule child cannot select another repository's toolchain.
For shell-backed actions, put Git for Windows' `usr\bin` on `PATH` or set
`$env:BAZEL_SH` to the full path of `bash.exe` before launching Bazel.
Finding `git.exe` does not guarantee Bash is on `PATH`. Setting only
`--repo_env=BAZEL_SH=…` does not select the client's shell for its strict
action environment.

After changing the Visual Studio, Xcode or SDK selection, repository
configuration may need refreshing before it observes the new selection.
Watched host inputs do not capture every change to an installation. A global
`bazel fetch --force --configure` is not a host-scoped refresh; see its
[Windows limitation](#windows-compiler-configuration). A warm Linux output
base reuses the fetched environment rather than re-entering Nix on every
invocation, but that is not a performance
guarantee, and private experiment metrics are not production acceptance. A
native build or test success is not a lint, hygiene or completion gate;
`ao check` retains complete validation and standalone `ao check lint` retains
scoped check-only validation.

## Scope

The graph covers static Core utility, LMDB/library, query, coroutine primitives,
encoded-media readers/containers, complete host-selected Core audio,
CoreRuntime/AppRuntime, UIModel, the real CLI/TUI entrypoints, and the complete
Linux GTK application and incremental macOS AppKit product with shared
desktop-launch composition. This does not establish GTK/WinUI feature parity.
The bounded native Windows slice additionally covers the real WinUI desktop
product ([WinUI product](#winui-product)); installation, signing, notarization
and complete release packaging remain outside this build scope.

Build or run the actual frontends explicitly:

```bash
bazel build //app/cli:aobus //app/tui:aobus-tui
bazel run //app/cli:aobus -- --help
bazel run //app/tui:aobus-tui -- --help
bazel test //:cli_tests //:tui_tests
# Linux only; native GUI execution is explicitly selected.
bazel build //app/linux-gtk:aobus-gtk
bazel run //app/linux-gtk:aobus-gtk
```

The Linux GTK library links desktop-launch, runtime, UIModel and catalog libraries. It also links the MPRIS adapter when system media is enabled.
The [GTK resource inventory](../../app/linux-gtk/BUILD.bazel) pairs each public
resource alias with its source file. The [resource rule](../../app/linux-gtk/gresource.bzl)
generates the GLib XML from those same declared files, so there is no second
search root where an undeclared file can shadow an asset. The generated C bundle
has its own standard library target and retained resource anchor; it is not a
source-tree runtime lookup. GTKmm, Boost.Process and the resource compiler come
from the pinned Nix provider, not a second installer. Cairo FT/FC metadata supplies
the font-backend headers exposed by Cairomm, without ambient Nix include flags.
GTK widget tests are not included by this product target, and a headless window
smoke does not establish Wayland, GPU, physical audio, desktop integration or
release acceptance.

### AppKit product

The [AppKit target](../../app/macos-appkit/BUILD.bazel) produces `Aobus.app`
separately from the smoke bundle. The GUI product is outside the default test
aggregate. For build and launch commands that keep the application away from
personal settings and library data, see [Run the AppKit product](macos.md#bazel-appkit-product).
GTK, WinUI and AppKit share the desktop-launch library. On macOS it links
static Boost.Process and Boost.Filesystem artifacts from the provisioned
vcpkg SDK.

Standard rules_cc `objc_library` targets own the mixed C++/Objective-C++
implementation, separate entry unit and smoke sources. ARC feature selection
uses the [governed Homebrew toolchain](#macos-toolchain-composition) for mode,
warning, SDK and external-header policy, not the default Apple/Xcode compiler. `alwayslink`
keeps all objects in the linked product. The local
[bundle rule](../../app/macos-appkit/bundle.bzl) declares the Info.plist template,
icon, brand license, catalog inventory and minimum-OS metadata as inputs. Its
native Python action uses the catalog SDK's declared host-absolute interpreter;
it is not hermetic. The graph builds the product and the separate smoke bundle,
including in Release. The [AppKit GUI smoke scenarios](macos.md#appkit-smoke-scenarios)
require explicit runs on a logged-in desktop. The graph does not install, sign
or notarize the bundle.
Product build/run support alone does not establish GUI or ARM64 acceptance.

### WinUI product

On native Windows, explicitly build or run the real WinUI product:

```bat
bazel build //app/windows-winui:aobus-winui
bazel run //app/windows-winui:aobus-winui
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
`n/<lock-id>/p`, which `ao.bat setup deps` restores
under its lock and completion marker; preparation selects it once complete,
and `AOBUS_NUGET_PACKAGES_DIR` selects another restored cache. The repository
rule consumes
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
runs no targets; the interface query runs its SDK initializers
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
link. The private generation project receives the IDL, both XAML pages and
their adjacent `.xaml.h` files. It keeps each paired `.xaml.cpp` as a logical
`ClCompile` item with `DependentUpon`, without copying the implementation or
creating a placeholder. Ordinary C++ sources and headers belong to the native
rules, not this project. The post-generation item check admits only the paired
sources and headers, SDK initializers and four standalone generated units.
Editing code-behind bodies or changing the ordinary C++ inventory therefore
does not invalidate XAML generation. A newly activated local XAML class may
require its implementation header in the generated type-info owner; a search
path or a name in `ClInclude` does not provide a C++ declaration.
SDK actions keep their private `TEMP`/`TMP` at a short path inside the
action's compact `p` tree because full-CLR MSBuild `Exec` appends GUID-named paths
against the legacy Windows path limit. The selected tree-path geometry remains
a constraint, and no automatic long-path fallback is provided. Bazel publishes
the SDK's `Microsoft.*` and `Windows.*` C++/WinRT projections and its runtime
support headers under `sdk_headers/` as system includes. `headers/` remains an
ordinary include root for Aobus projections, XAML and other generated C++.
Only complete SDK namespace families (the public header and its three `impl/`
layers) and the known runtime headers qualify; unfamiliar layouts stay normal.
Both roots preserve include spellings and each file has exactly one generated
provider. Compilation and lint consume the same native include roots. The Windows
native policy test checks each consumer's expanded MSVC arguments against those
two producer-owned directories, including consumers reached through private dependencies.
An action using another record's known projection root without declaring that
projection also fails. This comparison does not infer a missing dependency
when both its metadata and its include paths disappear.
The narrow
[rules_cc API patch](../../bazel/rules_cc_external_includes.patch) exposes the rules_cc
`external_includes` field to code generators: on the pinned MSVC
toolchain it emits `/external:I`, whereas `system_includes` emits ordinary `/I`.
It does not change the toolchain flags for other roots. XBFs are separate
declared outputs. MSBuild projects, logs and intermediate metadata
stay in private diagnostics, outside the compilation context and the staged
application. PRI generation consumes the catalog's RESW output, not ICU's
private build files. Staging consumes the finished PRI, the two XBFs and the
content assets directly, not the MSBuild log or temporary project. SDK import
libraries remain DLL-interface imports beside the reviewed dynamic vcpkg
dependencies, so this is not a DLL-free executable. The default completion and
GUI-scenario orchestration are owned by the Bazel completion gate and the
explicit desktop suites. Bazel also exposes the native
checker, lint and explicit probe targets, but build/run availability is neither
WinUI functional acceptance nor complete GUI acceptance. This route adds no
signing or install workflow.

CLI retains the complete-runtime/Unicode-only link boundary: no interactive
catalog or ICU i18n library. TUI embeds the full maintained/pseudo catalog and
uses UIModel, FTXUI and platform signal/terminal adapters. On Linux the
Bazel feature `--//bazel:system_media_enabled` defaults to `true` and controls
whether TUI and GTK link the shared MPRIS adapter. Passing `--system-media=off`
to an enabled TUI prevents the adapter from starting; it does not change the
build. `//test:systemmedia_test` owns the adapter contracts and real-TUI
PTY/private-bus cases, with declared product runfiles and the governed D-Bus
daemon. It is a fixed member of `//:tests` and compatible only on Linux with
system media enabled; `//:tui_system_media_tests` selects the same executable.
Benchmarks stay outside ordinary tests. Help/version smokes alone do not prove an interactive
terminal session or audible playback.

The [root BUILD](../../BUILD.bazel) owns the ordinary `//:tests` aggregate and
subsystem convenience suites. Aggregate membership is non-configurable;
platform compatibility belongs to the member executables.
[test/BUILD.bazel](../../test/BUILD.bazel) declares test membership by module.
Ordinary tests use `bazel test //:tests`; products are selected by their
native labels. The broader [completion roots](#completion-gate) supply selections
for compile-contract analysis, sanitizers, concept measurement and lint.
Raw sanitizer labels remain explicitly selected,
caller-owned runs ([sanitizer scope](#sanitizer-profiles)), excluding the
uninstrumented lint host. Runtime keeps its
fatal self-reentering main; catalog and shared desktop contracts have their
own module tests. The portable WinUI model is shared by product and
cross-host tests, while native WinRT cases join that frontend owner only on
Windows.

GTK and native-backend tests remain explicit service/hardware boundaries. They
carry `manual`, so a bare label wildcard does not select them directly; the
non-manual `//:check_tests` suite lists them, so completion's wildcard test
expansion runs them:

```bash
# Build native tests without claiming their runtime prerequisites are ready:
bazel build //test:gtk_test //test:backend_test  # GTK is Linux-only
bazel build --config=release //test:ao_perf_baseline //test:ao_tui_bench
```

On Linux an explicit `bazel test //test:gtk_test` or
`bazel test //test:backend_test` starts only the private fixtures declared by
that target's context. Both get a private no-activation D-Bus session and
private audio services. Only GTK gets an owned Xvfb display and a headless
child profile set before GTK selects its accessibility and input-method
backends. Supply the [native SDK and fixture tools](#direct-bazel-without-the-portal),
not hand-started services. GTK never adopts personal desktop endpoints or
the caller's bus; separate GTK shards need separate displays and buses to avoid
popup round-trip races and canonical MPRIS name contention.
The GTK test has its own main, accessibility/rendering profile and GApplication
probe. Native backend/hardware filters require the same admission as any native
test route; compilation does not authorize real playback. Performance executables
are manual, with independent entry points; plain `opt` compilation is not the
managed Release IPO performance baseline. AppKit smoke and the WinUI GUI
probes have independent native entry points and bundle/resource identities.
They require explicit runs, except that desktop-enabled Windows wildcard tests
run `//test:winui_uia_test` on an authenticated interactive console. The two
WinUI console probes run without that desktop opt-in.

Library/doc/WinUI source inputs use exact declared runfiles, including manifest-only
Windows execution. The WinUI source-inspection case receives a manifest of authored
inputs instead of recursively reading an undeclared checkout.
Explicit labels retain normal Bazel meaning: `//...` is not rewritten and may
be expensive on a shared source filesystem.

Audio includes every Core implementation for the selected host, not just
NullBackend. `//test:audio_platform_test` covers Core discovery and provider
contracts; the native backend cases have the explicit
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
sanitizer check runs normal completion first (fastbuild by default, Debug with
`--config debug`), then the managed sanitizer profile in that same tree.
Do not reconfigure a tree during another invocation or remove an active tree or
lock: the portal serializes writers and Bazel owns its server/output-base lock.

The [workspace launcher](../../script/ao/portal/bazel_workspace.py) writes:

- `<tree>/configuration/request.json`: mode-neutral native host, selected
  provider prefix and the original checkout source;
- `<tree>/configuration/aobus.bazelrc`: launch environment, output roots,
  startup `--max_idle_secs=900`, and a disk-cache path only when explicitly overridden;
- `<tree>/build.log`: noninteractive invocation output.

Each tree has an explicit `--output_base=<tree>/state/output`; its server,
analysis and materialized repositories stay separate. A shared native
`--output_user_root=<managed-state>/bazel` lets Bazel select installations by its
embedded key, not just the version string. Repository downloads use Bazel's
default content-addressed cache under that shared root. The [shared local action
cache](#shared-local-action-cache) has a separate location; the repository rc
keeps the repository contents cache disabled.
Install-base GC is disabled as a retention choice: Bazel re-extracts a missing
installation and owns invalid-install repair. Convenience links use
`<tree>/bazel-`. Bazel keeps external repository state in its output base.

To reuse a tree prepared with a legacy layout, reprepare it before running
Bazel. Preparation selects `state/output` without moving or deleting the
legacy hashed output base, installation or `state/cache/repos` downloads.
Bazel does not reuse the legacy server's in-memory analysis state.
The launch configuration requests a 900-second idle timeout for new managed
servers on all hosts and profiles. Preparation itself does not stop or restart
an existing server. On-disk caches survive server exit, but the next invocation
pays startup and analysis warm-up costs.

After preparation, use the pinned executable with the generated rc from the
same checkout for diagnostic work:

```bash
bazel --bazelrc=/tmp/build/aobus-bazel-fastbuild/configuration/aobus.bazelrc test //:audio_tests
```

No hidden artifact refresh is required. Missing configuration fails with setup
instructions. Reprepare when changing provider/profile; an old rc does not
certify another host environment or replace native prerequisites. Bare
commands that skip the generated rc entirely are covered in
[Direct Bazel without the portal](#direct-bazel-without-the-portal).
The portal pairs Debug/Release providers with Bazel `compilation_mode=dbg|opt|fastbuild`;
imported APIs use select guards requiring the matching mode — fastbuild consumers
alias the Release SDK — while host execution
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
It runs the [environment helper](../../script/ao/portal/bazel_environment.py)
and publishes a mode-neutral host environment. Linux enters the pinned
`nix-shell --pure` itself, independent of the caller's Nix environment;
Nix may download or build missing pinned store outputs. macOS uses the installed
Homebrew Python, LLVM and pkgconf and the selected SDK; Windows uses the governed
base Python. For managed launches, the [Windows portal](../../script/ao/portal/windows_portal.py)
runs the [build-environment batch script](../../script/ao/windows-build-env.bat)
and captures the x64 Visual Studio environment before launching Bazel. For bare
launches, the environment helper validates the caller's initialized x64
environment and [three matching selectors](#direct-bazel-without-the-portal);
it does not run the batch script. Native macOS/Windows discovery never installs
SDKs, ports, Python or runtimes. The Python discovery helper itself only validates
ready inputs; an invalid explicit selection fails without fallback. Every projection repository
— dependencies, tools, the Unix compiler toolchain and the lazy lint SDK/policy —
consumes that shared `environment.json` and its recorded interpreter instead of
per-invocation portal state.

The portal is a launcher, not an outer dependency freshness engine.
[Repository rules](../../bazel/dependencies.bzl) invoke the
[resolver](../../script/ao/portal/bazel_resolver.py), which composes independent
native observations without importing the launcher. Responsibilities follow the
input contract:

- [Native inputs](../../script/ao/core/bazel_native.py) own host/path admission,
  sanitized child environments and scoped probe observation; observers are
  restored on exit, not assigned through launcher globals.
- [Providers](../../script/ao/portal/bazel_providers.py) admit Nix report identity
  and installed vcpkg status/features; [library interfaces](../../script/ao/portal/bazel_dependencies.py)
  apply reviewed metadata repairs and ABI checks through the artifact
  adapter and header-ownership owner.
- [Host tools](../../script/ao/portal/bazel_host_tools.py) observe selected
  executables and their declared ICU/Python/runtime inputs.
- [Unix compiler probes](../../script/ao/portal/bazel_compiler_probe.py) admit
  exact installed tool/include interfaces and raw host/Nix metadata. Linux keeps
  an aggregate Nix byte ledger for each pinned compiler; macOS admits selected
  tool readiness and small
  selection metadata under the installed-SDK trust boundary
  ([macOS toolchain composition](#macos-toolchain-composition)). They do not
  return Bazel labels, CPU constraints, projection destinations or per-mode
  action environments. The Windows manifest-tool helper is owned by host tools
  below.
- [WinUI SDK admission](../../script/ao/portal/bazel_winui.py) validates the installed
  NuGet/MSBuild interface, native tool closure and COFF roles; it reports provider
  membership and files, not Bazel destinations or declarations.
- [Lint SDK admission](../../script/ao/portal/bazel_lint_sdk.py) validates the ready
  LLVM tools, archive closure and ABI interface, independently of product SDK imports.
- The [launcher](../../script/ao/portal/bazel_workspace.py) owns managed Bazel,
  output roots, explicit action-cache overrides, the governed `build`/`clean`
  invocations and logs. The repository rc selects the default action cache.

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
their own Debug or Release configuration. On Linux and on Windows with vcpkg,
Debug and Release share a sparse header projection for the same package only
when the complete, nonempty header records (including source paths) and ordered
include roots match. A mismatch keeps the two projections separate. macOS does
not share them. Shared files live under `artifacts/_headers/` in `release/`
(or `catalog-release/` for the catalog SDK). On Windows only Debug depends on
the Release header-only target; Release and fastbuild use the standard Release
compile interface. Libraries, defines, manifests and mode
guards are never shared by this comparison. It does not rehash header contents.
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
a directory `pycache_prefix` under the repository-owned `_tmp`.
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

[Production Core media](../../lib/media/BUILD.bazel) does not depend on the
application runtime. Runtime integration tests can link both layers without
adding a dependency to the media library.

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
- `@aobus_toolchain`: the governed Unix compiler identities — both pinned Nix
  Linux compilers or the selected macOS LLVM SDK. The extension declares this
  repository on all hosts, but ordinary Windows compilation uses rules_cc's
  default MSVC auto-configuration instead;
- `@aobus_winui_sdk`: the installed WinUI NuGet SDK's headers, bootstrap sources,
  import libraries, runtime payload and generation tools, projected lazily from
  the restored NuGet cache for the [WinUI product](#winui-product) and its shared projection.

Native, test and WinUI SDK repositories expose both Debug and Release
interfaces and alias them to each consumer's actual `compilation_mode`
(`dbg`/`opt`/`fastbuild`); fastbuild consumers alias the Release interfaces
instead of falling back. Execution tools resolve their own optimized
interfaces independently of the target mode.
Host-excluded native/test package names remain loadable at the repository root
so unconfigured queries can follow every `select` branch. These query-only
labels fail if selected for build analysis; they provide no empty SDK and never
silently mark a consumer incompatible. Only the policy's platform exclusions
produce these labels: a missing required SDK still fails resolution.

On Linux, the resolver inventories each distinct immutable Nix include root once
per configuration. A pkg-config or manual `-I` must name the admitted store
path, not a mutable alias such as `~/.nix-profile/include` that happens to
resolve there. The resolver checks symlink hops and keeps each header's lexical
spelling, canonical source, and digest. Only the inventory is reused across
packages: the generated include directories contain individual file links,
not links to provider directories. This preserves nested
includes such as RapidYaml's `../c4` without exposing provider siblings via `..`
in local builds. On every host, FakeIt exports only its reviewed Catch include
root: metadata advertising an additional root is rejected. On macOS and Windows,
[vcpkg header ownership](../../script/ao/portal/bazel_headers.py) then selects files
under that root from the installed per-port `.list`; listing a sibling integration
does not make it visible. No whole-triplet include directory is linked.
Header and library labels omit absolute provider prefixes for action-path stability,
not guaranteed cache hits. Linux shared Nix imports retain linker-only search hints
from their declared ELF runtime search directories. Indirect dependency lookup
therefore prefers those provider directories over command-line runtime paths that
might contain incompatible same-SONAME libraries. These hints do not inventory the
complete recursive runtime closure.

The [adapter](../../script/ao/portal/bazel_deps.py) and
[library interface resolver](../../script/ao/portal/bazel_dependencies.py) prefer metadata.
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
its version from the same contract entry and the actual provider,
with header macros and the genrb report required to match; the core/tools
split and static tool adjacency remain. A coherent SDK upgrade accepted by the
shared policy consequently passes the same structural checks without a
Bazel-side version edit — this is intentional. It is not a claim that the
structure certifies any future version's API/ABI compatibility: governed
upgrades still run the affected native validation. Structural preconditions
fail closed; missing or unexpected fields are not guessed, repaired with
blanket macros, or silently ignored. The stb overlay's fixed source remains
owned by its producer pins ([temporary
overlay ports](dependency-governance.md#temporary-overlay-ports)); the Bazel
header API checks are not proof of the undefined-behavior fix or of upstream
provenance.

A target that compiles against private runtime or audio headers must declare
its header interface explicitly. Tests use `//app/runtime:test_headers` or
`//lib/audio:test_headers` for those interfaces, not a broad include root.
The atomic-file transaction header also has a test-only interface in
`//lib/utility:test_headers`.

Non-definition compile flags are closed requirements, not generic `PACKAGE_COPTS`
for all consumers. Windows `/permissive-` is satisfied globally. On Linux,
each compiling consumer of PipeWire SPA headers must declare the named
`PIPEWIRE_SPA_COPTS` set in [compiler policy](../../bazel/compiler_policy.bzl);
a transitive dependency does not pass on those options. The named
`GTK_HEADER_SSE_COPTS` set applies to the x86_64 GTK header consumers, not
other CPUs. Unsatisfied thread/PIC/ISA requirements fail rather than disappear.
Defines and link requirements retain their own transitive semantics.

## Compiler policy ownership

Each actual toolchain owns its static compile/link flag values; they are not
rc-file, Python mode-branch or launch-JSON properties, and there is neither one
shared cross-compiler flag list nor an expectation of identical flags per
compiler. The shared [language policy](../../bazel/compiler_policy.bzl) owns
only the GNU-style values every supported compiler shares: the C++26 standard,
the Unix warning list, the common Linux SDK header requirement sets and the
GNU spelling of the LLVM RTTI/release ABI flags. Each toolchain's literal
profile values — modes, release IPO, sampling, sanitizers, coverage and its
named consumer compile sets — live in its own
[gcc_linux_policy.bzl](../../bazel/gcc_linux_policy.bzl),
[clang_linux_policy.bzl](../../bazel/clang_linux_policy.bzl),
[clang_macos_policy.bzl](../../bazel/clang_macos_policy.bzl) or
[msvc_policy.bzl](../../bazel/msvc_policy.bzl); a per-compiler test warning
opt-out, such as GCC's version-bound one, stays with its owning policy.
The named profiles, actions, features and `select` mechanics stay with the
toolchain and profile owners, so a value edit follows Bazel load/analysis
edges without a second copy of the wiring. The [repository rc](../../.bazelrc) selects
modes/toolchains, clears ambient repository/action channels, enables strict
action environments and requires Windows manifest validation; it contains no
raw Unix compiler/linker parameters, and supplies the Windows project profile
only through the upstream auto-configuration inlets of
[Windows compiler configuration](#windows-compiler-configuration).
The repository rc also sets the shared action-cache default and size target.
The launcher selects managed server and output roots and applies an explicit
portal cache override.
Repository clearing is separate from action clearing: `--repo_env==CPLUS_INCLUDE_PATH`
unsets the variable, rather than assigning an empty path entry. Linux target and
execution actions also leave `LIBRARY_PATH` unset: GCC interprets an empty value
as a request to search the working directory when linking.

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
feature for both target and host consumers, so `aobus_warnings`
owns the Unix warning policy; on Windows only the standard
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
The toolchain rules load their policy values directly. Python language and LLVM
compatibility consumers additionally declare the literal policy files they read
as repository/action inputs. Python returns Unix compiler facts and identity;
the checked-in repository projection creates tool links, data and the fixed call,
not copies of policy templates. Unselected hosts are not eagerly
loaded through separate main BUILD packages.

[Shared Unix warning values](../../bazel/compiler_policy.bzl) precede ordinary
user copts in the [Linux toolchain](../../bazel/linux_toolchain.bzl),
which adds the selected compiler's Linux-only diagnostics from
[gcc_linux_policy.bzl](../../bazel/gcc_linux_policy.bzl) or
[clang_linux_policy.bzl](../../bazel/clang_linux_policy.bzl), and the
[macOS upstream composition](../../bazel/macos_cc_config.bzl), which applies
its own [macOS policy values](../../bazel/clang_macos_policy.bzl).
Windows warnings come from the upstream inlets and standard features of the
[auto-configuration](#windows-compiler-configuration), not a local bridge; the
inlet and profile values are owned by [msvc_policy.bzl](../../bazel/msvc_policy.bzl).
The default-policy checker requires the full warning
contract; intentional opt-outs are not expected to pass it. With
`aobus_warnings` enabled, GCC Release links also pass `-Werror` to LTRANS:
compile-time warning promotion is not inherited by that late compiler stage.

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
the prepared provider. The registered Unix toolchains declare their inputs
directly rather than relying on a consumer macro or library dependency. Linux
records hashes for the selected Nix compiler inputs; macOS records the
[installed tool and SDK selection](#macos-toolchain-composition) in `identity.json`.
Windows records no local compiler identity ledger.

Consumer BUILD owners reference the named compile-policy sets the policy
modules export — GTK header SSE, PipeWire SPA, test warning and WinUI copts —
only for compile policy; which target applies a set stays with its consumer.
Header compatibility options belong to sources that actually consume those
headers, not every target with transitive access to them. In particular,
UIModel tests use the ordinary test warning set rather than PipeWire SPA's
aliasing/overflow options, preserving their normal C++ aliasing model in LTO.
Target-specific link libraries, resources, entrypoints, manifest/link-input
requirements and system frameworks remain consumer-owned. The package-local executable rules in
[test/executables.bzl](../../test/executables.bzl) attach the Windows UTF-8
manifest to every test executable and probe while preserving caller link options.
This executable artifact contract remains independently verified by the manifest
aspect and is separate from compiler `/utf-8`.
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
toolchains_llvm. The [pinned shell's report](../../shell.nix) identifies both Nix compiler
wrappers: GCC 16 and Clang 22.
The repository rc defaults the Linux target to `--compiler=gcc` and registers
both real toolchains (`registered_nix_gcc`/`registered_nix_clang`) keyed by
the `--compiler` value: `bazel build/test/run --compiler=gcc|clang` selects the
actual target compiler, and an unknown value fails toolchain resolution rather
than silently falling back to GCC. The selector is Linux-only. Preparation
uses the report to select SDK tools and read Nix wrapper hardening metadata.
It probes each selected wrapper for its standard-library and builtin include
paths, not the ambient `CC` or `CXX`. Execution configurations do not inherit the
target `--compiler` value: the registered native execution platform pins
`--compiler=gcc` on Linux, so compiled execution tools — the catalog compiler
and the LLVM lint checker — build with GCC independently of the selected target
compiler.
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
[compiler_policy.bzl](../../bazel/compiler_policy.bzl) that both Linux compiler
policies load, so it proves each captured first-party action carries that
declared list effectively, without wholesale suppression, not that the list
itself is right.
Only the per-compiler additions remain spelled independently in the checker.
Toolchain identity is declared data of
tools and policy arguments, including mode-specific hardening, so compilation
and link action keys include it.

| Mode        | Compiler policy                            | Nix hardening policy                         |
| ----------- | ------------------------------------------ | -------------------------------------------- |
| `dbg`       | `-O0 -g`, no `NDEBUG`                      | Supported policy except `fortify`/`fortify3` |
| `opt`       | `-O2 -g0 -DNDEBUG`, function/data sections | Full supported policy                        |
| `fastbuild` | `-O0 -g0`, no `NDEBUG`                     | Supported policy except `fortify`/`fortify3` |

Only optimization-dependent fortify is excluded in Debug/fastbuild; other
supported hardening remains. Each observed Nix wrapper owns its compiler's
supported hardening; the Clang wrapper filters unsupported `fortify3`
to level 2 in optimized mode. The common Linux compile policy preserves frame
pointers in every mode, including Release; the sampling profile is not the
only mode with frame pointers.
The portal exposes Debug, Release and fastbuild, with imported providers prepared and
guarded for `dbg`, `opt` or `fastbuild`; fastbuild consumers alias the prepared
Release providers.

## Release and sampling profiles

Release and sampling both use Bazel's `opt` compilation mode. Select them with the `release` and `profile` flavors respectively
(`--config release|profile` for `./ao check contracts`), never through
Bazel's generic `thin_lto`/`full_lto` features, and the repository rc selects
them with appendable `--features` entries so user and baseline features are
never silently replaced. Unix profile requests require the selected feature;
the boolean admission flag alone does not enable compiler options. Windows
Release instead uses its governed platform flags.
[Optimized builds](optimized-builds.md) owns the contributor-facing flavor contract.

- Unix Release enables interprocedural optimization for C++ translation units
  and final links through the selected compiler's own driver, including the
  Objective-C++-exclusive scope rules: `cpp_compile` and `linkstamp_compile`
  actions carry the Release C++-only policy, while Objective-C++ translation
  units retain normal Release optimization. On Linux, GCC uses driver-managed
  LTO (`-flto=auto` with non-fat objects and plugin-aware `gcc-ar`), while
  Clang uses ThinLTO (`-flto=thin`) with its vtable-localization workaround;
  neither enables Bazel's separate ThinLTO index/backend pipeline. Final link
  actions exclude archive
  actions from the IPO policy. Apple uses the `lto_object_path`
  mechanism; the generic Apple `thin_lto`/`full_lto` indexing is not used.
- GCC Release IPO links set `MAKE` to a declared GNU make helper from the
  [pinned shell report](../../shell.nix), rather than searching the restricted
  tool `PATH` and falling back to serial LTRANS. The helper is projected as
  `gcc/runtime/make`, outside compiler identity and `native_paths` inputs.
  rules_cc conservatively includes it in other GCC linker inputs too, but
  only Release IPO activates `MAKE`; ordinary compiler/archive inputs do not
  acquire it. With `-flto=auto`, each link chooses its own CPU-based
  parallelism; Bazel `--jobs` limits Bazel actions, not the LTRANS subprocess
  budget. This helper is specific to Linux GCC Release IPO.
- Windows Release applies MSVC `/GL` at compile and `/LTCG` at link through
  target-platform flags, with the exact per-file `/GL-` exception for the
  authored `app/windows-winui/MainWindow.xaml.cpp` on the selected MSVC 14.51
  toolset only. Standard `lib.exe` archives are not given `--linkopt`; LIB
  restarts in LTCG mode on `/GL` input. Because Bazel 9.2 platform flags
  replace all previous values of repeatable flags, native Windows Release
  callers must not assume their `--cxxopt`, `--linkopt`, or `--per_file_copt`
  values merge with that policy. Ordinary rule copts and standard baseline
  flags remain intact. `ao check` accepts no forwarded Bazel arguments.
- The Unix `profile` flavor is a sampling configuration: `-O2` with debug
  information and frame pointers, no IPO. It is not PGO, and no PGO workflow
  exists.
- Imported SDK interfaces follow the consumer's compilation mode. Release
  requires `@aobus_environment//:release_platform` as the target; a derived
  platform is not admitted. The `release_profile` constraint also matches a
  native execution platform for tests, but does not supply Release target
  flags. The ordinary unprofiled platform remains first for build tools.

The compile-contract verification checks effective
compile, link and archive arguments — including frame-pointer and no-LTO
sampling policy and the Windows per-file exception — rather than flag
membership alone. Native acceptance remains profile- and platform-specific:
analysis verifies the requested policy but does not replace the affected build,
link and runtime gates. Release evidence does not qualify the sampling profile.

## macOS toolchain composition

The [module graph](../../MODULE.bazel) fetches the pinned `apple_support`
implementation from BCR. The [local composition](../../bazel/macos_cc_config.bzl)
loads its public `cc_toolchain` macro. Aobus registers `registered_llvm22`
with the admitted Homebrew LLVM, SDK, deployment target and expected-header
shim, and disables automatic Apple Clang toolchain selection.

The [small external patch](../../bazel/apple_support_llvm.patch) selects an explicit
installed LLVM compiler while preserving its full child `argv[0]`, and lets the
caller replace automatic platform include/environment arguments. The wrapper rejects a present `TOOLCHAINS` selector rather than
falling back, and retains upstream response-file, path-placeholder, dSYM and strip
protocols. The platform-input seam keeps only the admitted include roots; upstream's
whole `/Applications` and `/Library` allowlists and automatic SDK selection do not
become Aobus policy. Retire the patch when the upstream public API supports both
installed-compiler selection and caller-owned platform inputs. The patch also
adds the wrapper's missing direct `<functional>` include: C++26 libc++ no longer
supplies `std::function` through its unrelated headers. Drop that include hunk
when the upstream pin declares the dependency itself.

A local instance of the public `apple_genrule` bootstraps the external wrapper
source with the selected LLVM at the project C++26 standard through the
[macOS policy](../../bazel/clang_macos_policy.bzl) entry flags, without
resolving the C++ toolchain it constructs. Its explicit command uses the
admitted developer, SDK, linker and host target selection. Compiler/tool bytes
and admitted SDK selection metadata are declared inputs, not compiler source
arguments. Repository fetch does not compile the wrapper.
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
It contains no SDK, libc++ or compiler-resource byte inventory. The projection
declares the selected tools, linker plugin and generated shim header as inputs.
Compile and lint actions find the header through its execution-root-relative
include directory. Moving the identical shim to another staging directory under
a new output base does not by itself invalidate those actions: the staging path
is neither an argument nor part of `identity.json`. Changing the header bytes
does invalidate actions that consume it. Implicit installed SDK content is trusted: a same-path, same-version rewrite of its headers is not guaranteed
to invalidate actions automatically. A governed SDK
upgrade must change the admitted identity and the affected cache inputs;
refetching unchanged metadata or selecting a new output base over the same disk
cache is not a blanket freshness cure. The Linux Nix toolchains retain their
byte ledgers, and the resolver watches only those Linux members. The
`ao measure concepts` consumer guards the identity file and actually
observed inputs without rehashing installed SDK members; ordinary
`./ao check contracts` checks compiler actions, not the trusted SDK content.

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
configuration so rules_cc registers the default MSVC toolchain.

This follows the installed-SDK trust boundary that the macOS toolchain also
uses; only the Linux Nix toolchains retain byte ledgers. Selected versions,
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
version token. These three base entries are owned only by
[msvc_policy.bzl](../../bazel/msvc_policy.bzl); the repository rc carries them
as a clearly marked checked-in generated view, so a normal checkout is ready
for bare Bazel with no generate-before-use command or wrapper. `./ao format`
on `bazel/msvc_policy.bzl` or `.bazelrc` rewrites that region, and the
`check format` stage fails a stale projection. The
[dependencies extension](../../bazel/dependencies.bzl) validates the
checked-in view with `ctx.read` before ordinary native build paths: the
governed Linux/macOS `--extra_toolchains` registrations and the Windows
manifest aspect force that extension, and a stale, duplicated or
outside-the-region `--repo_env` edit fails analysis. A bare `query` that loads
no governed repository, or arbitrary rc overrides, are outside that
validation claim. `BAZEL_CXXOPTS` supplies `/std:c++latest`; the upstream
`default_cpp_std` feature is disabled for both target and execution tools so
no redundant `/std:c++17` option produces a D9025 override warning. Upstream
still supplies `NOMINMAX` and `_CRT_SECURE_NO_WARNINGS`.
Native dynamic CRT defaults stay upstream: `/MDd` in `dbg`, `/MD` in `opt`
and `fastbuild`.
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
The Visual Studio installation and its SDK supply the compiler environment. Bare
Windows callers must supply the complete standard selector tuple and a matching
initialized x64 compiler environment, as shown in
[direct entry](#direct-bazel-without-the-portal). SDK admission checks the original
client selection before relying on upstream auto-configuration; it does not
attempt to influence that independent repository by exporting child variables.

The portal records `INCLUDE`, `LIB`, `PATH`, `TMP` and the original toolset and SDK fields in `request.json` under `compiler_environment` for WinUI and dependency admission. The generated launch rc supplies the three derived Bazel selectors as `--repo_env` options. The capture is
not a compiler-policy channel, a whole-SDK byte ledger or a hermetic SDK.
Governed host-tools discovery owns the Windows `mt.exe` manifest-tool helper.
Ordinary Windows builds must not require a Unix compiler.

**Windows refresh limitation:** Do not use the untargeted `bazel fetch --force --configure`
command to refresh a Windows SDK. It selects the Unix compiler repository
as well, and that repository's probe rejects Windows. Use ordinary builds or
tests to assess warm repository reuse, not to verify a forced refresh.

## Compile-contract verification

Run the explicit native Linux, macOS or Windows analysis gate separately from
build/test. For Linux/macOS:

```bash
./ao check contracts --config debug --jobs 2
./ao check contracts --config release --jobs 2
```

On Windows use `ao.bat check contracts` with the same options and a supported local
source checkout. UNC/mapped-network sources remain rejected, including by this
analysis entry. `--path` selects an external host-local Bazel tree as for other
Bazel commands, including its provisioned vcpkg triplet.

`check contracts` defaults to `fastbuild`. Linux and macOS also accept `debug`, `release`, `profile`, `asan` and `tsan`; Windows also accepts `debug`, `release` and `asan`, but not sampling or TSan. Other options select the path, provider prefix, jobs and Linux compiler. It accepts no labels or forwarded Bazel options. After provider preparation, the
[shared checker](../../script/ao/policy/bazel_contract.py) builds the folded
[native policy facts](#in-graph-native-policy) (`//:native_policy_inputs`)
under the caller's frozen prepared configuration and validates them with the
same compile/link/archive oracles the in-graph `//:native_policy` test runs.
The analysis universe is the folded completion roots (`//:check_cpp_tests`
plus `//:check_products` on every host), analyzed without compiling any C++:
the facts producer's single JSON write is the only executed action. The facts
must describe exactly the requested analysis configuration — a drifted frozen
configuration fails closed before any verdict or driver stage (a sanitizer
request passes its Debug analysis name, and the instrumented Debug facts
satisfy it). The single configured `DefaultInfo` output file is located by one
exact `cquery --output=files` of the facts target, never by guessing an
output-tree layout; the declared output filename stays the Starlark macro's
contract. The managed sanitizer route forwards the caller's trusted remote
cache to the facts build; the standalone CLI passes none. The test roots
cover the backend, CLI, lint and TUI members, plus GTK and system-media tests
when enabled on Linux. Native process tests own no C++ compile actions of
their own; they reuse the TUI executable and WinUI probes covered by the
product roots.

Checks cover effective C++ standard (`-std=c++26` or `/std:c++latest`), mode,
warnings, GSL, imported system includes and local SPA requirements; SPA checks
conservatively inspect every declared header input, not textual include usage.
Linux additionally requires its salted Nix hardening policy on target actions,
explicitly cleared ambient include/link channels, and an explicit GCC
hardening policy and archive for execution tools. Windows checks also cover
UTF-8, critical language/ABI defaults and Win10 definitions as known
switches/macros, not preprocessor execution, and admit only the exact authored
per-file exception sources against the frozen Release request's
`VCToolsVersion`. Native link/archive analysis also checks Objective-C++
actions; missing target Objective-C++ evidence fails, and this analysis does
not run GUI code. Zero first-party or required SPA matches fail. The captured
fixed-environment oracles do not replace the portal's closed launch
environment, strict action configuration or override restrictions; arbitrary
system/home rc overrides remain outside that managed guarantee.

On Linux, the analysis additionally expands one representative first-party
compiler wrapper with `-###`, resolving the declared SDK compiler through one
`bazel info execution_root` read of the prepared tree, not a query or graph
walk. It checks effective optimization,
fortify and stack protection without executing `cc1`; it is a representative
wrapper diagnostic, not literal or whole-graph action execution, linking or
runtime certification.

Each attempt retains `<tree>/contracts/<config>/verify-*/receipt.json` plus
the facts build, facts-target `cquery`, `bazel info execution_root` and Linux
driver streams when applicable.
Receipts record the native platform, concrete selection, commands, exits,
counts and failed/unrun stages. macOS/Windows mark the Linux driver diagnostic
inapplicable, not passed. The first failure stops later stages. This covers
captured compiler, link and archive policy, not native product execution or
AppKit GUI acceptance. Ordinary builds do not implicitly run this policy
checker.

### In-graph native policy

The root `//:native_policy_inputs` producer folds the native action facts of the
completion roots into one JSON file per configuration and exports the declared
SDK compiler inputs as an output group. It never consumes a dependency's files,
so building it forces no C++ compilation. The root `//:native_policy` `py_test`
consumes that file and uses the same compile, link and archive flag checks as
standalone verification. It also runs the lexical Catch2 registration detector,
the authored-C++ orphan audit over
the tracked and untracked non-ignored Git manifest
([test ownership](#test-ownership)), and the stale-`NOLINT` check. The test does
not invoke Bazel or run queries against the graph. All three native
hosts run the ambient policy; Linux additionally folds one Debug projection of
the same producer into that test.

The Linux-only `//:native_driver_policy` `py_test` consumes the same fastbuild
and Debug fact files plus the `native_policy_compilers` output group, which names
the declared SDK compiler file, and runs the representative Nix
compiler `-###` oracle. It executes no compiler front end and issues no query.
`ao check contracts` remains the standalone portal entry
([compile-contract verification](#compile-contract-verification)).

Deferral follows the configured scope: a path owned by another native platform,
or one under a frontend the configuration disables, is printed with its reason
and left to the owning host's policy test. Compatible orphans fail.

## Sanitizer and coverage instrumentation

Bazel exposes explicit, scoped sanitizer and coverage instrumentation beside
the ordinary fastbuild/Debug/Release modes. `./ao check lint` and
`./ao setup deps` accept these ordinary configurations, not sanitizer profiles.
Instrumentation does not substitute for ordinary completion. Task-level
validation guidance lives in [concurrency and sanitizer
validation](test/concurrency-and-sanitizer.md) and the [coverage
workflow](test/coverage-workflow.md). Sanitizer and coverage profiles use the
same `--path`/`BUILD_DIR` selection; without an explicit selection the
per-configuration default tree names apply, and the composite sanitizer check
uses one selected tree for normal completion and the managed profile.

### Sanitizer profiles

`./ao check --asan` and `./ao check --tsan` run normal completion first (fastbuild by default, Debug with `--config debug`), then the fixed Debug-based sanitizer scope in the same selected tree. `./ao check contracts`
accepts `--config asan|tsan` for analysis only. Native explicit builds/runs use
Bazel's named rc profiles and remain caller-owned; building a test does not
admit its sanitizer runtime.

```bash
./ao check --asan
./ao check --tsan
./ao check contracts --config asan --jobs 2
bazel build --config=tsan //app/tui:aobus-tui  # caller-owned runtime
```

Managed sanitizer completion accepts no labels, test filters, or forwarded
Bazel arguments: narrowing the instrumentation scope would silently change
what was measured. The [root
BUILD](../../BUILD.bazel) owns the suites: `//:asan_tests` covers Core, the
application layer, CLI and TUI; `//:tsan_tests` covers Core and the application layer. Both also select GTK tests on Linux when GTK is enabled; ASan additionally selects Linux system-media tests when that feature is enabled. The managed test adds the
`//:check_native_tests` backend selection through `cquery`, so every host adds
the native backend suite; a raw `//:asan_tests` invocation alone therefore
excludes the backend. Manual GUI and performance
targets and hidden hardware cases stay outside the sanitizer scope. The C++
fatal-contract sibling probes are transitive test dependencies. The ASan
profile sets `STBIR_NO_SIMD` on the TUI cover-art target.

Bare commands select a profile through the repository rc, not a compilation
mode: `-c asan` is invalid because Bazel accepts only `dbg|opt|fastbuild`.

```bash
bazel build -c dbg --config=asan //:asan_tests //test:sanitizer_profile
bazel test -c dbg --config=tsan //:tsan_tests
```

The bare suites do not include the compiler probe. A bare run gets none of
the managed runtime admission below — no
frozen rc, forced report status or suppression merge; context-bearing tests
still start their own fixtures. The caller owns the remaining runtime, and a
bare success is not a managed gate result.

The [repository rc](../../.bazelrc) turns each profile into the
`//bazel:asan_enabled`/`//bazel:tsan_enabled` bool build settings, a sanitizer
target platform and the root [admission
aspect](../../bazel/sanitizers.bzl). The aspect rejects combined profiles,
ThreadSanitizer on Windows, any effective compilation mode other than `dbg`,
and the uninstrumented lint SDK host targets — explicit sanitizer lint is
therefore impossible, and the normal full check retains lint. The profile
sets `--compilation_mode=dbg` explicitly, so
an explicit `-c opt` beside `--config=asan` fails admission instead of being
hidden by rc ordering. The [platforms](../../bazel/BUILD.bazel) derive from
the standard host constraints; the macOS profiles use the
deployment platform as their parent. Unix profiles select the standard upstream Apple
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
sanitizer injection, compiler or provider change exists. On Linux, the selected
compiler's observed SDK roots remain compiler-owned dependencies: Clang's
resource `share/` directory preserves its standard sanitizer ignorelists, and
GCC's builtin, resource and sanitizer include roots come from the same actual
Nix probes. The TSan compile profile
also explicitly admits the selected compiler's pinned sanitizer header
interface used by
libstdc++'s atomic-shared-pointer mutex annotations; clearing ambient Nix flags
does not remove those annotations. The governed LLVM symbolizer stays a
runtime selection of the managed sanitizer environment, never a toolchain
tool beside the archiver. Neither mechanism adds a project report
suppression or substitutes the production atomic implementation.

The managed test runtime is owned by
[bazel_sanitizers.py](../../script/ao/portal/bazel_sanitizers.py). It freezes
one per-attempt rc with the explicit sanitizer option variables before the
configured queries, and the build and test phases reuse the same rc. Individual
context-bearing tests own their Linux services. It forces
`halt_on_error=1` and positive report exit codes, adds UBSan stack traces, and
preserves other caller-supplied options. Caller TSan suppression files are
merged with the governed project rules into one immutable file without adding
suppressions. Managed Linux ASan replaces the caller's `LSAN_OPTIONS` rather
than merging it; managed TSan does not override it. Caller ASan, UBSan and
macOS LSan suppression files are each frozen in place as separate copies,
preserving option order. Every frozen copy is content-addressed under
`<tree>/sanitizers/<profile>/runtime/<sha256>/`: Bazel hashes the path in
`--test_env`, not the bytes behind it, so unchanged rules keep one path and
changed rules always miss the test-result cache.
Tests use `--test_strategy=standalone` with `--strategy=TestRunner=local`:
the standalone runner alone can still delegate to a sandboxed spawn. Local
execution preserves absolute suppression paths and native fatal-child
contracts; a trusted remote action cache stays optional for the build phase.
Linux and macOS select the governed LLVM symbolizer. macOS adds no service
context and requires the same console-user audio admission;
Windows freezes the already-selected MSVC runtime `PATH` and provisions no
additional DLL. Attempt receipts live under
`<tree>/sanitizers/<profile>/run-*/receipt.json`.

The `./ao check --asan` and `./ao check --tsan` commands run the full
normal completion gate first — including its completion products — then
a separate managed sanitizer profile in the same selected tree. Both use the same
[test-result cache admission](#test-result-cache-admission): a cached sanitizer
PASS belongs to the sanitizer-configured binary and runtime environment Bazel
hashed, so a Debug result is never relabeled as a sanitized one. `--fresh` requires new executions in both; the receipts record
executed and cached runs. Rerunning an unchanged binary only samples another
interleaving, random test order or heap layout; ask for that with `--fresh`.
Windows rejects `--tsan` before any tree or SDK work, exactly like the managed
route.

`check contracts --config asan|tsan` runs only the ordinary Debug compile-contract
oracle over the sanitizer-configured graph, admitting the selected compiler-rt
interface root as a compiler include root under Linux TSan; the managed test
runs the same phase before building. It performs no compilation or linking and
does not examine sanitizer flags.

For a compile-only check, build `//test:sanitizer_profile` with the desired
`--config`. Its [static assertions](../../test/helper/SanitizerProfile.cpp)
compare the selected profile with the compiler's instrumentation for one
translation unit. The managed sanitizer build includes this probe; its test
phase runs the selected test suites, not the probe.

Sanitizer runtime compatibility and symbolization require [native
validation](#completion); compile and analysis checks alone are not acceptance.
Linux TSan includes Aobus GTK tests when GTK is enabled, but it does not
instrument the imported GTK SDK binaries. Windows ASan likewise does not
instrument imported SDK binaries.

### Coverage

`./ao measure coverage` is the Linux-only Debug line-coverage route over the same
graph. It selects the target compiler with `--compiler=gcc|clang`, GCC by
default; the report reader follows that selection. Its suites, scoping,
strict per-test collection and report
contracts are defined in the [coverage
workflow](test/coverage-workflow.md).

The pinned rules_cc has one [local coverage-metadata
patch](../../bazel/rules_cc_coverage_private_headers.patch): private implementation
header aliases must reach the final binary's standard source replacement file
without exporting the private C++ compilation interface. Retire it when upstream
preserves that mapping closure; keep the private-interface and zero-hit-line
regressions. The coverage merger uses the selected JDK's declared
runtime files and Bazel's pinned precompiled stock deploy JAR. It does not build
the source-less Java launcher or its compiler/bootclasspath intermediates: those
upstream generic-Linux SDK executables cannot run directly on NixOS. This is a
consumer-specific native cold-build route, not support for arbitrary Java source
builds or a repair of the generic Java SDK. No Java header-extraction flag or
compiler toolchain is overridden.

<a id="ci-instrumentation"></a>

### Native instrumentation tests

The root [instrumentation macro](../../bazel/instrumentation_tests.bzl) declares
the Linux-only `//:instrumentation_tests` suite with the standard bazel_skylib
`build_test` and `analysis_test` rules. It adds no custom pipeline, runtime Bazel
invocation, query/aquery receipt or coverage execution. The suite contains five
verdicts:

- The two build tests compile and link `//test:sanitizer_profile` under
  each profile without executing it, so the profile still produces a linkable
  binary.
- The two compile tests inspect the configured `CppCompile` and `CppLink`
  command lines: the required `-fsanitize` flags, Debug mode with enabled debug
  information and no `NDEBUG`, and no cross-profile, coverage or IPO flags.
- The coverage test inspects the configured coverage graph over
  `//:coverage_default_tests`: instrumented sources, `.gcno` metadata, the
  standard source-mapping and instrumentation-manifest inputs of each
  `TestRunner`, declared merger runfiles and coverage support, private
  implementation-header mappings on the `CcInfo` provider at the project-owned
  rules_cc patch boundary, and the absence of coverage flags from execution-tool
  configurations. It requires the declared rules_java runtime and stock merger
  payload; `shell.nix` exports `JAVA_HOME` from the same Bazel JDK.

The daily sanitizer links cover only the probe, not the libraries linked by
`//test:utility_test` or other test binaries. To catch sanitizer-specific link
or runtime failures in those suites, run `./ao check --asan --fresh` or
`./ao check --tsan --fresh` on a [supported host](#sanitizer-profiles).
The coverage analysis does not produce a coverage report.

### Focused instrumentation analysis

`check instrumentation` is Linux Debug analysis with `--suite` selection and the
normal compiler, tree, jobs, and cache controls. It reuses the action oracle:
coverage flags and atomic counters must appear on measured target actions, while
execution tools remain unprofiled; each selected configured test must consume its
own standard metadata outputs. Queries of the actual C++ compilation context and
`InstrumentedFilesInfo` verify private-header mapping propagation, instrumented
source declarations and declared counter-note dependencies: the LMDB
private aliases must reach a selected consumer's context when the library closure
is present. Independent public JDK and stock-payload queries also constrain that
consumer's merger runfiles.

Bazel 9.2 does not export the generated contents of its native coverage-map and
instrumentation-manifest actions through analysis, despite their `FileWrite`
mnemonic. Coverage analysis therefore checks provider declarations and context
mapping propagation, not those generated file contents, complete host SDK identity,
materialization, counter values or report fidelity. Ordinary coverage retains
the independent standard-map and runtime/report cross-checks.

`check instrumentation` accepts canonical `--suite` selection but no report-only
`--scope` option or test filters. It issues no Bazel `build`, `test` or
`coverage` command, enters no private audio/display service context, and produces
no compilation, test execution or coverage report. Its unique receipt and query
evidence remain under `<tree>/coverage/run-*`, explicitly marked as
analysis-only. Ordinary coverage uses fresh tests and strict runtime/report
validation.

## Lint reports

The Bazel lint report actions cache native C++ analysis reports and gate the
selected findings; `./ao check lint` consumes them. They use
actual Bazel `CppCompile`/`ObjcCompile` contexts, not a separate compilation
database. The [aspect](../../bazel/lint/lint.bzl),
[launcher](../../script/ao/graph/bazel_lint_action.py) and
[current-report gate](../../script/ao/graph/bazel_lint_reports.py) implement the
contracts below. [Decision 0023](../decision/0023-full-input-native-lint-reports.md)
explains why a report's include list does not narrow its cache dependencies.

```bash
./ao check lint --target //lib/utility:utility
./ao check lint --target //lib/utility:utility include/ao/utility/Base64.h
./ao check lint lib/utility/Base64.cpp
./ao check lint lib/utility
./ao check lint --all
./ao check lint --commit origin/main
```

The C++ stage accepts the common scope selectors (file/directory paths,
`--commit`, `--all`), `--config`, `--path`, `--vcpkg-prefix`, `--tidy-check`,
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
The complete configured index must prove a native translation unit for every
compatible explicit root, even if its report was not requested and another root
covers the selected file. This index proof does not require delivery of every
sibling report and does not make sibling mains selected. For example, the
header-scoped utility command above gates the selected header, unlike the
broader target-only command. An ordinary compiler error in a delivered but
unselected sibling report is not a selected-main result; failure of a required
Bazel build instead makes the attempt incomplete. Compatibility uses the
top-level target configuration, not an exec-configuration row for the same
owner built as a tool.
Automatic discovery without `--target` queries the whole project graph.
For a supported direct main path, the portal requests its reports in
all actual native contexts plus the owner inventory, not owner sibling reports
for a direct-only scope. Donor discovery can also request sibling native TU reports.
If any path is unsupported by the selection codec, all compatible owners in the
request fall back to the full output group while gating only selected mains. Target-only
selection and the full completion gate retain their broader coverage.
Native lint queries and report builds select only the tracked repository rc and
its generated launch rc, ignoring system/home rc files and environment
`BAZELRC`, as the completion gate does.

Each report records the checkout files its analysis included. A selected header
without an unambiguous same-component companion source, and a selected include
fragment, are analyzed through a translation unit that included them. The portal
discovers one from translation-unit reports, preferring the same component and
then the longest shared directory prefix, and subsequently replays that consumer's native
context: a header becomes the main file, while a fragment stays inside the
consumer translation unit. Fragment warning blocks are selected by normalized
file identity; consumer compiler errors and unlocated diagnostics remain, and
scoping never hides an incomplete attempt. Without
`--target`, discovery builds only the possible consumers nearest the path;
the other owners whose dependencies declare it build only when none of those
reports included it. When every nearest consumer is platform-incompatible, the
held owners are queried for compatibility before the path can defer or fail: an
incompatible nearest group cannot prove farther consumers unavailable. The
consumer selection reaches the aspect as one canonical JSON string parameter,
distinct from output group selection. The portal writes the consumer parameter
and scoped output groups to separate named configs in its generated rc under the
tree lock. Only report builds select them. Input, native context and analysis
profile changes invalidate reports; selecting reports does not. Query/cquery do not use these configs;
no consumer repository, environment selector or pre-query reset is needed.
Identical consumer parameters and output groups keep the rc section stable; the
next workspace preparation regenerates the baseline rc without it.
Large maps stay out of the Windows client argv. This uses the normal
Bazel server mode: batch mode can re-expand rc options into an oversized Java
command line and is not supported for large maps. Each pass records its literal
parameter and exact parameterized aspect identity in the receipt. Naming a new
consumer adds its reports without changing other report actions. The
completion gate declares its own fallback replay actions without a
portal-written consumer parameter.

Scoped lint defers unreported, discovered paths that are incompatible with the
current platform. With `--gtk off` on Linux it also defers discovered GTK paths
when the configured query proves **all** their candidate owners incompatible.
Naming a file explicitly still requires a report. A compatible header or fragment
that no analyzed translation unit includes, or a compatible selected path with
no owner, fails the operation; it is not a quiet partial pass. `--all` selects
the project C++ scope and uses the same rules. The portal prints every deferred
owner or path.

The completion gate's C++ stage is the declared `//test/lint:production_lint`
test ([graph.bzl](../../bazel/lint/graph.bzl)), published as the root
`//:production_lint` alias so bounded product builds do not load the analysis
SDK until it is requested. It applies the report aspect to the
completion roots and gates the graph's declared reports in one action,
independently of Git diffs or rule-change filename lists. A direct report with
the path as its main file from any configured owner in the gate satisfies coverage
for that path, even when another owner lists it as uncovered. All declared direct
reports are still validated. Only a header or include fragment with no direct main
gets a replay, deduplicated by path. `AobusLintSelect` reads the candidate TUs'
include reports, not their source bytes. Its output selects one native compile
context or records that no consumer included the path. The item and selected
context have collision-checked, path-derived names: an unrelated insertion in
either inventory does not renumber the surviving replay. Bazel then expands an
`AobusLintGraphReplay` action with that context's actual compiler inputs and
header closure. The other contexts remain available during template expansion,
but their unrelated source edits cannot by themselves invalidate the selected
diagnostic. The header is the main file; a `.def` fragment instead retains the
consumer TU as its main. No test runs Bazel or an `ao` command; the launcher
only reads the completed gate result. The CLI's scoped diagnostic entries are
[`ao check lint`](../../script/ao/portal/bazel_lint.py) and `ao check analyzer`.
Bazel source-file targets distinguish
authored checkout inputs from generated ones. Eligible owners declare STRICT or
RELAXED inputs through `srcs`, `hdrs`, `textual_hdrs` or additional compiler inputs;
compatible owners, including header-only libraries, contribute configured
inventories. Those inventories identify inactive `select()` inputs, while the
stage preserves `check`'s compiler and graph-feature selections. Declared header
closures can include other platforms' headers through broad globs, so the
native ownership policy defers unreported platform-specific
headers. Fallback replay still requires an observed include consumer; a
configured portable declaration with neither a direct main nor a consumer fails.
The stale `NOLINT` scan covers the complete candidate set. The independent
authored-source ownership scan is the root `//:native_policy` test
([test ownership](#test-ownership)). The lint test excludes generated-only
owners from authored policy: Bazel compiles GLib-generated C and catalog arrays
but does not lint them as project source. A declared portable
path that no admitted configuration selects — for example, only under an
off-by-default feature flag — fails rather than silently passing; deferral
stays reserved for recognized foreign-native-platform paths.

The graph lints each owner's top-level target configuration, including authored
tool source, without separately covering exec-configuration compilations of the
same source. Full coverage is not full fresh execution: matching reports are
reused and cached warnings still fail. Cold caches pay the complete analysis
cost; conservative declared input closures can invalidate more reports than the
actual include relationships alone would require.

The report aspect is nonpropagating; its required provider-only aspect follows
dependencies solely to map virtual include headers. It captures actual native
argv and declared inputs, adds reports for paired headers analyzed as main
files, and retains
AppKit Objective-C++/ARC and WinUI generated-header dependencies.
Unsupported compiler views, modules, PCH/header-unit contexts and unresolved
response files fail rather than being stripped. The portal reads only the
current invocation's BEP and local report artifacts, binding each owner to the
fixed production aspect and that pass's exact consumer parameter. Before using
reports from a pass, including for donor discovery, the portal requires every
owner's outputs to come from the same local execution root. It does not scan
stale output directories or join `aquery` data. The native lint stage returns
0 for a clean selection, 1 for warnings, and 2 for selected compiler errors,
incomplete coverage, infrastructure failure or invalid reports. `--report-only`
makes only warnings advisory; cache hits do not change the verdict.
`--debug` displays per-run evidence paths
and publishes all action completions in the BEP for diagnosis; those events are
not the report inventory or proof that a disk-cached action executed anew. The
tree keeps the eight most recent run evidence directories.

`--tidy-check` and `--header-filter` change report-action keys. Consumer parameters
change aspect analysis identity, but unchanged report argv, environment and
inputs retain their action keys; the selected file set is not a report input.
Raw `--tidy-arg` values are limited to diagnostic-only
presentation options (`--quiet`, `--system-headers`, `--system-headers=true`,
`--system-headers=false`, `--enable-check-profile`, `--use-color=false`); other
raw options, including fix/export, overlay/config, extra-arg and
warnings-as-errors forms, are rejected before workspace or native preparation.
The ownership and suppression policy applies, including stale `NOLINT`
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
`external_include_paths`, `static_link_msvcrt`, `dynamic_link_msvcrt`,
`default_compile_flags`, `default_cpp_std` and `default_exception_handling`
for the actual report context; unsupported requested or
disabled features fail rather than receiving an approximate replay. Linux
clang-tidy does not execute the compiler wrapper from its database, so the
launcher projects the governed Nix wrapper of the selected target compiler,
with its ordered before/original/after
arguments, through `NIX_DEBUG=1` and a driver-only `-###` probe. It preserves raw
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
cannot be hidden by selecting fewer mains. The lint report build passes
`--norun_validations` to avoid pulling PE/link actions into lint. This does not
change the [Windows executable validation](#generated-files-and-runtime-data)
required by the repository rc for ordinary build/test/run. STRICT/RELAXED mode,
tools, checks, filters and compiler policy remain action inputs even though
selected scope is not. The selected tree lock covers preparation through report consumption and
the final gate decision; it is not a source-filesystem snapshot. The portal
launches no detached lint workers and does not mutate source-checkout links.

### Physical header mains

A paired header borrows its actual companion command but remains the physical
main file. Derivation replaces only the native source argument, adds the explicit
header language and reserved `AOBUS_LINT_HEADER_MAIN=1` definition, and preserves
semantic forced includes, ARC and SDK settings. On Windows a relative `/FI`
that resolves to the physical header main itself is omitted for that replay;
other forced includes remain. A native argument referencing
that reserved mode is rejected. There is no wrapper, VFS overlay, remapped file
or rewritten source buffer: main-file-sensitive checks and suppression scans
must see the original header.

Normal product compilation does not enable the reserved header-main mode.
[WinUI components and probes](windows.md#bazel-product-details) include their
own projections; neither native compilation nor lint supplies a shared XAML
prefix.

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
SDK without configuring CMake, adding another version authority or parsing
installed CMake exports. Explicit Windows provisioning installs that governed SDK —
`ao.bat setup deps` or `ao.bat setup llvm-sdk`, which call the same
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
native environment and product SDK prerequisites still apply. Both checker
forms inherit the compiling toolchain's language standard — C++26 on every
Unix toolchain, including the GCC execution toolchain that builds the Linux
checker independently of the selected target compiler, and `/std:c++latest` on
MSVC — beside the selected SDK ABI. On Linux the governed LLVM SDK uses
libstdc++ with RTTI enabled and assertions disabled; the checker retains those
facts and the SDK's exception policy when built with GCC. Linux checker
compilations force-include a GCC/LLVM compatibility header that the tracked
[SDK repository generator](../../bazel/lint/sdk.bzl) emits into the
`@aobus_lint_sdk` facade, which exports the SDK imports. The checker build
declares and force-includes the generated header on Linux only. For GCC 16
with LLVM 22, the shim disables `-Wnonnull` while parsing
`clang/AST/ExternalASTSource.h`, whose guarded lazy-pointer fast path triggers
a false positive, then restores the full diagnostic policy. It does not
modify the pinned SDK files or its projected ABI facts. Checker implementation
code keeps `-Werror=nonnull`; the shim does not downgrade the whole translation
unit or apply to ordinary products and tests. Retire the shim when the pinned
compiler/SDK pair accepts the fast-path reproducer without it.
The SDK importer drops the SDK metadata's own standard flag, including the MSVC
dash-colon `-std:c++17` spelling, and projects the LLVM EH/RTTI/static-build
ABI facts from the shared and MSVC policy literals; those overrides remain
necessary for the uninstrumented static LLVM host, not a separate language
policy. The local [rules_cc patch](../../bazel/rules_cc_msvc_exception_handling.patch)
makes the default `/EHsc` independently selectable and preserves the ordinary
MSVC compile arguments and their order when it is enabled. The Windows checker
disables `default_exception_handling` while retaining the SDK's explicit `/EHs-c-`;
this avoids D9025 overrides without disabling the other default compile flags
or changing the product's exception policy. Retire the patch when upstream
provides that selection directly.
Unix builds the plugin against its externally supplied host, excluding
product sanitizer instrumentation from compilation and linking. Windows builds
the linked `AobusClangTidy` host optimized with `/MT`, independently of product
`/MDd` or `/MD`. Plugin/host ABI and sanitizer compatibility must be preserved;
macOS SDK C include ordering stays driver-owned. Registry objects and required upstream modules must survive linking. The
[SDK admission](../../script/ao/portal/bazel_lint_sdk.py) derives the Windows
initializer closure from selected archive references rather than linking all
LLVM. The [repository projection](../../bazel/lint/sdk.bzl) and checked-in
[import declarations](../../bazel/lint/imports.bzl) own SDK file destinations,
metadata/flag exports and the `cc_import`/`CcInfo` graph; Python returns native
file and ABI observations, not BUILD source. Required system libraries, including DIA when enabled by the SDK, remain
part of that closure; a required library must exist. XML omission is allowed
only when the selected archives have no corresponding references.

The explicit `bazel test //test/lint:lint_test --nocache_test_results` gate
checks the result of real fixture discovery, marker/message assertions,
source-policy smokes, and compiler-error canaries. Its verified producer action
starts no nested project Bazel or `ao check lint` preparation. Producer reports can
be cached; fresh test-result assertions alone do not prove fresh Clang fixture
execution. It stays outside `//:tests`
and is tagged `manual`, but `//:check_tests` lists it, so completion's wildcard
test expansion runs it; the completion lint test consumes the same checker
verification action.

Target-only scope does not imply unpaired-header coverage. Selected-check
samples, source parity and cached action completion do not establish every
header, complete scopes, other native profiles, GUI or hardware acceptance.
Record actual invocation scope, deferrals and fresh execution versus cache reuse
with PR/CI evidence, not as a second changing acceptance ledger here. The
`./ao check lint` command consumes these same report actions for a
selected scope; the completion role belongs to `//:production_lint`.

### Scoped lint and tool selection

`./ao check lint` is a scoped semantic check: test and naming audits, the
Linux-owned [workflow lint](linting.md#github-actions-workflow-lint) over direct
`.github/workflows` YAML (skipped on macOS/Windows), Python checks, Buildifier
semantic lint, and native C++ diagnostics from the Bazel lint report actions.
It accepts file/directory paths, `--commit`, or `--all` as one source scope,
plus `--config`, `--path`, `--vcpkg-prefix`, `--jobs`, and `--remote-cache`.
Missing paths and invalid selections fail before discovery or native preparation.
`--tidy-check <name>` selects only the requested C++ diagnostics, with no
unrelated language lint or audits. Warnings fail unless `--report-only` makes
them advisory; selected compiler errors, tool and admission failures always fail.

Lint performs no formatting or global document check. Use `./ao check format`
for scoped read-only formatting and `./ao check docs` for global semantics.
`ao check` does not invoke scoped lint: its whole-graph coverage is the declared
`//:production_lint` test ([lint reports](#lint-reports)), where `--commit`
selects nothing. Its test-name, class-name and workflow-lint stages also run as
`test_audit`, `name_audit`, and `workflows` graph targets
([shared audits](#shared-audits-and-the-root-version)).

On Windows, `format` and `check format` use the ready `clang-format` from the
LLVM SDK that `ao.bat setup deps` or `ao.bat setup llvm-sdk` explicitly installs:
no CMake, provisioning, or Visual Studio environment is needed for formatting.
Python-only, Bazel-only, YAML-only, and empty scopes need no LLVM SDK lookup.
Bazel-only formatting selects the ready governed Buildifier tool;
`./ao setup buildifier` (`ao.bat setup buildifier`) prepares it explicitly.
The formatter never installs a missing tool or SDK. On macOS and Windows,
checks stay on base Python and use the managed tooling environment only for
Ruff/mypy, preserving Bazel interpreter and cache identity. See
[validation](test/validation-and-review.md) for stage boundaries.

## Completion gate

Use `./ao check` (`ao.bat check` on Windows) for full local validation. To
reproduce a [daily CI lane](test/validation-and-review.md#continuous-integration)
locally, select its command explicitly. Each command runs one Bazel test graph
and verifies the required suite expansions in the Build Event Protocol (BEP):

| Command                 | Bazel test selection                               | Required BEP suite expansions                                 |
| ----------------------- | -------------------------------------------------- | ------------------------------------------------------------- |
| `./ao check`            | `//...`                                            | `//:check_tests` and `//test/script:repository_checks`        |
| `./ao check --ci`       | `//...` minus `//test/script:ci_repository_checks` | `//:check_tests` and `//test/script:native_repository_checks` |
| `./ao check repository` | `//test/script:ci_repository_checks` only          | `//test/script:ci_repository_checks`                          |

The native selections build the products and run the applicable tests, the
[native-policy checks](#in-graph-native-policy) and whole-graph C++ lint.
Both also require `//:instrumentation_tests` on Linux. `--ci` requires fastbuild
and adds `--config=ci` to keep going after failures and enable Windows desktop
tests. `check repository` runs on Linux in the pinned shell, keeps going after
failures and accepts only `--path` (honoring `BUILD_DIR`) and `--jobs`.
The [tooling BUILD](../../test/script/BUILD.bazel) owns suite membership;
`//test/script:format` checks both the documentation inventory and the
remaining source files.

Use ordinary `check --config release` for the full Release graph. `--asan` and
`--tsan` add the [instrumented scope](#sanitizer-profiles) after normal completion.
No completion command takes a diff, targets, forwarded Bazel arguments or test
filters. Checks lock the selected tree and use `--lockfile_mode=error` to avoid
rewriting the module lock. They load only the checked-in repository rc and the
tree's launch rc, excluding system and personal rc files and removing `BAZELRC`
(including Windows case variants) so local options cannot silently filter tests.
Standalone `check contracts`, managed sanitizer admission and coverage use the
same isolation. Ordinary build/test invocations retain their personal rc behavior.

The [root BUILD](../../BUILD.bazel) declares the completion roots other
tools select. `//:check_tests` groups the portable default aggregate with the
Linux GTK test, the native backend suite and the lint fixtures;
`//:check_native_tests` is the backend filegroup the managed sanitizer route
adds. `//:check_products` groups the CLI/TUI entrypoints, the enabled host
product (GTK, AppKit or WinUI), the GUI probes and the performance benchmarks
as build inputs. The filegroup does not run them; the test graph separately
launches applicable native process probes. `//:check_runs` names the two
unattended WinUI console probes; `//:check_cpp_tests` is the configured C++
test owner set used by the native policy and lint graph tests.
`//:native_policy_inputs` is the non-test producer that folds the native action
facts and owned-source closures into one JSON file, with
the Linux Debug projection reusing the same producer under a debug transition.
`//:production_lint` is the public alias for the declared whole-graph lint test
in `//test/lint` ([lint reports](#lint-reports)); the alias keeps the analysis
SDK lazy for bounded product builds.

Wildcard discovery is Bazel's: `bazel test //...` selects every test target in
the tree that is not tagged `manual`, then expands the selected test suites.
The GTK, native-backend and lint-fixture tests carry `manual`, so a bare label
wildcard does not pick them up on their own, but the non-manual `//:check_tests`
suite lists them explicitly and wildcard test expansion follows that membership.
A filegroup would only build them. On Linux the
[context-specific wrapper](../../bazel/testing.bzl) around each context-bearing
test starts that test's owned private fixtures from its declared context.
On Windows the wildcard adds the two unattended native console probes (track
item and localization) through `//test:native_process_tests`. Windows completion
also independently requires `//test/winui/projection:winui_projection_test` and
`//bazel/windows_header_share:mode_interfaces_test`. The latter analyzes the
public RapidYAML import in all three modes and verifies that only Debug depends
on the Release header helper. These requirements do not apply on other hosts.
`--config=ci` additionally enables the [WinUI automation probe](windows.md#ui-automation-naming-gate),
which requires the Windows App Runtime and an authenticated interactive console.
Other GUI probes and performance harnesses remain build-only in wildcard
completion; the gate does not certify manual scenarios, performance, coverage
or distribution.

Linux private fixtures are owned per test, not per gate. There is no global
`--run_under` rc entry: only context-bearing `cc_test` targets are wrapped by
the thin [`native_context_test`](../../bazel/testing.bzl) rule. Its inner
`cc_test` is tagged `manual` and owns compilation, coverage instrumentation
and aspect facts. The GTK and backend tests use `local = True` for both the
public and wrapped TestRunners, and `external` forces fresh test execution.
Their compile, link and lint actions remain cacheable. The wrapper supplies
the public test label, re-exports `InstrumentedFilesInfo`, merges the wrapped
test's runfiles and forwards its run environment with the wrapper's context
and executable settings. On Windows the Python launcher opens the `.zip` (or unzipped
bootstrap) beside the wrapper executable. That file is a declared test input,
not just a build output, so Bazel materializes it before starting the test.
Off Linux the wrapper still launches the test but starts no private Linux
fixtures. The wrapper's [runner](../../script/ao/graph/bazel_test_runner.py) starts only the
fixtures the target declares through `AOBUS_TEST_CONTEXT` (`audio`, `dbus`,
`gtk`) on Linux before forwarding the test's own arguments and environment; a
test without a context marker starts no fixture. The bounded
[service fixture](../../script/ao/graph/bazel_test_services.py) starts owned
processes under private XDG directories and a private D-Bus session, using short
per-invocation runner-owned socket directories even when the build tree or
`TMPDIR` is deep. It requires daemon readiness with `AOBUS_REQUIRE_PIPEWIRE=1`, and adopts no personal
runtime, service configuration, default audio or installation. Cleanup only
signals admitted owned children; uncertain spawn or stop keeps the runtime in
place and fails. Windows native process tests resolve their own runfiles, and
the UIA probe's smoke entrypoint performs the Windows App Runtime and
interactive-session admission itself.

On Linux the isolated launcher selects the pinned Nix JDK through
`AOBUS_BAZEL_SERVER_JAVABASE`. Managed sanitizer and coverage runs keep their
own per-attempt rc snapshots.

Each attempt writes `tests.bep.json`, `profile.json.gz` and `test.log` under
`<tree>/checks/<config>/check-*` or, for `check repository`,
`<tree>/checks/fastbuild/repository-*`. The test stream goes to stderr and
is copied to `build.log`. Failed attempts retain their evidence; interruption
alone does not establish that native writers have stopped.

After Bazel exits successfully, the launcher requires complete BEP evidence
from that invocation. Every applicable suite member and independently required
check must have one passing configured test summary. Removing the backend or an enabled GTK or system-media
test from its suite cannot silently drop it; WinUI automation is required on
Windows under `--ci`. Missing or skipped required checks fail, and a Bazel
`SKIPPED` abort reports the skipped configuration. No second query or test
invocation is needed.

### Test-result cache admission

A passing Bazel test summary counts toward local or CI completion whether the
test ran on this host or its result came from a [trusted cache](#native-hosts-and-caching).
Another checkout can reuse the result, not just the executable, when Bazel's
declared inputs, options and relevant environment match. The repository rc does
not forward the caller's `PATH`, `AOBUS_STATE_ROOT`, `LOCALAPPDATA` or `CI` to
every test. Targets opt in to what they use: Linux private-service tests inherit
`PATH` for fixture tools; Windows tests that locate managed tools can inherit
`AOBUS_STATE_ROOT` and `LOCALAPPDATA`.

A cached pass is not a new execution on this host. It samples no new random
schedule or interleaving. Use `--fresh` when investigating intermittent failures.

`--fresh` adds `--nocache_test_results` to request new executions of the same
selected tests. It does not force an unchanged compiler or lint producer to run.
`--clean` removes the selected tree's Bazel outputs first. The
`external`-tagged native audio, platform and Linux system-media tests execute on
every attempt, and the repository checks are `local`/`external` because they
read the physical checkout rather than a runfiles copy. A test's declared
context is part of its environment, so changing an owned fixture input
invalidates a cached result even when the source is unchanged. The cache choice
does not change the gate's scope or expose test filters, and lint producers and
other actions may still be legitimately cached, so the gate makes no
fresh-producer claim beyond test results.

The managed sanitizer and coverage routes keep the stricter binding: they
reconcile passing BEP summaries against their exact configured selection,
record run/cached/executed counts in their receipts, and reject cached results
under their own fresh proof ([sanitizer and coverage
instrumentation](#sanitizer-and-coverage-instrumentation)). The CI summary
reads the standard BEP test summaries of a check attempt instead of a receipt.

### Test ownership

The audit does not maintain a separate source inventory. The
[test ownership helper](../../script/ao/portal/bazel_test_ownership.py) discovers
registrations with the lexical test-audit detector and normalizes executable labels.
The root `//:native_policy` test's [registration audit](../../script/ao/portal/bazel_validation.py)
reconciles those registrations with the folded native facts
([in-graph native policy](#in-graph-native-policy)): each compatible source
that registers cases needs a declared executable owner. A `cc_test` owner
must be reachable from `//:check_cpp_tests`; a `cc_binary` owner may be
reachable from those roots or `//:check_products`, including binaries whose
cases run only on request. This audit checks configured owned-source closures,
not test execution. For completion, include a `cc_test` owner in the test
selection as well, through its public wrapper if it has one. Adding an owner
only to an analysis or build root does not schedule it. The
[completion gate](#completion-gate) separately checks passing summaries for its
required tests.

Tests link the ordinary product libraries where they exercise production
implementations. For example, `//test:utility_test` links the utility and
Unicode-text libraries without compiling product implementation sources
itself. A sibling probe for utility fatal-contract tests runs independently
of the test main. `//test:media_fixture_test` links the application runtime
through its test-header interface; `//test:media_test` tests Core media without
the application runtime. `//test:uimodel_test` links the production UIModel
library. Its playback cases can use controlled output providers and real
decoder inputs, but do not establish physical or audible playback. See the macOS
[time-zone fallback](macos-portability.md#10-date-time-zone-database) for the
formatter test's host-zone expectation.

`//:native_policy` also checks C++ files in the tracked and untracked non-ignored Git inventory. It considers files present on disk that the shared source policy does not ignore. Eligible files must have a native owner. Sources the configured universe cannot reach are deferred only by the
platform policy or by an explicitly reported disabled-frontend
boundary (`gtk_enabled`/`system_media_enabled`), with the reason printed;
compatible orphans always fail. Deferral follows the configured scope, so each
native policy test catches orphans applicable to its analyzed host. The
[configured native CI matrix](test/validation-and-review.md#continuous-integration)
covers the paths that a single host's policy test cannot reconcile. The audit combines the declared sources from the supplied
configured graphs, including their `select()` results; diagnostics name the
source or label, not a per-source configuration matrix. Source ownership is not a line-coverage claim. The
managed sanitizer route uses the test ownership helper's configured-target
identities to bind its selection to BEP summaries.

### Shared audits and the root version

The lexical completion audits — forbidden scans, boundary and naming rules —
are implemented once in [guardrails.py](../../script/ao/portal/guardrails.py);
the completion graph runs them as the `guardrails` repository-check target,
which writes its report to the test's undeclared-output directory, and the
standalone [shared CLI](../../script/ao/guardrails.py) exposes the same
inventory for ad-hoc use, so there is one audit inventory, not one per route.
The `test_audit` and `name_audit` targets run the same Catch2 naming/tag and
class/file naming audits their standalone commands expose, and `workflows` runs
the pinned actionlint over direct `.github/workflows` YAML on Linux. The
application version is owned by the
plaintext root [VERSION.txt](../../VERSION.txt) file: the build reads those bytes
directly and never parses CMake syntax.
The `.txt` suffix avoids shadowing the standard C++
`<version>` header on case-insensitive filesystems.

## Generated files and runtime data

The [gperf rule](../../bazel/gperf.bzl) uses the governed executable in execution
configuration, `LC_ALL=C`, declared grammar/tool/runtime/output and direct
arguments. Resolution executes `gperf --version` and rejects an unexecutable or
mismatched tool. It generates query's unit dispatch and four media dispatch
headers without pre-generated headers or shell redirection.

The [catalog rule](../../bazel/catalog.bzl) declares the authored
inventory, text/templates, native ICU/Python SDK inputs and the real compiled
[`ao_catalog_compiler`](../../tool/catalog/BUILD.bazel) in exec configuration.
One [action helper](../../tool/catalog/build_catalog.py) runs the strict
genrb, validation, pseudo-locale and pkgdata common-data pipeline and emits aligned embedded
C++ data plus maintained locale tags. A private tree owns all resource/RESW/temp
side outputs; package.lst remains the sole locale inventory. No external configure,
prebuilt project tool or pre-generated resource is consumed. Common-data
packaging does not compile C, and this does not establish C/Objective-C support.
The action launches Python at its selected host-absolute path; the interpreter
bytes and selected runtime files are declared through `python_action_tools`,
which `catalog_tools` combines with the ICU tool payload. Lint, the Linux
coverage collector and platform packaging use the Python inputs without
inheriting ICU tools. An expanded lint action requires an executable artifact:
the native replay launcher runs the same installed base interpreter with
`-I -S -B -X utf8` before loading any replay code. Its inputs do not select the
tooling venv or its site packages.
This is a native SDK boundary, not a relocatable/hermetic Python toolchain.
Switching to a repository-local executable requires separate native validation
of DLL and standard-library lookup, particularly on Windows.
The application version header expands its template from the project's
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

In the Windows CLI and TUI, executable-global `operator new` and `operator delete`
forward to mimalloc; see [Windows dependency linkage](windows.md#dependency-linkage)
for the allocation boundary. Ordinary tests do not link mimalloc. Catalog tools
are a separate execution boundary: the compiled catalog compiler and actual
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
bazel build //test:windows_utf8_manifest_checks
```

This target also materializes the declared XML/stamp outputs for inspection.
A cached `_validation` result on an ordinary test need not leave those files
materialized in the output tree; absence there alone is not a validation failure.

The tracked Windows Bazel configuration enables executable validation.
Failure fails the command and prevents `run` launching, but validation need
not precede individual test execution. Native custom overrides are caller-owned;
`ao check` rejects disabling required validation. Installed `mt.exe`/PowerShell runtime locations remain in use;
`PATHEXT=.EXE` enables PowerShell's native invocation. Resource verification is
separate from compiler `/utf-8` and observed effective runtime code pages.

## Native hosts and caching

macOS uses governed Homebrew LLVM 22, the SDK, deployment target and libc++
expected shim with the tree's provisioned vcpkg artifacts; the launcher, not Bazel,
bootstraps the pinned vcpkg, and Bazel does not bootstrap CMake/Ninja. Windows uses Visual Studio, its selected SDK, reviewed dynamic portable target
dependencies and the dynamic CRT. Bazel adds no global `/MT` or replacement CRT
flag; upstream toolchain mode features still select `/MDd` and `/MD`.
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
For shell-backed rules the Windows portal honors an explicit `BAZEL_SH`;
otherwise it looks for Bash in the detected Git for Windows installation. It
uses the selected shell for both the Bazel client and its repository rules.
For bare Bazel, [select Bash in the developer shell](#direct-bazel-without-the-portal).

Keep selected SDK/tool installations stable while resolution and actions run;
admission is not an atomic installation snapshot. These are native
installation inputs, not a packaged relocatable SDK or a complete
syscall/dynamic-loader closure; the project does not emulate loader search paths.
Runtime libraries, .NET/registry state and other inputs outside the enumerated
families remain trusted host state; changing
those is not certified by the selected SDK identity. Linux similarly trusts
immutable native Nix SDK state. None is an RBE contract. Changing the host's
installation selection — Visual Studio, Xcode or an SDK — can require refreshing
configured repositories so they observe the new selection. On Linux and macOS,
`bazel fetch --force --configure` forces configure repositories to reevaluate.
Do not use that untargeted command for a Windows SDK refresh; see
[Windows compiler configuration](#windows-compiler-configuration). The bootstrap
treats selection as watched host state, not a complete registry-drift guarantee.

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

### Shared local action cache

By default, bare `bazel build`, `bazel test`, `bazel run` and portal-launched
Bazel share a per-user disk cache across checkouts and build trees. The
[repository rc](../../.bazelrc) selects it without setup or a personal rc. Bazel expands
`~` using the JVM's `user.home`, not the shell. On Linux this is normally the
account home; setting `HOME` to a temporary directory does not isolate the cache:

| Host    | Action cache directory               |
| ------- | ------------------------------------ |
| Linux   | `~/.cache/Aobus/cache/bazel`         |
| macOS   | `~/Library/Caches/Aobus/cache/bazel` |
| Windows | `~/AppData/Local/Aobus/cache/bazel`  |

`AOBUS_STATE_ROOT` selects portal-managed tool and installation state.
Neither it nor a redirected `XDG_CACHE_HOME` or `LOCALAPPDATA` relocates the
action cache. If you used a cache under a custom state root, the portal does
not move or delete it. Each output base retains its own incremental state.

To move only the action cache, set `AOBUS_BAZEL_DISK_CACHE` for the portal to a
dedicated directory on a trusted local disk. Supply a printable absolute path;
a relative path, a literal `~/…` or a Windows UNC path fails preparation rather
than silently selecting another cache. Leaving the variable unset or empty keeps
the default. Bare Bazel does not read this variable: pass `--disk_cache` with
the same directory if you want both entry points to share it.
For example, in a POSIX shell:

```bash
export AOBUS_BAZEL_DISK_CACHE='/mnt/build-cache/aobus'
./ao check
bazel test --disk_cache="$AOBUS_BAZEL_DISK_CACHE" //test:utility_test
```

The shared directory has one **50 GiB** size target
(`--experimental_disk_cache_gc_max_size=50G`), not a quota per workspace.
After the Bazel server has been idle for five minutes, its [disk-cache garbage
collector](https://bazel.build/remote/caching#garbage-collection) evicts the
least recently used entries first if the cache is over target. There is no
age-based expiry or periodic full reset. The cache can exceed 50 GiB during
builds or if no server stays idle long enough. Other caches and build outputs
are outside this budget.

A trusted remote cache is optional; forward
`--noremote_upload_local_results` for read-only remote use. Cross-tree/worktree
hits still require matching declared inputs, toolchain, options and relevant
environment. To check reuse of a test **result**, first run
`bazel test //test:utility_test`. In a second checkout at the same revision,
use a fresh output base and matching build options. Both commands here use the
default disk cache; if you relocated it, pass the same `--disk_cache` to both.
For example, in a POSIX shell:

```bash
probe=$(mktemp -d)
bazel --output_base="$probe/output" test --execution_log_json_file="$probe/actions.json" //test:utility_test
```

In `actions.json`, look for the `TestRunner` entry with `runner` set to
`disk cache hit` and `cacheHit` set to `true`. A compile or link hit alone does
not establish that the test result was reused.
`//test:audio_test`, `//test:audio_platform_test` and, on Linux,
`//test:systemmedia_test` carry Bazel's `external` tag and execute on warm
invocations of the default aggregate. Cached results from other tests are not
fresh binary executions.

Only `Linux / native` archives its Bazel disk cache across runs. The portal,
bare Bazel lint build and cache steps share `$RUNNER_TEMP/aobus-state/cache/bazel`;
Windows's job-local disk cache is `D:\aobus-bazel-cache`. Tool and dependency
archives are separate. The OS, architecture, pin contract and commit identify
candidate archives, with fallback to the same pin contract. Bazel action keys
still decide reuse. PRs may restore the main archive subject to GitHub's cache
access rules, but only pushes to `main` can save, after native completion and
its scoped lint build pass.

A marker outside the cache records when restore finished. Exact hits need no
trim or save. Before saving, trim deletes only `ac/` and `cas/` files whose
mtime is not newer than the marker, preserving `gc/lock` and `tmp/`. Bazel
updates mtime on cache access, so this retains the run's working set rather
than imposing a size quota or waiting for idle GC. A failed restore, marker or
trim prevents saving; cache transport is optional and does not waive validation.
Restore and save select only `ac/` and `cas/`; locks and in-progress writes
stay on the runner.

### CI product gates and R2 validation

See [CI routing](test/validation-and-review.md#continuous-integration) for job
selection and required verdicts.

After native completion passes on Linux, a [scoped lint build](../../test/lint/check_scoped_reports.py)
checks bare Bazel bootstrap and delivery of a usable paired-header report
without the portal launch rc or R2. It can reuse the disk cache. This daily
step does not run a bare `TestRunner`; to check that boundary directly, run
`bazel test --nocache_test_results //:tests` with the [native prerequisites](#direct-bazel-without-the-portal).

The daily Bazel jobs upload attempt evidence and per-target test logs even
when checks fail. Each native job also appends a best-effort summary to
`GITHUB_STEP_SUMMARY`: the standard BEP verdict and admitted test counts of the
check attempt, per-invocation Bazel timing and action-cache counters, and
available R2 stop accounting. Nested phase times overlap and are not additive job time.
Bazel's local incremental action-cache counters, reported execution runners,
test-result reuse and whole-job R2 blob transfers remain separate; none is
presented as an R2 hit rate. Missing or incomplete evidence is marked unavailable,
not zero or successful. Summary generation adds no measurements, cache probes or
validation gates and does not replace the raw artifacts.
The macOS and Windows jobs restore separately keyed vcpkg and managed-tool
caches. They seal vcpkg archives after dependency preparation so an admitted
cache fill can survive a later validation failure. The workflow saves managed
tools only after their preparation checks pass; Windows keys the LLVM SDK
separately and checks its completion marker before saving. For local timing
and evidence, see [phase capture](build.md#measure-native-validation).

With `AOBUS_BAZEL_R2_CACHE_AUTHORIZED=true`, trusted main pushes write the shared
`bazel-v1/` prefix and same-repository PRs consume it with proxy uploaders
disabled. Fork PRs receive no R2 binding; secretless runs keep their local cache.
Only CI supplies this R2 binding; local commands retain their disk-cache
default. A manual `r2=true` dispatch trusts the selected checkout and uses
run/attempt-isolated `bazel-ci-validation/` prefixes. It requires a nonempty
proxy URL and a successful drain, so local-cache fallback cannot pass. It uses
the `R2_SCCACHE_ENDPOINT`,
`R2_SCCACHE_ACCESS_KEY_ID` and `R2_SCCACHE_SECRET_ACCESS_KEY` secrets and the
`aobus-sccache` bucket; it needs no other bucket or credential.

Same-repository PR consumers use the same credentials with proxy uploads
disabled; they do not receive a backend read-only token. Reused bucket-level
writer credentials retain their write rights, including in an upload-disabled
consumer. Prefix separation, a YAML condition and disabling
upload workers are **not backend authorization**. Repository-level secrets
remain addressable by a malicious same-repository workflow edit; this route
does not claim to isolate that threat. Fork/secretless runs cannot perform the
requested live validation.

The [remote-cache helper](../../script/ao/portal/bazel_remote_cache.py) downloads
the version and SHA-256 that
[`bazel-remote.json`](../../script/ao/bazel-remote.json) pins. Its CI start and
stop calls use the governed Python: the Nix shell on Linux, the selected
Homebrew Python on macOS, and the managed interpreter selected from
`toolchain.json` on Windows, never an ambient runner Python. Each cached Bazel
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

<a id="scope-boundaries"></a>

## Completion

Native `bazel build`/`test` success is not complete validation. Required
executable validation is an artifact contract, not a general quality gate;
ordinary tests do not replace tooling, lint, hygiene, documentation,
physical-output, or product acceptance. Raw sanitizer profiles are
caller-owned scoped instrumentation, not managed completion substitutes;
coverage keeps its separate project-specific route. Select completion checks
by changed behavior in [validation and review](test/validation-and-review.md).
Tests of the Bazel support layer follow the
[Bazel support test boundary](test.md#bazel-support-tests): generators, integration
mechanisms and specific failure contracts, not a second representation of the
complete product build. C++ tests executed by Bazel retain their own behavior
contracts.

Validate build and policy changes with the affected native hosts and profiles.
On Windows, validate the selected SDK's builds, product startup, decoder
workflows, allocator integration and catalog pipeline; an archive inventory
or results from a former linkage are insufficient. See the [Windows SDK
validation requirements](windows.md#dependency-linkage). Compiler-action
evidence must match the compiler selection and profile being validated,
including the effective optimization and debug information required by the
macOS mode. Development build and test results do
not establish physical playback, complete GUI, ARM64, shipping/release, RBE
or cold-build performance acceptance.
Development build/run support does not establish distribution readiness; see
[binary distribution](dependency-governance.md#binary-distribution).
