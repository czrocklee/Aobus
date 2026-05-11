---
name: reviewing-code
description: Provides Aobus-specific guidance for code reviews focused on correctness, regressions, performance, ownership, tests, and architecture risk. Use with the builtin code-review workflow when reviewing Aobus patches, commits, branches, or local changes.
---

# Reviewing Code

Use this skill as the Aobus-specific context layer for the builtin `code-review` workflow. Prefer the builtin `code_review` tool for formal reviews; this skill supplies the project-specific focus areas and exclusions.

Do not report pure style or convention issues here. Delegate those to `check-code-conformance` when conformance validation or merge-readiness is requested.

## Workflow

1. For formal review, load the builtin `code-review` skill and pass the Aobus guidance below through the `code_review` tool's `instructions` field.
2. If doing a manual review because the tool is unavailable, start from the requested delta and read each changed file fully before judging a hunk.
3. Use `rg` to pull in nearby callers or callees only when behavior crosses file boundaries.
4. Review in this order:
   - behavior regressions and crash risk
   - ownership, lifetime, and thread-safety
   - boundary validation for CLI, filesystem, subprocess, or C-library interop
   - missing or incorrect tests at important behavioral edges
   - performance, API, and architecture risks with concrete impact
5. Do not report pure formatting, naming, member-order, `const`, `final`, initialization-style, or C API `::` prefix findings here.
6. If the task includes staging or committing, load `manage-git-flow` for formatting and commit mechanics instead of duplicating that checklist.

## Instructions For `code_review`

When using the builtin tool, pass a concise instruction like this:

```text
Review this Aobus change for behavior regressions, crash risk, ownership/lifetime/thread-safety issues, boundary validation, important missing tests, performance, API, and architecture risk. Do not report pure style/convention issues such as formatting, naming, member order, const/final/init style, designated initializers, or C API :: prefixes; those belong to check-code-conformance. If a tooling warning indicates a bug, performance issue, security risk, concurrency issue, or lifetime issue, review it here.
```

## Aobus Hotspots

- GTK or GLib ownership and callback lifetimes
- FFmpeg, ALSA, and PipeWire resource cleanup and error paths
- `std::jthread`, stop-token, mutex, and UI-thread handoff behavior
- Default handling for config-like structs
- **Cognitive Complexity**: Identify high-complexity functions (e.g. > 30) as refactoring candidates. Suggest abstraction patterns but defer to human judgment for architectural trade-offs.
- Subtle test gaps around playback state, async teardown, and parser or CLI edges

## Fast Paths

- Docs or CMake-only changes: review commands, paths, and broken examples; skip the C++ standards pass.
- Large mechanical diffs: verify the pattern quickly, then focus on the non-mechanical edits.

## Reporting

- Lead with findings only, highest risk first.
- Each finding needs file and line, impact, and the smallest fix.
- Skip speculative nits and pure style/convention issues.
- If a tooling warning indicates a bug, performance issue, security risk, concurrency problem, or lifetime issue, review it here; otherwise route it to `check-code-conformance`.
- If no findings exist, say so explicitly and note any checks you skipped.
