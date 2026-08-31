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
CLI, FTXUI terminal application, native tests, and shared-mode playback through
Core Audio. It does not provide a Cocoa or GTK desktop frontend. The TUI uses
the same interactive playback stack as the other native frontends and can play
through any live Core Audio output device published by macOS.

The Core Audio provider publishes concrete devices by their persistent Core
Audio UID and orders the current system default first. Selecting that device is
an explicit route: a later system-default change updates ordering but does not
silently move an active or persisted selection. The backend uses the playback-
only AUHAL output unit, disables input, and does not request microphone access.
It uses the shared profile; macOS may resample, remap channels, or convert the
lossless client PCM stream downstream of Aobus.

The build targets macOS 15.0 or newer. The project-maintained validation host is
macOS 15.7.9 on x86_64. GitHub Actions runs the native gate on both the
`macos-15-intel` x86_64 image and the `macos-15` arm64 image, using a dedicated
vcpkg triplet and compiler-cache namespace for each architecture.

## Policy

Run repository operations from the project root through `./ao`. The portal
selects the required Homebrew host tools, bootstraps the repository-pinned
vcpkg checkout, and configures the matching project triplet. Do not run ambient
CMake or invoke vcpkg manually for a normal build.
Nix is a Linux-only resolver: `shell.nix` deliberately rejects Darwin, and the
macOS portal must not grow a second Nix bootstrap path.

For a normal Git checkout, the portal also configures the repository-local
`core.hooksPath` as `script/git-hook` after entering the native environment.

`script/ao/macos-toolchain.json` owns the Clang major version, deployment
target, vcpkg tool revision, archive URL, and archive SHA-256.
`vcpkg-configuration.json` owns registry revisions, `vcpkg.json` owns ports and
features, and `cmake/vcpkg-triplets/` owns macOS linkage and deployment flags.
The native dependency contract is shared with Linux and Windows.

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
- `ccache` contains compiler-cache state when the optional tool is installed;
- `tools/libcxx-expected-shim/<llvm-version>` contains the generated libc++
  compatibility header.
- `tools/venvs/<checkout>/<fingerprint>` contains the managed Ruff/mypy
  environment when a Python-check command needs it.

`AOBUS_STATE_ROOT` replaces that base. An explicit `VCPKG_ROOT` may select a
pre-bootstrapped checkout for diagnosis, but the registry and manifest locks
still apply. `ccache` is optional; install it with Homebrew if desired.
When present, it uses `$AOBUS_STATE_ROOT/ccache`, a 10 GiB maximum, compression,
and the same time-macro policy as the Linux development environment.

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

## Validation

A normal macOS change completes with `./ao check` followed by `./ao hygiene`.
Changes to Release configuration, dependency resolution, or
optimizer-sensitive code also run `./ao check release`. Sanitizer-sensitive changes follow
[concurrency and sanitizer validation](test/concurrency-and-sanitizer.md) and
run the relevant `--asan` or `--tsan` gate.

The GitHub Actions matrix runs that normal completion gate natively on Intel
and Apple Silicon. These jobs validate the shared libraries, CLI, TUI, Core
Audio provider, tests, and native lint integration; they do not claim a Cocoa
desktop frontend.

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
