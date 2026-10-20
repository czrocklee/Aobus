---
id: development.macos
---
# macOS development

The native macOS profile builds the shared core, CLI, FTXUI terminal frontend,
AppKit desktop development slice, Core Audio backend, and native tests. GTK is
not built. The deployment target is owned by
`script/ao/macos-toolchain.json`; the portal and project triplets apply it.

This page owns host setup, portal commands, local state, native AppKit smoke,
and macOS validation. Current AppKit and Core Audio behavior belongs to the
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

### Unattended native host

Native GUI and Core Audio validation still requires a logged-in console user.
For a dedicated unattended validation host, enable automatic login through the
macOS user settings and disable system sleep with the host administrator's
power policy:

```bash
sudo pmset -a sleep 0
```

After a reboot or configuration change, verify the observable state rather than
assuming that SSH access created a console session:

```bash
stat -f "%Su" /dev/console
pmset -g custom
```

The console owner must be the validation user rather than `root`, and the active
power profile must report `sleep 0`. Automatic login and power-policy changes
are host provisioning, not actions performed by `./ao`.

## Build, run, and check

Use `./ao <command> --help` for current options. Common tasks are:

```bash
./ao build                    # Debug graph
./ao build release            # Release graph; C++ uses ThinLTO
./ao build profile            # RelWithDebInfo sampling build with frame pointers
./ao run cli
./ao run tui
./ao run appkit
./ao test                     # core and TUI fast loop
./ao test --lint              # custom clang-tidy integration fixtures
./ao test --all               # core, TUI, CLI, integration, and lint
./ao check                    # complete native Debug gate
./ao check release            # complete native Release gate
./ao check --asan             # supported suites with ASan/UBSan
./ao check --tsan             # TSan-safe core suite
./ao hygiene                  # scoped format/audit/tidy gate
./ao deps report
./ao deps verify
```

`./ao check` does not run fixture-dependent AppKit GUI scenarios. The macOS
`all` group intentionally excludes AppKit smoke and the Python `tooling` suite.
`./ao check --tsan` is narrower still: it builds and runs only the macOS
TSan-safe suite group (currently Core) plus architecture guardrails. It does not
build either AppKit GUI bundle and is not evidence for native GUI behavior. Use
an explicit `./ao test --appkit --tsan ...` invocation when the selected AppKit
scenario is required under TSan.

The managed macOS environment matches Ruff and mypy pins for format/tidy work,
but macOS does not claim exact Python patch parity and does not expose
`./ao test --tooling`.

Release IPO is checked for C++ only. C++ translation units and final links use
ThinLTO; Objective-C++ translation units use normal Release optimization.
See [optimized builds](optimized-builds.md).

## Build and tool state

Default build trees are `/tmp/build/<checkout-directory>/<flavor>` and contain
`build.log`. Default sanitizer trees use sibling suffixes; tidy and analyzer
use separate tool trees. `<checkout-directory>` is only the checkout folder's
final name, not a unique identity. Same-named checkouts sharing one base select
the same paths; use distinct names or absolute local build bases.

`AOBUS_BUILD_ROOT` replaces only the `/tmp/build` base; the portal still appends
the checkout name and flavor or tool suffix. `BUILD_DIR` selects one exact tree
instead, bypassing flavor, sanitizer, tidy, and analyzer path separation. It is
never a base and takes precedence over `AOBUS_BUILD_ROOT`. An explicit `-p` or
`--path` takes precedence over both.

By default, C++ tidy/hygiene without `-p` refreshes the selected tree with the
`macos-debug` lint preset, even when selected through `BUILD_DIR`; this can
reconfigure a Release tree as Debug. `tidy --no-build` skips that provisioning.
Prefer `AOBUS_BUILD_ROOT` to retain independent product and tool trees.
An explicit tidy/hygiene `-p` suppresses that forced preset refresh, but does
not make the selected tree read-only.

The default `/tmp` location is temporary storage, not a persistence guarantee.
For a persistent local base, for example, use
`AOBUS_BUILD_ROOT="$HOME/Library/Caches/Aobus/build" ./ao check`.
Repeat the assignment for related invocations, or use a directory-scoped
environment. That base holds build trees only; `AOBUS_STATE_ROOT` still selects
the managed tool and package state described below.

Managed state defaults to `$HOME/Library/Caches/Aobus`:

- `tools/vcpkg/<revision>` — bootstrapped pinned vcpkg;
- `cache/vcpkg/downloads` and `cache/vcpkg/binaries` — reusable package state;
- `tools/venvs/<checkout>/<fingerprint>` — managed Ruff/mypy environment;
- `tools/libcxx-expected-shim/<llvm-version>` — generated compatibility header;
- `ccache` — optional compiler cache after explicit setup.

`AOBUS_STATE_ROOT` replaces that base. `VCPKG_ROOT` is a diagnostic override for
a complete pre-bootstrapped checkout; registry and manifest locks still apply.
Use `./ao setup compiler-cache` for optional managed ccache behavior.

