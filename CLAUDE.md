# CLAUDE.md

Guidance for Claude Code when working in this repository.

## Project Summary

RockStudio is a C++23 music library application with multiple frontends and a shared core:

- Core/data/logic: `include/rs/**`, `src/core/**`, `src/expr/**`, `src/tag/**`
- Qt frontend: `app/**`
- GTK frontends: `app/gtk/**`, `app/gtkmm/**`
- CLI tool: `tool/**` (`rsc` target)
- FlatBuffers schemas and generated code inputs: `fbs/**`

Build is CMake-based and relies on generated artifacts (FlatBuffers + custom generator).

## Environment Setup

Dependencies are managed with Nix. Start work inside the project shell before running build, test, or search commands:

```bash
nix-shell
```

Core tools expected to be available in the shell include `cmake`, `pkg-config`, `gcc`, `flatbuffers`, Qt/GTK dependencies, and `rg` (ripgrep).

## Working Rules

1. Make the smallest correct change that solves the request.
2. Do not rewrite unrelated files or refactor broadly unless asked.
3. Preserve existing coding style in each touched file.
4. Prefer `rg`/`rg --files` for search.
5. If assumptions are required, state them clearly in your final response.

## Build And Validation

All builds should be run from within `nix-shell`:

```bash
nix-shell --run "cmake --preset linux-debug"
nix-shell --run "cmake --build --preset linux-debug --parallel"
```

Or use direct commands:

```bash
nix-shell --run "cmake -S . -B /tmp/build -DCMAKE_BUILD_TYPE=Debug -DCMAKE_VERBOSE_MAKEFILE=ON -DCMAKE_EXPORT_COMPILE_COMMANDS=ON"
nix-shell --run "cmake --build /tmp/build"
```

**IMPORTANT**: Always use `cmake --build` (not `make` directly). Use `--parallel` (or `-- -j$(nproc)`) to enable parallel builds — `cmake --build` does not parallelize automatically with the Unix Makefiles generator.

The project builds into `/tmp/build` (as configured in CMakePresets.json).

When changing core logic (`src/core`, `src/expr`, `src/tag`, `include/rs`), always run at least a debug build before finishing.

## Generated Code Notes

This project generates code from FlatBuffers schemas.

- Schemas: `fbs/*.fbs`
- Generated output directory: `${build_dir}/gen`

Do not hand-edit generated files in build output. Instead edit sources (`fbs/**` or generator sources in `src/gen/**`).

## Editing Guidance

1. For parser/evaluator work, keep `src/expr/*` and `include/rs/expr/*` in sync.
2. For tag parsing, keep format-specific code constrained to:
   - MP3/MPEG: `src/tag/mpeg/**`
   - FLAC: `src/tag/flac/**`
   - MP4: `src/tag/mp4/**`
3. For UI changes, avoid mixing Qt and GTK concerns in one patch unless required.
4. Avoid changing public headers in `include/rs/**` unless needed for the feature or fix.

## Response Expectations

In final responses:

1. List exactly which files were changed.
2. Explain user-visible or behavior-impacting changes first.
3. Report what validation was run (or why not run).
4. Call out risks, assumptions, or follow-up tasks when relevant.

## Btrfs Snapshot Safety Net

Before making multi-file or cross-module edits, create a btrfs snapshot to protect against data loss.

Treat a task as requiring a snapshot when any of the following is true:

1. You expect to modify 2 or more files.
2. The change touches multiple subsystems (for example core + UI, or parser + tag code).
3. The task is a refactor, migration, or broad mechanical update.

For these tasks, run the snapshot command **before** your first file edit:

```bash
# Create snapshot
btrfs-snap /home/rocklee/dev rockstudio-<description>-$(date +%Y%m%d-%H%M%S)

# Create read-only snapshot
btrfs-snap /home/rocklee/dev rockstudio-<description>-$(date +%Y%m%d-%H%M%S) -r

# List snapshots
ls -la /.snapshots/
```

After creating the snapshot, mention the snapshot name once in your next progress update so recovery points are traceable.

**Important**: Do NOT attempt to restore snapshots yourself. If restoration is needed, ask the user to do it manually.
