# Aobus Agent Guide

Aobus is a C++26 music application: GTK4 (gtkmm) desktop frontend and a CLI tool over a shared core library. CMake build; dependencies come from `nix-shell` (needs `shell.nix` in CWD — always work from the project root).

> [!TIP]
> External library headers live in the Nix store; check build config for paths, or use `nix-shell --run "pkg-config --cflags <lib>"`. `nix-shell -p` for extra tools.

## Working Rules

1. **Language:** English for all code comments, commit messages, and docs.
2. **Search:** Use `rg`, prefer narrow scopes.
3. **Assumptions:** State technical assumptions in your response.
4. **No TACO:** Do not over-promise and under-deliver; no shortcuts when things get difficult.
5. **Design docs:** Sync `doc/design/` when user-facing behavior changes.
6. **Tests:** All changes include appropriate test coverage.
7. **Scratch files:** Throwaway artifacts go to `/tmp`, never into the repo.
8. **Hygiene:** Never run format/lint tools (`./ao format`/`tidy`, clang-format/-tidy, Ruff, mypy) mid-session — file rewrites disturb in-flight work; validate with builds and targeted tests instead. Commit gate: `./ao hygiene` (check-only, never edits files) right before staging; on failure run one `./ao format` pass, fix lint findings manually, re-run until clean. Explicit lint requests go through `./ao tidy`, never the raw tools.

> [!TIP]
> Heavy development, no compatibility/migration constraints — propose the best approach without historical baggage.

## Build and Validation

Everything goes through the `./ao` portal (Python package in `script/ao/`; re-enters nix-shell automatically). `./ao help` lists commands; `./ao <cmd> --help` has all options.

```bash
./ao check                    # full gate: build everything + all test suites (--clang/--asan/--tsan)
./ao build [release] [--clean] [--target <t>]    # incremental build, no tests
./ao test [--core|--gtk|--all|...] "[tag]"       # default: core+gtk; "[a],[b]"=OR, "[a][b]"=AND
./ao test --tooling           # ao tooling itself: Ruff + mypy + unit tests (test/script/)
./ao hygiene                  # check-only commit gate: format --check + tidy on changed files
./ao tidy [paths|--folder <d>|--all]             # C++ clang-tidy + Python Ruff/mypy (opt-in, rule 8)
./ao analyze                  # Clang Static Analyzer, report-only
./ao coverage "rt::Foo"       # gcov coverage for a test subset
./ao format                   # clang-format + ruff format (gate fixes / explicit request only)
```

Manual CMake, rarely needed: `nix-shell --run "cmake --preset linux-debug -B /tmp/build/debug && cmake --build /tmp/build/debug --parallel"`.

Preserve `/tmp/build/...` trees when chasing failures; `./ao build`/`check` tee all output to `$BUILD_DIR/build.log`.
