---
id: development.test.test-suite
---
# Test suites

For test authoring standards, layer placement, tags, assertion quality, and
helper boundaries, see `doc/development/test.md`.

The `./ao test` command exposes individual suites and four suite groups:

- `core`: shared core, runtime, UIModel, portable frontend policy, and host-native Catch2 tests (`ao_core_test`).
- `tui`: terminal frontend Catch2 tests (`ao_tui_test`).
- `cli`: command-line frontend Catch2 tests (`ao_cli_test`).
- `gtk`: GTK Catch2 tests (`ao_gtk_test`).
- `integration`: the standalone media-file, decoder, graph, native-provider, and enabled Linux system-media suite (`ao_integration_test`), not every case tagged `[integration]`.
- `tooling`: Python tests for the `./ao` tooling.
- `lint`: integration tests for the Aobus clang-tidy plugin.
- `appkit`: an opt-in native macOS GUI smoke harness with scenario selection and required disposable library and isolated state-root inputs; it is not part of a suite group.
- `default`: the native fast-loop group. Linux runs core and GTK; macOS and Windows run core and TUI.
- `all`: every suite enabled by the native build profile.
- `tsan`: suites with a clean ThreadSanitizer baseline.
- `concurrency`: every native Catch2 suite, filtered to non-hidden concurrency cases with `[concurrency]~[.]`.

Suites choose binaries; Catch2 filters choose cases within them. So
`./ao test --core "[runtime][integration]"` runs integration-scoped cases in core,
while `--integration` never collects cases from other binaries. A filter skips
tooling and lint, and it fails only when no selected suite matches; a matched
case that skips still counts as matched.

`./ao check` is the complete native gate. CTest is not equivalent: it omits the
standalone integration suite and the portal's tooling, lint, and environment setup.

`default` is intentionally the normal development loop. On Linux, the TUI, CLI,
integration, tooling, and lint suites take longer, so they are included
only when selected directly or through `all`. The Windows `all` group contains
core, TUI, CLI, integration, tooling, and lint. GTK is not built.
`ao.bat test --lint`, `ao.bat test --all`, and `ao.bat check` prepare the static
`AobusClangTidy.exe` through the existing `windows-tidy` preset before running
fixtures. The checker host stays separate from Debug, Release, and ASan product
trees: standalone and group runs use `windows-tidy` beside the default flavor
trees, or `<primary>-tidy` when `BUILD_DIR` selects a nondefault primary tree.
For a group, `-p <dir>` selects the primary product tree and derives its host
sibling. Standalone `--lint -p <dir>` instead selects the exact prepared host
tree and preserves that tree's configured preset rather than resetting it.
`--no-build` requires that host and its configured SDK to exist and still verifies
LLVM version and the complete Aobus check registry. First use adds the governed
LLVM SDK download and static-host build; subsequent gates build it incrementally
and run the full compatible fixture and smoke stages.

Windows fixture commands use that host, the shared portal checker configuration,
and explicit clang-cl arguments rather than borrowing product PCH/compiler flags.
Diagnostic invocations use the configured LLVM SDK clang-tidy host and resource
headers. Standard includes come from the portal-initialized Visual Studio
`INCLUDE` search list, including under `--no-build`. Empty search-list elements are ignored as by the native
compiler; unused nonexistent directories do not invalidate the remaining
providers. Missing required headers still fail compilation. Ambient `CXX`
and `g++` are not substitutes.
Objective-C++ `.mm` fixtures are explicitly listed as platform-incompatible and
not tested on Windows; C++ and header fixtures retain diagnostic assertions and
compiler-error detection in the diagnostic invocation.

The macOS `all` group contains core, TUI, CLI, integration, and lint. GTK is not
built. Its Homebrew host Python is outside the exact repository-tooling
contract; native clang-tidy and its integration fixtures do have a Darwin
baseline. `./ao test --all` and `./ao check` resolve the same supported
five-suite group. macOS Catch2 executables run directly, like their Linux and
Windows counterparts. The portal neither creates a console login nor changes
the host's power policy. Core Audio coverage requires a logged-in console user;
the GitHub Actions job checks that prerequisite before running the gate, and an
unattended local host should provide it through host configuration.

