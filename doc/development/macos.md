---
id: development.macos
---
# macOS development

The native macOS profile builds the shared core, CLI, FTXUI terminal frontend,
AppKit desktop development slice, Core Audio backend, and native tests. GTK is
not built. The deployment target is owned by
`script/ao/macos-toolchain.json`; native compiler setup rejects project triplets
whose deployment values disagree with that authority.

This page owns platform prerequisites, portal commands, local state, native
AppKit smoke, and macOS validation. Machine access, source mounts, login/power
configuration, and persistent shell settings belong in machine-local guidance.
Current AppKit and Core Audio behavior belongs to the
[frontend](../system/frontend/README.md),
[session lifecycle](../system/session-lifecycle.md), and
[playback](../system/playback/README.md) contracts.

## Host setup

Install Xcode Command Line Tools and the Homebrew formulas selected by the
current lock:

```bash
xcode-select --install
brew install llvm@22 cmake ninja pkgconf python@3.14 \
  autoconf autoconf-archive automake libtool
```

The formula names above match the current lock; when diagnosing a future
mismatch, follow the exact formula named by the portal rather than an old shell
history entry. Nix is not used on macOS: `shell.nix` intentionally rejects
Darwin.

Run repository operations from the project root through `./ao`. The portal:

- starts with the locked Homebrew Python major/minor;
- selects the governed Homebrew Clang major and deployment target;
- downloads and SHA-256-verifies the pinned vcpkg source archive;
- selects `x64-aobus-osx` or `arm64-aobus-osx` from the native architecture; and
- resolves the checked-in vcpkg registry and manifest locks.

A cold vcpkg build may take several minutes. Later builds reuse host-local
source downloads and binary packages. Do not run ambient CMake or vcpkg for a
normal build.

