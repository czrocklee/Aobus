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
`build.log`. Sanitizers use sibling suffixes. `AOBUS_BUILD_ROOT` replaces the
`/tmp/build` base while retaining the checkout-directory component; `BUILD_DIR`
or `-p` selects one exact tree.

Managed state defaults to `$HOME/Library/Caches/Aobus`:

- `tools/vcpkg/<revision>` — bootstrapped pinned vcpkg;
- `cache/vcpkg/downloads` and `cache/vcpkg/binaries` — reusable package state;
- `tools/venvs/<checkout>/<fingerprint>` — managed Ruff/mypy environment;
- `tools/libcxx-expected-shim/<llvm-version>` — generated compatibility header;
- `ccache` — optional compiler cache after explicit setup.

`AOBUS_STATE_ROOT` replaces that base. `VCPKG_ROOT` is a diagnostic override for
a complete pre-bootstrapped checkout; registry and manifest locks still apply.
Use `./ao setup compiler-cache` for optional managed ccache behavior.

Generated state must be on the macOS host's local disk. A source checkout may be
mounted over SMB, but build trees, vcpkg state, Python environments, and compiler
caches must not be written to that mount.

For the project validation VM, mount the authoritative checkout read-write and
keep all generated state local, for example:

```bash
mkdir -p ~/mnt/aobus
mount_smbfs -N //guest@10.200.200.1/aobus ~/mnt/aobus
cd ~/mnt/aobus
./ao check
./ao hygiene
```

The share does not survive a guest reboot. Credentials are managed outside the
repository. Do not copy the checkout into the guest or run two writers against
the same source files.

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
| `media` | MediaPlayer metadata/artwork publication, the seven supported remote commands and disabled unsupported commands, stale-seek guards, main-thread admission, runtime handoff, and retirement. |

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

For audio-backend changes, additionally run the opt-in audible probe on a host
where a short tone is acceptable:

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
- If generated state appears under the SMB checkout, stop and select a local
  `AOBUS_BUILD_ROOT`.
- If no Core Audio outputs appear, confirm a live output in Audio MIDI Setup.
  Aobus does not invent a route when macOS publishes none.
- If AUHAL or `afplay` fails over SSH, `stat -f "%Su" /dev/console` must name
  the logged-in user rather than `root`; on an unattended host also verify that
  `pmset -g custom` still reports `sleep 0`.
- In a QEMU guest, a frozen framebuffer clock can mean display sleep rather than
  a stalled guest. Compare the guest clock with the network before changing
  timekeeping:

  ```bash
  date
  sntp -d time.apple.com
  ```

  Add no periodic clock-setting service unless `date` or `sntp` demonstrates
  real drift; wake the display through Screen Sharing for visual checks.
- macOS does not provide a `timeout` command by default. Bound remote native
  operations from the controlling host rather than adding an ambient utility
  or leaving a hidden blocking loop in the guest.
- Preserve the selected build tree, `build.log`, and local caches while
  diagnosing a cold or failed native build.

See [macOS portability compromises](macos-portability.md) for removable
workarounds and permanent Darwin differences.
