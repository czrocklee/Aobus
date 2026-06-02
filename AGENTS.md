# AGENTS.md

Guidance for AI agents working in the Aobus repository.

## Project Summary

Aobus is a C++26 music application with a GTK4 (gtkmm) desktop frontend and a CLI tool, sharing a robust core library.

Build is CMake-based and uses `nix-shell` for dependency management.

## Environment Setup

`nix-shell` needs `shell.nix` in CWD. Always run from the project root.

> [!TIP]
> External library headers are in the Nix store; check build config for paths. Use `nix-shell -p` for extra tools. For rapid library discovery, use `nix-shell --run "pkg-config --cflags <lib-name>"` (e.g., `gtkmm-4.0`).
## Working Rules
1. **Language:** Always use English for code comments, commit messages, and documentation to maintain international accessibility.
2. **Search:** Use `rg` for searching. Prefer narrow scopes when possible.
3. **Assumptions:** State any technical assumptions clearly in your response.
4. **No TACO (Trump Always Chickens Out):** Do not over-promise and under-deliver. Always follow through on complex requirements, without taking shortcuts when things get difficult.
5. **Design Docs:** Update `doc/design/` when modifying code that affects user-facing behavior. Sync all affected docs.
6. **Test Coverage:** All changes must include appropriate test coverage.
7. **Temporary Workspace:** Do not use the repository directory as a scratch or temporary workspace. Generate throwaway files, experiment outputs, and ad hoc test artifacts under `/tmp` or another explicit external workspace, unless the file is an intentional source, test, fixture, or documentation change.
8. **Formatting Timing:** Do not run `clang-format` or other formatting commands during normal implementation, debugging, validation, or final response prep. Keep edits manually style-aware during iteration. Run formatting only when the user explicitly asks for formatting or when creating a commit; in that case, run one targeted formatting pass immediately before staging/committing.
9. **clang-tidy Opt-In:** Do not run clang-tidy, `./script/run-clang-tidy.sh`, or lint-only validation during a session unless the user explicitly asks for linting, clang-tidy, or lint cleanup in that session. Prefer build and targeted test validation by default. If the user explicitly requests clang-tidy/lint work, use the repository script rather than invoking clang-tidy directly.
> [!TIP]
> This repo is still under heavy development thus has no compatiblity/migration requrements, do suggests the best approach without historical baggage.

## Build And Validation

### Using `build.sh` (Recommended)

```bash
./build.sh debug               # Configures and builds with sanitizers, runs tests
./build.sh debug --clang       # Clang build in its own cache/build tree
./build.sh debug --clean       # Full clean rebuild

# Run specific tests with Catch2 filters (requires existing build)
./script/run-tests.sh --core "[audio]"         # Run core tests matching [audio]
./script/run-tests.sh --gtk "[layout],[model]" # Run GTK tests matching [layout] OR [model]
./script/run-tests.sh --gtk --list "[layout]"  # List GTK tests matching [layout]

# clang-tidy (standalone, via shell script; run only when explicitly requested)
./script/run-clang-tidy.sh                     # Changed files (default)
./script/run-clang-tidy.sh --all               # Check entire repo
./script/run-clang-tidy.sh --folder test       # Check test folder

# Clang Static Analyzer (standalone, report-only by default)
./script/run-clang-analyzer.sh                 # Changed files (default)
./script/run-clang-analyzer.sh --all           # Analyze entire repo
./script/run-clang-analyzer.sh --folder lib    # Analyze one folder
```

### Manual CMake

```bash
nix-shell --run "cmake --preset linux-debug -B /tmp/build/debug"
nix-shell --run "cmake --build /tmp/build/debug --parallel"
nix-shell --run "/tmp/build/debug/test/ao_test"
```

When chasing a failure, clang-tidy warnings, or analyzer diagnostics, prefer preserving the current `/tmp/build/...` directory. Note that `build.sh` automatically redirects all console output (configure, build, and tests) to `$BUILD_DIR/build.log` for easier inspection and diffing without full rebuilds.
