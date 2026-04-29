# AGENTS.md

Guidance for AI agents working in the RockStudio repository.

## Project Summary

RockStudio is a C++23 music application with a GTK4 (gtkmm) desktop frontend and a CLI tool, sharing a robust core library.

Build is CMake-based and uses `nix-shell` for dependency management.

## Environment Setup

Dependencies are managed with shell.nix. Always work inside the project shell:

```bash
nix-shell
```

> [!TIP]
> External library headers are located in the Nix store. You can find the exact paths by checking the build configuration or exploring the `/nix/store` directory.
> Use 'nix-shell -p' for additional tools required 

## Working Rules
4. **Search:** Use `rg` for searching. Prefer narrow scopes when possible.
5. **Assumptions:** State any technical assumptions clearly in your response.

## Build And Validation

### Using `build.sh` (Recommended)

```bash
./build.sh debug               # Configures and builds with sanitizers, runs tests
./build.sh debug --clang       # Clang build in its own cache/build tree
./build.sh debug --clean       # Full clean rebuild
./build.sh debug --tidy        # Debug build with clang-tidy enabled (uses clang)
./build.sh debug --clean --tidy # Clean debug build with clang-tidy enabled (uses clang)
```

### Manual CMake

```bash
nix-shell --run "cmake --preset linux-debug -B /tmp/build/debug"
nix-shell --run "cmake --build /tmp/build/debug --parallel"
nix-shell --run "/tmp/build/debug/test/rs_test"
```

When chasing a failure or clang-tidy warnings, prefer preserving the current `/tmp/build/...` directory and storing the relevant output alongside it in `/tmp` (for example `/tmp/rs-debug.log`) so you can inspect or diff results without paying for another full rebuild.
