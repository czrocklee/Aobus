# Test Suites

For test authoring standards, layer placement, tags, assertion quality, and
helper boundaries, see `doc/dev/testing.md`.

The `./ao test` command exposes individual suites and two suite groups:

- `core`: core library Catch2 tests (`ao_core_test`).
- `tui`: terminal frontend Catch2 tests (`ao_tui_test`).
- `gtk`: GTK Catch2 tests (`ao_gtk_test`).
- `integration`: standalone integration tests (`ao_integration_test`).
- `fleet`: fleet C++ tests (`ao_fleet_test`).
- `tooling`: Python tests for the `./ao` tooling.
- `lint`: integration tests for the Aobus clang-tidy plugin.
- `default`: the core and GTK suites. This is selected when no suite option is given.
- `all`: every suite listed above.

`default` is intentionally the normal development loop. The TUI, integration, fleet, tooling, and lint suites
take longer, so they are included only when selected directly or through `all`.

Each suite is registered once in `script/ao/command/test.py` through `SUITES`
and `SUITE_GROUPS`. The registry defines the display name, runner kind, and
optional CMake target. Both `./ao test --all` and `./ao check` consume the same
`all` group, so the interactive test command and the full gate cannot drift
apart.

`--no-build` applies uniformly. Catch2 executables and the lint plugin must already exist in the selected
build tree; tooling tests never need a CMake build. `--path`, compiler, and sanitizer options select the same
tree for C++ and lint integration suites.

Tooling tests are exposed as `./ao test --tooling`, not as a separate top-level command. Their normal
output is a concise pass count; unittest output is captured in the gate log and shown in full on failure.

The lint suite is implemented by `script/ao/core/linttest.py`; `test/integration/lint/` contains only
checker fixtures. The runner invokes the existing `ao tidy` implementation without rebuilding, verifies
diagnostics against `POSITIVE` and `NEGATIVE` markers, and derives auto-fix expectations directly from
`FIX-TO` markers. Only fixtures that declare `FIX-TO` expectations enter the auto-fix stage. This keeps
checker execution policy in `ao tidy` and avoids a second shell-based test orchestration layer.

Coverage keeps its narrower `all` definition of core, TUI, and GTK because tooling and standalone integration
tests are not part of the application source coverage calculation.
