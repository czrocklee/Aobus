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

## Build And Validation

### Using `build.sh` (Recommended)

```bash
./build.sh debug               # Configures and builds with sanitizers, runs tests
./build.sh debug --clang       # Clang build in its own cache/build tree
./build.sh debug --clean       # Full clean rebuild

# clang-tidy (standalone, via shell script)
./script/run-clang-tidy.sh                     # Changed files (default)
./script/run-clang-tidy.sh --all               # Check entire repo
./script/run-clang-tidy.sh --folder test       # Check test folder
```

### Manual CMake

```bash
nix-shell --run "cmake --preset linux-debug -B /tmp/build/debug"
nix-shell --run "cmake --build /tmp/build/debug --parallel"
nix-shell --run "/tmp/build/debug/test/ao_test"
```

When chasing a failure or clang-tidy warnings, prefer preserving the current `/tmp/build/...` directory. Note that `build.sh` automatically redirects all console output (configure, build, and tests) to `$BUILD_DIR/build.log` for easier inspection and diffing without full rebuilds.
