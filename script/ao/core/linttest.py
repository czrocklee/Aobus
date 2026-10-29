"""Integration runner for Aobus clang-tidy checker fixtures."""

import concurrent.futures
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
from collections import defaultdict
from collections.abc import Iterable
from dataclasses import dataclass, field
from pathlib import Path

from . import builddir, phases, tidyconfig, tidyengine
from .paths import PROJECT_ROOT
from .proc import die

FIXTURE_DIR = PROJECT_ROOT / "test" / "lint" / "fixture"
DIAGNOSTIC_RE = re.compile(r"^(.+):(\d+):(\d+): warning: (.*) \[(.*)\]$")
# LLVM diagnostic headers start their line: a location, a clang driver
# banner, or a bare severity, then the severity and its message. The console
# renderer also echoes source and fix-it lines behind an "<indent>N | " gutter,
# and a header's message can quote other headers -- both may contain arbitrary
# text -- so classification skips gutter lines and reads the header that
# STARTS each line, at any severity. The parse is staged to bound message
# reinterpretation to the known header forms: a bare severity at the start
# (followed by ':' or a space, as in the MSVC "warning C4996:" code form)
# stops the parse, a driver banner is read before any location, and of the
# two location grammars the header whose severity starts earliest wins, so
# a message quoting the other location form cannot win. A line whose driver
# banner is followed by a non-severity still falls back to a location read.
_LLVM_GUTTER_RE = re.compile(r"^\s*(?:\d+\s*)?\|")
# A bare severity starting the line: a clang console/driver header such as
# "error: invalid argument" or "fatal error: 'Missing.h' file not found". A
# severity word followed by filename continuation characters (a word
# character, '-', '.', '/', or a path separator) starts a location header's
# filename instead, such as "warning.cpp:1:2:".
_BARE_SEVERITY_RE = re.compile(r"^\s*(?P<severity>fatal error|error|warning|note|remark)(?![\w./\\-])")
# A clang driver banner followed by its severity, read before any location.
_DRIVER_BANNER_SEVERITY_RE = re.compile(
    r"^cla[n]g(?:\+\+|-cl|-tidy)?(?:\.exe)?:\s*(?P<severity>fatal error|error|warning|note|remark)(?![\w-])"
)
# The two location grammars, each followed by its severity. Each grammar can
# match a location quoted later in a message, so the caller reads the header
# whose severity starts earliest.
_CLANG_LOCATION_SEVERITY_RE = re.compile(
    r"^.+?:\d+:\d+:\s*(?P<severity>fatal error|error|warning|note|remark)(?![\w-])"
)
_MSVC_LOCATION_SEVERITY_RE = re.compile(
    r"^.+?\(\d+(?:,\d+)?\):\s*(?P<severity>fatal error|error|warning|note|remark)(?![\w-])"
)


@dataclass(frozen=True)
class DiagnosticHeader:
    """The first diagnostic header starting one line of captured tool output."""

    severity: str
    text: str

    @property
    def is_error(self) -> bool:
        """Errors and fatal errors abort the checked translation unit."""
        return self.severity in ("error", "fatal error")


def parse_diagnostic_header(line: str) -> DiagnosticHeader | None:
    """Return the first diagnostic header that starts *line*, or None.

    Echoed source and fix-it lines (an ``<indent>N |`` gutter) and message text
    quoting other headers never classify: only a header that starts the line is
    read, at any severity, so a warning whose message quotes ``fake.cpp:1:2:
    error:`` stays a warning and an error whose message quotes ``fake.cpp:1:2:
    warning:`` stays an error.
    """
    if _LLVM_GUTTER_RE.match(line):
        return None
    match = _BARE_SEVERITY_RE.match(line)
    if match is None:
        match = _DRIVER_BANNER_SEVERITY_RE.match(line)
    if match is None:
        located = [
            candidate
            for candidate in (
                _CLANG_LOCATION_SEVERITY_RE.match(line),
                _MSVC_LOCATION_SEVERITY_RE.match(line),
            )
            if candidate is not None
        ]
        match = min(located, key=lambda candidate: candidate.start("severity")) if located else None
    if match is None:
        return None
    return DiagnosticHeader(severity=match.group("severity"), text=line)


def has_fatal_compiler_error(output: str, *, tag: str | None = None) -> bool:
    """Return whether captured output carries a real error-severity header.

    With *tag*, the error header's line must also carry that marker, anchoring
    bracketed check names to a genuine header instead of a whole-output
    substring that an echoed comment could satisfy.
    """
    return any(
        (header := parse_diagnostic_header(line)) is not None
        and header.is_error
        and (tag is None or tag in header.text)
        for line in output.splitlines()
    )


POSITIVE_RE = re.compile(r"//\s*POSITIVE(?::\s*([a-zA-Z0-9_-]+))?")
NEGATIVE_RE = re.compile(r"//\s*NEGATIVE(?::\s*([a-zA-Z0-9_-]+))?")
LOCAL_INCLUDE_RE = re.compile(r'^\s*#\s*(?:include|import)\s*"([^"]+)"', re.MULTILINE)


@dataclass(frozen=True)
class Fixture:
    path: Path
    check: str


