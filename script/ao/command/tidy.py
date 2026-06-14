"""ao tidy — project clang-tidy with the Aobus plugin, strict/relaxed modes, and dedup.

Each file is classified as STRICT (lib/app/include/tool) or RELAXED (test/). Diagnostics
are de-duplicated across translation units before being printed or written to a file.
"""

import argparse
import contextlib
import re
import shutil
import subprocess
import sys
from pathlib import Path

from ..core import builddir, gitfiles, pythoncheck, tidyengine
from ..core.dedup import deduplicate
from ..core.paths import PROJECT_ROOT
from ..core.proc import die
from ..core.tidyconfig import CONFIG_BASE

HELP = "Run C++ clang-tidy and Python Ruff/mypy checks"

EPILOG = """\
scope: with no arguments, checks files changed against local main + working tree +
staged + untracked. Explicit files, --folder, --commit, or --all override that.

examples:
  ./ao tidy                                  # changed files
  ./ao tidy --all                            # whole repository
  ./ao tidy --folder lib --folder script     # selected folders
  ./ao tidy --commit HEAD~3 -o report.txt    # changes since a commit, to a file
  ./ao tidy --check readability-implicit-bool-conversion
  ./ao tidy --fix lib/audio/Foo.cpp          # apply exported fixes (use with caution)
"""

ALL_FOLDERS = ["lib", "app", "include", "script", "test", "tool"]

# Check groups: start from nothing, enable curated groups, then disable known false
# positives or project-preference conflicts.
STRICT_CHECKS = ",".join(
    [
        "-*",
        # === enabled check groups ===
        "aobus-*",  # Aobus custom checks (naming, spacing, etc.)
        "bugprone-*",  # bug-prone pattern detection
        "performance-*",  # performance issues
        "cppcoreguidelines-*",  # C++ Core Guidelines
        "misc-*",  # miscellaneous useful checks
        "modernize-*",  # modern C++ usage
        "readability-*",  # readability improvements
        "portability-*",  # portability concerns
        # === disabled: false positives or project preference ===
        "-bugprone-easily-swappable-parameters",  # frequent false positives
        "-cppcoreguidelines-avoid-magic-numbers",  # handled by aobus readability check
        "-cppcoreguidelines-avoid-const-or-ref-data-members",  # common pattern for views
        "-cppcoreguidelines-pro-bounds-pointer-arithmetic",  # common in layout/audio code
        "-misc-no-recursion",  # recursion is idiomatic in some modules
        "-misc-non-private-member-variables-in-classes",  # impl structs use this pattern
        "-modernize-use-trailing-return-type",  # style choice: prefer leading type
        "-modernize-use-nodiscard",  # not enforced globally
        "-modernize-use-auto",  # handled by aobus custom check
        "-performance-unnecessary-value-param",  # too noisy for interface types
        "-performance-move-const-arg",  # conflicts with const-correctness
        "-readability-redundant-member-init",  # explicit init is intentional
        "-readability-convert-member-functions-to-static",  # prefers explicit design intent
        "-portability-avoid-pragma-once",  # project standard is #pragma once
        "-clang-diagnostic-*",  # compiler diagnostics not our domain
    ]
)

RELAXED_CHECKS = ",".join(
    [
        STRICT_CHECKS,
        # === additionally disabled for test code ===
        "-bugprone-unchecked-optional-access",  # test code uses .value() liberally
        "-bugprone-unused-return-value",  # tests often discard return values
        "-cppcoreguidelines-avoid-c-arrays",  # Catch2 API and test data arrays
        "-cppcoreguidelines-avoid-magic-numbers",  # not enforced in tests
        "-cppcoreguidelines-pro-type-reinterpret-cast",  # common pattern in test mocks
        "-cppcoreguidelines-pro-type-vararg",  # debug helpers use printf-style
        "-portability-template-virtual-member-function",  # test fixtures use vfunc_* pattern
        "-readability-function-cognitive-complexity",  # test bodies are inherently complex
        "-readability-identifier-length",  # test locals (id, v, it) are fine
        "-readability-magic-numbers",  # not enforced in tests
    ]
)

STRICT_HEADER_FILTER = f"{PROJECT_ROOT}/(lib|app|include|tool)/.*"
RELAXED_HEADER_FILTER = f"{PROJECT_ROOT}/(test|include)/.*"


def mode_disabled_checks(mode: str) -> list[str]:
    """The explicit disable directives a mode applies, e.g. ['-bugprone-...', ...]."""
    source = RELAXED_CHECKS if mode == "RELAXED" else STRICT_CHECKS
    return [token for token in source.split(",") if token.startswith("-") and token != "-*"]


def checks_for(mode: str, selected: str | None) -> str:
    """Resolve the clang-tidy check list for a mode.

    With --check, run only the selected check(s) but still honour the mode's disable
    list, so a check disabled for tests (e.g. readability-magic-numbers) stays off there.
    """
    if selected:
        return ",".join(["-*", selected, *mode_disabled_checks(mode)])
    return RELAXED_CHECKS if mode == "RELAXED" else STRICT_CHECKS


