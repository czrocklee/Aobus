# AGENTS.md

Guidance for AI agents working in the RockStudio repository.

## Project Summary

RockStudio is a C++23 music library application with a GTK4 (gtkmm) desktop frontend and a CLI tool, sharing a robust core library.

- **Core Library (`rs`):**
  - Logic & Data Structures: `include/rs/**`, `lib/core/**`.
  - Expression Engine: `include/rs/expr/**`, `lib/expr/**` (uses `gperf` for dispatch).
  - Tag Parsing: `lib/tag/**` (FLAC, MP4, MPEG support).
  - Database: LMDB wrappers in `include/rs/lmdb/**`, `lib/lmdb/**`.
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

> [!TIP]
> External library headers (e.g., PipeWire, SPA, or GTK) are located in the Nix store. You can find the exact paths by checking the build configuration or exploring the `/nix/store` directory.

## Working Rules

1. **Correctness & Architecture:** Provide the most correct and architecturally sound solution. Avoid workarounds or "quick fixes" that compromise long-term maintainability.
2. **Focus:** Do not rewrite unrelated files or refactor broadly unless explicitly directed. However, proactively advise on better architectural alternatives or improvements if you identify them during your research.
3. **Source of Truth:** Rigorously adhere to [CONTRIBUTING.md](CONTRIBUTING.md) for all C++ coding standards, including naming, member ordering, and modern features.
4. **Search:** Use `rg` for searching. Prefer narrow scopes when possible.
5. **Assumptions:** State any technical assumptions clearly in your response.
6. **Btrfs Snapshots:** Create a snapshot before multi-file or cross-module edits (see Safety Net below).

## Build And Validation

`build.sh` keeps each build mode and clang-tidy setting in its own build directory under `/tmp/build` so debug results can be reused across iterations instead of forcing full rebuilds.

- `debug` -> `/tmp/build/debug`
- `debug --clang` -> `/tmp/build/debug-clang`
- `debug --tidy` -> `/tmp/build/debug-clang-tidy` (`--tidy` implies `--clang`)
- `release` -> `/tmp/build/release`
- `release --clang` -> `/tmp/build/release-clang`
- `release --tidy` -> `/tmp/build/release-clang-tidy` (`--tidy` implies `--clang`)
- `profile` -> `/tmp/build/profile`
- `profile --clang` -> `/tmp/build/profile-clang`
- `profile --tidy` -> `/tmp/build/profile-clang-tidy` (`--tidy` implies `--clang`)
- `pgo1` / `pgo2` -> `/tmp/build/pgo`
- `pgo1 --clang` / `pgo2 --clang` -> `/tmp/build/pgo-clang`
- `pgo1 --tidy` / `pgo2 --tidy` -> `/tmp/build/pgo-clang-tidy` (`--tidy` implies `--clang`)

The two PGO steps intentionally share the same per-toolchain build directory so the generated profile data remains available for the optimize pass.

Keep debug-build artifacts, failing test binaries, logs, and repro inputs in temporary files or directories under `/tmp` whenever possible. Prefer rerunning the affected target or test from the existing build tree over deleting the build directory and starting from scratch.

During debugging, default to an incremental workflow:

1. Reuse the existing `/tmp/build/...` tree for the current compiler/tooling combination.
2. Save compiler output, test output, and repro data to temporary files when they will be needed again.
3. Only use `--clean` when the cache is genuinely invalid or the task specifically requires a fresh configure.

### Using `build.sh` (Recommended)

```bash
./build.sh debug               # Configures and builds with sanitizers, runs tests
./build.sh debug --clang       # Clang build in its own cache/build tree
./build.sh release             # Optimized build
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

When chasing a failure, prefer preserving the current `/tmp/build/...` directory and storing the relevant output alongside it in `/tmp` (for example `/tmp/rs-debug.log`) so you can inspect or diff results without paying for another full rebuild.

Always run tests after modifying core logic or the expression engine.

## Generated Code Notes

- **Gperf:** The expression engine (`lib/expr/`) uses `.gperf` files to generate perfect hash tables for metadata, property, and unit dispatching. CMake handles regeneration automatically.

## Editing Guidance

1. **Architecture:** Keep business logic in the `rs` core. Application-specific state and UI logic should reside in `app/core` or `app/platform`.
2. **Expression Engine:** When modifying the parser or evaluator, ensure `lib/expr` and `include/rs/expr` stay in sync.
3. **Tagging:** Keep format-specific code constrained to `lib/tag/{flac,mp4,mpeg}`.
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
