---
id: development.macos
---

# macOS development

The native macOS profile builds the shared core, CLI, FTXUI terminal frontend,
AppKit desktop development slice, Core Audio backend, and native tests. GTK is
not built. The deployment target is owned by
`script/ao/macos-toolchain.json`; native compiler setup rejects project triplets
whose deployment values disagree with that authority.

This page covers platform prerequisites, portal commands, local state,
AppKit launches and smoke scenarios, and macOS validation. Machine access,
source mounts, login/power configuration, and persistent shell settings belong
in machine-local guidance.
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

Run from the project root. Use pinned native Bazel for ordinary build/test/run;
use `./ao` for project-specific commands. Explicit setup is:

```bash
./ao setup bazel
./ao setup deps --path /tmp/build/aobus-native-sdk
```

Use the pinned executable printed by setup (or a Bazelisk pinned to
`.bazelversion`); it is not automatically on `PATH`. Select the provisioned
target-triplet prefix with `AOBUS_BAZEL_VCPKG_PREFIX`, including both Debug and
Release artifacts and host tools. Follow [native Bazel setup](bazel.md#direct-bazel-without-the-portal)
for the complete ready-SDK contract. Provisioning through the portal:

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
bazel build //app/cli:aobus //app/tui:aobus-tui //app/macos-appkit:aobus-appkit
bazel build --config=release //app/macos-appkit:aobus-appkit  # C++ ThinLTO
bazel build --config=profile //app/tui:aobus-tui  # sampling with frame pointers
bazel run //app/cli:aobus -- --help
bazel run //app/tui:aobus-tui
bazel test //:tests           # ordinary default selection
bazel test //test/lint:lint_test --nocache_test_results  # fresh result assertions
./ao check                    # complete native fastbuild gate
./ao check --config release   # complete native Release gate
./ao check --asan             # supported suites with ASan/UBSan
./ao check --tsan             # TSan-safe core, application, and backend suites
./ao check lint               # scoped semantic lint
./ao check format             # scoped read-only formatting
./ao setup prettier           # pinned formatter/Node bundle only
./ao check deps
```

For the desktop app, use the [isolated AppKit launch](#bazel-appkit-product).

`./ao check` runs the same wildcard `bazel test //...` graph as every host, so
it includes live documentation, formatting, source guardrails, the native-policy
audit and the whole-graph lint test, but it does not run fixture-dependent
AppKit GUI scenarios. The macOS tooling selection covers AppKit SDK imports
(`test_bazel_appkit_deps`), native environment selection (`test_bazel_environment`),
and the remote-cache helper (`test_bazel_remote_cache`). Workflow lint and the
representative compiler driver diagnostic are Linux-only. Ordinary `//:tests` excludes GUI smoke, backend service checks,
and lint fixtures; the wildcard graph is broader.
`./ao check --tsan` runs the full normal fastbuild completion first — which builds
the completion products, including the AppKit product bundle (built, never
run) — then a separate
managed TSan profile over the graph-owned macOS-compatible scope (core,
application, and backend) in the same selected tree; the sanitizer
profile itself does not build GUI smoke. This is not evidence for native GUI
behavior. Use
an explicitly configured native smoke target with `--config=tsan` when an
AppKit scenario needs TSan; that raw run and its runtime are caller-owned.

The portal starts with Homebrew Python and selects the managed environment
on demand for completion, documentation semantics, and Python formatting/lint scopes.
Other commands can use the base
interpreter; `/usr/bin/python3` only reads initial bootstrap JSON fields.
The managed environment matches Ruff and mypy pins, but macOS does not claim
exact Python patch parity. Full `check`
requires the managed Python tools, including documentation parser dependencies.
Markdown/JSON/YAML/CSS-only formatting instead uses the governed Prettier/Node
bundle without a C++ SDK, managed Python tools, or a system Node installation;
see [formatter setup](linting.md#authored-text-formatting).

Release IPO is checked for C++ only. C++ translation units and final links use
ThinLTO; Objective-C++ translation units use normal Release optimization.
See [optimized builds](optimized-builds.md).

The native triplets link third-party libraries statically. Before publishing
binaries or an AppKit bundle, review the shared
[binary-distribution obligations](dependency-governance.md#binary-distribution),
including mpg123's library modification/relinking requirements. Development
build and smoke-test success is not release-compliance evidence.

## Build and tool state

Managed validation trees are `/tmp/build/<checkout-directory>/bazel-<config>`
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
check uses one selected tree for its normal fastbuild completion and the managed
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

The Bazel environment repository stages its own shim under `_state`; the
[toolchain](bazel.md#macos-toolchain-composition) exposes the header through
a declared include directory, not that staging path. Ordinary portal tooling
uses the managed-state copy.
Both use the same fail-closed transform over the selected LLVM header.

`AOBUS_STATE_ROOT` replaces that base, but does not relocate the [Bazel action
cache](bazel.md#shared-local-action-cache) shared with bare builds.
`VCPKG_ROOT` is a diagnostic override for
a complete pre-bootstrapped checkout; registry and manifest locks still apply.
`./ao setup deps` installs the selected tree's vcpkg manifest.

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
`./ao check lint` cannot initialize the environment.
Set one stable, unique `AOBUS_CHECKOUT_ID` for that checkout, per invocation or
in a directory-scoped environment.

An unconditional shell-profile export also overrides native clones' stored
IDs and can select different managed Python environments for them.
The override does not repair Git linkage. Default Git-backed scope discovery
can fail before managed-tool setup. See [shared-source Git discovery](build.md#shared-source-git-discovery)
for scope options and binding requirements.

### Bind Git metadata on macOS

After verifying the metadata and source root as described in [shared-source Git discovery](build.md#shared-source-git-discovery), pass them to the individual command:

```bash
AOBUS_GIT_DIR="$HOME/local-metadata/worktrees/aobus" \
AOBUS_GIT_WORK_TREE="/Volumes/source/aobus" ./ao check format
```

## Native desktop development slice

### Bazel AppKit product

With the [ready native SDK](bazel.md#direct-bazel-without-the-portal) and an
active macOS desktop session, build and launch the product from the repository
root:

```bash
appkit_root=$(mktemp -d /tmp/aobus-appkit-product.XXXXXX) &&
mkdir "$appkit_root/library" &&
bazel run //app/macos-appkit:aobus-appkit -- \
  --state-root "$appkit_root/state" --library "$appkit_root/library"
```

The product requires `--library` to name an existing directory; it opens and
scans that directory. The separate `--state-root` keeps this launch away from
your saved state. Without these arguments the app can use your personal data;
do not switch to a personal library during a disposable run. See the [AppKit product graph](bazel.md#appkit-product) for
build and bundle details.

A successful development launch does not establish GTK/WinUI feature parity,
native GUI acceptance, ARM64 validation, or release readiness. This is not an
installation, signing, or notarization workflow. The
[AppKit smoke scenarios](#appkit-smoke-scenarios) use a separate target.

For an optional UI-thread diagnostic, set `checker` to the absolute path of
Main Thread Checker's dylib from an installed full Xcode; Command Line Tools
alone do not include it. Inject it into the bundle's native executable, not the
Bazel server or the launcher script that `bazel run` starts:

```bash
bazel build //app/macos-appkit:aobus-appkit &&
bazel_bin=$(bazel info bazel-bin) &&
appkit_root=$(mktemp -d /tmp/aobus-appkit-product.XXXXXX) &&
mkdir "$appkit_root/library" &&
DYLD_INSERT_LIBRARIES="$checker" \
  "$bazel_bin/app/macos-appkit/Aobus.app/Contents/MacOS/aobus-appkit" \
  --state-root "$appkit_root/state" --library "$appkit_root/library"
```

Give `bazel build` and `bazel info` the same options, such as `--config=release`,
so that `bazel-bin` names the tree holding the bundle you built. For a
portal-prepared tree, pass its generated rc to both commands
(`bazel --bazelrc="$tree/configuration/aobus.bazelrc" …`). The diagnostic is
separate from static analysis, clang-tidy, and sanitizers.

### AppKit smoke scenarios

Run the smoke bundle with an active macOS desktop session and the
[ready native SDK](bazel.md#direct-bazel-without-the-portal):

```bash
smoke_root=$(mktemp -d /tmp/aobus-appkit.XXXXXX) &&
bazel run //app/macos-appkit:ao_appkit_smoke -- --scenario desktop \
  --state-root "$smoke_root/state" --library "$smoke_root/music"
```

Use one scenario name from the table and fresh disposable paths on each run.
Both directories must be absent or empty and must not equal or contain one
another after path resolution. The smoke entry point checks the paths and
generates deterministic media before creating `NSApplication`. It links the
same AppKit implementation as the shipping bundle, which does not accept
`--scenario`. Both entry points follow the
[AppKit restoration and geometry contract](../system/session-lifecycle.md#appkit).

`desktop` and `media` need a live Core Audio output; a
[software HAL output](#silent-core-audio-test-output) is sufficient.
`presentation` uses a silent test provider for playback, including its check
that Play resumes the paused track after selecting another track in a different
List. Its separate preference sessions honor the optional Core Audio UID. The
authoring and Credits fixtures also honor that UID but do not drive playback.
The isolated `quality-presentation` scenario needs no session, runtime, or
audio endpoint.

| Scenario               | Direct evidence                                                                                                                                                                                                                                                                       |
| ---------------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `desktop`              | Shipping window, browsing and transport, close/quit, disabled AppKit restoration, `desktop.plist` defaults for malformed values, unknown-key preservation, per-mode inspector visibility, fallback to Follow System for an unknown appearance choice, and supervised library handoff. |
| `membership`           | The track-table menu offers Add for other writable Playlists and Remove for the current one. Its native actions use the tracks and Playlist captured when the menu was built, without starting playback.                                                                              |
| `authoring`            | Native Properties and List editing, validation and stale drafts, save/discard/close ordering, and teardown.                                                                                                                                                                           |
| `presentation`         | Browser projection and cell reuse, selection/sort/drag adaptation, activity, inspector and playback presentation, direct accessibility dismissal, and inert callbacks after detach.                                                                                                   |
| `quality-presentation` | Classic quality-tooltip binding and clearing across resize and repeated layout, while preserving Modern transport and its separate output action.                                                                                                                                     |
| `media`                | MediaPlayer metadata/artwork, admission of the seven supported remote commands, disabled unsupported commands, stale-seek guards, main-thread delivery, runtime handoff, and retirement.                                                                                              |
| `credits-controls`     | Native scoped Credits editing and retirement of detached controls.                                                                                                                                                                                                                    |
| `credits-previews`     | Category-scoped preview edits stage without writing until parent Save; retired preview callbacks no longer reopen the editor.                                                                                                                                                         |

Collect the run's exit status and retain the disposable state for diagnostics.
The [bundle supervisor](../../script/ao/graph/appkit_process.py) bounds runs and
owns the `desktop` successor launch bridge; desktop success requires both
processes to exit successfully and leave markers for the same invocation.

Use `quality-presentation` for isolated English and Simplified Chinese checks at
normal and constrained sizes. It drives real playback bars with synthetic state;
its captures show the bars, not opened tooltip pixels or a shipping playback
trigger. The session-backed `presentation` scenario is not a substitute.

The scenarios drive native controls and production adapters with generated
fixtures. The media probe does not send physical media keys or prove Control
Center event delivery. Direct accessibility actions in `presentation` do not
prove VoiceOver navigation. Verify compositor appearance, physical keyboard
routing, audible output, and energy behavior separately in a logged-in desktop
session, and report that interactive evidence apart from smoke results.

For native lifecycle or authoring changes, run applicable scenarios with
`--config=debug`, `--config=release`, and `--config=tsan`; add `--config=asan`
for memory-sensitive changes. Put the config before `--`. These raw sanitizer
runs have caller-owned runtime options and do not count as managed sanitizer-gate
evidence. Include `app/macos-appkit` and `test/macos-appkit` in scoped `check lint`
and `check format`.

## Validation

Choose completion scope from
[validation and review](test/validation-and-review.md). A macOS C++ change
normally runs `./ao check`, which includes the whole-graph lint test and
documentation. Changes to Release
configuration, dependencies, or optimizer-sensitive code also run
`./ao check --config release`. Concurrency and memory work follows
[concurrency and sanitizer validation](test/concurrency-and-sanitizer.md).

The CI native matrix covers Intel and Apple Silicon. Native AppKit GUI
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

Verify the intended non-default software output in native device enumeration
and set `testOutputUid` to its exact UID. The macOS `//test:backend_test` target
inherits `AOBUS_TEST_COREAUDIO_DEVICE_UID` in both runs below. For a fresh full
Release gate:

```bash
AOBUS_TEST_COREAUDIO_DEVICE_UID="$testOutputUid" ./ao check --config release --fresh
```

For a focused Release run of the ordinary Core Audio backend cases:

```bash
AOBUS_TEST_COREAUDIO_DEVICE_UID="$testOutputUid" \
  bazel test --config=release --nocache_test_results //test:backend_test \
  --test_filter='"CoreAudioBackend*"~[.]'
```

Check the backend test's Catch2 report (`test.xml`) for an AUHAL-open case such as
`CoreAudioBackend - native AUHAL open confirms the exact lossless client stream`.
It must pass, not skip. Neither run includes the hidden hardware probes, and
the focused run does not replace the full gate.

To check AppKit recovery from a missing saved output, pass the approved silent
output's UID to a disposable desktop smoke run:

```bash
smoke_root=$(mktemp -d /tmp/aobus-appkit.XXXXXX) &&
AOBUS_TEST_COREAUDIO_DEVICE_UID="$testOutputUid" \
  bazel run //app/macos-appkit:ao_appkit_smoke -- --scenario desktop \
  --state-root "$smoke_root/state" --library "$smoke_root/music"
```

Only test support reads the variable. It does not affect shipping executables,
production device ordering, or system defaults.

- Core Audio backend cases open the matching live output instead of the first
  enumerated device.
- AppKit component fixtures using platform audio select the UID and wait for
  the playback snapshot to confirm that backend, device, and shared profile.
- With the UID set, `desktop` starts with a missing device in its private
  `desktop.plist`. The saved request must survive startup; the native menu
  shows a disabled advisory without checking an alternate route. Before
  playback, the probe chooses the verified test output. Both parent and
  successor must preserve that choice and check the engine-confirmed backend,
  device, and profile in the menu. This is not a physical unplug, a readback of
  the AudioUnit route, or evidence of rendered PCM.

If the variable is set, an empty, unknown, or ambiguous UID fails instead of
skipping or falling back to the default output. Display names, case differences,
and surrounding whitespace do not match. With the variable unset, there is no
selection override. The native AUHAL open and run-state cases skip if there is
no live output; the native fatal reentry probe and the `desktop` and `media`
smoke scenarios require one. Use the UID, not a numeric device ID: numeric IDs
are not stable across a Core Audio reload.

GUI or service launches that do not inherit the shell environment need the
setting passed explicitly. To exercise normal output with this test override
unset, remove it for one command and select the backend suite:

```bash
env -u AOBUS_TEST_COREAUDIO_DEVICE_UID bazel test //test:backend_test \
  --test_filter='CoreAudioBackend*'
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
bazel test //test:backend_test \
  --test_filter='CoreAudioProvider - enumerates and renders through a real output'
```

The exact case name selects the provider probe only; the hidden Core Audio
channel-map readback probe has different prerequisites and stays separate.

Then exercise output selection, pause/resume, seek/flush, drain, and device
removal through `bazel run //app/tui:aobus-tui`. The silent automated probe does not prove audible
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
  before checkout-ID initialization. [Bind verified Git metadata or name the files individually](build.md#shared-source-git-discovery);
  a [checkout ID](#native-checkout-identity) alone does not repair Git linkage.
- If no Core Audio outputs appear, confirm a live output in Audio MIDI Setup.
  Aobus does not invent a route when macOS publishes none.
- If AUHAL fails from a noninteractive invocation, verify the logged-in console
  prerequisite under [validation](#validation). SSH connectivity alone does not
  establish that session; machine login and power policy are outside the portal.
- Preserve the selected build tree, `build.log`, and local caches while
  diagnosing a cold or failed native build.

See [macOS portability compromises](macos-portability.md) for removable
workarounds and permanent Darwin differences.