@dataclass(frozen=True)
class Expectations:
    expected: dict[int, set[str]]
    negated: dict[int, set[str]]
    messages: dict[tuple[int, str], str] = field(default_factory=dict)


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
    paths = sorted(path for path in fixture_dir.glob("*/*") if path.is_file() and path.suffix in {".cpp", ".h", ".mm"})
    return [Fixture(path, path.parent.name) for path in paths]


def _recorded_diagnostics(
    output: str,
) -> tuple[dict[tuple[Path, int], set[str]], dict[tuple[Path, int, str], list[str]]]:
    """Index warning checks and messages by resolved file, line, and alias."""
    checks: dict[tuple[Path, int], set[str]] = defaultdict(set)
    messages: dict[tuple[Path, int, str], list[str]] = defaultdict(list)
    for line in output.splitlines():
        match = DIAGNOSTIC_RE.match(line)
        if match is None:
            continue
        path = (PROJECT_ROOT / match.group(1)).resolve()
        line_number = int(match.group(2))
        checks[path, line_number].add(match.group(5))
        messages[path, line_number, match.group(5)].append(match.group(4))
    return dict(checks), dict(messages)


def parse_diagnostics(output: str) -> dict[tuple[Path, int], set[str]]:
    checks, _messages = _recorded_diagnostics(output)
    return checks


def _marker_location(line_number: int, path: Path | None) -> str:
    return f"{path}:{line_number}" if path is not None else f"line {line_number}"


def _positive_message(line: str, positive: re.Match[str], line_number: int, path: Path | None) -> str | None:
    """Return required diagnostic text from an optional pipe suffix, if present."""
    suffix = line[positive.end() :]
    if re.match(r"\s*\|", suffix) is None:
        return None
    required = suffix.split("|", 1)[1].strip()
    if not required:
        raise ValueError(
            f"{_marker_location(line_number, path)}: empty explicit diagnostic text in POSITIVE marker; "
            "omit '|' or provide required text"
        )
    return required


def parse_expectations(source: str, check: str, *, path: Path | None = None) -> Expectations:
    expected: dict[int, set[str]] = defaultdict(set)
    negated: dict[int, set[str]] = defaultdict(set)
    messages: dict[tuple[int, str], str] = {}

    for line_number, line in enumerate(source.splitlines(), 1):
        # The marker regexes require "//", so required text that merely mentions
        # POSITIVE or NEGATIVE is not a second marker. A second real marker on
        # the same physical line is unsupported grammar.
        positives = list(POSITIVE_RE.finditer(line))
        negatives = list(NEGATIVE_RE.finditer(line))
        if not positives and not negatives:
            continue
        if len(positives) + len(negatives) > 1:
            raise ValueError(
                f"{_marker_location(line_number, path)}: unsupported multiple lint fixture markers on one line; "
                "use one POSITIVE or NEGATIVE marker"
            )

        positive = positives[0] if positives else None
        negative = negatives[0] if negatives else None
        code = line.split("//", 1)[0].strip()
        target_line = line_number if code else line_number + 1
        if positive is not None:
            alias = positive.group(1) or check
            if alias in negated.get(target_line, ()):
                raise ValueError(
                    f"{_marker_location(line_number, path)}: conflicting POSITIVE and NEGATIVE markers "
                    f"for {alias!r} on line {target_line}"
                )
            expected[target_line].add(alias)
            if (required := _positive_message(line, positive, line_number, path)) is not None:
                previous = messages.get((target_line, alias))
                if previous is not None and previous != required:
                    raise ValueError(
                        f"{_marker_location(line_number, path)}: conflicting POSITIVE diagnostic text "
                        f"for {alias!r} on line {target_line}"
                    )
                messages[target_line, alias] = required
        if negative is not None:
            alias = negative.group(1) or check
            if alias in expected.get(target_line, ()):
                raise ValueError(
                    f"{_marker_location(line_number, path)}: conflicting POSITIVE and NEGATIVE markers "
                    f"for {alias!r} on line {target_line}"
                )
            negated[target_line].add(alias)

    return Expectations(dict(expected), dict(negated), messages)


def verify_diagnostics(
    sources: dict[Path, str], output: str, check: str, *, required_checks: tuple[str, ...] = ()
) -> list[str]:
    checked = frozenset((check, *required_checks))
    actual, actual_messages = _recorded_diagnostics(output)
    expectations: dict[Path, Expectations] = {}
    for path, source in sources.items():
        resolved = path.resolve()
        expectations[resolved] = parse_expectations(source, check, path=resolved)
    errors: list[str] = []

    locations = set(actual)
    for path, expected in expectations.items():
        locations.update((path, line) for line in expected.expected | expected.negated)
    for path, line in sorted(locations):
        actual_checks = actual.get((path, line), set())
        expected = expectations.get(path, Expectations({}, {}))
        expected_checks = expected.expected.get(line, set())
        negated_checks = expected.negated.get(line, set())
        location = f"{path}:{line}"

        for actual_check in sorted(actual_checks - expected_checks):
            if actual_check in negated_checks:
                errors.append(f"diagnostic found on explicitly NEGATIVE line {location}: [{actual_check}]")
            else:
                errors.append(f"unexpected diagnostic on line {location}: [{actual_check}]")
        for expected_check in sorted(expected_checks - actual_checks):
            if expected_check in checked:
                errors.append(f"missing expected diagnostic on line {location}: [{expected_check}]")
        message_aliases = sorted(alias for message_line, alias in expected.messages if message_line == line)
        for alias in message_aliases:
            required = expected.messages[line, alias]
            texts = actual_messages.get((path, line, alias), [])
            if any(required in text for text in texts):
                continue
            if not texts:
                errors.append(f"missing expected diagnostic text on line {location}: [{alias}] required {required!r}")
            else:
                rendered = "; ".join(repr(text) for text in texts)
                errors.append(
                    f"diagnostic text mismatch on line {location}: [{alias}] required {required!r}; actual {rendered}"
                )

    return errors