The tooling gate uses the pinned Ruff and mypy environment supplied by Nix on
Linux and the checkout-specific managed environment supplied by `ao.bat` on
Windows. It is not exposed on macOS. On its supported hosts it probes the
running Python, Ruff, and mypy versions against
`script/ao/toolchain.json`, verifies the Windows hash lock agrees with that
contract, and tests documentation validation against isolated fixtures. Run
`./ao docs check` separately to validate the live documentation tree. It never
depends on unrelated tools from the ambient Windows `PATH`.

Tooling also exercises real LLVM processes through the portal's selected-file
diagnostic filters, including unselected-file
exclusion. These upstream-rule controls do not replace the custom-checker lint
integration suite. On Windows, prepare their governed SDK with
`ao.bat setup llvm-sdk` before the tooling gate;
CI does so explicitly. Missing local tools may skip these controls outside CI;
under `CI=true`, missing required tools fail on every host. POSIX controls use
the portal's native LLVM 22 environment and require the compiler and checker to
report the same release version; Windows uses the exact configured SDK.
Configured-tool or version failures never become skips.

Each suite is registered once in `script/ao/command/test.py` through `SUITES`
and the native groups are defined by `script/ao/core/builddir.py` platform profiles.
The registry defines the display name, runner kind, and optional CMake target.
Both `ao test --all` and `ao check` resolve the same native `all` group, so the
interactive test command and the full gate cannot drift apart.

ThreadSanitizer is intentionally different. `ao test --tsan` and
`ao check --tsan` resolve `default`/`all` to the native `tsan` group. Explicit
suite selection, such as `ao test --gtk --tsan`, remains available for focused diagnosis.
The Linux TSan group contains core and GTK; reviewed uninstrumented UI dependencies use module-scoped interceptor suppressions.
Registered real-TUI MPRIS process cases in the integration binary require Linux system media and TUI support and are disabled under TSan, because they launch the actual FTXUI product. Normal and ASan runs cover that composition; focused adapter tests remain available under TSan.
The macOS TSan group contains core.
Windows has no TSan suite group because the MSVC toolchain does not provide
ThreadSanitizer; requesting `--tsan` is an error. `ao.bat check --asan` runs the
entire native Windows `all` group with MSVC AddressSanitizer in its own build
tree.
See `concurrency-and-sanitizer.md` for the suppression boundary.

`--repeat N` repeats the selected tests and stops on the first failure. It is a
stress aid; deterministic synchronization remains mandatory for regression
tests.

`--no-build` applies uniformly. Catch2 executables and the native lint artifact
must already exist in the selected build tree. Tooling tests do not build Aobus,
but their registration-guard fixtures need CMake and a C++ compiler; on Windows
they use Visual Studio's bundled CMake. On Linux and macOS, `--path`, compiler,
and sanitizer options select the same flavor tree for C++ and lint integration
suites. Windows product tests reuse their selected flavor tree; the separately
configured lint host follows the selection rules above. Tests are configured by default, while `cmake --build
--target ...` limits an incremental build to the selected suite targets.
Native test invocations, including `--no-build`, require the tree's recorded C/C++
compiler family and sanitizer configuration to match the requested options.
Configure the intended sanitizer mode with `ao build -p <dir>` before testing a
reused explicit tree. Every configure writes both sanitizer switches explicitly.
Changing compiler family requires a separate tree or an explicit clean build;
the portal rejects incompatible reuse before configuring or building it.

Tooling tests are exposed as `./ao test --tooling` on Linux and
`ao.bat test --tooling` on Windows, not as a separate top-level command. Their
normal output is a concise pass count; unittest output is captured in the gate
log and shown in full on failure.

