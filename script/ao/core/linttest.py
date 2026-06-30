"""Integration runner for Aobus clang-tidy checker fixtures."""

import concurrent.futures
import os
import re
import shutil
import subprocess
import sys
import tempfile
from collections import defaultdict
from dataclasses import dataclass
from pathlib import Path

from .paths import PROJECT_ROOT

FIXTURE_DIR = PROJECT_ROOT / "test" / "integration" / "lint" / "fixture"
DIAGNOSTIC_RE = re.compile(r"^([^:]+):(\d+):(\d+): warning: (.*) \[(.*)\]$")
POSITIVE_RE = re.compile(r"//\s*POSITIVE(?::\s*([a-zA-Z0-9_-]+))?")
NEGATIVE_RE = re.compile(r"//\s*NEGATIVE(?::\s*([a-zA-Z0-9_-]+))?")
FIX_TO_RE = re.compile(r"//\s*(?:POSITIVE:\s*)?FIX-TO:\s*(.*)")


@dataclass(frozen=True)
class Fixture:
    path: Path
    check: str


@dataclass(frozen=True)
class Expectations:
    expected: dict[int, set[str]]
    negated: dict[int, set[str]]


class Reporter:
    def __init__(self, log: Path | None) -> None:
        self._log = log

    def write(self, message: str = "") -> None:
        print(message)
        if self._log is not None:
            with self._log.open("a", encoding="utf-8") as sink:
                sink.write(f"{message}\n")

    def write_file(self, path: Path) -> None:
        text = path.read_text(encoding="utf-8", errors="replace")
        print(text, end="" if text.endswith("\n") else "\n")
        if self._log is not None:
            with self._log.open("a", encoding="utf-8") as sink:
                sink.write(text)
                if not text.endswith("\n"):
                    sink.write("\n")


def discover_fixtures(fixture_dir: Path = FIXTURE_DIR) -> list[Fixture]:
    paths = sorted(path for path in fixture_dir.glob("*/*") if path.is_file() and path.suffix in {".cpp", ".h"})
    return [Fixture(path, path.parent.name) for path in paths]


def parse_diagnostics(output: str) -> dict[int, set[str]]:
    diagnostics: dict[int, set[str]] = defaultdict(set)
    for line in output.splitlines():
        if match := DIAGNOSTIC_RE.match(line):
            diagnostics[int(match.group(2))].add(match.group(5))
    return dict(diagnostics)


def parse_expectations(source: str, check: str) -> Expectations:
    expected: dict[int, set[str]] = defaultdict(set)
    negated: dict[int, set[str]] = defaultdict(set)

    for line_number, line in enumerate(source.splitlines(), 1):
        positive = POSITIVE_RE.search(line)
        negative = NEGATIVE_RE.search(line)
        if positive is None and negative is None:
            continue

        code = line.split("//", 1)[0].strip()
        target_line = line_number if code else line_number + 1
        if positive is not None:
            expected[target_line].add(check if positive.group(1) in {None, "FIX-TO"} else positive.group(1))
        if negative is not None:
            negated[target_line].add(check if negative.group(1) in {None, "FIX-TO"} else negative.group(1))

    return Expectations(dict(expected), dict(negated))


def verify_diagnostics(source: str, output: str, check: str) -> list[str]:
    actual = parse_diagnostics(output)
    expectations = parse_expectations(source, check)
    errors: list[str] = []

    lines = sorted(set(actual) | set(expectations.expected) | set(expectations.negated))
    for line in lines:
        actual_checks = actual.get(line, set())
        expected_checks = expectations.expected.get(line, set())
        negated_checks = expectations.negated.get(line, set())

        for actual_check in sorted(actual_checks - expected_checks):
            if actual_check in negated_checks:
                errors.append(f"diagnostic found on explicitly NEGATIVE line {line}: [{actual_check}]")
            else:
                errors.append(f"unexpected diagnostic on line {line}: [{actual_check}]")
        for expected_check in sorted(expected_checks - actual_checks):
            if expected_check == check:
                errors.append(f"missing expected diagnostic on line {line}: [{expected_check}]")

    return errors


def expected_fixes(source: str) -> list[tuple[str, str]]:
    return [
        (match.group(0).strip(), match.group(1).replace("\\n", "\n"))
        for line in source.splitlines()
        if (match := FIX_TO_RE.search(line))
    ]