@dataclass(frozen=True)
class WindowsFixtureTools:
    tidy: str
    flags: tuple[str, ...]


def _windows_fixture_tools(build_dir: Path, toolchain: tidyengine.TidyToolchain) -> WindowsFixtureTools:
    """Use only the caller-verified tidy host and the initialized VS standard includes.

    The toolchain context is prepared and verified once by the portal preflight;
    its SDK root is the single verified source for the fixture tidy host and
    the required-file check, so a stale build cache cannot name a different SDK.
    The complete required-file set and the VS include search stay local: the
    context record carries only what its actual consumers need.
    """
    sdk = toolchain.sdk_root
    if toolchain.expected_llvm_version is None or toolchain.resource_dir is None or sdk is None:
        raise die("Windows lint fixtures require a toolchain context prepared for native Windows.")
    _, _, required_files = tidyengine.windows_llvm_sdk_manifest()
    for relative in required_files:
        if not (sdk / relative).is_file():
            raise die(f"configured LLVM SDK file not found: {sdk / relative}")
    # The VS x64 environment and cl.exe tolerate empty segments, a trailing
    # semicolon, and absent directories: the compiler walks every nonempty
    # entry in order and simply finds nothing in a missing one. Mirror that
    # search instead of enforcing a stricter directory policy; only a wholly
    # empty INCLUDE is a missing environment.
    includes = [value for value in os.environ.get("INCLUDE", "").split(";") if value.strip()]
    if not includes:
        raise die("Windows lint fixtures require the INCLUDE directories from the Visual Studio x64 environment.")
    return WindowsFixtureTools(
        tidy=toolchain.clang_tidy,
        flags=(
            "--driver-mode=cl",
            "/nologo",
            "/TP",
            "/EHsc",
            "/Zc:__cplusplus",
            "/X",
            *tidyengine.windows_analysis_arguments(toolchain.resource_dir, driver_mode=True),
            f"/imsvc{toolchain.resource_dir / 'include'}",
            *(f"/imsvc{value}" for value in includes),
            f"/I{build_dir / 'generated'}",
        ),
    )


def _direct_tidy_command(
    host: str,
    source: str,
    config: str,
    *,
    flags: tuple[str, ...],
    tidy_args: tuple[str, ...] = (),
    plugin: Path | None = None,
) -> list[str]:
    """Compose a direct native host invocation without borrowing a product TU."""
    return [
        host,
        "--quiet",
        f"-config={config}",
        *([f"-load={plugin}"] if plugin is not None else []),
        *tidy_args,
        source,
        "--",
        *flags,
    ]


def _tidy_command(
    fixture: Fixture, build_dir: Path, *extra: str, tools: WindowsFixtureTools | None = None
) -> list[str]:
    if tools is not None:
        # A fixed compilation command avoids borrowing a product TU's forced
        # PCH, defines, or GCC flags. The portal runs explicit fixtures as
        # STRICT, so the direct host must apply the same effective check policy.
        config = tidyconfig.config_for(tidyconfig.checks_for("STRICT", fixture.check))
        return _direct_tidy_command(
            tools.tidy,
            str(fixture.path),
            config,
            flags=tools.flags,
            tidy_args=tuple(argument.removeprefix("--tidy-arg=") for argument in extra),
        )
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


def _fixture_tidy_args(include_dir: Path, language: str = "c++", *, windows: bool = False) -> tuple[str, ...]:
    if windows:
        return tuple(
            f"--tidy-arg=--extra-arg={argument}"
            for argument in (
                "/clang:-std=c++26",
                "/clang:-Wno-error",
                "/clang:-fdiagnostics-format=clang",
                "/W0",
                "/WX-",
                f"/I{PROJECT_ROOT / 'include'}",
                f"/I{PROJECT_ROOT / 'lib'}",
                f"/I{FIXTURE_DIR}",
                f"/I{include_dir}",
            )
        )
    args: tuple[str, ...] = (
        "--tidy-arg=--extra-arg=-Wno-error",
        "--tidy-arg=--extra-arg=-Wno-unused-function",
        "--tidy-arg=--extra-arg=-Wno-unused-parameter",
        "--tidy-arg=--extra-arg=-Wno-unused-private-field",
        "--tidy-arg=--extra-arg=-Wno-unused-variable",
        "--tidy-arg=--extra-arg=-std=c++26",
        "--tidy-arg=--extra-arg=-x",
        f"--tidy-arg=--extra-arg={language}",
        f"--tidy-arg=--extra-arg=-I{PROJECT_ROOT / 'include'}",
        f"--tidy-arg=--extra-arg=-I{PROJECT_ROOT / 'lib'}",
        f"--tidy-arg=--extra-arg=-I{FIXTURE_DIR}",
        f"--tidy-arg=--extra-arg=-I{include_dir}",
    )
    if language == "objective-c++":
        header_filter = re.escape(include_dir.as_posix()).replace("/", r"[/\\]") + r"[/\\]"
        args += ("--tidy-arg=--extra-arg=-fblocks", f"--header-filter={header_filter}")
    return args