The lint suite is implemented by `script/ao/core/linttest.py`; `test/integration/lint/` contains only
checker fixtures. On Linux and macOS the runner invokes the existing `ao tidy` implementation without rebuilding and verifies
diagnostics against `POSITIVE` and `NEGATIVE` markers, with optional message assertions for semantic advice.
Clang parses each original fixture during that invocation; compiler errors fail the fixture, and separate compiler-error canaries verify that gate.
No second syntax-only compiler pass or independent GCC compatibility requirement is imposed on checker fixtures.
This keeps checker execution policy in `ao tidy` and avoids a second shell-based test orchestration layer.

For fixture placement, marker semantics, context-header constraints, and Objective-C++ handling, use [checker development](../lint/checker-development.md#fixture-contract). Those are fixture-authoring rules rather than suite-selection rules.

Coverage has a separate `--all` selection of core, TUI, CLI, and GTK. It excludes standalone integration, tooling, and lint suites from the application-source coverage run.

## Environment availability

Missing or malformed repository fixtures fail. Only a genuinely optional host
capability may `SKIP()` with a reason; `WARN()` or `SUCCEED()` followed by
`return` is not a skip.

The PipeWire provider test skips when no daemon is reachable, unless
`AOBUS_REQUIRE_PIPEWIRE=1` makes that a failure. Linux CI sets it and runs a
private PipeWire daemon with a policy-only WirePlumber, so the test exercises its
own null sinks, not host audio hardware.

## Sharded Catch2 execution

Every Catch2 suite runs as several parallel shards of one binary. The default
shard count is `min(16, cores - 1)`; `AOBUS_TEST_SHARDS` overrides it in either
direction, and `AOBUS_TEST_SHARDS=1` restores a single process.
CI sets the override to `min(16, logical processor count)`.
The cap bounds process count: a shard is a whole test process with its own fixtures, temporary
tree, and, for GTK, its own display and session bus.

Four kinds of run stay single-process, because parallel siblings would change
what they measure rather than only how long they take: `--list`, `--repeat N`,
`--tsan`, and the `concurrency` group. `ao coverage` is also unsharded; parallel
processes would interleave writes to the same gcov counters.

Sharding multiplies memory, which matters for sanitizer trees: a core ASan
shard holds about 1.2 GB. Lower `AOBUS_TEST_SHARDS` on a smaller machine.

Each GTK shard gets its own Xvfb and private session bus; see
[GTK process isolation](uimodel-and-gtk.md#process-isolation-and-diagnostics).
Sharing one display is not safe: tests that drain only pending events can assert
before a popover's X round trip completes.

Every shard of one run receives the same `--rng-seed`. Catch2 orders cases
randomly and `--shard-index` slices that order, so shards with different seeds
would run some cases twice and others never while reporting a full count. One
shared seed partitions the suite exactly.

The portal prints the seed, then one combined tally per suite in place of the
per-shard summaries. Every shard's console output goes to the gate log; only
shards that failed are echoed to the terminal, each followed by the command
that reruns it:

```text
--- shard 3 of 8 (exit 42) ---
...
rerun this shard: /tmp/build/Aobus/debug/test/ao_core_test --rng-seed 2914 --allow-running-no-tests --shard-count 8 --shard-index 2
```

Copy that line rather than assembling one. Shards are counted from one when
reported and from zero by Catch2, so the third shard of eight is
`--shard-index 2`.

On a sanitizer tree the line is prefixed with the shard's environment, because
part of it decides whether the failure reproduces rather than only how the run
is configured -- `UBSAN_OPTIONS=halt_on_error=1` is what makes undefined
behaviour stop the run instead of logging and continuing:

```text
rerun this shard: env LSAN_OPTIONS=suppressions=... UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 /tmp/build/Aobus/debug-asan/test/ao_core_test --rng-seed 2914 ...
```

A GTK rerun line disables the retired display and bus instead of falling back to
the host session, so rerun GTK failures through the portal, for example
`./ao test --gtk "SeekControlWidget*"`.
