---
id: development.macos
type: development
status: current
domain: development
summary: Defines native macOS prerequisites, portal bootstrap, local state, supported commands, and current limitations.
---
# macOS development

## Scope

The native macOS profile supports development of the shared core libraries,
CLI, FTXUI terminal application, AppKit desktop, native tests, and shared-mode
playback through Core Audio. The desktop is an incremental development slice;
GTK is not built on macOS. The TUI uses
the same interactive playback stack as the other native frontends and can play
through any live Core Audio output device published by macOS.

The Core Audio provider publishes concrete devices by their persistent Core
Audio UID and orders the current system default first. Selecting that device is
an explicit route: a later system-default change updates ordering but does not
silently move an active or persisted selection. The backend uses the playback-
only AUHAL output unit, disables input, and does not request microphone access.
It uses the shared profile; macOS may resample, remap channels, or convert the
lossless client PCM stream downstream of Aobus.

The build targets macOS 15.0 or newer. The current local validation VM runs
macOS 26.6.2 on x86_64. GitHub Actions runs the native gate on both the
`macos-15-intel` x86_64 image and the `macos-15` arm64 image, using a dedicated
vcpkg triplet and compiler-cache namespace for each architecture.

## Policy

Run repository operations from the project root through `./ao`. The portal
selects the required Homebrew host tools, bootstraps the repository-pinned
vcpkg checkout, and configures the matching project triplet. Do not run ambient
CMake or invoke vcpkg manually for a normal build.
Nix is a Linux-only resolver: `shell.nix` deliberately rejects Darwin, and the
macOS portal must not grow a second Nix bootstrap path.