def _fixture_sources(fixture: Fixture) -> dict[Path, str]:
    """Snapshot the main file and literal local Objective-C++ context headers."""
    root = fixture.path.parent.resolve()
    pending = [fixture.path.resolve()]
    sources: dict[Path, str] = {}
    while pending:
        path = pending.pop()
        if path in sources:
            continue
        source = path.read_text(encoding="utf-8")
        sources[path] = source
        if fixture.path.suffix != ".mm":
            continue
        for match in LOCAL_INCLUDE_RE.finditer(source):
            for directory in (path.parent, root):
                header = (directory / match.group(1)).resolve()
                if header.is_relative_to(root) and header.suffix == ".h" and header.is_file():
                    pending.append(header)
                    break
    return sources


def _preflight_expectation_errors(fixtures: list[Fixture]) -> list[str]:
    """Parse discovered fixtures, including Objective-C++ context headers, before any subprocess."""
    errors: list[str] = []
    for fixture in fixtures:
        for path, source in _fixture_sources(fixture).items():
            try:
                parse_expectations(source, fixture.check, path=path)
            except ValueError as error:
                errors.extend(str(error).splitlines())
    return errors


def _diagnostic_errors(
    fixture: Fixture,
    sources: dict[Path, str],
    result: subprocess.CompletedProcess[str],
    *,
    direct_host: bool,
    required_checks: tuple[str, ...] = (),
) -> list[str]:
    errors = verify_diagnostics(sources, result.stdout, fixture.check, required_checks=required_checks)
    has_expected_diagnostics = any(
        fixture.check in checks
        for source in sources.values()
        for checks in parse_expectations(source, fixture.check).expected.values()
    )
    if has_fatal_compiler_error(result.stdout):
        errors.append("clang-tidy reported a fatal compiler diagnostic")
    # The portal normalizes native tool failures to 1 too. Its failed-batch
    # marker distinguishes a crashed invocation from ordinary expected findings.
    if not direct_host and re.search(r"^=== Failed logs ===$", result.stdout, re.MULTILINE):
        errors.append("portal reported a failed clang-tidy invocation")
    # The portal returns 1 for findings; the direct host returns 0 for warnings.
    # Neither contract permits a crash or another failure after expected output.
    portal_findings = not direct_host and result.returncode == 1 and has_expected_diagnostics and not errors
    if result.returncode != 0 and not portal_findings:
        errors.append(f"clang-tidy exited with status {result.returncode}")
    return errors


def _run_diagnostic(
    fixture: Fixture, build_dir: Path, run_dir: Path, *, tools: WindowsFixtureTools | None = None
) -> tuple[bool, Path]:
    case_dir = Path(tempfile.mkdtemp(prefix=f"{fixture.path.name}.diag.", dir=run_dir))
    log = case_dir / "run.log"
    sources = _fixture_sources(fixture)
    result = subprocess.run(
        _tidy_command(
            fixture,
            build_dir,
            *_fixture_tidy_args(
                fixture.path.parent,
                "objective-c++" if fixture.path.suffix == ".mm" else "c++",
                windows=tools is not None,
            ),
            tools=tools,
        ),
        cwd=PROJECT_ROOT,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
    )
    errors = _diagnostic_errors(fixture, sources, result, direct_host=tools is not None)
    with log.open("w", encoding="utf-8") as sink:
        sink.write(result.stdout)
        for error in errors:
            sink.write(f"ERROR: {error}\n")
    return not errors, log


_SOURCE_POLICY_CHECKS = (
    "aobus-readability-forbid-raw-throw",
    "aobus-readability-forbid-raw-fatal",
    "aobus-portability-explicit-path-conversion",
)


def _source_policy_ascii_case_variant(path: Path) -> Path:
    # Vary the ASCII case contract without inventing a different Unicode name.
    return Path("".join(character.swapcase() if character.isascii() else character for character in str(path)))