def verify_fixes(source: str, fixed: str) -> list[str]:
    fixed_lines = fixed.splitlines()
    errors: list[str] = []
    search_from = 0

    for comment, expected in expected_fixes(source):
        comment_index = next(
            (index for index in range(search_from, len(fixed_lines)) if comment in fixed_lines[index]),
            None,
        )
        if comment_index is None:
            errors.append(f"expected comment line not found in fixed file: {comment}")
            continue
        search_from = comment_index + 1

        expected_lines = expected.split("\n")
        if expected.startswith("\n"):
            if comment_index == 0 or fixed_lines[comment_index - 1].strip():
                errors.append(f"expected empty line before comment at line {comment_index + 1}")
            expected_lines = expected_lines[1:]

        inline = bool(fixed_lines[comment_index].split("//", 1)[0].strip())
        start = comment_index if inline else comment_index + 1
        for offset, expected_line in enumerate(expected_lines):
            index = start + offset
            if index >= len(fixed_lines):
                errors.append(f"expected line '{expected_line}' but reached end of file")
                break
            actual_line = fixed_lines[index]
            if inline and offset == 0:
                actual_line = actual_line.split("//", 1)[0]
            if actual_line.strip() != expected_line.strip():
                errors.append(f"expected '{expected_line.strip()}' at line {index + 1}, got '{actual_line.strip()}'")

    return errors


def _tidy_command(fixture: Fixture, build_dir: Path, *extra: str) -> list[str]:
    return [
        sys.executable,
        "-m",
        "ao",
        "tidy",
        "--check",
        fixture.check,
        "-p",
        str(build_dir),
        "--no-build",
        *extra,
        str(fixture.path),
    ]


def _fixture_tidy_args(include_dir: Path) -> tuple[str, ...]:
    return (
        "--tidy-arg=--extra-arg=-Wno-error",
        "--tidy-arg=--extra-arg=-Wno-unused-function",
        "--tidy-arg=--extra-arg=-Wno-unused-parameter",
        "--tidy-arg=--extra-arg=-Wno-unused-private-field",
        "--tidy-arg=--extra-arg=-Wno-unused-variable",
        "--tidy-arg=--extra-arg=-std=c++26",
        "--tidy-arg=--extra-arg=-x",
        "--tidy-arg=--extra-arg=c++",
        f"--tidy-arg=--extra-arg=-I{PROJECT_ROOT / 'include'}",
        f"--tidy-arg=--extra-arg=-I{PROJECT_ROOT / 'lib'}",
        f"--tidy-arg=--extra-arg=-I{FIXTURE_DIR}",
        f"--tidy-arg=--extra-arg=-I{include_dir}",
    )


def _run_diagnostic(fixture: Fixture, build_dir: Path, run_dir: Path) -> tuple[bool, Path]:
    case_dir = Path(tempfile.mkdtemp(prefix=f"{fixture.path.name}.diag.", dir=run_dir))
    log = case_dir / "run.log"
    source = fixture.path.read_text(encoding="utf-8")
    result = subprocess.run(
        _tidy_command(fixture, build_dir, *_fixture_tidy_args(fixture.path.parent)),
        cwd=PROJECT_ROOT,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
    )
    errors = verify_diagnostics(source, result.stdout, fixture.check)
    has_expected_diagnostics = bool(parse_expectations(source, fixture.check).expected)
    if "[clang-diagnostic-error]" in result.stdout:
        errors.append("clang-tidy reported a fatal compiler diagnostic")
    if result.returncode != 0 and (errors or not has_expected_diagnostics):
        errors.append(f"clang-tidy exited with status {result.returncode}")
    with log.open("w", encoding="utf-8") as sink:
        sink.write(result.stdout)
        for error in errors:
            sink.write(f"ERROR: {error}\n")
    return not errors, log


def _copy_fixture_context(fixture: Fixture, case_dir: Path) -> Path:
    fixed = case_dir / fixture.path.name
    shutil.copy2(fixture.path, fixed)
    shutil.copy2(FIXTURE_DIR / "TestHelpers.h", case_dir / "TestHelpers.h")
    for sibling in fixture.path.parent.glob("*.h"):
        destination = case_dir / sibling.name
        if destination != fixed:
            shutil.copy2(sibling, destination)
    return fixed