Git hooks are separate optional setup; see the
[contributor workflow](../../CONTRIBUTING.md#optional-local-setup).

## Build, run, and check

Use `./ao <command> --help` for current options. Common tasks are:

```bash
./ao build                    # Debug graph
./ao build release            # Release graph; C++ uses ThinLTO
./ao build profile            # sampling build with frame pointers
./ao run cli
./ao run tui
./ao run appkit
./ao test                     # core, application, backend, and TUI fast loop
./ao test --lint              # custom clang-tidy integration fixtures
./ao test --all               # core, application, backend, TUI, CLI, and lint
./ao check                    # complete native Debug gate
./ao check release            # complete native Release gate
./ao check --asan             # supported suites with ASan/UBSan
./ao check --tsan             # TSan-safe core, application, and backend suites
./ao hygiene                  # scoped format/audit/tidy gate
./ao deps report
./ao deps verify
```

`./ao check` does not run fixture-dependent AppKit GUI scenarios. The macOS
`all` group intentionally excludes AppKit smoke and the Python `tooling` suite.
`./ao check --tsan` runs the full normal Debug completion first — which builds
the completion products, including the AppKit product bundle (built, never
run) — then a separate
managed TSan profile over the macOS TSan-safe suite group (currently
core, application, and backend) in the same selected tree; the sanitizer
profile itself does not build GUI smoke. This is not evidence for native GUI
behavior. Use
an explicit `./ao test --appkit --tsan ...` invocation when the selected AppKit
scenario is required under TSan.

The portal starts with Homebrew Python and selects the managed Ruff/mypy venv
on demand for format/tidy/hygiene work. Other commands can use the base
interpreter; `/usr/bin/python3` only reads initial bootstrap JSON fields.
The managed environment matches Ruff and mypy pins, but macOS does not claim
exact Python patch parity and does not expose `./ao test --tooling`.

Release IPO is checked for C++ only. C++ translation units and final links use
ThinLTO; Objective-C++ translation units use normal Release optimization.
See [optimized builds](optimized-builds.md).

The native triplets link third-party libraries statically. Before publishing
binaries or an AppKit bundle, review the shared
[binary-distribution obligations](dependency-governance.md#binary-distribution),
including mpg123's library modification/relinking requirements. Development
build and smoke-test success is not release-compliance evidence.

## Build and tool state

Default Bazel build trees are `/tmp/build/<checkout-directory>/bazel-<config>`
and contain `build.log`. `<checkout-directory>` is only the checkout folder's
final name, not a unique identity. Same-named checkouts sharing one base select
the same paths; use distinct names or absolute local build bases.

`AOBUS_BUILD_ROOT` replaces only the `/tmp/build` base; the portal still appends
the checkout name and `bazel-<config>`. `BUILD_DIR` selects one exact tree
instead. It is
never a base and takes precedence over `AOBUS_BUILD_ROOT`. An explicit `-p` or
`--path` takes precedence over both. An explicitly selected tree is
mode-neutral: the selected
profile, not the tree, picks the compilation mode, and the composite sanitizer
check uses one selected tree for its normal Debug completion and the managed
profile.

The default `/tmp` location is temporary storage, not a persistence guarantee.
For a persistent local base, for example, use
`AOBUS_BUILD_ROOT=/absolute/local/build-base ./ao check`.
Repeat the assignment for related invocations, or use a directory-scoped
environment. That base holds build trees only; `AOBUS_STATE_ROOT` still selects
the managed tool and package state described below.

Managed state defaults to `$HOME/Library/Caches/Aobus`:

- `tools/vcpkg/<revision>` — bootstrapped pinned vcpkg;
- `cache/vcpkg/downloads` and `cache/vcpkg/binaries` — reusable package state;
- `tools/venvs/<checkout>/<fingerprint>` — managed Ruff/mypy environment;
- `tools/libcxx-expected-shim/<llvm-version>` — generated compatibility header.

The Bazel environment repository materializes its own shim copy under its
repository-local `_state`; ordinary portal tooling uses the managed-state copy.
Both use the same fail-closed transform over the selected LLVM header.

`AOBUS_STATE_ROOT` replaces that base. `VCPKG_ROOT` is a diagnostic override for
a complete pre-bootstrapped checkout; registry and manifest locks still apply.
`./ao deps provision` installs the selected tree's vcpkg manifest.

Keep build trees, vcpkg state, managed Python environments, tools and SDK
caches on host-local storage, independently of the source location. The portal
does not prescribe or administer a source-sharing topology. Keep source files
stable while building or collecting validation evidence; concurrent edits are
not a supported snapshot mechanism.

This is a storage requirement, not automatic local-volume enforcement; verify
the actual backing storage for overrides. Ruff check/format and mypy use
`.ruff_cache` and `.mypy_cache` under the managed environment's `sys.prefix`,
including for local source checkouts. These explicit cache paths take precedence
over inherited tool cache settings. Check-only commands may write caches without
rewriting source.

This does not imply a write-free source checkout. Checkout-ID creation writes
private Git metadata, and ordinary Python `__pycache__` may appear beside sources.
`AOBUS_BUILD_ROOT` moves only build trees, not Git metadata or Python bytecode.

### Native checkout identity

The managed Ruff/mypy environment under `tools/venvs/<checkout>` is keyed by
the normalized absolute checkout path supplied to the native bootstrap and an
ID stored in its private Git directory. macOS does not canonicalize every path
alias for this key; a different symlink or path spelling can select another
environment. If no stored ID can be read or created, commands such as
`./ao hygiene` cannot initialize the environment.
Set one stable, unique `AOBUS_CHECKOUT_ID` for that checkout, per invocation or
in a directory-scoped environment.

An unconditional shell-profile export also overrides native clones' stored
IDs and can select different managed Python environments for them.
The override does not repair Git linkage. Default Git-based scope discovery
can fail during preflight, before managed-tool setup. Select explicit files,
`--folder <dir>`, or `--all` for format/tidy/hygiene to avoid that discovery;
these scopes still need a usable managed-tool identity. `--commit` remains
Git-dependent. For alternate metadata, use the separate binding below.

### Shared-source Git discovery

If a foreign linked-worktree `.git` pointer is unusable on macOS, pass the
intended private Git directory and the portal's actual source root per invocation:

```bash
AOBUS_GIT_DIR="$HOME/local-metadata/worktrees/aobus" \
AOBUS_GIT_WORK_TREE="/Volumes/source/aobus" ./ao format --check
```

These are example paths, not metadata to create or copy. Both variables must be
present, nonempty native absolute paths; malformed pairs fail when Git-backed
source discovery runs. Explicit files, `--folder`, and `--all` bypass this
discovery and its binding validation. A complete pair for another normalized
root is ignored; a discovery failure reports the mismatch. Use the root
spelling seen by the portal, rather than assuming aliases will match.
Before accepting the mapping, compare actual private/common/index paths, exact
HEAD, and semantic staged index with the intended checkout. The binding is
trusted configuration, not repository-association verification;
`--show-toplevel` alone is insufficient.

Only Git-backed source discovery calls use the pair. Discovery also uses
process-local `--no-optional-locks` and
`-c diff.autoRefreshIndex=false`: optional-lock suppression alone does not
prevent `git diff` from writing stat-only index refreshes.
Working-tree discovery also compares content, so stat-only differences do not
expand the selected source scope.
`setup git-hooks`, performance revision commands, and arbitrary Git operations
remain outside this support. The binding
does not rewrite `.git` or redirect checkout-ID creation. If the original
private Git directory is inaccessible, also supply a separate stable, unique
`AOBUS_CHECKOUT_ID`. Never export standard `GIT_DIR` or `GIT_WORK_TREE` across
the portal: dependencies and disposable test repositories must remain independent.
Neither the binding nor these discovery options prove a write-free checkout.

## Native desktop development slice

### Bazel AppKit product

The explicit `//app/macos-appkit:aobus-appkit` target builds the existing
incremental AppKit product as `Aobus.app`; it is outside Bazel's default test
aggregate. After `./ao deps provision` installs the tree's vcpkg manifest, build
and run from the repository root:

```bash
./ao build --target //app/macos-appkit:aobus-appkit
./ao run appkit -- \
  --state-root /tmp/aobus-bazel-appkit-state --library /tmp/aobus-bazel-appkit-music
```

Use separate disposable state and library directories, not your personal data:
the shipping application opens and scans `--library`, and an unisolated launch
can use its normal saved state. These are existing shipping arguments, not smoke
scenario controls.

Standard rules_cc `objc_library` targets compile the mixed C++/Objective-C++
implementation and the separate entry and smoke units with the existing governed
Homebrew compiler and SDK. The standard rule selects ARC; mode, warning, SDK and
external-header policy stay with the [composed toolchain](bazel.md#macos-toolchain-composition),
not a default Apple/Xcode compiler or a new compiler pin. The bundle action declares the original
Info.plist template, icon, brand license, catalog inventory and minimum-OS
metadata; Python retains the existing catalog SDK's host-absolute interpreter
boundary. Shared desktop launch uses the existing static vcpkg Boost.Process
and Boost.Filesystem artifacts without another installer.

This is the same incremental native frontend, not GTK/WinUI feature parity or
a new GUI smoke framework. The explicit
`ao_appkit_smoke` bundle builds the same existing smoke scenario
units and preserves their separate identity and controls; it is not an unattended
test. The scenario runner below owns the controlled scenarios. This bundle adds no installation,
signing or notarization workflow. Product build/run support is not native
GUI acceptance and does not establish ARM64 validation.

### AppKit smoke scenarios

`./ao run appkit` builds and launches the shipping development bundle. Native
smoke uses `ao_appkit_smoke`, which links the same production objects but owns
disposable scenario controls. The shipping executable does not accept those
test flags. Both startup paths must preserve the
[AppKit restoration and geometry ownership contract](../system/session-lifecycle.md#appkit).

Each scenario requires an absent or empty music directory and an absent or empty
state directory. The two paths must not equal or contain one another. The portal
rejects unsafe paths before creating the native application and prepares its own
deterministic media fixtures.

```bash
./ao test --appkit --scenario desktop \
  --state-root /tmp/aobus-desktop-state --library /tmp/aobus-desktop-music
./ao test --appkit --scenario authoring \
  --state-root /tmp/aobus-authoring-state --library /tmp/aobus-authoring-music
./ao test --appkit --scenario presentation \
  --state-root /tmp/aobus-presentation-state --library /tmp/aobus-presentation-music
./ao test --appkit --scenario quality-presentation \
  --state-root /tmp/aobus-quality-state --library /tmp/aobus-quality-music
./ao test --appkit --scenario media \
  --state-root /tmp/aobus-media-state --library /tmp/aobus-media-music
./ao test --appkit --scenario credits-controls \
  --state-root /tmp/aobus-credits-state --library /tmp/aobus-credits-music
./ao test --appkit --scenario credits-previews \
  --state-root /tmp/aobus-credits-previews-state --library /tmp/aobus-credits-previews-music
```

Use `--asan`, `--tsan`, or `--path` when the applicable bundle already belongs
to that tree. `--no-build` is valid only after the selected tree contains the
bundle. For native lifecycle or authoring work, run the relevant scenarios in
Debug, Release, and TSan; add ASan for memory-sensitive changes. Include both
`app/macos-appkit` and `test/macos-appkit` in scoped hygiene.

Treat each scenario only as evidence for the surface it drives:

| Scenario | Direct evidence |
|---|---|
| `desktop` | Shipping-window composition, native browsing and playback controls, close/quit coordination, restoration and geometry ownership, `desktop.plist` defaults for malformed values and unknown appearance names, per-mode inspector visibility restoration, unknown-key preservation, and supervised library-successor handoff. |
| `authoring` | Native property/List editors, validation and stale-draft behavior, save/close/discard ordering, and authoring teardown. |
| `presentation` | Browser projection and reuse, selection/sort/drag adaptation, activity and inspector presentation, accessibility dismissal actions, and inert callbacks after detach. |
| `quality-presentation` | Isolated classic quality-tooltip updates, clearing, resize and repeated-layout preservation, plus modern transport separation, using synthetic state and native bar captures without a runtime or audio endpoint. |
| `media` | MediaPlayer metadata/artwork publication, the seven supported remote commands and disabled unsupported commands, stale-seek guards, main-thread admission, runtime handoff, and retirement. |

The `credits-controls` and `credits-previews` scenarios follow the same
disposable-path admission, timeout, and evidence rules; their specific
direct-evidence surfaces are not enumerated here.

The isolated `quality-presentation` scenario exercises English and Simplified
Chinese catalogs at normal and constrained client sizes. Its captures show the
native playback bars, not opened tooltip pixels or a shipping playback trigger.
The broader `presentation` scenario also starts session-backed playback; it is
not a substitute for the isolated quality-binding check.

These scenarios invoke native controls and production adapters with generated
fixtures. They do not synthesize physical media keys or prove that Control
Center sends events; nor do they prove compositor appearance, physical keyboard
routing, VoiceOver navigation, audible output, or energy behavior. The
presentation scenario calls accessibility actions directly, which is narrower
than a VoiceOver session. Validate any claimed OS surface separately in a
logged-in desktop and report that interactive evidence apart from scenario
results.

For an optional UI-thread diagnostic with a full Xcode installation:

```bash
DEVELOPER_DIR=/Applications/Xcode.app ./ao run appkit --main-thread-checker
```

`DEVELOPER_DIR` may name either the Xcode application bundle or its
`Contents/Developer` directory. If it is unset, the portal uses the developer
directory selected by `xcode-select`. Command Line Tools alone do not include
Main Thread Checker. This launch option is independent of static analysis,
clang-tidy, and sanitizers.

## Validation

Choose completion scope from
[validation and review](test/validation-and-review.md). A macOS C++ change
normally runs `./ao check` and scoped `./ao hygiene`. Changes to Release
configuration, dependencies, or optimizer-sensitive code also run
`./ao check release`. Concurrency and memory work follows
[concurrency and sanitizer validation](test/concurrency-and-sanitizer.md).

The CI native matrix currently covers Intel and Apple Silicon. Native AppKit GUI
behavior still requires the smoke scenarios above; a passing headless gate does
not prove it.

Core Audio validation requires a logged-in console session even when the portal
is invoked over SSH. The portal neither creates a login session nor changes
power policy. At the login window, device enumeration may succeed while AUHAL
startup fails.

### Silent Core Audio test output

Native Core Audio cases can use a live software HAL output endpoint. This is
still a real Core Audio route, not the application's null backend. Endpoint
installation, driver choice and persistent environment settings are operator
concerns; `ao` does not configure them or change the system default output.

Set `testOutputUid` from native device enumeration and select that exact UID
for the test process:

```bash
AOBUS_TEST_COREAUDIO_DEVICE_UID="$testOutputUid" ./ao check release
AOBUS_TEST_COREAUDIO_DEVICE_UID="$testOutputUid" ./ao test --appkit --scenario presentation \
  --state-root /tmp/aobus-presentation-state --library /tmp/aobus-presentation-music
```

Only test support reads the variable. Shipping executables, production device
ordering, and system defaults are unchanged.

- Core Audio backend cases open the matching live output instead of the first
  enumerated device.
- AppKit component fixtures select it and wait until the playback snapshot
  reports that backend, device, and the shared profile.
- The `desktop` scenario seeds the request in its isolated `desktop.plist`,
  checks that the parent and its successor keep it, and opens each process's
  native output menu to verify the exact checked backend, device, and profile.
  The checkmark reflects the engine-confirmed selection, independently of the
  saved request; it is not a readback of the AudioUnit route or rendered PCM.

An empty, missing, or ambiguous UID fails instead of skipping or falling back
to the default output; display names, case differences, and surrounding
whitespace do not match. With the variable unset, the cases keep their existing
selection and skip on a host without an output device. Take the UID from native
device enumeration; numeric device IDs do not survive a Core Audio reload.

GUI or service launches that do not inherit the shell environment need the
setting passed explicitly. To exercise normal output with this test override
unset, remove it for one command and select the backend suite:

```bash
env -u AOBUS_TEST_COREAUDIO_DEVICE_UID ./ao test --backend "CoreAudioBackend*"
```

The backend suite owns the native Core Audio cases that open a live output;
the core suite's matching `CoreAudioBackend*` cases are device-free refusal,
prewarm, and software-cache contracts that never reach an output device.

A pass on a software endpoint covers only that native software path. It does
not prove physical routing, audibility, hardware removal, or latency.

For audio-backend changes, additionally run the opt-in audible probe on a host
where a short tone is acceptable. It ignores `AOBUS_TEST_COREAUDIO_DEVICE_UID`
and uses the host's normal output:

```bash
./ao test --backend "CoreAudioProvider - enumerates and renders through a real output"
```

The exact case name selects the provider probe only; the hidden Core Audio
channel-map readback probe has different prerequisites and stays separate.

Then exercise output selection, pause/resume, seek/flush, drain, and device
removal through `./ao run tui`. The silent automated probe does not prove audible
hardware behavior.

## Troubleshooting

- Install the exact Homebrew formula named by a missing-formula error.
- Repair only a named Homebrew-owned path; never recursively change ownership of
  `/usr/local` or `/opt/homebrew`.
- For an incomplete managed vcpkg root, move aside only the exact directory
  named by the portal and rerun the command.
- Never bypass a vcpkg archive hash mismatch. Follow the
  [verified archive recovery](dependency-upgrade.md#recovering-a-regenerated-macos-vcpkg-archive).
- If a build tree is selected on non-local storage, choose an absolute local path
  with `-p` or `BUILD_DIR`, or omit `-p` and unset `BUILD_DIR` before selecting
  an absolute local `AOBUS_BUILD_ROOT`. Also check `AOBUS_STATE_ROOT` for managed
  tool and package state, including managed Ruff/mypy caches. These settings do
  not relocate Git metadata or ordinary Python bytecode.
- "cannot locate the private Git directory", "cannot create the checkout ID",
  or "checkout ID remained empty" can prevent managed-tool initialization.
  Follow the scoped override guidance under
  [native checkout identity](#native-checkout-identity).
- "git branch --show-current failed" during default source discovery can occur
  before checkout-ID initialization. Use a verified
  [source-discovery binding](#shared-source-git-discovery) or the Git-independent
  scopes described under [native checkout identity](#native-checkout-identity);
  an ID override alone does not repair Git linkage.
- If no Core Audio outputs appear, confirm a live output in Audio MIDI Setup.
  Aobus does not invent a route when macOS publishes none.
- If AUHAL fails from a noninteractive invocation, verify the logged-in console
  prerequisite under [validation](#validation). SSH connectivity alone does not
  establish that session; machine login and power policy are outside the portal.
- Preserve the selected build tree, `build.log`, and local caches while
  diagnosing a cold or failed native build.

See [macOS portability compromises](macos-portability.md) for removable
workarounds and permanent Darwin differences.
