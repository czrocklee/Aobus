# AGENTS.md

Guidance for AI agents working in the RockStudio repository.

## Project Summary

RockStudio is a C++23 music library application with a GTK4 (gtkmm) desktop frontend and a CLI tool, sharing a robust core library.

- **Core Library (`rs`):**
  - Logic & Data Structures: `include/rs/**`, `src/core/**`.
  - Expression Engine: `include/rs/expr/**`, `src/expr/**` (uses `gperf` for dispatch).
  - Tag Parsing: `src/tag/**` (FLAC, MP4, MPEG support).
  - Database: LMDB wrappers in `include/rs/lmdb/**`, `src/lmdb/**`.
  - Reactive Patterns: `include/rs/reactive/**`.
- **Desktop Application (`app`):**
  - Shared App Logic: `app/core/**`.
  - Linux Platform (GTK4/gtkmm): `app/platform/linux/**`.
  - Windows Platform: `app/platform/windows/**` (Stubbed).
- **CLI Tool (`tool`):** `rsc` target, logic in `tool/**`.

Build is CMake-based and uses `nix-shell` for dependency management.

## Environment Setup

Dependencies are managed with Nix. Always work inside the project shell:

```bash
nix-shell
```

Key dependencies: `gtkmm-4.0`, `lmdb`, `boost`, `ffmpeg`, `pipewire`, `alsa`, `gperf`, `spdlog`, `catch2`.

## Working Rules

1. **Small Changes:** Make the smallest correct change that solves the request.
2. **Focus:** Do not rewrite unrelated files or refactor broadly unless explicitly directed.
3. **Style:** Rigorously adhere to existing coding style (e.g., C++23 features, naming conventions).
4. **Search:** Use `rg` for searching. Prefer narrow scopes when possible.
5. **Assumptions:** State any technical assumptions clearly in your response.

## Build And Validation

The project builds into `/tmp/build` (configured in `CMakePresets.json`).

### Using `build.sh` (Recommended)

```bash
./build.sh debug       # Configures and builds with sanitizers, runs tests
./build.sh release     # Optimized build
./build.sh debug clean # Full clean rebuild
```

### Manual CMake

```bash
nix-shell --run "cmake --preset linux-debug"
nix-shell --run "cmake --build /tmp/build --parallel"
nix-shell --run "/tmp/build/rs_test"
```

Always run tests after modifying core logic or the expression engine.

## Generated Code Notes

- **Gperf:** The expression engine (`src/expr/`) uses `.gperf` files to generate perfect hash tables for metadata, property, and unit dispatching. CMake handles regeneration automatically.

## Editing Guidance

1. **Architecture:** Keep business logic in the `rs` core. Application-specific state and UI logic should reside in `app/core` or `app/platform`.
2. **Expression Engine:** When modifying the parser or evaluator, ensure `src/expr` and `include/rs/expr` stay in sync.
3. **Tagging:** Keep format-specific code constrained to `src/tag/{flac,mp4,mpeg}`.
4. **UI:** The Linux frontend uses GTK4 via `gtkmm-4.0`. Avoid mixing UI frameworks or platform-specific code outside of `app/platform`.
5. **Headers:** Avoid changing public headers in `include/rs/**` unless necessary for the feature or fix.

## Btrfs Snapshot Safety Net

Before making multi-file or cross-module edits, create a btrfs snapshot if supported by the environment.

Treat a task as requiring a snapshot when:
1. Modifying 2 or more files.
2. Touching multiple subsystems (e.g., core + UI).
3. Performing refactors or migrations.

```bash
# Create snapshot
btrfs-snap /home/rocklee/dev rockstudio-<description>-$(date +%Y%m%d-%H%M%S)
```

Mention the snapshot name in your progress update. Do NOT attempt to restore snapshots yourself.