Git hooks are an explicit setup action; see the
[contributor workflow](../../CONTRIBUTING.md#development-workflow).
Help and Python-only checks do not require the C++/vcpkg environment.
For source checks, the preflight passes its selected file list to the command
in a temporary invocation file, preserving the original scope and avoiding
a second Git scan. The portal removes the file on success or failure.

`script/ao/macos-toolchain.json` owns the Clang major version, deployment
target, vcpkg tool revision, archive URL, and archive SHA-256.
`vcpkg-configuration.json` owns registry revisions, `vcpkg.json` owns ports and
features, and `cmake/vcpkg-triplets/` owns macOS linkage and deployment flags.
The native dependency contract is shared with Linux and Windows.

The Release IPO probe covers C++ only.
With the current CMake toolchain, C++ translation units and the final executable link use ThinLTO; Objective-C++ translation units use normal Release optimization without IPO.
A successful Release build validates the native bundle, not LTO coverage of every `.mm` file.

Keep generated state on the guest or workstation's local disk. A source tree
may be mounted over SMB, but build trees, compiler caches, and toolchains must
not be written to that mount.

The Homebrew Python starts the portal. Format, tidy, and hygiene commands then
use a checkout-isolated environment whose x86_64 and arm64 wheel hashes are
locked in `script/ao/macos-requirements.txt`. Ruff and mypy match
`script/ao/toolchain.json`; the Homebrew Python must match its major/minor but
not its exact patch release. macOS therefore does not own
`./ao test --tooling`. Native clang-tidy and its integration fixtures are
supported; `./ao test --lint` is part of both `./ao test --all` and
`./ao check`.

## Workflow

Install the Xcode Command Line Tools and Homebrew, then install the host tools:

```bash
xcode-select --install
brew install llvm@22 cmake ninja pkgconf python@3.14 \
  autoconf autoconf-archive automake libtool
```

Nix is not required on macOS. The first build downloads a SHA-256-verified
vcpkg source archive, bootstraps its tool, resolves the locked registries, and
builds the manifest dependencies. This cold build can take several minutes.
Later builds restore packages from the host-local vcpkg binary cache.

The supported commands are:

```bash
./ao build                    # Debug build of the enabled native graph
./ao build release            # Release build
./ao run cli                  # Build and run the CLI
./ao run tui                  # Build and run the terminal frontend
./ao run appkit               # Build and run the native desktop
./ao test                     # Core and TUI fast loop
./ao test --lint              # Native Aobus clang-tidy fixture suite
./ao test --all               # Core, TUI, CLI, integration, and lint suites
./ao check                    # Native build/test gate
./ao hygiene                  # Changed-file formatting, audits, and clang-tidy
./ao check release            # Release build and supported suites
./ao check --asan             # Supported suites with ASan/UBSan
./ao check --tsan             # Core suite with TSan
./ao deps report              # Governed versions and vcpkg identities
./ao deps report --concepts   # Public-concept baseline into concept-report.json
./ao deps verify              # Reject stale or mismatched dependency evidence
```

The default build roots are `/tmp/build/<source-directory>/debug` and
`/tmp/build/<source-directory>/release`; sanitizer suffixes create separate
sibling trees. `AOBUS_BUILD_ROOT` replaces `/tmp/build` while retaining the
source-directory component. Each build writes `build.log` inside its tree.

Managed tool and package state defaults to `$HOME/Library/Caches/Aobus`:

- `tools/vcpkg/<revision>` contains the bootstrapped pinned checkout;
- `cache/vcpkg/downloads` contains verified source downloads;
- `cache/vcpkg/binaries` contains reusable built packages;
- `ccache` contains the shared compiler-cache state after explicit setup;
- `tools/libcxx-expected-shim/<llvm-version>` contains the generated libc++
  compatibility header.
- `tools/venvs/<checkout>/<fingerprint>` contains the managed Ruff/mypy
  environment when a Python-check command needs it.

`AOBUS_STATE_ROOT` replaces that base.
An explicit `VCPKG_ROOT` may select a pre-bootstrapped checkout for diagnosis, but the registry and manifest locks still apply.
Run `./ao setup compiler-cache` to install the governed Homebrew ccache formula, verify its minimum compatible version, and enable it for portal builds.
The shared store defaults to 20 GB.
See [Compiler cache](compiler-cache.md) for capacity and override precedence.

The project validation VM sees the authoritative Linux checkout through SMB:

```bash
mkdir -p ~/mnt/aobus
mount_smbfs -N //guest@10.200.200.1/aobus ~/mnt/aobus
cd ~/mnt/aobus
./ao check
./ao hygiene
```

The share is read-write and does not survive a guest reboot. Credentials are
managed outside the repository. Do not copy the checkout into the guest or run
two writers against the same source files.

## Native desktop development slice

`./ao run appkit` builds and launches the AppKit bundle. The application keeps one
library-bound runtime and native main window, with sidebar navigation, grouped
track tables, column sorting, filtering, playback, seeking, volume and output
selection, artwork, and metadata/List editing. Tracks can be dragged into saved
Lists; a dropped music folder and Open Recent use the same library-switch path
as the folder picker. Classic and Modern are built-in compositions over
the same retained session. Closing the window keeps the session alive; reopening
uses that session. Quit drains runtime callbacks before teardown, and switching
libraries releases the old graph before starting a successor process.

The session publishes title, artist, album, duration, elapsed time, playback
state, and available artwork through MediaPlayer. System commands support Play,
Pause, Toggle Play/Pause, Previous, Next, Stop, and absolute position changes.
Closing or hiding the window keeps this integration active. Accepted Quit and
library switching retire command admission and clear Now Playing before
releasing the old session. Repeated Play/Pause commands are idempotent; a queued
position change is rejected when its playback occurrence has been replaced,
including replay of the same track.
Valid position deliveries join the runtime FIFO rather than being dropped by an orthogonal backlog; adapter retirement fences native callbacks, while already submitted commands remain runtime-owned until execution or runtime shutdown.
Ordinary Next commands instead navigate from the execution-time subject, as on Windows SMTC and Linux MPRIS; commands outside the supported MediaPlayer surface are disabled.
The adapter does not create a separate playback engine or poll native windows.

The deployment baseline remains macOS 15. Newer native split-view materials use
availability checks and fall back on older macOS versions. Native shell and
editor copy use the startup-selected MessageCatalog; missing AppKit translations
fall back to English root. Custom layouts, complete preferences, signing and
distribution remain outside this development slice.

The Window menu retains a Show Aobus Window command (Command-0) after closing the
window. Properties uses Command-I, Inspector uses Option-Command-I, and Sidebar
uses Control-Command-S. Modern toolbar customization, recent library paths, and
window geometry belong to the selected application state root. Appearance is an
application preference shared by the main window, sheets, and popovers.

Static UI changes are coalesced onto the main run loop. Visible playback and
Soul animation use an on-demand frame timer; hidden, fully occluded, or idle
windows do not keep that timer running. Playback position is interpolated from the shared model
anchor, recorded at publication even while the window is hidden. Separate
service scheduling advances Quit and library switching even when rendering is
suspended. A close request waits through saving and field-menu tracking and
joins any existing nested discard sheet; only the actual Keep response cancels
it.

Pass an absent or empty disposable music directory and an absent or empty
isolated state directory for each GUI scenario. The portal and native test
bundle reject ordinary files, nonempty directories, and either path containing
the other, so they never overwrite user media or state. The bundle fills the
music directory with 20 deterministic, silent, mono 16-bit 44.1 kHz INFO WAV
tracks at least 12 seconds long; the desktop successor receives an independent
fixture generated by the same support code. No external media encoder is
required. Before creating `NSApplication`, both the shipping desktop and smoke bundle call the same restoration opt-out, merging
`ApplePersistenceIgnoreState=YES` into the volatile `NSArgumentDomain` without writing global or persistent defaults.
This follows AppKit's documented
[restoration opt-out](https://developer.apple.com/library/archive/releasenotes/AppKit/RN-AppKitOlderNotes/index.html)
and the [volatile argument-domain precedence](https://developer.apple.com/library/archive/documentation/Cocoa/Conceptual/UserDefaults/AboutPreferenceDomains/AboutPreferenceDomains.html).
The main window is also non-restorable: `desktop.plist` alone owns its persisted geometry, and AppKit restoration must not compete with library switching or a fresh state root after a crash.
These scenarios run in `ao_appkit_smoke`, a test bundle linked
against the same production objects as the shipping desktop. The shipping
`aobus-appkit` executable does not accept test flags or contain scenario code.

```bash
./ao test --appkit --scenario desktop --state-root /tmp/aobus-desktop-state --library /tmp/aobus-desktop-music
./ao test --appkit --scenario authoring --state-root /tmp/aobus-authoring-state --library /tmp/aobus-authoring-music
./ao test --appkit --scenario presentation --state-root /tmp/aobus-presentation-state --library /tmp/aobus-presentation-music
./ao test --appkit --scenario media --state-root /tmp/aobus-media-state --library /tmp/aobus-media-music
```

The media scenario exercises the production adapter and native Now Playing
center with a hidden window. It checks each native command's enabled state
against its logical command identity, including distinct final-track navigation
and transport availability, and verifies that unsupported commands remain disabled.
It covers foreign-thread admission and main-thread delivery, idempotent transport, two queued Next commands advancing across three tracks, invalid positions, stale seeks across Stop and same-track replay, consecutive valid seeks, and queued and retained callbacks after retirement.
An occupied run-loop executor holds a mute backlog while main-queue position callbacks submit their captured targets; the scenario asserts FIFO final effects both with the adapter live and retired after handoff.
A controlled clock checks metadata-only anchor retention, and native observation checks that a synchronous artwork cache hit publishes Now Playing exactly once.
It invokes the same owned handler installed at the framework
boundary; it does not synthesize physical media keys or prove Control Center
interaction. Validate those OS surfaces separately in a logged-in desktop.

For an optional runtime UI-thread diagnostic, use a full Xcode installation:

```bash
./ao run appkit --main-thread-checker
```

The portal locates `libMainThreadChecker.dylib` through `DEVELOPER_DIR` (an Xcode
bundle or Developer directory) or `xcode-select --print-path`, and augments the
child process environment without replacing existing injected libraries. It
rejects missing Xcode diagnostics before building or launching. Command Line
Tools alone do not provide Main Thread Checker; this diagnostic is independent
of normal builds, clang-tidy, the static analyzer, and sanitizers.

Do not pre-populate the fixture directories; successful preparation is part of
the native scenario contract and failures are reported before AppKit creates a
session. The desktop scenario drives the real application through native windows,
controls and menus. It covers browsing, progressing playback time, modes, filtering, native
editor-close arbitration, blocked library-switch commands while editing,
retained close/reopen, and an accepted library switch. Its
successor reenters the test executable using the shared successor protocol.
The test launch configuration points the unmodified production launcher at a
temporary bridge. That bridge forwards the real arguments to the portal's
process supervisor, which owns both GUI processes and collects their exit
statuses. Both must exit successfully and write completion markers matching
the current invocation. Timeouts and interrupted or failed runs terminate and
collect outstanding GUI children. This proves the production coordinator,
launcher invocation and successor protocol, but not the separate
shipping executable's argument parser. That entry point remains a thin adapter
over the independently tested shared protocol.

The authoring scenario owns a real library session and editor through their
normal APIs. It covers native field editing, numeric validation, mixed values,
captured bulk edits, stale drafts, List expression recovery, membership,
subtree deletion, and editor completion. It mutates the disposable library.
The presentation scenario tests browser callback detachment and selection,
playback controls, inspector sheets, and activity updates through component
APIs. Native mouse tracking covers a seek spanning track replacement and replay
of the same track, plus a valid gesture. Artwork rescans cover replacement,
addition and removal while selection stays fixed, including filtered-out tracks.
Activity expiration uses a controlled model clock and native one-shot timer delivery
to cover batched A-B-A updates and early wakeup rearming without a playback session.
Neither scenario accesses private coordinator state.

Native view captures omit system window frames and do not replace compositor,
keyboard, accessibility, or energy measurements. Hidden-window scheduling
needs separate energy or callback observation; a visual smoke alone does not
prove the absence of timers.

Run the applicable GUI scenarios in Debug, Release and TSan when changing native
lifecycle or authoring behavior; add ASan for memory-sensitive changes. Use
`--tsan` or `--asan` for sanitizer trees, or `--path` to select an already-built
Release tree. Default and ASan `check` builds include both bundles, but `check --tsan` builds only the core suite's targets and guardrails.
Run `./ao test --appkit --tsan ...` to build the native GUI test bundle separately; use `--no-build` only after that bundle has been built in the selected tree.
None of these `check` invocations executes the fixture-dependent GUI scenarios.
Scoped `hygiene` must include changed
sources under both `app/macos-appkit` and `test/integration/macos`.
`./ao analyze --folder app/macos-appkit --fail-on-diagnostics` adds Cocoa path
analysis with an exact native compile database.

## Validation

Select the completion route in [validation and review](test/validation-and-review.md).
A macOS C++ change completes with `./ao check` followed by scoped `./ao hygiene`.
Changes to Release configuration, dependency resolution, or
optimizer-sensitive code also run `./ao check release`. Sanitizer-sensitive changes follow
[concurrency and sanitizer validation](test/concurrency-and-sanitizer.md) and
run the relevant `--asan` or `--tsan` gate.

The GitHub Actions matrix runs that native gate on Intel and Apple Silicon
for changes requiring product validation. Documentation-only changes use the
documentation route described by the completion authority. Native jobs build the AppKit desktop and validate the shared libraries, CLI,
TUI, Core Audio provider, tests, and native lint integration. GUI behavior
requires the separate desktop smoke described above.

The macOS `all` group is exactly core, TUI, CLI, integration, and lint. Its core
suite opens the native AUHAL path and exercises a silent render/drain cycle on
a live output when the host exposes one. A passing gate does not claim a GUI or
the exact Python tooling contract. Linux and Windows remain required for those
platform-specific contracts.

Native audio validation requires a logged-in console session, including when
`./ao` itself runs over SSH. GitHub's hosted macOS images establish that session,
and the workflow rejects a runner whose `/dev/console` owner is `root` before it
starts the native gate. An unattended local validation host should use automatic
login and disable system sleep; display sleep may remain enabled because it does
not stop SSH, compilation, or headless tests.

The portal runs test executables directly. It does not create a login session,
change host power policy, or keep the display awake. At the login window, Core
Audio may enumerate the device while refusing to start AUHAL, so establish the
console session before running the gate.

Audio-backend changes also run the opt-in audible probe on a host where a short,
quiet tone is acceptable:

```bash
./ao test --integration "[coreaudio][.manual]"
```

Then use `./ao run tui` with a local track to verify output selection,
pause/resume, seek/flush, end-of-track drain, and device removal. The silent
automated probe does not by itself claim audible hardware behavior.

## Troubleshooting

- If the portal reports a missing Homebrew formula, install the exact formula
  named in the error and run the command again.
- If Homebrew reports a permission error, repair only the named Homebrew-owned
  path. Do not recursively change ownership of `/usr/local` or `/opt/homebrew`.
- If the managed vcpkg root is incomplete, move only the exact directory named
  by the portal aside and run `./ao` again. The immutable revision remains in
  the directory name.
- If the vcpkg source archive fails SHA-256 verification, do not bypass the
  check or replace the digest from a fresh download alone. Follow the
  [verified archive-recovery procedure](dependency-upgrade.md#recovering-a-regenerated-macos-vcpkg-archive).
- If CMake reports `AOBUS_LIBCXX_EXPECTED_SHIM` as missing, the configure step
  was run outside the portal. Remove that manual build tree and use `./ao`.
- If a build tree appears under the SMB checkout, stop and choose a local
  `AOBUS_BUILD_ROOT`; network and case-insensitive filesystems are unsupported
  for generated state.
- If the TUI lists no outputs, confirm that macOS shows a live output device in
  Audio MIDI Setup. Aobus does not synthesize a default route when Core Audio
  publishes no concrete device.
- If AUHAL or `afplay` cannot start over SSH, run `stat -f "%Su" /dev/console`.
  A result of `root` means no console user is logged in; log in through the desktop
  or repair automatic login before retrying native audio validation. For an
  unattended local VM, `pmset -g custom` should report `sleep 0`; set that policy
  once with `sudo pmset -a sleep 0` rather than coupling power management to every
  test process.
- Display sleep can leave the QEMU framebuffer showing an old clock even while
  the system time, SSH, and tests remain live. Check `date` and
  `sntp -d time.apple.com` before diagnosing clock drift. Wake the display for a
  visual check, or disable `displaysleep` only if the extra WindowServer load is
  acceptable.
- If a selected output disappears, choose one of the newly published concrete
  devices. Aobus reports device loss and does not redirect the stream to a new
  system default behind the user's selection.
- macOS has no `timeout` command by default. Put command timeouts on the
  controlling host when operating the validation VM.
- A cold vcpkg build is expected to be slow. Preserve `/tmp/build/...` and the
  local vcpkg caches while diagnosing failures.

## Related documents

- [macOS portability compromises](macos-portability.md) owns removable
  toolchain workarounds and permanent Darwin source differences.
- [Dependency governance](dependency-governance.md) owns cross-platform version
  policy and native resolver pins.
- [Dependency upgrade](dependency-upgrade.md) owns pin changes and validation.
- [Test suites](test/test-suite.md) owns platform suite membership.
- [Validation and review](test/validation-and-review.md) owns completion gates.