def _source_policy_cases(root: Path, *, windows: bool = False) -> list[tuple[str, Path, tuple[Path, ...], bool]]:
    plain = root / "plain" / "checkout"
    unicode_root = root / "josé 测试" / "checkout"
    ancestor = root / "test" / "checkout"
    fixture_ancestor = root / "test" / "lint" / "fixture" / "checkout"
    spaced = root / "test" / "space;and checkout"
    alias = plain / "source"
    return [
        ("production", plain / "lib" / "Policy.cpp", (plain,), True),
        ("same-file-system", plain / "lib/Policy.cpp", (plain,), True),
        ("header-file-switch", plain / "lib/Policy.cpp", (plain,), True),
        ("header-file-switch-unicode", unicode_root / "lib/Policy.cpp", (unicode_root,), True),
        ("multi-tu-exempt-first", plain / "lib/Policy.cpp", (plain,), True),
        ("multi-tu-production-first", plain / "lib/Policy.cpp", (plain,), True),
        ("macro-spelling-exempt-definition", plain / "lib/Policy.cpp", (plain,), True),
        ("macro-spelling-production-definition", plain / "test/Policy.cpp", (plain,), False),
        ("ordinary-test", plain / "test" / "Policy.cpp", (plain,), False),
        ("fixture", plain / "test/lint/fixture/probe/Policy.cpp", (plain,), True),
        ("nested-test-component", plain / "app/nested/test/Policy.cpp", (plain,), True),
        ("test-ancestor-production", ancestor / "lib/Policy.cpp", (ancestor,), True),
        ("test-ancestor-test", ancestor / "test/Policy.cpp", (ancestor,), False),
        ("test-ancestor-fixture", ancestor / "test/lint/fixture/probe/Policy.cpp", (ancestor,), True),
        ("fixture-ancestor-test", fixture_ancestor / "test/Policy.cpp", (fixture_ancestor,), False),
        ("fixture-near-miss", plain / "test/lint/fixtures/Policy.cpp", (plain,), False),
        ("root-near-miss", plain.parent / "checkout-other/test/Policy.cpp", (plain,), True),
        ("longest-root", alias / "test/Policy.cpp", (plain, alias), False),
        ("root-encoding", spaced / "lib/Policy.cpp", (spaced,), True),
        ("encoded-root-test", spaced / "test/Policy.cpp", (spaced,), False),
        ("unknown-root", plain / "test/Policy.cpp", (root / "unknown",), True),
        ("case-varied-root", plain / "test/Policy.cpp", (_source_policy_ascii_case_variant(plain),), not windows),
        ("case-varied-test", plain / "TEST/Policy.cpp", (plain,), not windows),
    ]


def _source_policy_metadata_cases(root: Path) -> list[tuple[str, dict[str, str], bool, bool, bool]]:
    """Raw option fault injection bypasses the normal nonempty-root serializer."""
    encoded = json.dumps([root.as_posix()])
    values = [
        ("missing-option", None, True, True, False),
        ("malformed-json", "{", True, False, True),
        ("truncated-matching-root", encoded[:-1], True, False, True),
        ("non-array", "{}", True, False, True),
        ("empty-array", "[]", True, False, False),
        ("unusable-roots", '[null,17,false,{},""]', True, False, True),
        ("mixed-roots", json.dumps([None, root.as_posix(), ""]), False, False, True),
        ("dump-explicit-option", encoded, False, True, False),
    ]
    cases = [
        (name, {} if value is None else {"AobusSourceRoots": value}, expected, dump, warning)
        for name, value, expected, dump, warning in values
    ]
    for name, global_value, local_value, expected in (
        ("local-empty-overrides-global", encoded, "[]", True),
        ("local-root-overrides-global", "[]", encoded, False),
    ):
        options = {"AobusSourceRoots": global_value}
        options.update({f"{alias}.AobusSourceRoots": local_value for alias in _SOURCE_POLICY_CHECKS})
        cases.append((name, options, expected, False, False))
    return cases


def _source_policy_case_count(*, windows: bool) -> int:
    root = Path("source-policy")
    metadata = _source_policy_metadata_cases(root)
    # Dump controls execute both --dump-config and a semantic round trip.
    count = (
        len(_source_policy_cases(root, windows=windows)) + len(metadata) + sum(dump for _, _, _, dump, _ in metadata)
    )
    return count * (2 if windows else 1)


def _source_policy_task_count(*, windows: bool) -> int:
    root = Path("source-policy")
    return (len(_source_policy_cases(root, windows=windows)) + len(_source_policy_metadata_cases(root))) * (
        2 if windows else 1
    )


