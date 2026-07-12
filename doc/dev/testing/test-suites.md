# Test Suites

For test authoring standards, layer placement, tags, assertion quality, and
helper boundaries, see `doc/dev/testing.md`.

The `./ao test` command exposes individual suites and two suite groups:

- `core`: core library Catch2 tests (`ao_core_test`).
- `tui`: terminal frontend Catch2 tests (`ao_tui_test`).
- `cli`: command-line frontend Catch2 tests (`ao_cli_test`).
- `gtk`: GTK Catch2 tests (`ao_gtk_test`).
- `integration`: standalone integration tests (`ao_integration_test`).
- `council`: council C++ tests (`ao_council_test`).
- `tooling`: Python tests for the `./ao` tooling.
- `lint`: integration tests for the Aobus clang-tidy plugin.
- `default`: the native fast-loop group. Linux runs core and GTK; Windows runs core and TUI.
- `all`: every suite enabled by the native build profile.
- `tsan`: suites with a clean ThreadSanitizer baseline.
- `concurrency`: every native Catch2 suite, filtered to `[concurrency]` tests.

`default` is intentionally the normal development loop. On Linux, the TUI, CLI,
integration, council, tooling, and lint suites take longer, so they are included
only when selected directly or through `all`. The Windows `all` group contains
core, TUI, CLI, integration, and tooling. The normal Windows presets do not
build GTK, the council tool, or the lint integration suite. Native `ao.bat tidy`
uses a separate `windows-tidy` preset that builds the self-contained
`AobusClangTidy.exe`.

The tooling gate uses the pinned Ruff and mypy environment supplied by Nix on
Linux and the checkout-specific managed environment supplied by `ao.bat` on
Windows. It probes the running Python, Ruff, and mypy versions against
`script/ao/toolchain.json` and verifies the Windows hash lock agrees with that
contract. It never depends on unrelated tools from the ambient Windows `PATH`.

Each suite is registered once in `script/ao/command/test.py` through `SUITES`
and the native groups are defined by `script/ao/core/builddir.py` platform profiles.
The registry defines the display name, runner kind, and optional CMake target.
Both `ao test --all` and `ao check` resolve the same native `all` group, so the
interactive test command and the full gate cannot drift apart.

ThreadSanitizer is intentionally different. `ao test --tsan` and
`ao check --tsan` resolve `default`/`all` to the native `tsan` group. Explicit
suite selection, such as `ao test --gtk --tsan`, remains available for diagnosis
but does not claim a clean platform baseline. See
`concurrency-and-sanitizers.md` for the current GTK limitation.

`--repeat N` repeats the selected tests and stops on the first failure. It is a
stress aid; deterministic synchronization remains mandatory for regression
tests.

`--no-build` applies uniformly. Catch2 executables and the native lint artifact
must already exist in the selected build tree; tooling tests never need a CMake
build. `--path`, compiler, and sanitizer options select the same tree for C++
and lint integration suites. On both platforms, build and test commands reuse
the same flavor tree. Tests are configured by default, while `cmake --build
--target ...` limits an incremental build to the selected suite targets.

Tooling tests are exposed as `./ao test --tooling` on Linux and
`ao.bat test --tooling` on Windows, not as a separate top-level command. Their
normal output is a concise pass count; unittest output is captured in the gate
log and shown in full on failure.

The lint suite is implemented by `script/ao/core/linttest.py`; `test/integration/lint/` contains only
checker fixtures. The runner invokes the existing `ao tidy` implementation without rebuilding, verifies
diagnostics against `POSITIVE` and `NEGATIVE` markers, and derives auto-fix expectations directly from
`FIX-TO` markers. Only fixtures that declare `FIX-TO` expectations enter the auto-fix stage. This keeps
checker execution policy in `ao tidy` and avoids a second shell-based test orchestration layer.

Coverage keeps its narrower `all` definition of core, TUI, and GTK because tooling and standalone integration
tests are not part of the application source coverage calculation.
