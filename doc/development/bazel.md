---
id: development.bazel
---
# Bazel development slice

Use Bazel for Core, CLI, TUI, Linux GTK, incremental macOS AppKit and the
bounded native Windows WinUI product, plus the bounded Linux CI slice.
CMake retains complete native product/release builds and completion gates.
Nix/vcpkg remain the dependency version and installation authorities; Bazel
consumes their artifacts without configuring CMake or parsing installed CMake
exports. BCR supplies build rules, not application dependencies.

The cacheable native C++ lint route is `./ao bazel lint`;
[lint reports](#lint-reports) defines its workflow, cache and failure contracts. It is explicit and
has bounded native Debug evidence on Linux, macOS and Windows.
Existing `./ao tidy`, hygiene and CMake-backed fixture commands remain the
default and are not replaced automatically.

## Setup and commands

Run from the repository root through `./ao`; Windows uses `ao.bat`.
Linux uses Bazel from the pinned Nix shell. On native macOS/Windows, `setup`
verifies or downloads the official pinned executable into managed state with
SHA-256 verification. `AOBUS_BAZEL` overrides the executable for diagnostics,
but still requires the pinned version.
The pins are owned by [bazel-toolchain.json](../../script/ao/bazel-toolchain.json),
[.bazelversion](../../.bazelversion) and [MODULE.bazel](../../MODULE.bazel).

```bash
./ao bazel setup
./ao bazel provision --path /tmp/build/aobus-bazel-debug  # native macOS/Windows only
./ao bazel deps --path /tmp/build/aobus-bazel-debug
./ao bazel test --path /tmp/build/aobus-bazel-debug
./ao bazel test --config release --path /tmp/build/aobus-bazel-release
./ao bazel hygiene --path /tmp/build/aobus-bazel-debug
```

On native macOS and Windows, `provision` installs the shared vcpkg manifest
into the selected tree's `vcpkg_installed` with the pinned vcpkg and the
triplets, overlays and binary cache of the native CMake presets, without a CMake
configure, so unchanged package ABIs restore from that cache. Windows target libraries
use `x64-windows-static-md`: static portable dependencies with the dynamic CRT,
not the former `x64-windows` DLL SDK. The Windows presets select `x64-windows` for host tools, independently of the
static target triplet; host tools need not be static. Windows `provision` also
restores the locked [WinUI NuGet closure](#winui-product) and installs the
pinned [LLVM SDK](windows.md#llvm-sdk-and-native-lint-tools) for
[lint](#lint-reports) into the verified cache that CMake trees share, under the
same lock and completion marker. Linux needs no
provisioning. Other actions use the tree's provisioned triplet and never read
CMake trees; `--vcpkg-prefix <installed/triplet>` replaces it, for example with
an existing CMake tree's install. Rerun `provision` after changing the vcpkg
manifest, configuration or overlays: Bazel itself never restores missing ports
or substitutes an ambient installation. An interrupted `provision` waits for
the running installer process to exit before propagating the interruption and
releasing tree or shared-cache locks. Cancellation can wait for the tool's natural
completion; it does not contain detached descendants a tool leaves behind. The
[process runner](../../script/ao/core/proc.py) uses owned-child POSIX status polling
to avoid an interrupt-sensitive Python wait lock; an unavailable exit status is
an error, never a successful installation.
Follow [macOS setup](macos.md) or [Windows setup](windows.md) for native SDK
prerequisites and the [shared-source Windows boundary](#native-hosts-and-caching).
Windows Bazel commands use governed base Python without probing or provisioning
Ruff, mypy or documentation packages; explicit tooling/hygiene/docs commands keep
their managed checks.

The [command owner](../../script/ao/command/bazel.py) advertises `setup`, `provision`, `deps`,
`build`, `test`, `query`, `cquery`, `aquery`, `run`, native
[`verify`](#compile-contract-verification), native [`lint`](#lint-reports) and
its [`hygiene`](#hygiene-gate) gate.
`lint` is a scope/report action, not a label-forwarding build action; it does
not accept forwarded Bazel arguments. Bazel coverage is unsupported; use the
existing CMake-backed [`./ao coverage` workflow](test/coverage-workflow.md) on Linux.
`deps` fetches the four governed repositories and may reevaluate them; use
ordinary build/test invocations to assess warm repository reuse.
Common options include `--config debug|release`, `--path`, `--vcpkg-prefix`,
`--jobs` (default 2), and `--remote-cache`; availability depends on the action.
`AOBUS_BAZEL_REMOTE_CACHE`, when set, is `--remote-cache`'s default.

Pass ordinary Bazel arguments after `--`:

```bash
./ao bazel test -- //:audio_tests --test_filter=Decoder --test_output=all
./ao bazel aquery -- 'mnemonic("CppCompile", //lib/audio:audio)' --output=jsonproto
./ao bazel cquery -- //lib/query:query
./ao bazel test -- --test_filter=Decoder
```

When omitting labels, use `--option=value` for value-taking forwarded options
so the launcher can retain its bounded defaults without a second Bazel parser.
Query actions require explicit expressions; `run` requires a target.
Query/cquery/aquery stdout contains only the result; preparation, progress and
build-lock waits go to stderr. Analysis failure stderr is emitted once, not
replayed from the combined `<tree>/build.log`. `run` inherits the caller's standard
input, output and error, preserving terminal detection for interactive frontends;
it does not overwrite an existing build log.

## Scope

The graph covers static Core utility, LMDB/library, query, coroutine primitives,
encoded-media readers/containers, complete host-selected Core audio,
CoreRuntime/AppRuntime, UIModel, the real CLI/TUI entrypoints, and the complete
Linux GTK application and existing incremental macOS AppKit product with shared
desktop-launch composition. This does not establish GTK/WinUI feature parity.
The bounded native Windows slice additionally covers the real WinUI desktop
product ([WinUI product](#winui-product)); installation, signing, notarization
and complete release packaging remain outside this development slice. The complete
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
that full library and reuses the complete CMake-owned UIModel case membership,
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
./ao bazel build -- //app/cli:aobus //app/tui:aobus-tui
./ao bazel run -- //app/cli:aobus -- --help
./ao bazel run -- //app/tui:aobus-tui -- --help
./ao bazel test -- //:cli_tests //:tui_tests
# Linux only; native GUI execution is explicitly selected.
./ao bazel build -- //app/linux-gtk:aobus-gtk
./ao bazel run -- //app/linux-gtk:aobus-gtk
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
./ao bazel build -- //app/macos-appkit:aobus-appkit
./ao bazel run -- //app/macos-appkit:aobus-appkit -- \
  --state-root /tmp/aobus-bazel-appkit-state --library /tmp/aobus-bazel-appkit-music
```

The [AppKit target](../../app/macos-appkit/BUILD.bazel) produces `Aobus.app`,
not a replacement smoke executable. Use separate disposable state and library
paths for development launches; `--library` opens and scans that directory.
Omitting isolation can use personal application state and library data.
The GUI product is outside the unlabelled test aggregate.
The shared desktop-launch owner supports Linux and macOS; macOS Boost.Process
and Boost.Filesystem use static artifacts from the existing vcpkg SDK, without a
new installer.

The package-local [Objective-C++ rule](../../app/macos-appkit/objc.bzl) uses public
`cc_common` APIs and the governed Homebrew toolchain. That toolchain owns ARC,
mode flags, warnings and external-header policy alongside the SDK and deployment
target; no default Apple/Xcode toolchain or new compiler pin is introduced.
The local [bundle rule](../../app/macos-appkit/bundle.bzl) declares the original
Info.plist template, icon, brand license, catalog inventory and minimum-OS
metadata as inputs. Its native Python action retains the catalog SDK's declared
host-absolute interpreter boundary, not a new hermetic Python toolchain.
CMake retains complete native product/release gates and the existing
[AppKit GUI smoke scenarios](macos.md#appkit-smoke-scenarios). This development
bundle adds no installation, signing or notarization workflow.
Product build/run support alone does not establish GUI or ARM64 acceptance.

### WinUI product

On native Windows, explicitly build or run the real WinUI product:

```bat
ao.bat bazel build -- //app/windows-winui:aobus-winui
ao.bat bazel run -- //app/windows-winui:aobus-winui
```

The [WinUI target](../../app/windows-winui/BUILD.bazel) stages a real
executable-local layout — `Aobus.exe`, `Aobus.pri`, both XBFs, `Assets` and the
SDK's selected runtime DLLs — not a launcher around a CMake build. Generated
XAML/MIDL output follows the selected `--config`'s Debug/Release mapping. The
shared Windows stream adapter
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
`n/<lock-id>/p`, which `ao.bat bazel provision` and CMake WinUI trees restore
under the same lock and completion marker; preparation selects it once complete,
and `AOBUS_NUGET_PACKAGES_DIR` selects another restored cache. The repository
rule consumes the existing
[packages.config](../../app/windows-winui/packages.config) and dependency
policy and never restores, installs, or registers versions. The original `Y:`
shared-source Bazel root, guest-local NTFS outputs and the portal-selected vcpkg
static-md provider modes are unchanged. Its `sdk.json` carries only the execution
metadata consumed by codegen and PRI actions; `sdk-diagnostics.json` retains the
full SDK observation for inspection, without becoming an action input. Both are
projections of the same SDK query, not additional dependency authorities.

The query and action projects share the
[native VC project spine](../../app/windows-winui/msbuild_project.py), while
each caller retains its own properties, targets and forbidden operations. Private
projects suppress Common user/machine wildcard imports, project extensions,
Directory.Build customization and forced C++ imports; declared installed VC SDK
imports remain enabled. Query and action commands disable automatic MSBuild
response files. The helper is a watched repository source and a declared
codegen/PRI input; final executable staging does not load it. Generation tools
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
libraries remain DLL-interface imports beside the static portable vcpkg
dependencies, so this is not a DLL-free executable. The default completion and
GUI-scenario orchestration remain CMake-backed. Bazel also exposes the native
checker, lint and explicit probe targets, but build/run availability is neither
WinUI functional acceptance nor full CMake-check equivalence. This route adds no
signing or install workflow.

Historical native Debug/Release product builds passed authored/generated C++
compilation, archive and PE linking, embedded UTF-8 manifest validation and the
executable-local staging described above. The observed AMD64 GUI-subsystem
executables used the dynamic debug CRT in Debug and the normal dynamic CRT in
Release; their manifests declared `asInvoker`, UTF-8, PerMonitorV2 and longPathAware.
Distribution, signing and full CMake-check equivalence remain outside this evidence.

Observed interactive launches of both configurations under the same non-elevated
console user covered empty-library startup, rendering and shutdown after an
ordinary close, not complete WinUI GUI acceptance. Those diagnostics isolated only the child's `LOCALAPPDATA` and
`APPDATA` and used fresh working directories; they were not a whole-profile write
audit or shipping isolation controls. Settings, the library database and workspace
persisted while the fallback library stayed empty. The initial window extended
beyond the framebuffer's right/bottom edge. Constrained or full-window layout,
transport geometry, menus, keyboard interaction, playback and personal-library
scans remain unvalidated by those launches.

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

Unlabelled build/test commands select `//:tests` through
[`DEFAULT_TEST_TARGETS`](../../script/ao/core/bazel_workspace.py).
The [root BUILD](../../BUILD.bazel) owns that aggregate and existing subsystem
convenience suites. Its membership is non-configurable; platform compatibility
belongs to the member executables. [test/BUILD.bazel](../../test/BUILD.bazel)
owns complete case membership at module boundaries, not migration milestones.
Runtime keeps its fatal self-reentering main; catalog and shared desktop contracts
have their own module tests. The portable WinUI model is shared by product and
cross-host tests, while native WinRT cases join that frontend owner only on Windows.

GTK and native-backend tests remain explicit service/hardware boundaries:

```bash
./ao bazel test --headless -- //test:gtk_test
./ao bazel build -- //test:backend_test
./ao bazel build --config release -- //test:ao_perf_baseline //test:ao_tui_bench
```

Linux `--headless` reuses the portal's owned Xvfb and no-activation private session
bus for the selected test process. It never adopts personal desktop endpoints.
GTK retains its original main, accessibility/rendering profile and GApplication
probe. Native backend/hardware filters require the same admission as CMake tests;
compilation does not authorize real playback. Performance executables stay manual,
with independent entry points; Bazel opt compilation is not CMake's IPO performance
baseline. AppKit smoke and WinUI probes likewise retain their existing independent
native entry points and bundle/resource identities, outside unattended selection.

Library/doc/WinUI source inputs use exact declared runfiles, including manifest-only
Windows execution. The WinUI source-inspection case receives a manifest of authored
inputs instead of recursively reading an undeclared checkout. CMake retains the
same test bodies with its source-root fixture adapter.
Explicit labels retain normal Bazel meaning: `//...` is not rewritten and may
be expensive on a shared source filesystem.

Audio includes every Core implementation for the selected host, not just
NullBackend. Common and bounded native SDK tests retain existing assertions
and case markers. Core discovery and provider contracts join
`//test:audio_platform_test`; the native `ao_backend_test` cases have the explicit
`//test:backend_test` owner. The [graph parity test](../../test/script/test_bazel_audio.py)
compares their platform membership with CMake. Ordinary Core coverage includes
ALSA null/missing-device calls, synthetic Core Audio UID lookup and native
provider construction, not only pure fakes; live endpoint and service requirements
remain explicit on the backend suite.

## Trees and direct Bazel

`--path`, then `BUILD_DIR`, selects one exact external tree; otherwise the
platform build root supplies `bazel-<config>`. Source-contained trees, ancestors
of the checkout and CMake trees are rejected. Keep separate Debug/Release trees.
Do not reconfigure a tree during another invocation or remove an active tree or
lock: the portal serializes writers and Bazel owns its server/output-base lock.

The [workspace launcher](../../script/ao/core/bazel_workspace.py) writes:

- `<tree>/configuration/request.json`: native host, profile and selected provider;
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
certify another host environment or replace native prerequisites.
The portal pairs Debug/Release providers with Bazel `compilation_mode=dbg|opt`;
imported APIs use select guards requiring the matching mode, while host execution
tools are selected independently.

The portal reserves configuration, provider, environment, startup/output-root
and unsafe CRT/GSL overrides. This pairing is not proof that arbitrary user
copts preserve ABI/assertion semantics. The rc is configuration, not a security
boundary against rewritten inputs or arbitrary direct Bazel overrides.

## Architecture

```text
./ao selects environment, provider and external tree; writes launch configuration
  -> persistent Bazel server
     -> repository rules invoke the governed Python resolver when inputs change
     -> imported interfaces, native tools and project BUILD graph
     -> generation, compilation, linking, tests and cached actions
```

The portal is a launcher, not an outer dependency freshness engine.
[Repository rules](../../bazel/dependencies.bzl) invoke the
[resolver](../../script/ao/core/bazel_resolver.py), reusing existing provider
identity, metadata and guarded-interface logic. Python returns metadata and
explicit artifact links, never installs/builds dependencies. Bazel owns
materialization, watched-input invalidation and repository failure handling;
the launcher does not stage and compare whole Python-owned artifact trees.

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

- `@aobus_native_deps`: Core libraries/codecs, Linux ALSA/PipeWire/SPA/udev,
  GTKmm, and host-selected Boost.Process/Boost.Filesystem interfaces;
- `@aobus_test_deps`: static Catch2/Catch2Main and reviewed FakeIt Catch integration;
- `@aobus_native_tools`: governed gperf, catalog ICU/Python SDK, Linux resource
  compiler and test daemon, and Windows manifest tools; catalog compiler ICU
  imports cover Debug/Release
  independently of the target provider mode;
- `@aobus_toolchain`: compiler identity and the selected Nix, macOS LLVM or MSVC SDK;
- `@aobus_winui_sdk`: the installed WinUI NuGet SDK's headers, bootstrap sources,
  import libraries, runtime payload and generation tools, projected lazily from
  the restored NuGet cache for the [WinUI product](#winui-product) and its shared projection.

[Header ownership](../../script/ao/core/bazel_headers.py) comes from installed
per-port vcpkg `.list` files. Sparse imports project only reviewed owned closures,
preserving include order and topology such as RapidYaml's `../c4` wrappers.
Boost family ports must be coherent; FakeIt's child-only interface excludes
standalone/sibling integrations. No whole-triplet include directory is linked.
Header and library labels omit absolute provider prefixes for action-path stability,
not guaranteed cache hits. Linux shared Nix imports retain linker-only search hints
from their declared ELF runtime search directories. Indirect dependency lookup
therefore prefers those provider directories over command-line runtime paths that
might contain incompatible same-SONAME libraries. These hints do not inventory the
complete recursive runtime closure.

The [adapter](../../script/ao/core/bazel_deps.py) and
[provider projection](../../script/ao/core/bazel_workspace.py) prefer metadata.
Missing module groups permit only complete reviewed fallbacks; partial groups
fail. Windows and macOS default to static dependency imports; Linux defaults to
shared imports with reviewed per-package static interfaces. Windows static
imports use reviewed archive names, installed non-importing headers and
applicable consumer definitions. ICU and FLAC use their static SDK headers;
shared-package headers are not repaired by inventing a blanket consumer macro.
A `.lib` suffix or absent DLL alone is not static-link evidence;
old DLL/import-library layouts are not a fallback. Provider versions, raw port
revisions, artifact roles, configuration roots, expected shape and applicable
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

Toolchains own compiler parameters, not rc files, Python mode branches or launch
JSON. The [repository rc](../../.bazelrc) selects modes/toolchains, clears ambient
repository/action channels, enables strict action environments and requires
Windows manifest validation; it contains no raw compiler/linker parameters.
Launch/server/cache configuration belongs to the launcher.
Repository clearing is separate from action clearing: `--repo_env==CPLUS_INCLUDE_PATH`
unsets the variable, rather than assigning an empty path entry.

The rc sets the upstream repository guards
`BAZEL_DO_NOT_DETECT_CPP_TOOLCHAIN=1` and `BAZEL_NO_APPLE_CPP_TOOLCHAIN=1` through
`common --repo_env`, disabling both generic and Apple default C++ toolchains.
With the current [module graph](../../MODULE.bazel), resolution must use a
registered governed native toolchain; no compatible governed toolchain means
failure, not ambient fallback. These guards do not replace repository/action
environment clearing or control warning-feature selection.

[REPO.bazel](../../REPO.bazel) requests `aobus_warnings` for standard rules_cc
compiling targets in the main repository. Ownership follows the compiling target,
not the source filename, suffix, generator or macro origin. Generated C++ and
external sources compiled by a main-repository target receive it; external
compiling targets, including rules_cc runfiles, do not inherit it.
Features do not propagate through `CcInfo` or `compile_contract`. No wrapper,
macro, target registry or repeated per-target feature attribute is needed;
custom compile rules must honor feature selection themselves.

Native Bazel feature merging applies:

- Package/rule settings can override repository defaults; a rule can re-enable a
  package-disabled feature.
- An empty rule feature list does not clear inherited defaults.
- Global `--features=-aobus_warnings` disables even local requests; global
  `--features=aobus_warnings` can enable external targets too.

Main-repository Starlark definitions are instantiated by a thin host-selected
BUILD in `@aobus_toolchain`, retaining external target names and caller-relative
tool inputs. Compiler policy edits follow Bazel load/analysis edges rather than
rerunning native provider discovery; Python emits only metadata, links and the
thin call, not copies of policy templates. Unselected hosts are not eagerly
loaded through separate main BUILD packages.

[Shared Unix warning values](../../bazel/compiler_policy.bzl) precede ordinary
user copts in the [Linux toolchain](../../bazel/linux_toolchain.bzl),
which adds Linux diagnostics, and the
[macOS classic configuration](../../bazel/macos_cc_config.bzl).
The Windows bridge appends warnings after user flags: opt out with the feature,
not `/WX-` precedence. The default-policy checker requires the full warning
contract; intentional opt-outs are not expected to pass it.

External-header classification, language/ABI defaults and hardening are
independent of warnings. The Windows toolchain default-enables the pinned
`external_include_paths` feature through a data-free implication, for both target
and exec consumers, including external targets. The legacy bridge registers only
the enabled wrapper, not a duplicate of the upstream header-path feature.
Linux/macOS default-enable their corresponding feature. MSVC represents Bazel's
quote include contexts with `/I`: checks recognize exact declared per-action
contexts, including private dependencies, separately from provider `/external:I`
interfaces, not arbitrary prefix exemptions.

The headerless [`//bazel:compile_contract`](../../bazel/BUILD.bazel) always enables
GSL checking for utility headers and pure probe support. Actual `compilation_mode`
selects `TERMINATES` for `opt`, `ASSERTS` for `dbg`/`fastbuild`, independently of
the prepared provider. Selected compiler/runtime/SDK/shim identity is recorded in
`identity.json` and declared directly through toolchain tool/policy inputs;
it does not rely on a consumer macro or library dependency.

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
[pinned shell's report](../../shell.nix); default CMake builds use the shell's
GCC, so this is not universal compiler parity. Preparation does not probe
`CC`/`CXX`: SDK tools, standard-library/builtin roots and Nix wrapper hardening
metadata come from that report.
The builtin include allowlist contains compiler, standard C++ library and sysroot
include directories, never the whole Nix store or application dependency roots.

Actual Bazel mode features select arguments and salted
`NIX_HARDENING_ENABLE_<suffix>` action environments; Python supplies metadata,
not profile-specific `--copt` branches. Toolchain identity is declared data of
tools and policy arguments, including mode-specific hardening, so compilation
and link action keys include it.

| Mode | Compiler policy | Nix hardening policy |
|---|---|---|
| `dbg` | `-O0 -g`, no `NDEBUG` | Supported policy except `fortify`/`fortify3` |
| `opt` | `-O2 -g0 -DNDEBUG`, function/data sections | Full supported policy |
| `fastbuild` | `-O0 -g0`, no `NDEBUG` | Supported policy except `fortify`/`fortify3` |

Only optimization-dependent fortify is excluded in Debug/fastbuild; other
supported hardening remains. The Clang wrapper filters unsupported `fortify3`
to level 2 in optimized mode. Ordinary Linux CMake Debug is also unoptimized;
its [persisted Nix launcher policy](build.md#linux) separately normalizes shell
hardening channels. Sanitizer profiles retain their existing CMake policy.
The portal exposes only Debug/Release, with imported providers prepared and
guarded for `dbg` or `opt`; the mode table does not establish whole-provider
`fastbuild` acceptance.

## Windows compiler configuration

The [Windows definition](../../bazel/windows_toolchain.bzl) narrowly reuses
pinned rules_cc's private `windows_cc_toolchain_config.bzl` and documented
`extra_known_features` migration bridge for disabled `/W4`/`/WX` warnings and
`extra_enabled_features` for the independent external-header policy.
Revisit this private dependency when upgrading rules_cc. The default module
extension has no public native MSVC policy-injection hook; adopting the modern
public API would require owning the full action configuration. No private
provider-field or visibility bypass is used.

The legacy toolchain keeps upstream response files, archive/link behavior, PDBs,
runtime copying and CRT features. C++ language, UTF-8 and ABI defaults are local;
upstream supplies `NOMINMAX` and `_CRT_SECURE_NO_WARNINGS`. Effective standard is
`/std:c++latest`, after upstream `/std:c++17`, retaining the known D9025 override
diagnostic. Native dynamic CRT defaults are `/MDd` in `dbg`, `/MD` in `opt`;
redundant target/host rc selections are unnecessary. Actual compilation mode
selects these for each consumer. The catalog compiler is a real compiled
execution tool: its execution transition must use optimized imports and `/MD`,
independently of a Debug target's `/MDd`. Target-only evidence cannot establish
that host-tool contract.

Windows `request.json` captures original `INCLUDE`, `LIB`, `PATH`, `TMP`, VC/SDK
versions/roots and optional `NETFXSDKDir` before resolver scratch TMP replacement.
The [workspace projection](../../script/ao/core/bazel_workspace.py) validates exact
selected VC/SDK interfaces, versioned SDK directories and x64 libraries, never a
whole SDK root or provider-header exemption. Checked auxiliary VS includes and
selected NETFX `include/um` and `lib/um/x64` follow the same narrow validation.
Existing Visual Studio bootstrap/SDK installation remain authoritative; JSON
and Python supply no compiler flags/mode branches or new MSVC pin/discovery manager.

`cl.exe`, `link.exe`, `lib.exe` and `ml64.exe` execute at their installed paths to
preserve DLL layout; Bazel-owned links declare executable bytes. Identity and
native-path metadata are declared through legacy toolchain file groups, not the
bridge's `cc_args.data`. Provider/input guards and mutable-input watches remain.
Required unused Unix tool roles point to missing placeholders and fail if used;
these do not advertise Unix tools, assembly or module support.

## Compile-contract verification

Run the explicit native Linux, macOS or Windows analysis gate separately from
build/test. For Linux/macOS:

```bash
./ao bazel verify --config debug --jobs 2
./ao bazel verify --config release --jobs 2
```

On Windows use `ao.bat bazel verify` with the same options and a supported local
source checkout. UNC/mapped-network sources remain rejected, including by this
analysis entry. `--path` selects an external host-local Bazel tree as for other
Bazel commands, including its provisioned vcpkg triplet.

`verify` accepts profile, path, provider prefix and jobs, not labels or forwarded
Bazel options. After provider preparation, the
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
not passed. The first failure stops later stages. This covers captured
`CppCompile` actions, not AppKit Objective-C++ acceptance or native product
execution. Ordinary builds do not implicitly run this policy checker.

## Lint reports

`./ao bazel lint` caches native C++ analysis reports and gates the selected
findings. It uses actual Bazel `CppCompile`/`ObjcCompile` contexts, not a CMake
compilation database. The [aspect](../../bazel/lint/lint.bzl),
[launcher](../../script/ao/core/bazel_lint_action.py) and
[current-report gate](../../script/ao/core/bazel_lint_reports.py) implement the
contracts below.

```bash
./ao bazel lint --target //lib/utility:utility
./ao bazel lint lib/utility/Base64.cpp
./ao bazel lint --folder lib/utility
./ao bazel lint --all
./ao bazel lint --commit origin/main
```

The command accepts the common C++ scope selectors (`files`, `--folder`,
`--commit`, `--all`), `--config`, `--path`, `--vcpkg-prefix`, `--check`,
`--header-filter`, `--output`, `--debug`, `--jobs`, `--clang-tidy-arg` and
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
selection reaches the aspect through a repository rule
outside the policy that every report action reads, so naming a consumer reruns
no existing report.

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
current invocation's BEP and local report artifacts; it does not scan stale
output directories or join `aquery` data. A clean selected set returns 0,
selected warnings or ordinary compiler errors return 1, and incomplete
coverage, infrastructure failure or an invalid report returns 2. Cached warnings still fail. `--debug` displays per-run evidence paths
and publishes all action completions in the BEP for diagnosis; those events are
not the report inventory or proof that a disk-cached action executed anew. The
tree keeps the eight most recent run evidence directories.

`--check` and `--header-filter` change the analysis key; the selected file set
does not. Raw `--clang-tidy-arg` values are limited to diagnostic-only
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
feature and action environments in producer precedence order. Its pinned
feature admission accepts `aobus_warnings`, `static_link_msvcrt`,
`dynamic_link_msvcrt` and `default_compile_flags`; unsupported requested or
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
provider identity belongs to the shared toolchain, not a second lint policy.

One cacheable verification action checks the host version and complete
fixture-derived `aobus-*` registry; reports consume that result. The helper's
declared import closure is tested, so unrelated portal edits need not invalidate
analysis. Temporary compilation databases are action-local. Logs normalize
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
parsing. Explicit Windows `ao.bat bazel provision` installs that governed SDK;
ordinary lint does not. Both checker forms use C++23 and the selected SDK ABI.
Unix builds the existing plugin against its externally supplied host, excluding
product sanitizer instrumentation from compilation and linking. Windows builds
the linked `AobusClangTidy` host optimized with `/MT`, independently of product
`/MDd` or `/MD`. Plugin/host ABI and sanitizer compatibility must be preserved;
macOS SDK C include ordering stays driver-owned. Registry objects and required upstream modules must survive linking. The
[SDK projection](../../script/ao/core/bazel_lint_sdk.py) derives the Windows
initializer closure from selected archive references rather than linking all
LLVM. Required system libraries, including DIA when enabled by the SDK, remain
part of that closure; a required library must exist. XML omission is allowed
only when the selected archives have no corresponding references.

The explicit/manual entry `./ao bazel test -- //test:lint_test` reuses real
fixture discovery, marker/message assertions, source-policy smokes and compiler
error canaries. Its direct verified action starts no nested project CMake,
Bazel or `ao tidy` preparation. Its resource declaration reserves the two
clang-tidy workers it launches. It stays outside the default test aggregate.

Target-only scope does not imply unpaired-header coverage. Selected-check
samples, source parity and cached action completion do not establish every
header, full presets, other native profiles, GUI or hardware acceptance.
Record actual invocation scope, deferrals and fresh execution versus cache reuse
with PR/CI evidence, not as a second changing acceptance ledger here. Existing
`./ao tidy`, hygiene and CMake-backed fixture commands retain the default
contributor/completion role until that policy is explicitly changed.

### Hygiene gate

`./ao bazel hygiene` runs the check-only stages of `./ao hygiene` on the same
scope: format check, test and name audits, and Python checks. Bazel lint
replaces CMake tidy for the C++ sources. It accepts hygiene's scope selectors
(`files`, `--folder`, `--commit`, `--all`) and lint's `--config`, `--path`,
`--vcpkg-prefix`, `--jobs` and `--remote-cache`. For C++ scope, Windows formats
with the `clang-format` of the LLVM SDK that `ao.bat bazel provision` installs
and requires that SDK to be ready before any stage. It never configures a CMake
lint tree, unlike ordinary `ao.bat format`, which keeps its CMake-backed SDK
ensure. Python-only and empty scopes need no LLVM SDK lookup. The managed Python
tooling environment is provisioned only when the scope contains Python files;
C++-only and empty scopes do not prepare it.
On macOS and Windows the gate itself stays on the base Python that Bazel
records as its interpreter, like every other Bazel command, and runs only Ruff and
mypy with the managed tooling environment, which it provisions; alternating it
with other Bazel commands therefore keeps their repository and action caches.
This gate serves Bazel trees. It does not replace `./ao hygiene` in
[validation](test/validation-and-review.md).

## Generated files and runtime data

The [gperf rule](../../bazel/gperf.bzl) uses the governed executable in execution
configuration, `LC_ALL=C`, declared grammar/tool/runtime/output and direct
arguments. Resolution executes `gperf --version` and rejects an unexecutable or
mismatched tool. It generates query's unit dispatch and four media dispatch
headers without CMake-generated headers or shell redirection.

The [catalog rule](../../bazel/catalog.bzl) declares the existing authored
inventory, text/templates, native ICU/Python SDK inputs and the real compiled
[`ao_catalog_compiler`](../../tool/catalog/BUILD.bazel) in exec configuration.
One [action helper](../../tool/catalog/build_catalog.py) follows the existing
strict genrb/validation/pseudo/pkgdata-common pipeline and emits aligned embedded
C++ data plus maintained locale tags. A private tree owns all resource/RESW/temp
side outputs; package.lst remains the sole locale inventory. No CMake configure,
prebuilt project tool or old CMake-generated resource is consumed. Common-data
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
and a declared marker for manifest/directory runfiles; CMake keeps its existing
data directory. There is no checkout fallback or fixture copy.
Windows static FLAC links Ogg's archive through its private dependency closure;
products, tests and probes no longer stage Ogg or mimalloc redirect DLLs.
CLI/TUI retain the existing executable-local `mimalloc-new-delete.h` integration
with static mimalloc. This preserves that C++ allocation interface, not broad
CRT `malloc`/`free` interception or arbitrary cross-DLL allocation redirection.
Ordinary tests do not link mimalloc. Catalog tools are a separate execution
boundary: the compiled catalog compiler and actual `genrb`/`pkgdata` tools retain
their declared inputs and runtime guards. The generated Windows static-md SDK
contains executable target-triplet ICU tools without adjacent ICU DLLs; its
separate host triplet supplies shared ICU tools, gperf and pkgconf. This observed
layout is not a blanket DLL-free host-tools guarantee. Catalog generation and
the compiled execution tool still need native validation.

The [Windows manifest aspect](../../bazel/windows_utf8_manifest.bzl) checks
first-party C++ executables through `deps`/`data` and declared exec `tools`,
including the default module tests, their sibling probes, explicit performance
executables, product frontends and the execution-configuration catalog compiler. `_validation` actions declare the EXE, verifier script and selected
SDK/system executable bytes, producing extracted XML and a success artifact;
unchanged inputs reuse Bazel's cache. Request the checks explicitly with:

```bat
ao.bat bazel build -- //test:windows_utf8_manifest_checks
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
bootstraps the pinned vcpkg, and Bazel does not bootstrap CMake/Ninja. Windows uses Visual Studio, its selected SDK, static portable target
dependencies and the existing dynamic CRT. No global `/MT` or replacement CRT
flag is added; upstream toolchain mode features still select `/MDd` and `/MD`.
Mutable selected native inputs are watched and content-hashed, not represented
only by version markers. On macOS this includes the real `xcrun`-selected linker,
libc++ and compiler-resource trees, SDK headers and link stubs, and the driver's
explicit LTO plugin. Link actions select that linker rather than relying on the
`/usr/bin/ld` dispatcher; compiler
and link actions disable implicit Clang configuration files. On Windows the
validated original compiler environment selects the host tool/runtime directory
and exact `INCLUDE`/`LIB` trees for identity, not resolver scratch or ambient
include paths. Directory membership changes also invalidate resolution.
Shell-backed rules need `BAZEL_SH`; Windows preparation selects the
`usr/bin/bash.exe` that Git for Windows installs beside `git`, unless the caller
sets `BAZEL_SH`.

Keep selected SDK/tool installations stable while resolution and actions run;
identity collection is not an atomic installation snapshot. These are native
installation inputs, not a packaged relocatable SDK or a complete
syscall/dynamic-loader closure; the project does not emulate loader search paths.
Runtime libraries, .NET/registry state and other inputs outside the enumerated
families remain trusted host state; changing
those is not certified by the selected SDK identity. Linux similarly trusts
immutable native Nix SDK state. None is an RBE contract.

The supported Windows shared-source route exports the same Linux checkout
read-only as a fixed virtiofs volume, tested as `Y:`, with guest-local NTFS
outputs and official Bazel. Run from the repository root: pinned drive-root
workspace discovery does not find it from a subdirectory. Mount provisioning
belongs to the administrator. Plain mapped SMB/UNC build/test roots remain
rejected; setup/dependency inspection does not prove SMB compilation works.
Do not copy the checkout, create aliases, change mounts or grant privileges to
bypass this boundary.

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

The [Linux workflow](../../.github/workflows/ci.yml) retains Bazel test and
verification failures while continuing to the complete CMake gate and, if that
passes, hygiene. A final non-cancelled check of the original Bazel step outcomes
keeps either failure or unexpected skipping fatal to the Linux job; CMake and
hygiene failures remain fatal independently.

The same workflow transports only
`${{ runner.temp }}/aobus-state/cache/bazel` using pinned cache restore/save
actions. Both Bazel steps explicitly set `AOBUS_STATE_ROOT`, the same root used
by compiler-cache activation, even when sccache setup is unavailable.
The versioned OS/architecture/pin-contract/commit key and same-contract fallback
select candidate archives, not ABI compatibility; Bazel action keys decide reuse.
Only successful pushes to `refs/heads/main` may save, after Bazel test and both
Debug/Release verification steps, before the CMake gate. PRs/manual dispatches
never publish; GitHub's branch restore rules may let untrusted PRs reuse main's
cache. After restore and before Bazel runs, CI creates a marker outside the cache.
Before saving, it removes only cache files whose mtime is not newer than that
marker, retaining the current run's used/produced working set rather than
accumulating old blobs through the fallback archive. This uses the pinned
Bazel's cache-access mtime behavior; it is not a fixed archive-size quota.
CI does not wait for idle GC. Failed trimming prevents publication, without
bypassing the required build/test gates. Transport failure is optional, not
failure of those gates.
The archive excludes servers, install bases, output trees, repositories, provider
installations and Nix state; it adds no remote endpoint or dependency manager.

### Standalone Bazel CI and R2 validation

The existing CI workflow accepts a manual `mode=bazel` selection. It skips the
Aobus CMake project jobs while retaining documentation validation, then runs
Linux, macOS Intel/ARM64 and Windows Bazel jobs. Each builds its shipping
frontends explicitly, runs the default tests, verifies compile contracts, runs
lint fixtures and checks scoped Bazel hygiene. Linux also runs headless GTK;
Linux and Windows run portal tooling tests. These manual native jobs fail on
errors rather than masking them with job-level `continue-on-error`. Normal
PR/push runs retain the existing CMake completion gates; macOS/Windows Bazel
jobs remain advisory there. No standalone sanitizer, coverage or full
`ao check` equivalence is implied. Package providers and isolated tooling
fixtures may still use CMake; the project itself is not configured or built
with it in this route.

For a reviewed, trusted checkout, explicitly select `r2=true` together with
`mode=bazel`. This reuses the existing `R2_SCCACHE_ENDPOINT`,
`R2_SCCACHE_ACCESS_KEY_ID` and `R2_SCCACHE_SECRET_ACCESS_KEY` secrets and the
`aobus-sccache` bucket; no new bucket or credential is required. Native
Linux/macOS jobs use run/attempt-specific `bazel-ci-validation/` prefixes,
separate from the automatic main cache's `bazel-v1/` prefix. Routine PR runs do
not bind R2 credentials. Automatic main-push use remains separately gated by
`AOBUS_BAZEL_R2_CACHE_AUTHORIZED=true`; all three compiler-cache R2 activation
steps likewise remain main-push-only.

This manual selection explicitly trusts the chosen workflow and checkout.
Reused bucket-level writer credentials retain their write rights, including in
an upload-disabled consumer. Prefix separation, a YAML condition and disabling
upload workers are **not backend authorization**. Repository-level secrets
remain addressable by a malicious same-repository workflow edit; this route
does not claim to isolate that threat. Fork/secretless runs cannot perform the
requested live validation. A requested R2 run fails on missing credentials or
an absent proxy URL rather than silently passing with local cache only.

The [remote-cache helper](../../script/ao/core/bazel_remote_cache.py) downloads
the version and SHA-256 that
[`bazel-remote.json`](../../script/ao/bazel-remote.json) pins. Each cached Bazel
build/test/lint/hygiene step selects its loopback URL explicitly. Startup
rejects occupied fixed ports and checks that its own child is live even when
HTTP status answers. Shutdown first waits for uploads with a bounded drain,
matching acknowledged writes and backend completions by object kind/hash;
timeout or stalled progress does not imply completion. An optional structured
stop receipt records prefix, traffic, failures and unfinished uploads without
credentials. Expired backend blobs make bazel-remote's gRPC dependency check
miss; a lifecycle rule is a storage/retention choice, not build correctness.

The dedicated Linux [R2 proof](../../script/ao/core/bazel_cache_probe.py) uses no
Actions cache restore. It builds one canary with a fresh client and proxy,
requires nonzero accepted uploads and a successful drain, then stops that
proxy. A second empty client and proxy build the same canary with uploads
disabled. BEP artifact uploads are kept local too: disabling action-result
uploads alone does not disable that separate uploader. Acceptance requires an
actual remote-cache-hit `CppCompile` record
for that canary in the structured execution log, a complete successful BEP and
backend downloads, not merely a ready server or a warm local proxy. This proves
same-runner producer/consumer use, not cross-host cache portability. The probe
never deletes a prior cache. Artifacts retain receipts, BEPs, execution logs
and client build logs, not raw proxy logs or cache contents. Workflow presence
and unit regressions alone are not evidence that a live R2 run passed.

The current helper has no pinned Windows proxy binary, so `windows_bazel` uses
its restored dependency/tool caches and local Bazel cache. That is a limitation
of this integration, not a prohibition on a Windows Bazel client using a
separately hosted remote cache. The existing Linux mixed job also runs optional
Bazel hygiene independently of its mandatory Bazel aggregation and cache save.
Native Bazel jobs restore dependency/tool caches but do not publish Actions
caches themselves.

## Completion

`ao bazel build` does not run formatting, hygiene, Ruff, mypy, docs checks,
`verify` or `lint`. Required executable validation is an artifact contract, not
a general quality gate. `ao bazel lint` is an explicit report gate, not part of
`ao bazel test` or `verify`. `ao bazel test` does not replace `./ao check`,
tooling, lint, hygiene, sanitizers, physical-output or product validation;
select checks through
[validation and review](test/validation-and-review.md).
Source/platform parity tests reconcile handwritten BUILD membership with CMake,
not compiler/link/runtime equivalence. Action checks do not transfer CMake's
full ownership or replace native tests.
Tests of the Bazel support layer follow the
[Bazel support test boundary](test.md#bazel-support-tests): generators, integration
mechanisms and specific failure contracts, not a second representation of the
complete product build. C++ tests executed by Bazel retain their own behavior
contracts.

Historical selected-graph tests and compiler-action evidence cover native
Debug/Release on Linux, macOS and Windows, not subsequent policy changes. Rerun
affected native gates after changes; Windows static-md validation must cover the
selected SDK's builds, product startup, decoder workflows, allocator integration
and catalog pipeline, not merely archive inventory or former dynamic-SDK receipts.
Older macOS receipts predate repaired mode flags and do not by themselves prove
optimized Release or debug information. No physical playback, complete GUI,
ARM64, shipping/release, RBE or cold-build performance acceptance is implied.
Development build/run support does not establish distribution readiness; see
[binary distribution](dependency-governance.md#binary-distribution).