def _run_fix(fixture: Fixture, build_dir: Path, run_dir: Path) -> tuple[bool, Path]:
    case_dir = Path(tempfile.mkdtemp(prefix=f"{fixture.path.name}.fix.", dir=run_dir))
    log = case_dir / "run.log"
    fixed = _copy_fixture_context(fixture, case_dir)
    source = fixture.path.read_text(encoding="utf-8")
    tidy_args = (
        "--fix",
        *_fixture_tidy_args(case_dir),
    )
    tidy = subprocess.run(
        _tidy_command(Fixture(fixed, fixture.check), build_dir, *tidy_args),
        cwd=PROJECT_ROOT,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
    )
    errors: list[str] = []
    fixed_text = fixed.read_text(encoding="utf-8")
    fixes = expected_fixes(source)
    if fixes and fixed_text == source:
        errors.append("fixture declares FIX-TO expectations but clang-tidy made no changes")
    errors.extend(verify_fixes(source, fixed_text))

    syntax = subprocess.run(
        [
            "g++",
            "-std=c++26",
            "-fsyntax-only",
            f"-I{PROJECT_ROOT / 'include'}",
            f"-I{PROJECT_ROOT / 'lib'}",
            f"-I{case_dir}",
            str(fixed),
        ],
        cwd=PROJECT_ROOT,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
    )
    if syntax.returncode != 0:
        errors.append("auto-fixed fixture does not compile")

    with log.open("w", encoding="utf-8") as sink:
        sink.write(tidy.stdout)
        sink.write(syntax.stdout)
        for error in errors:
            sink.write(f"ERROR: {error}\n")
    return not errors, log


def _run_replacement_smoke(run_dir: Path) -> tuple[bool, Path]:
    case_dir = Path(tempfile.mkdtemp(prefix="apply-conflict.", dir=run_dir))
    source = case_dir / "sample.cpp"
    log = case_dir / "run.log"
    source.write_text("int main() {}\n", encoding="utf-8")

    for name, marker in (("a", "/*a*/"), ("b", "/*b*/")):
        (case_dir / f"{name}.yaml").write_text(
            "\n".join(
                [
                    "---",
                    f"MainSourceFile:  '{source}'",
                    "Diagnostics:",
                    f"  - DiagnosticName:  test-{name}",
                    "    DiagnosticMessage:",
                    f"      Message:         'insert {name}'",
                    f"      FilePath:        '{source}'",
                    "      FileOffset:      0",
                    "      Replacements:",
                    f"        - FilePath:        '{source}'",
                    "          Offset:          0",
                    "          Length:          0",
                    f"          ReplacementText: '{marker}'",
                    "    Level:           Warning",
                    f"    BuildDirectory:  '{case_dir}'",
                    "...",
                    "",
                ]
            ),
            encoding="utf-8",
        )

    result = subprocess.run(
        ["clang-apply-replacements", "--ignore-insert-conflict", str(case_dir)],
        cwd=PROJECT_ROOT,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
    )
    content = source.read_text(encoding="utf-8")
    errors = []
    if result.returncode != 0:
        errors.append("clang-apply-replacements failed")
    if "/*a*/" not in content or "/*b*/" not in content:
        errors.append("conflicting insertions were not both applied")
    with log.open("w", encoding="utf-8") as sink:
        sink.write(result.stdout)
        for error in errors:
            sink.write(f"ERROR: {error}\n")
    return not errors, log