Keep build trees, vcpkg state, managed Python environments, and compiler-cache
stores on the macOS host's local disk. A source checkout may be local or shared,
including over SMB. Mounting and maintaining a source share is host
administration, not portal behavior. Keep source files stable while building
or collecting validation evidence; do not edit the same shared files
concurrently from multiple hosts.

This storage requirement does not imply a write-free source checkout.
Checkout IDs live in private Git metadata, and Ruff/Python may create
`.ruff_cache` and `__pycache__` beside sources. Darwin's mypy cache is kept in
the managed Python environment. `AOBUS_BUILD_ROOT` moves only build trees,
not those metadata or source-side caches.

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
Git-dependent.

## Native desktop development slice

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
```

Use `--asan`, `--tsan`, or `--path` when the applicable bundle already belongs
to that tree. `--no-build` is valid only after the selected tree contains the
bundle. For native lifecycle or authoring work, run the relevant scenarios in
Debug, Release, and TSan; add ASan for memory-sensitive changes. Include both
`app/macos-appkit` and `test/integration/macos` in scoped hygiene.

Treat each scenario only as evidence for the surface it drives:

| Scenario | Direct evidence |
|---|---|
| `desktop` | Shipping-window composition, native browsing and playback controls, close/quit coordination, restoration and geometry ownership, and supervised library-successor handoff. |
| `authoring` | Native property/List editors, validation and stale-draft behavior, save/close/discard ordering, and authoring teardown. |
| `presentation` | Browser projection and reuse, selection/sort/drag adaptation, activity and inspector presentation, accessibility dismissal actions, and inert callbacks after detach. |
| `quality-presentation` | Isolated classic quality-tooltip updates, clearing, resize and repeated-layout preservation, plus modern transport separation, using synthetic state and native bar captures without a runtime or audio endpoint. |
| `media` | MediaPlayer metadata/artwork publication, the seven supported remote commands and disabled unsupported commands, stale-seek guards, main-thread admission, runtime handoff, and retirement. |

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

A host without usable audio hardware, such as a VM, can run the native Core
Audio cases against a software HAL endpoint such as BlackHole 2ch. That is
still a real Core Audio route, not the application's null backend. Installing,
reloading, or removing the driver is host administration that `ao` never
performs, and the system default output does not need to change.

Select the endpoint by its exact UID for the test process:

```bash
AOBUS_TEST_COREAUDIO_DEVICE_UID=BlackHole2ch_UID ./ao check release
AOBUS_TEST_COREAUDIO_DEVICE_UID=BlackHole2ch_UID ./ao test --appkit --scenario presentation \
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

A dedicated development VM may keep the driver installed and set a default in
the development user's `~/.zshenv`:

```zsh
export AOBUS_TEST_COREAUDIO_DEVICE_UID="${AOBUS_TEST_COREAUDIO_DEVICE_UID-BlackHole2ch_UID}"
```

An explicit value, including an empty one, still wins. GUI or service launches
that do not inherit the shell environment need the setting passed explicitly.
To exercise the host's normal output from such a shell, remove it for one
command:

```bash
env -u AOBUS_TEST_COREAUDIO_DEVICE_UID ./ao test --core "CoreAudioBackend*"
```

A pass on a software endpoint covers only that native software path. It does
not prove physical routing, audibility, hardware removal, or latency.

For audio-backend changes, additionally run the opt-in audible probe on a host
where a short tone is acceptable. It ignores `AOBUS_TEST_COREAUDIO_DEVICE_UID`
and uses the host's normal output:

```bash
./ao test --integration "[coreaudio][.manual]"
```

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
- `AOBUS_LIBCXX_EXPECTED_SHIM` missing at configure time means CMake was run
  outside the portal. Remove that manual tree and use `./ao`.
- If a build tree is selected on a source share, choose an absolute local path
  with `-p` or `BUILD_DIR`, or omit `-p` and unset `BUILD_DIR` before selecting
  an absolute local `AOBUS_BUILD_ROOT`. Also check `AOBUS_STATE_ROOT` for managed
  tool and package state; these settings do not relocate Git metadata or
  source-side caches.
- "cannot locate the private Git directory", "cannot create the checkout ID",
  or "checkout ID remained empty" can prevent managed-tool initialization.
  Follow the scoped override guidance under
  [native checkout identity](#native-checkout-identity).
- "git branch --show-current failed" during default source discovery can occur
  before checkout-ID initialization. Use the Git-independent scopes described
  under [native checkout identity](#native-checkout-identity); an ID override
  alone does not repair Git linkage.
- If no Core Audio outputs appear, confirm a live output in Audio MIDI Setup.
  Aobus does not invent a route when macOS publishes none.
- If AUHAL or `afplay` fails over SSH, `stat -f "%Su" /dev/console` must name
  the logged-in user rather than `root`; on an unattended host also verify that
  `pmset -g custom` still reports `sleep 0`.
- macOS does not provide a `timeout` command by default. Bound remote native
  operations from the controlling host rather than adding an ambient utility
  or leaving a hidden blocking loop in the guest.
- Preserve the selected build tree, `build.log`, and local caches while
  diagnosing a cold or failed native build.

See [macOS portability compromises](macos-portability.md) for removable
workarounds and permanent Darwin differences.