FIX_REPLACEMENT_RE = re.compile(r"^\s+- FilePath:")


def register(subparsers: "argparse._SubParsersAction[argparse.ArgumentParser]") -> None:
    parser = subparsers.add_parser(
        "tidy", help=HELP, description=HELP, epilog=EPILOG, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    tidyengine.add_scope_arguments(parser, verb="check")
    parser.add_argument("--fix", action="store_true", help="apply exported fixes automatically (use with caution)")
    parser.add_argument("-n", "--no-build", action="store_true", help="use an existing plugin and compile database")
    parser.add_argument(
        "--tidy-arg",
        action="append",
        default=[],
        metavar="<arg>",
        help="extra argument passed to clang-tidy verbatim (repeatable)",
    )
    parser.add_argument("--header-filter", metavar="<regex>", help="override the per-mode header filter")
    parser.set_defaults(func=run_command)


def classify(path: Path, explicit: bool) -> str:
    """STRICT for production code, RELAXED for tests, IGNORE for lint fixtures and main."""
    try:
        rel = path.resolve().relative_to(PROJECT_ROOT.resolve()).as_posix()
    except ValueError:
        rel = path.resolve().as_posix()

    rel_slash = "/" + rel

    if "/test/integration/lint/fixture/" in rel_slash:
        # Fixture files: check when explicitly requested, skip in batch scans.
        return "STRICT" if explicit else "IGNORE"
    if "/test/integration/lint/" in rel_slash:
        return "IGNORE"
    if rel.endswith("/test/main.cpp") or rel == "test/main.cpp":
        return "IGNORE"
    if "/tool/lint/" in rel_slash:
        return "STRICT"
    if "/test/" in rel_slash:
        return "RELAXED"
    return "STRICT"


def prepare_plugin(build_dir: Path, *, no_build: bool) -> Path:
    plugin = build_dir / "tool" / "lint" / "libAobusLintPlugin.so"

    if no_build:
        if not (build_dir / "compile_commands.json").is_file():
            raise die(f"compile_commands.json not found in {build_dir}")
        if not plugin.is_file():
            raise die(f"AobusLintPlugin not found at {plugin}")
        return plugin

    tidyengine.ensure_compile_db(build_dir, ["-DAOBUS_ENABLE_CLANG_TIDY=ON"])
    print("Building AobusLintPlugin (incremental)...")
    plugin_build = subprocess.run(
        ["cmake", "--build", str(build_dir), "--target", "AobusLintPlugin", "--parallel"],
        cwd=PROJECT_ROOT,
        stdout=subprocess.DEVNULL,
    )
    if plugin_build.returncode != 0:
        raise die("failed to build AobusLintPlugin.")

    return plugin


def classify_existing(files: list[str], explicit: bool) -> dict[str, list[Path]]:
    """Resolve, dedupe, and bucket the candidate files."""
    buckets: dict[str, list[Path]] = {"STRICT": [], "RELAXED": []}
    seen: set[Path] = set()
    for name in files:
        path = Path(name)
        if not path.is_absolute():
            path = PROJECT_ROOT / path
        if not path.is_file():
            continue
        path = path.resolve()
        if path in seen:
            continue
        seen.add(path)
        mode = classify(path, explicit)
        if mode != "IGNORE":
            buckets[mode].append(path)
    return buckets


def split_existing(files: list[str]) -> tuple[list[str], list[str]]:
    cpp_files: list[str] = []
    python_files: list[str] = []
    seen: set[Path] = set()
    for name in files:
        path = Path(name)
        if not path.is_absolute():
            path = PROJECT_ROOT / path
        if not path.is_file():
            continue
        resolved = path.resolve()
        if resolved in seen:
            continue
        seen.add(resolved)
        try:
            rel = resolved.relative_to(PROJECT_ROOT.resolve()).as_posix()
        except ValueError:
            rel = resolved.as_posix()
        if resolved.suffix in gitfiles.CPP_SUFFIXES:
            cpp_files.append(rel)
        elif resolved.suffix in gitfiles.PYTHON_SUFFIXES:
            python_files.append(rel)
    return cpp_files, python_files


def filter_fixes_yaml(text: str) -> str:
    """Drop identifier-naming diagnostics from exported fixes; renames are never auto-applied."""
    output: list[str] = []
    skip = False
    for line in text.splitlines(keepends=True):
        if line.startswith("  - DiagnosticName:"):
            skip = "readability-identifier-naming" in line
            if not skip:
                output.append(line)
        elif line.startswith("    "):
            if not skip:
                output.append(line)
        else:
            skip = False
            output.append(line)
    return "".join(output)


def apply_fixes(tmpdir: Path) -> bool:
    yaml_files = sorted(tmpdir.glob("*.yaml"))
    for yaml_file in yaml_files:
        yaml_file.write_text(filter_fixes_yaml(yaml_file.read_text(encoding="utf-8")), encoding="utf-8")

    replacements = sum(
        1
        for yaml_file in yaml_files
        for line in yaml_file.read_text(encoding="utf-8").splitlines()
        if FIX_REPLACEMENT_RE.match(line)
    )
    if replacements == 0:
        if not yaml_files:
            print("  No automatic fixes were exported.")
        else:
            print(
                f"  No automatic replacements were exported ({len(yaml_files)} diagnostic fix file(s) "
                "contained no replacements)."
            )
        return True

    print(f"  Applying {replacements} replacement(s) from {len(yaml_files)} fix file(s) safely...")
    try:
        status = subprocess.call(["clang-apply-replacements", "--ignore-insert-conflict", str(tmpdir)])
    except FileNotFoundError:
        print("ERROR: clang-apply-replacements not found in PATH.", file=sys.stderr)
        return False
    if status != 0:
        print("ERROR: clang-apply-replacements failed.", file=sys.stderr)
        return False
    return True


def run_command(args: argparse.Namespace) -> int:
    build_dir = Path(args.path) if args.path else builddir.TIDY_DIR
    files, explicit = tidyengine.resolve_scope(args, ALL_FOLDERS, "Checking", suffixes=gitfiles.SOURCE_SUFFIXES)
    cpp_files, python_files = split_existing(files)

    out = open(args.output, "w", encoding="utf-8") if args.output else sys.stdout
    with contextlib.ExitStack() as stack:
        if args.output:
            stack.callback(out.close)

        overall_failed = False

        if python_files:
            if args.check:
                print("Skipping Python checks because --check selects a clang-tidy check.", file=sys.stderr)
            elif args.fix:
                print("Skipping Python checks because --fix only applies clang-tidy fixes.", file=sys.stderr)
            else:
                print(f"=== Python hygiene — {len(python_files)} file(s) ===")
                if pythoncheck.run_paths(python_files, sink=out if args.output else None) != 0:
                    overall_failed = True

        if not cpp_files:
            if not python_files:
                print("No .cpp/.h/.hpp/.py files found.", file=sys.stderr)
            return 1 if overall_failed else 0

        plugin = prepare_plugin(build_dir, no_build=args.no_build)

        isystem = tidyengine.system_include_args()
        if args.debug:
            print(f"DEBUG ISYSTEM_ARGS: {isystem}", file=sys.stderr)

        buckets = classify_existing(cpp_files, explicit)
        if not buckets["STRICT"] and not buckets["RELAXED"]:
            return 1 if overall_failed else 0

        def run_one(mode: str, file: str, log: Path) -> int:
            checks = checks_for(mode, args.check)
            header_filter = args.header_filter or (RELAXED_HEADER_FILTER if mode == "RELAXED" else STRICT_HEADER_FILTER)
            extra: list[str] = list(isystem)
            if "linux-gtk/" in file:
                # GTK framework patterns: gtkmm widget trees own children, printf-style APIs.
                checks += ",-cppcoreguidelines-owning-memory"
                extra.append("--extra-arg=-Wno-format-nonliteral")
            if plugin.is_file():
                extra.append(f"-load={plugin}")
            if args.fix:
                extra.append(f"-export-fixes={log.with_suffix('.yaml')}")
            config = " ".join(CONFIG_BASE.replace("PLACEHOLDER", checks).split())
            command = [
                "clang-tidy",
                "-p",
                str(build_dir),
                *args.tidy_arg,
                f"-config={config}",
                f"-header-filter={header_filter}",
                *extra,
                file,
            ]
            with open(log, "w", encoding="utf-8") as sink:
                if args.debug:
                    sink.write(f"DEBUG CONFIG: {config}\n")
                return subprocess.call(command, cwd=PROJECT_ROOT, stdout=sink, stderr=subprocess.STDOUT)

        for mode in ("STRICT", "RELAXED"):
            batch = [str(path) for path in buckets[mode]]
            if not batch:
                continue
            print(f"=== clang-tidy [{mode}] — {len(batch)} file(s) ===")
            tmpdir = tidyengine.make_tmpdir("tidy-")

            def run_file(file: str, log: Path, current_mode: str = mode) -> int:
                return run_one(current_mode, file, log)

            try:
                result = tidyengine.run_parallel(batch, args.jobs, tmpdir, run_file)
                if result.failed:
                    overall_failed = True
                    print("=== Failed logs ===", file=sys.stderr)
                    for log in result.failed_logs:
                        print(f"--- Log: {log} ---", file=sys.stderr)
                        sys.stderr.write(log.read_text(encoding="utf-8", errors="replace"))

                noisy = tidyengine.logs_with_diagnostics(result.logs)
                if noisy:
                    deduplicate(noisy, out, PROJECT_ROOT)
                    out.flush()
                    if args.output:
                        print(f"  Output appended to {args.output}")
                else:
                    print("  No warnings found.")

                if args.fix:
                    if result.failed:
                        print("  Skipping automatic fixes because clang-tidy failed for at least one file.")
                    elif not apply_fixes(tmpdir):
                        overall_failed = True
            finally:
                shutil.rmtree(tmpdir, ignore_errors=True)

        return 1 if overall_failed else 0
