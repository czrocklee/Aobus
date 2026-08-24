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
CLI, FTXUI terminal application, and native tests. It does not provide a Cocoa
or GTK desktop frontend, and it has no native audio backend. CLI and TUI
commands therefore run, but playback has no provider and the TUI reports `--`
for the backend.

The build targets macOS 14.0 or newer. The project-maintained validation host is
macOS 15.7.9 on x86_64. That host is the current support evidence; it is not
evidence that arm64 or the macOS 14.0 runtime has been validated.

## Policy

Run repository operations from the project root through `./ao`. Do not invoke a
system CMake toolchain or use Homebrew packages as substitutes for the pinned
environment. `shell.nix` selects `nixpkgs-darwin.json` on Darwin and supplies
Clang 22 plus every governed native dependency.

Keep generated state on the guest or workstation's local disk. A source tree
may be mounted over SMB, but build trees, compiler caches, and toolchains must
not be written to that mount.

The Darwin Nix pin intentionally does not promise the exact Python, Ruff, and
mypy versions in `script/ao/toolchain.json`, so `./ao test --tooling` remains
unavailable. Native clang-tidy and its integration fixtures are supported;
`./ao test --lint` is part of both `./ao test --all` and `./ao check`.

## Workflow

Install Nix and the Xcode Command Line Tools before the first portal command.
No separate CMake, Ninja, Python, or Homebrew dependency setup is required.
The first `./ao` invocation resolves Bash 5 through `darwin-bash.nix` because
Apple's Bash 3.2 cannot enter the current nixpkgs environment, then re-enters
the Darwin-specific `shell.nix` automatically. The x86_64 Darwin binary cache
is incomplete, so a cold bootstrap can build some packages from source.
For non-interactive SSH shells, the portal recovers Nix from the standard
multi-user or single-user profile before invoking `nix-build`; callers do not
need to edit shell startup files merely to use `./ao`.

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
./ao deps report              # Governed versions and Darwin Nix identities
./ao deps verify              # Reject stale or mismatched dependency evidence
```

The default build roots are `/tmp/build/<source-directory>/debug` and
`/tmp/build/<source-directory>/release`; sanitizer suffixes create
separate sibling trees. `AOBUS_BUILD_ROOT` replaces `/tmp/build` while retaining
the source-directory component. Each build writes `build.log` inside its tree.
The compiler cache defaults to
`$HOME/Library/Caches/Aobus/ccache`; `AOBUS_STATE_ROOT` replaces the
`$HOME/Library/Caches/Aobus` base for that cache.

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

The macOS `all` group is exactly core, TUI, CLI, integration, and lint. A
passing gate does not claim GUI, audio, or the exact Python tooling contract.
Linux and Windows remain required for those platform-specific contracts.

## Troubleshooting

- If the portal reports an unsupported Apple Bash, unset a stale
  `NIX_BUILD_SHELL` and run `./ao` again so `darwin-bash.nix` can select the
  pinned shell.
- If CMake reports `AOBUS_LIBCXX_EXPECTED_SHIM` as missing, the configure step
  was run outside the portal. Remove that manual build tree and use `./ao`.
- If a build tree appears under the SMB checkout, stop and choose a local
  `AOBUS_BUILD_ROOT`; network and case-insensitive filesystems are unsupported
  for generated state.
- macOS has no `timeout` command by default. Put command timeouts on the
  controlling host when operating the validation VM.
- A cold Nix evaluation can be slow because x86_64 Darwin cache coverage is
  limited. Preserve `/tmp/build/...` and the local compiler cache while
  diagnosing failures.

## Related documents

- [macOS portability compromises](macos-portability.md) owns removable
  toolchain workarounds and permanent Darwin source differences.
- [Dependency governance](dependency-governance.md) owns cross-platform version
  policy and the two Nix resolver pins.
- [Dependency upgrade](dependency-upgrade.md) owns pin changes and validation.
- [Test suites](test/test-suite.md) owns platform suite membership.
- [Validation and review](test/validation-and-review.md) owns completion gates.
