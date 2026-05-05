---
name: reviewing-code
description: Reviews Aobus diffs for correctness, regressions, and standards risk. Use when asked to review a patch, commit, branch, or local changes in this repository.
---

# Reviewing Code

Use this skill for high-signal Aobus review. Optimize for bugs, regressions, and risky omissions before style.

## Workflow

1. Start from the delta: inspect the requested diff or commit range, plus `git diff --stat` to understand scope.
2. Read each changed file fully before judging a hunk. Use `rg` to pull in nearby callers or callees only when the behavior crosses file boundaries.
3. Review in this order:
   - behavior regressions and crash risk
   - ownership, lifetime, and thread-safety
   - boundary validation for CLI, filesystem, subprocess, or C-library interop
   - missing or incorrect tests at important behavioral edges
   - `CONTRIBUTING.md` violations with real maintenance cost
4. For changed C++ code, load `check-code-conformance` only when standards verification or merge-readiness matters. Do not default every review to `clang-tidy`.
5. If the task includes staging or committing, load `manage-git-flow` for formatting and commit mechanics instead of duplicating that checklist.

## Aobus Hotspots

- GTK or GLib ownership and callback lifetimes
- FFmpeg, ALSA, and PipeWire resource cleanup, plus required `::` C API prefixes
- `std::jthread`, stop-token, mutex, and UI-thread handoff behavior
- Designated initialization and default handling for config-like structs
- Subtle test gaps around playback state, async teardown, and parser or CLI edges

## Fast Paths

- Docs or CMake-only changes: review commands, paths, and broken examples; skip the C++ standards pass.
- Large mechanical diffs: verify the pattern quickly, then focus on the non-mechanical edits.

## Reporting

- Lead with findings only, highest risk first.
- Each finding needs file and line, impact, and the smallest fix.
- Skip speculative nits unless the coding guide requires them.
- If no findings exist, say so explicitly and note any checks you skipped.