def _run_source_policy_smoke(
    build_dir: Path,
    run_dir: Path,
    *,
    plugin: Path | None,
    tools: WindowsFixtureTools | None = None,
    jobs: int = 1,
) -> tuple[bool, Path]:
    if tools is None and plugin is None:
        raise ValueError("source-policy smoke requires the verified native plugin")
    if jobs < 1:
        raise ValueError("source-policy smoke requires at least one worker")
    case_dir = Path(tempfile.mkdtemp(prefix="source-policy.", dir=run_dir))
    log = case_dir / "run.log"
    check = _SOURCE_POLICY_CHECKS[0]
    selected_checks = ",".join(_SOURCE_POLICY_CHECKS)

    @dataclass(frozen=True)
    class PreparedCase:
        label: str
        path: Path
        command: tuple[str, ...]
        sources: tuple[tuple[Path, str], ...]
        dump: bool
        expects_configuration_warning: bool

    tasks: list[PreparedCase] = []
    if tools is None:
        host = tidyengine.clang_tool(build_dir, "clang-tidy")
        flags: tuple[str, ...] = ("-std=c++26",)
    else:
        host = tools.tidy
        flags = (*tools.flags, "/clang:-std=c++26", "/clang:-fdiagnostics-format=clang")
    # Freeze all case/profile inputs before any worker starts. Distinct filenames
    # preserve each tested directory layout without sharing mutable marker bytes.
    for forward_slashes in (False, True) if tools is not None else (False,):
        spelling = "forward" if forward_slashes else "native"
        profile_dir = case_dir / spelling
        cases: list[tuple[str, Path, str, bool, bool, bool]] = []
        for name, path, roots, expected in _source_policy_cases(profile_dir, windows=tools is not None):
            option_roots = (
                tuple(_source_policy_ascii_case_variant(root) for root in roots) if forward_slashes else roots
            )
            config = tidyconfig.config_for(tidyconfig.checks_for("STRICT", selected_checks), source_roots=option_roots)
            cases.append((name, path.with_name(f"{name}.cpp"), config, expected, False, False))
        metadata_root = profile_dir / "metadata checkout"
        for name, options, expected, dump, warning in _source_policy_metadata_cases(metadata_root):
            config = json.dumps({"Checks": "-*," + selected_checks, "CheckOptions": options})
            cases.append((name, metadata_root / "test" / f"{name}.cpp", config, expected, dump, warning))
        for name, path, config, expected, dump, warning in cases:
            path.parent.mkdir(parents=True, exist_ok=True)
            marker = "POSITIVE" if expected else "NEGATIVE"
            source = (
                "namespace std { [[noreturn]] void abort() noexcept; "
                "namespace filesystem { struct path { path(char const*); }; } }\n"
                f"void fatalProbe() {{ std::abort(); }} // {marker}:aobus-readability-forbid-raw-fatal\n"
                f"void throwProbe() {{ throw 1; }} // {marker}:aobus-readability-forbid-raw-throw\n"
                "void pathProbe(char const* text) { std::filesystem::path value(text); } "
                f"// {marker}:aobus-portability-explicit-path-conversion\n"
            )
            sources: dict[Path, str] = {}
            input_paths: tuple[Path, ...] = (path,)
            diagnostic_args: tuple[str, ...] = ()
            separator = "\\" if tools is not None and not forward_slashes else "/"
            if name == "same-file-system":
                body = "\n".join(source.splitlines()[1:]) + "\n"
                system = body.replace("Probe", "SystemProbe").replace("POSITIVE", "NEGATIVE")
                restored = body.replace("Probe", "RestoredProbe")
                # Line-marker filenames are string literals, unlike header-name
                # tokens. Derive presumed lines from the physical buffer.
                filename = json.dumps(path.as_posix() if forward_slashes else str(path))
                system_line = source.count("\n") + 2
                source += f"# {system_line} {filename} 3\n{system}"
                restored_line = source.count("\n") + 2
                source += f"# {restored_line} {filename}\n{restored}"
                # Expose system diagnostics so the check, not host filtering,
                # must reject the marked region. Other cases keep normal flags.
                diagnostic_args = ("--system-headers",)
            elif name in ("header-file-switch", "header-file-switch-unicode"):
                other = path.parent.parent / "test" / f"{name}-ordinary.h"
                sources[other] = source.replace("Probe", "HeaderProbe").replace("POSITIVE", "NEGATIVE")
                # Relative ASCII tokens need no string escaping and remain valid
                # when an ancestor contains Unicode, quotes or separators.
                header_name = separator.join(("..", "test", other.name))
                source = f'#include "{header_name}"\n' + "\n".join(source.splitlines()[1:]) + "\n"
                diagnostic_args = ("--header-filter=.*",)
            elif name in ("multi-tu-exempt-first", "multi-tu-production-first"):
                other = path.parent.parent / "test" / f"{name}-ordinary.cpp"
                sources[other] = source.replace("POSITIVE", "NEGATIVE")
                input_paths = (other, path) if name == "multi-tu-exempt-first" else (path, other)
            elif name in ("macro-spelling-exempt-definition", "macro-spelling-production-definition"):
                header_directory = "test" if expected else "lib"
                other = path.parent.parent / header_directory / f"{name}-macros.h"
                sources[other] = (
                    source.splitlines()[0] + "\n"
                    "#define SOURCE_POLICY_FATAL std::abort()\n"
                    "#define SOURCE_POLICY_THROW throw 1\n"
                    "#define SOURCE_POLICY_PATH std::filesystem::path macroValue(text)\n"
                )
                macro_marker = "NEGATIVE" if expected else "POSITIVE"
                header_name = separator.join(("..", header_directory, other.name))
                source = (
                    f'#include "{header_name}"\n'
                    "void macroFatalProbe() { SOURCE_POLICY_FATAL; } "
                    f"// {macro_marker}:aobus-readability-forbid-raw-fatal\n"
                    "void macroThrowProbe() { SOURCE_POLICY_THROW; } "
                    f"// {macro_marker}:aobus-readability-forbid-raw-throw\n"
                    "void macroPathProbe(char const* text) { SOURCE_POLICY_PATH; } "
                    f"// {macro_marker}:aobus-portability-explicit-path-conversion\n"
                    + "\n".join(source.splitlines()[1:])
                    + "\n"
                )
            sources[path] = source
            for source_path, source_text in sources.items():
                source_path.parent.mkdir(parents=True, exist_ok=True)
                source_path.write_text(source_text, encoding="utf-8")
            input_names = tuple(
                input_path.as_posix() if forward_slashes else str(input_path) for input_path in input_paths
            )
            command = _direct_tidy_command(
                host,
                input_names[-1],
                config,
                flags=flags,
                plugin=plugin if tools is None else None,
                tidy_args=(*diagnostic_args, *input_names[:-1]),
            )
            tasks.append(
                PreparedCase(f"{name}/{spelling}", path, tuple(command), tuple(sources.items()), dump, warning)
            )

    def run_case(task: PreparedCase) -> tuple[bool, Path]:
        profile = task.label
        case_log = task.path.with_suffix(".log")
        errors: list[str] = []
        outputs: list[str] = []
        sources = dict(task.sources)
        command = list(task.command)
        if task.dump:
            separator = command.index("--")
            dumped = subprocess.run(
                [*command[:separator], "--dump-config", *command[separator:]],
                cwd=PROJECT_ROOT,
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
                text=True,
            )
            outputs.append(f"=== {profile}/dump ===\n{dumped.stdout}")
            if dumped.returncode != 0 or has_fatal_compiler_error(dumped.stdout):
                errors.append(f"{profile}: ERROR: --dump-config failed")
            for alias in _SOURCE_POLICY_CHECKS:
                if f"{alias}.AobusSourceRoots" not in dumped.stdout:
                    errors.append(f"{profile}: ERROR: missing dumped source-root option for {alias}")
            command = [f"-config={dumped.stdout}" if arg.startswith("-config=") else arg for arg in command]
        result = subprocess.run(command, cwd=PROJECT_ROOT, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        configuration_warnings = [
            line
            for line in result.stdout.splitlines()
            if (header := parse_diagnostic_header(line)) is not None
            and header.severity == "warning"
            and "AobusSourceRoots" in line
            and "[clang-tidy-config]" in line
        ]
        if bool(configuration_warnings) != task.expects_configuration_warning:
            errors.append(f"{profile}: ERROR: unexpected source-root configuration warning state")
        policy_output = "\n".join(line for line in result.stdout.splitlines() if line not in configuration_warnings)
        policy_result = subprocess.CompletedProcess(result.args, result.returncode, stdout=policy_output)
        errors.extend(
            f"{profile}: {error}"
            for error in _diagnostic_errors(
                Fixture(task.path, check),
                sources,
                policy_result,
                direct_host=True,
                required_checks=_SOURCE_POLICY_CHECKS,
            )
        )
        outputs.append(f"=== {profile} ===\n{result.stdout}")
        case_log.write_text("\n".join([*outputs, *(f"ERROR: {error}" for error in errors)]), encoding="utf-8")
        return not errors, case_log

    passed = True
    with concurrent.futures.ThreadPoolExecutor(max_workers=min(jobs, len(tasks))) as pool:
        with log.open("w", encoding="utf-8") as sink:
            for success, case_log in pool.map(run_case, tasks):
                passed = passed and success
                sink.write(case_log.read_text(encoding="utf-8"))
                sink.write("\n")
    return passed, log


# One canary source per case; the stage's phase count is derived from this
# tuple so the recorded work always matches the executed work.
_COMPILER_ERROR_SMOKE_CASES = (
    ("missing-include", '#include "DefinitelyMissingHeader.h"\n', False),
    ("syntax-error", "void brokenSyntax(;\n", False),
    ("unknown-type", "DefinitelyUnknownType value;\n", False),
    (
        "unused-code",
        "static void unusedFunction(int unusedParameter) { int unusedVariable; }\n",
        True,
    ),
)


def _run_compiler_error_smoke(
    build_dir: Path, run_dir: Path, *, tools: WindowsFixtureTools | None = None
) -> tuple[bool, Path]:
    case_dir = Path(tempfile.mkdtemp(prefix="compiler-errors.", dir=run_dir))
    log = case_dir / "run.log"
    cases = _COMPILER_ERROR_SMOKE_CASES
    errors: list[str] = []
    outputs: list[str] = []

    for name, source, should_succeed in cases:
        path = case_dir / f"{name}.cpp"
        path.write_text(source, encoding="utf-8")
        fixture = Fixture(path, "aobus-modernize-braced-initialization")
        result = subprocess.run(
            _tidy_command(fixture, build_dir, *_fixture_tidy_args(case_dir, windows=tools is not None), tools=tools),
            cwd=PROJECT_ROOT,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
        )
        outputs.append(f"=== {name} ===\n{result.stdout}")
        if should_succeed:
            if result.returncode != 0:
                errors.append(f"unused-code canary exited with status {result.returncode}")
        elif result.returncode != 1 or not has_fatal_compiler_error(result.stdout, tag="[clang-diagnostic-error]"):
            errors.append(f"{name} canary did not produce a fatal compiler diagnostic")

    with log.open("w", encoding="utf-8") as sink:
        sink.write("\n".join(outputs))
        for error in errors:
            sink.write(f"ERROR: {error}\n")
    return not errors, log


def _report_results(
    label: str,
    fixtures: list[Fixture],
    results: Iterable[tuple[bool, Path]],
    reporter: Reporter,
) -> bool:
    """Report fixtures in submission order, without waiting for the whole stage.

    The pool's iterator is consumed lazily, so the first line appears once the
    first fixture finishes rather than once the last one does -- a stage that is
    silent for a minute is otherwise indistinguishable from a hang. Order stays
    the submission order, which keeps two runs comparable line by line; the cost
    is that a slow first fixture still holds back the finished ones behind it.
    """
    passed = True
    for fixture, (success, log) in zip(fixtures, results, strict=True):
        reporter.write(f"  [{'PASS' if success else 'FAIL'}] {fixture.check}/{fixture.path.name}")
        passed = passed and success
        if not success:
            reporter.write_file(log)
    reporter.write(f"{label}: {'passed' if passed else 'failed'}")
    return passed


def run(
    build_dir: Path,
    *,
    log: Path | None = None,
    jobs: int | None = None,
    toolchain: tidyengine.TidyToolchain | None = None,
) -> int:
    """Run the lint fixture integration suite.

    *toolchain* is the caller-verified tidy context prepared by the portal's
    toolchain preflight; it is required before the Windows diagnostic stage and
    ignored elsewhere, where the portal route supplies the fixture runs.
    Expectation errors are reported for every discovered fixture before that
    setup and before diagnostic workers start.
    """
    windows = builddir.platform_profile().name == "windows"
    reporter = Reporter(log)
    fixtures = discover_fixtures()
    if not fixtures:
        reporter.write(f"ERROR: no lint fixtures found in {FIXTURE_DIR}")
        return 1
    # Current marker grammar on the discovered list, including context headers
    # _fixture_sources already follows. Windows still executes only the
    # non-.mm selection after this validation.
    expectation_errors = _preflight_expectation_errors(fixtures)
    if expectation_errors:
        for error in expectation_errors:
            reporter.write(f"ERROR: {error}")
        return 1
    if windows:
        deferred = [fixture for fixture in fixtures if fixture.path.suffix == ".mm"]
        for fixture in deferred:
            reporter.write(f"  [DEFER] {fixture.check}/{fixture.path.name}: Objective-C++ is incompatible with Windows")
        fixtures = [fixture for fixture in fixtures if fixture.path.suffix != ".mm"]
        reporter.write(
            f"Windows fixture coverage: {len(fixtures)} selected, {len(deferred)} Objective-C++ deferred (not tested)"
        )
        if not fixtures:
            reporter.write("ERROR: no compatible lint fixtures selected")
            return 1
        if toolchain is None:
            raise die("the Windows lint suite requires the toolchain verified by the tidy portal preflight.")
        tools = _windows_fixture_tools(build_dir, toolchain)
    else:
        tools = None
    plugin = build_dir / "tool" / "lint" / "libAobusLintPlugin.so"
    compile_db = build_dir / "compile_commands.json"
    if not compile_db.is_file():
        reporter.write(f"ERROR: compile_commands.json not found in {build_dir}")
        return 1
    if not windows and not plugin.is_file():
        reporter.write(f"ERROR: AobusLintPlugin not found at {plugin}")
        return 1
    run_dir = Path(tempfile.mkdtemp(prefix="aobus-lint-integration-"))
    failed = True
    try:
        reporter.write(f"=== Diagnostic Verification ({len(fixtures)} fixtures) ===")
        worker_count = min(jobs or max((os.cpu_count() or 1) - 1, 1), len(fixtures))
        # The phase encloses the whole executor so it also covers the lazy
        # pool.map consumption inside _report_results; Reporter line ordering
        # is untouched by the record.
        with phases.measure("lint-diagnostics", tree="tidy", jobs=worker_count, count=len(fixtures)) as phase:
            with concurrent.futures.ThreadPoolExecutor(max_workers=worker_count) as pool:
                diagnostics_passed = _report_results(
                    "Diagnostics",
                    fixtures,
                    pool.map(lambda fixture: _run_diagnostic(fixture, build_dir, run_dir, tools=tools), fixtures),
                    reporter,
                )
            phase.status = 0 if diagnostics_passed else 1

        reporter.write("=== Source Policy Scope Smoke ===")
        policy_jobs = min(worker_count, _source_policy_task_count(windows=windows))
        with phases.measure(
            "lint-source-policy", tree="tidy", jobs=policy_jobs, count=_source_policy_case_count(windows=windows)
        ) as phase:
            source_policy_passed, source_policy_log = _run_source_policy_smoke(
                build_dir, run_dir, plugin=plugin if not windows else None, tools=tools, jobs=policy_jobs
            )
            phase.status = 0 if source_policy_passed else 1
        reporter.write(f"  [{'PASS' if source_policy_passed else 'FAIL'}] root-relative production policy")
        if not source_policy_passed:
            reporter.write_file(source_policy_log)

        reporter.write("=== Compiler Error Detection Smoke ===")
        with phases.measure("lint-compiler-errors", tree="tidy", count=len(_COMPILER_ERROR_SMOKE_CASES)) as phase:
            compiler_errors_passed, compiler_errors_log = _run_compiler_error_smoke(build_dir, run_dir, tools=tools)
            phase.status = 0 if compiler_errors_passed else 1
        reporter.write(f"  [{'PASS' if compiler_errors_passed else 'FAIL'}] fatal compiler diagnostics")
        if not compiler_errors_passed:
            reporter.write_file(compiler_errors_log)

        failed = not (diagnostics_passed and source_policy_passed and compiler_errors_passed)
        reporter.write(
            "=== LINT INTEGRATION TESTS PASSED ===" if not failed else "=== LINT INTEGRATION TESTS FAILED ==="
        )
        if failed:
            reporter.write(f"Failure artifacts preserved at {run_dir}")
        return int(failed)
    finally:
        if not failed:
            shutil.rmtree(run_dir, ignore_errors=True)
