# Aobus Agent Guide

Aobus is a C++26 music application: a GTK4 (gtkmm) desktop frontend and a CLI
tool over a shared core library. CMake builds the project. Dependencies come
from `nix-shell`, which needs `shell.nix` in the current directory, so always
work from the project root.

> [!TIP]
> External library headers live in the Nix store; check build config for paths, or use `nix-shell --run "pkg-config --cflags <lib>"`. `nix-shell -p` for extra tools.

## Human References

Read the human docs for project policy instead of duplicating them here:

- `CONTRIBUTING.md` for contributor rules and review references.
- `doc/README.md` for where documentation belongs.
- `doc/dev/coding-style.md` for C++ style.
- `doc/dev/naming-conventions.md` for identifier, type, file, and helper naming.
- `doc/dev/testing.md` for testing policy.
- `doc/dev/linting.md` for lint policy (warning fix/suppress rules, NOLINT playbook).
- `doc/dev/commit-messages.md` for commit message rules.

## Working Rules

1. **Language:** English for all code comments, commit messages, and docs.
2. **Search:** Use `rg`, prefer narrow scopes.
3. **Assumptions:** State technical assumptions in your response.
4. **No TACO:** Do not over-promise and under-deliver; no shortcuts when things get difficult.
5. **Docs:** When behavior or architecture changes, use `doc/README.md` to
   decide whether to update `doc/design/`, `doc/dev/`, or another doc area.
6. **Tests:** All changes include appropriate test coverage.
7. **Scratch files:** Agent throwaway artifacts go to `/tmp`, never into the repo.
8. **Hygiene:** Do not run format or lint tools mid-session unless the user
   explicitly asks for linting.
9. **Validation:** Follow `doc/dev/testing/validation-and-review.md`; completed
   work normally uses one full `./ao check`.
10. **Concurrency:** Follow `doc/dev/testing/concurrency-and-sanitizers.md` for
    concurrency-sensitive changes.

> [!TIP]
> Heavy development, no compatibility/migration constraints. Propose the best approach without historical baggage.

## Agent Delegation

`aobus-council` (registry `config/agent-council.yaml`) provides the only agent workflow:
multi-model advisory council review. The workflow lives in `.agents/skills/run-council`;
council results are dossiers for chair review, never automatic edits to the real tree.

## Build and Validation

Everything goes through the `./ao` portal (Python package in `script/ao/`; re-enters nix-shell automatically). `./ao help` lists commands; `./ao <cmd> --help` has all options.

```bash
./ao check                    # full gate: build everything + all test suites (--clang/--asan/--tsan)
./ao build [release] [--clean] [--target <t>]    # incremental build, no tests
./ao test [--core|--gtk|--all|...] "[tag]"       # default: core+gtk; "[a],[b]"=OR, "[a][b]"=AND
./ao test --tooling           # ao tooling itself: Ruff + mypy + unit tests (test/script/)
./ao test --concurrency       # all native Catch2 [concurrency] contracts
./ao hygiene                  # check-only commit gate: format --check + tidy on changed files
./ao tidy [paths|--folder <d>|--all]             # C++ clang-tidy + Python Ruff/mypy (opt-in, rule 8)
./ao analyze                  # Clang Static Analyzer, report-only
./ao coverage "rt::Foo"       # gcov coverage for a test subset
./ao format                   # clang-format + ruff format (gate fixes / explicit request only)
```

Manual CMake, rarely needed: `nix-shell --run "cmake --preset linux-debug -B /tmp/build/debug && cmake --build /tmp/build/debug --parallel"`.

Preserve `/tmp/build/...` trees when chasing failures; `./ao build`/`check` tee all output to `$BUILD_DIR/build.log`.
