"""ao analyze — Clang Static Analyzer via clang-tidy, report-only by default."""

import argparse
import contextlib
import re
import shutil
import subprocess
import sys
from pathlib import Path

from ..core import builddir, tidyengine
from ..core.dedup import deduplicate
from ..core.paths import PROJECT_ROOT, absolute_path
from ..core.proc import die

HELP = "Run the Clang Static Analyzer (report-only unless --fail-on-diagnostics)"
NAME = "analyze"
# True when ao.bat must initialize the MSVC/vcpkg build environment first.
REQUIRES_BUILD_ENV = True


EPILOG = """\
Analyzer diagnostics are report-only by default; tool failures still return non-zero.

examples:
  ./ao analyze                               # changed files
  ./ao analyze --folder lib --folder app     # production code
  ./ao analyze --all -o /tmp/analyzer.txt    # whole repo, report to a file
  ./ao analyze --all --fail-on-diagnostics   # gate once the baseline is clean
  ./ao analyze --alpha --folder lib          # include experimental checks
"""

ALL_FOLDERS = ["lib", "app", "include", "test", "tool/lint"]

ANALYZER_CHECK_GROUPS = [
    "clang-analyzer-core.*",
    "clang-analyzer-cplusplus.*",
    "clang-analyzer-deadcode.*",
    "clang-analyzer-nullability.*",
    "clang-analyzer-optin.cplusplus.*",
    "clang-analyzer-optin.performance.*",
    "clang-analyzer-optin.portability.*",
    "clang-analyzer-security.*",
    "clang-analyzer-unix.*",
    "clang-analyzer-valist.*",
]

_PATH_SEPARATOR_RE = r"[/\\]"


def project_header_filter() -> str:
    """Return an analyzer header filter that accepts native and POSIX separators."""
    root = re.escape(absolute_path(PROJECT_ROOT).as_posix().rstrip("/")).replace("/", _PATH_SEPARATOR_RE)
    return f"{root}{_PATH_SEPARATOR_RE}(lib|app|include|test|tool{_PATH_SEPARATOR_RE}lint){_PATH_SEPARATOR_RE}.*"


HEADER_FILTER = project_header_filter()


def register(subparsers: "argparse._SubParsersAction[argparse.ArgumentParser]") -> None:
    parser = subparsers.add_parser(
        NAME, help=HELP, description=HELP, epilog=EPILOG, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    tidyengine.add_scope_arguments(parser, verb="analyze")
    parser.add_argument("--fail-on-diagnostics", action="store_true", help="return non-zero when diagnostics are found")
    parser.add_argument(
        "--include-external-diagnostics",
        action="store_true",
        help="include diagnostics whose primary location is outside the repo",
    )
    parser.add_argument("--alpha", action="store_true", help="also enable clang-analyzer-alpha.* checks")
    parser.add_argument(
        "--tidy-arg",
        action="append",
        default=[],
        metavar="<arg>",
        help="extra argument passed to clang-tidy verbatim (repeatable)",
    )
    parser.set_defaults(func=run_command)


def classify(path: Path) -> bool:
    """True when the file should be analyzed (lint fixtures and the Catch2 main are skipped)."""
    try:
        rel = absolute_path(path).relative_to(absolute_path(PROJECT_ROOT)).as_posix()
    except ValueError:
        rel = absolute_path(path).as_posix()

    rel_slash = "/" + rel
    if "/test/integration/lint/" in rel_slash:
        return False
    if rel.endswith("/test/main.cpp") or rel == "test/main.cpp":
        return False
    return True


def analyzer_checks(alpha: bool, only: str | None) -> str:
    if only:
        return f"-*,{only}"
    groups = list(ANALYZER_CHECK_GROUPS)
    if alpha:
        groups.append("clang-analyzer-alpha.*")
    return ",".join(["-*", *groups])


def run_command(args: argparse.Namespace) -> int:
    if args.jobs < 1:
        raise die("-j must be at least 1.")
    build_dir = Path(args.path) if args.path else builddir.analyze_dir()

    out = open(args.output, "w", encoding="utf-8") if args.output else sys.stdout
    with contextlib.ExitStack() as stack:
        if args.output:
            stack.callback(out.close)

        tidyengine.ensure_compile_db(build_dir)
        isystem = tidyengine.system_include_args()
        if args.debug:
            print(f"DEBUG ISYSTEM_ARGS: {isystem}", file=sys.stderr)

        candidates, _ = tidyengine.resolve_scope(args, ALL_FOLDERS, "Analyzing")
        files: list[Path] = []
        seen: set[Path] = set()
        for name in candidates:
            path = Path(name)
            if not path.is_absolute():
                path = PROJECT_ROOT / path
            if not path.is_file():
                continue
            path = absolute_path(path)
            if path not in seen and classify(path):
                seen.add(path)
                files.append(path)
        if not files:
            print("No analyzable .cpp/.h/.hpp files found.", file=sys.stderr)
            return 0

        checks = analyzer_checks(args.alpha, args.check)

        def run_one(file: str, log: Path) -> int:
            file_checks = checks
            rel = file[len(f"{PROJECT_ROOT}/") :] if file.startswith(f"{PROJECT_ROOT}/") else file
            if not args.check and rel.startswith("test/"):
                # Catch2 fixtures intentionally leak through REQUIRE-terminated paths.
                file_checks += ",-clang-analyzer-cplusplus.NewDeleteLeaks"
            extra: list[str] = list(isystem)
            if "linux-gtk/" in rel:
                extra.append("--extra-arg=-Wno-format-nonliteral")
            config = f"{{Checks: '{file_checks}'}}"
            command = [
                "clang-tidy",
                "-p",
                str(build_dir),
                *args.tidy_arg,
                f"-config={config}",
                f"-header-filter={HEADER_FILTER}",
                *extra,
                file,
            ]
            with open(log, "w", encoding="utf-8") as sink:
                if args.debug:
                    sink.write(f"DEBUG CONFIG: {config}\n")
                return subprocess.call(command, cwd=PROJECT_ROOT, stdout=sink, stderr=subprocess.STDOUT)

        print(f"=== clang-analyzer — {len(files)} file(s) ===")
        tmpdir = tidyengine.make_tmpdir("analyzer-")
        try:
            result = tidyengine.run_parallel([str(path) for path in files], args.jobs, tmpdir, run_one)

            diagnostic_count = 0
            noisy = tidyengine.logs_with_diagnostics(result.logs)
            if noisy:
                diagnostic_count = deduplicate(
                    noisy, out, PROJECT_ROOT, include_external=args.include_external_diagnostics
                )
                out.flush()
                if args.output:
                    print(f"Output written to {args.output}")
            if diagnostic_count == 0:
                print("No analyzer diagnostics found.")

            if result.failed:
                print("Analyzer tool failure output:", file=sys.stderr)
                for log in result.logs:
                    text = log.read_text(encoding="utf-8", errors="replace") if log.is_file() else ""
                    if text:
                        print(f"=== {log.name} ===", file=sys.stderr)
                        sys.stderr.write("".join(text.splitlines(keepends=True)[:120]))
        finally:
            shutil.rmtree(tmpdir, ignore_errors=True)

        if diagnostic_count > 0:
            print(f"Analyzer diagnostics: {diagnostic_count}")
            if args.fail_on_diagnostics:
                return 1
        return 1 if result.failed else 0