def _run_compiler_error_smoke(build_dir: Path, run_dir: Path) -> tuple[bool, Path]:
    case_dir = Path(tempfile.mkdtemp(prefix="compiler-errors.", dir=run_dir))
    log = case_dir / "run.log"
    cases = (
        ("missing-include", '#include "DefinitelyMissingHeader.h"\n', False),
        ("syntax-error", "void brokenSyntax(;\n", False),
        ("unknown-type", "DefinitelyUnknownType value;\n", False),
        (
            "unused-code",
            "static void unusedFunction(int unusedParameter) { int unusedVariable; }\n",
            True,
        ),
    )
    errors: list[str] = []
    outputs: list[str] = []

    for name, source, should_succeed in cases:
        path = case_dir / f"{name}.cpp"
        path.write_text(source, encoding="utf-8")
        fixture = Fixture(path, "aobus-modernize-braced-initialization")
        result = subprocess.run(
            _tidy_command(fixture, build_dir, *_fixture_tidy_args(case_dir)),
            cwd=PROJECT_ROOT,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
        )
        outputs.append(f"=== {name} ===\n{result.stdout}")
        if should_succeed:
            if result.returncode != 0:
                errors.append(f"unused-code canary exited with status {result.returncode}")
        elif result.returncode == 0 or "[clang-diagnostic-error]" not in result.stdout:
            errors.append(f"{name} canary did not produce a fatal compiler diagnostic")

    with log.open("w", encoding="utf-8") as sink:
        sink.write("\n".join(outputs))
        for error in errors:
            sink.write(f"ERROR: {error}\n")
    return not errors, log


def _report_results(label: str, fixtures: list[Fixture], results: list[tuple[bool, Path]], reporter: Reporter) -> bool:
    passed = True
    for fixture, (success, log) in zip(fixtures, results, strict=True):
        reporter.write(f"  [{'PASS' if success else 'FAIL'}] {fixture.check}/{fixture.path.name}")
        passed = passed and success
        if not success:
            reporter.write_file(log)
    reporter.write(f"{label}: {'passed' if passed else 'failed'}")
    return passed


def run(build_dir: Path, *, log: Path | None = None, jobs: int | None = None) -> int:
    plugin = build_dir / "tool" / "lint" / "libAobusLintPlugin.so"
    compile_db = build_dir / "compile_commands.json"
    reporter = Reporter(log)
    if not compile_db.is_file():
        reporter.write(f"ERROR: compile_commands.json not found in {build_dir}")
        return 1
    if not plugin.is_file():
        reporter.write(f"ERROR: AobusLintPlugin not found at {plugin}")
        return 1

    fixtures = discover_fixtures()
    if not fixtures:
        reporter.write(f"ERROR: no lint fixtures found in {FIXTURE_DIR}")
        return 1
    run_dir = Path(tempfile.mkdtemp(prefix="aobus-lint-integration-", dir="/tmp"))
    failed = True
    try:
        reporter.write("=== Diagnostic Verification ===")
        worker_count = min(jobs or max((os.cpu_count() or 1) - 1, 1), len(fixtures))
        with concurrent.futures.ThreadPoolExecutor(max_workers=worker_count) as pool:
            diagnostic_results = list(pool.map(lambda fixture: _run_diagnostic(fixture, build_dir, run_dir), fixtures))
        diagnostics_passed = _report_results("Diagnostics", fixtures, diagnostic_results, reporter)

        reporter.write("=== Auto-Fix Verification ===")
        fix_fixtures = [fixture for fixture in fixtures if expected_fixes(fixture.path.read_text(encoding="utf-8"))]
        with concurrent.futures.ThreadPoolExecutor(max_workers=worker_count) as pool:
            fix_results = list(pool.map(lambda fixture: _run_fix(fixture, build_dir, run_dir), fix_fixtures))
        fixes_passed = _report_results("Auto-fixes", fix_fixtures, fix_results, reporter)

        reporter.write("=== Replacement Application Smoke ===")
        smoke_passed, smoke_log = _run_replacement_smoke(run_dir)
        reporter.write(f"  [{'PASS' if smoke_passed else 'FAIL'}] insert conflict smoke")
        if not smoke_passed:
            reporter.write_file(smoke_log)

        reporter.write("=== Compiler Error Detection Smoke ===")
        compiler_errors_passed, compiler_errors_log = _run_compiler_error_smoke(build_dir, run_dir)
        reporter.write(f"  [{'PASS' if compiler_errors_passed else 'FAIL'}] fatal compiler diagnostics")
        if not compiler_errors_passed:
            reporter.write_file(compiler_errors_log)

        failed = not (diagnostics_passed and fixes_passed and smoke_passed and compiler_errors_passed)
        reporter.write(
            "=== LINT INTEGRATION TESTS PASSED ===" if not failed else "=== LINT INTEGRATION TESTS FAILED ==="
        )
        if failed:
            reporter.write(f"Failure artifacts preserved at {run_dir}")
        return int(failed)
    finally:
        if not failed:
            shutil.rmtree(run_dir, ignore_errors=True)
