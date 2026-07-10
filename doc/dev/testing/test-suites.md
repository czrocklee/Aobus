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

`default` is intentionally the normal development loop. On Linux, the TUI, CLI,
integration, council, tooling, and lint suites take longer, so they are included
only when selected directly or through `all`. The Windows `all` group contains
core, TUI, integration, and tooling. The normal Windows test presets do not
build GTK, CLI, the council tool, or the lint integration suite. Native
`ao.bat tidy` uses a separate `windows-tidy` preset that builds the
self-contained `AobusClangTidy.exe`.

The tooling gate uses the pinned Ruff and mypy environment supplied by Nix on
Linux and the checkout-specific managed environment supplied by `ao.bat` on
Windows. It never depends on unrelated tools from the ambient Windows `PATH`.

Each suite is registered once in `script/ao/command/test.py` through `SUITES`
and the native groups are defined by `script/ao/core/builddir.py` platform profiles.
The registry defines the display name, runner kind, and optional CMake target.
Both `ao test --all` and `ao check` resolve the same native `all` group, so the
interactive test command and the full gate cannot drift apart.

`--no-build` applies uniformly. Catch2 executables and the native lint artifact
must already exist in the selected build tree; tooling tests never need a CMake
build. `--path`, compiler, and sanitizer options select the same tree for C++
and lint integration suites. On Windows, a build-enabled test command configures
the matching `windows-tui-*-tests` preset automatically; application-only and
test-enabled trees are intentionally separate under the local state root.

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
