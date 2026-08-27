# Aobus Agent Guide

Aobus is a C++26 music application: a GTK4 (gtkmm) desktop frontend, a TUI, and
a CLI tool over a shared core library. CMake builds the project. Linux
dependencies come from `nix-shell`; macOS and Windows use the shared vcpkg
manifest. Always work from the project root. Read `doc/development/macos.md`
before native macOS work. On native Windows, use `ao.bat` with the same command
vocabulary and read `doc/development/windows.md` first.

> [!TIP]
> On Linux, external headers live in the Nix store; use `nix-shell --run
> "pkg-config --cflags <lib>"` when needed. On macOS and Windows, inspect the
> configured vcpkg installation under the active build tree.

## Human References

Read the human docs for project policy instead of duplicating them here:

- `CONTRIBUTING.md` for contributor rules and review references.
- `doc/README.md` for where documentation belongs.
- `doc/development/coding-style.md` for C++ style.
- `doc/development/naming-convention.md` for identifier, type, file, and helper naming.
- `doc/development/test.md` for testing policy.
- `doc/development/linting.md` for lint policy (warning fix/suppress rules, NOLINT playbook).
- `doc/development/commit-message.md` for commit message rules.
- `doc/development/macos.md` for the native macOS support boundary and workflow.

## Working Rules

1. **Language:** English for all code comments, commit messages, and docs.
2. **Search:** Use `rg`, prefer narrow scopes.
3. **Assumptions:** State technical assumptions in your response.
4. **No TACO:** Do not over-promise and under-deliver; no shortcuts when things get difficult.
5. **Docs:** When behavior or architecture changes, use `doc/README.md` to
   select the authoritative documentation type and owner.
6. **Tests:** All changes include appropriate test coverage.
7. **Scratch files:** Agent throwaway artifacts go to `/tmp`, never into the repo.
8. **Hygiene:** Do not run format or tidy tools mid-session unless the user
   explicitly asks for linting. The final check-only `./ao hygiene` pass is
   part of completed-work validation.
9. **Validation:** Follow `doc/development/test/validation-and-review.md`; completed
   work normally runs one full `./ao check`, then `./ao hygiene`.
10. **Concurrency:** Follow `doc/development/test/concurrency-and-sanitizer.md` for
    concurrency-sensitive changes.
11. **Proportionality:** Aobus is a music application, not a flight-control or
    life-support system. Match engineering rigor to actual product risk and do
    not over-design for speculative hazards.

> [!TIP]
> Heavy development, no compatibility/migration constraints. Propose the best approach without historical baggage.

## Build and Validation

On Linux and macOS, everything goes through the `./ao` portal (Python package
in `script/ao/`). It re-enters Nix on Linux and prepares Homebrew plus pinned
vcpkg state on macOS. Platform suite groups and available frontends differ;
`./ao help` lists commands and `./ao <cmd> --help` has all options.

```bash
./ao check                    # build/test gate: everything + all native suites (--clang/--asan/--tsan)
./ao build [release] [--clean] [--target <t>]    # incremental build, no tests
./ao run <app> [release] [-n] [-- args]           # apps follow the native platform profile
./ao test [--core|--gtk|--all|...] "[tag]"       # suite groups are platform-specific
./ao test --tooling           # Linux tooling gate; use ao.bat on Windows; unavailable on macOS
./ao test --concurrency       # all native Catch2 [concurrency] contracts
./ao hygiene                  # completion hygiene gate: format/audits/tidy on changed files
./ao tidy [paths|--folder <d>|--all]             # C++ clang-tidy + Python Ruff/mypy (opt-in, rule 8)
./ao analyze                  # Clang Static Analyzer, report-only
./ao coverage "rt::Foo"       # gcov coverage for a test subset
./ao deps report|verify       # governed dependency report / verification
./ao deps report --concepts   # RFC 0002 concept baseline into concept-report.json
./ao docs check               # documentation metadata, links, anchors, and reachability
./ao format                   # clang-format + ruff format (gate fixes / explicit request only)
```

Manual CMake is rarely needed. On Linux, enter `nix-shell` first; on macOS,
source `script/ao/macos-vcpkg-bootstrap.sh` and prepare the build environment
before using the native preset. Prefer `./ao build -p <tree>` in both cases.

Preserve `/tmp/build/...` trees when chasing failures; `./ao build`/`check` tee all output to `$BUILD_DIR/build.log`.
