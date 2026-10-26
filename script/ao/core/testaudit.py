"""Bounded lexical Catch2 registration discovery and the name/tag audit.

One detector feeds every audit route: repository scanning, directory
resolution, explicit-file resolution, and the selected-file path that
``ao hygiene`` routes through ``ao test-audit``. Discovery is content-driven:
every candidate C++/Objective-C++ file under ``test/`` is scanned, including
headers and include fragments, so a registration cannot hide behind a file
name. Executable ownership stays with CMake (see
``cmake/AobusTestSources.cmake``); this module never assigns or infers it.

The detector is deliberately bounded. It skips comments, character and
string literals, and custom-delimiter raw strings while locating
registrations, and recognizes only the public registration forms the project
uses:

- ``TEST_CASE(name)``
- ``TEST_CASE(name, tags)``
- ``TEMPLATE_TEST_CASE(name, tags-or-empty, types...)``

Template declarations keep the mandatory tags argument: ``""`` when they
have no tags, or the retained ``[concurrency]``/``[.manual]`` tags under the
same validation as ordinary cases.

Every other public Catch2 registration family, non-literal invocation
argument, comment inside an invocation, and registration placed in a header
or include fragment is reported as an issue instead of being silently
skipped. An unterminated ordinary literal ends at its own line, and every
digit separator of a number belongs to the number, so a stray quote or
apostrophe in prose (``isn't`` in a disabled block, ``#warning`` text,
``0xDEAD'BEEF'CAFE``) cannot swallow later registrations.
Backslash-newline line splices are honored outside invocations, so
a comment, identifier, or literal continued by a splice is recognized as
such; an invocation that itself contains a splice is reported as
unsupported syntax instead of being silently missed. The audit is advisory
by default; ``--fail-on-issue`` gates it.

Detector reuse: ``discover_test_files`` enumerates the candidate universe,
``parse_test_cases`` returns the registrations detected in one file, and
``scan_test_file`` additionally returns the lexical issues. Consumers that
need declaration spans (for example a retagger or a configured-manifest
completeness check) can use ``TestCase.tags_literal`` and
``TestCase.tags_span`` to rewrite exactly the tags argument without touching
any other byte.
"""

import re
from bisect import bisect_left
from collections.abc import Iterable
from dataclasses import dataclass
from pathlib import Path

from .paths import PROJECT_ROOT, absolute_path
from .proc import die
from .testregistry import TEST_DISCOVERY_SUFFIXES, TEST_PLACEMENT_SUFFIXES

# Lint fixtures intentionally violate project rules; they are never auditable.
LINT_FIXTURE_PARTS = ("lint", "fixture")

ALLOWED_TAGS = frozenset({"concurrency", ".manual"})
CONCURRENCY_TAG = "concurrency"

SUPPORTED_REGISTRATION_MACROS = frozenset({"TEST_CASE", "TEMPLATE_TEST_CASE"})
# Every other public registration family from the pinned Catch2 3.16.0
# headers (catch_test_macros.hpp, catch_template_test_macros.hpp), including
# the CATCH_-prefixed compatibility spellings. Each one is reported instead
# of silently skipped: an empty audit result for them would be
# indistinguishable from an audited absence.
UNSUPPORTED_REGISTRATION_MACROS = frozenset(
    {
        "CATCH_METHOD_AS_TEST_CASE",
        "CATCH_REGISTER_TEST_CASE",
        "CATCH_SCENARIO",
        "CATCH_SCENARIO_METHOD",
        "CATCH_TEMPLATE_LIST_TEST_CASE",
        "CATCH_TEMPLATE_LIST_TEST_CASE_METHOD",
        "CATCH_TEMPLATE_PRODUCT_TEST_CASE",
        "CATCH_TEMPLATE_PRODUCT_TEST_CASE_METHOD",
        "CATCH_TEMPLATE_PRODUCT_TEST_CASE_METHOD_SIG",
        "CATCH_TEMPLATE_PRODUCT_TEST_CASE_SIG",
        "CATCH_TEMPLATE_TEST_CASE",
        "CATCH_TEMPLATE_TEST_CASE_METHOD",
        "CATCH_TEMPLATE_TEST_CASE_METHOD_SIG",
        "CATCH_TEMPLATE_TEST_CASE_SIG",
        "CATCH_TEST_CASE",
        "CATCH_TEST_CASE_METHOD",
        "CATCH_TEST_CASE_PERSISTENT_FIXTURE",
        "METHOD_AS_TEST_CASE",
        "REGISTER_TEST_CASE",
        "SCENARIO",
        "SCENARIO_METHOD",
        "TEMPLATE_LIST_TEST_CASE",
        "TEMPLATE_LIST_TEST_CASE_METHOD",
        "TEMPLATE_PRODUCT_TEST_CASE",
        "TEMPLATE_PRODUCT_TEST_CASE_METHOD",
        "TEMPLATE_PRODUCT_TEST_CASE_METHOD_SIG",
        "TEMPLATE_PRODUCT_TEST_CASE_SIG",
        "TEMPLATE_TEST_CASE_METHOD",
        "TEMPLATE_TEST_CASE_METHOD_SIG",
        "TEMPLATE_TEST_CASE_SIG",
        "TEST_CASE_METHOD",
        "TEST_CASE_PERSISTENT_FIXTURE",
    }
)
REGISTRATION_MACROS = SUPPORTED_REGISTRATION_MACROS | UNSUPPORTED_REGISTRATION_MACROS

# Performance workload regions carry no tags at all: selection uses case
# names. Bounded to the current workload consumers only; test/perf/report/
# is ordinary core correctness and stays outside this rule.
WORKLOAD_DIRECTORY_PARTS = ("perf", "baseline")
WORKLOAD_BENCHMARK_PARTS = ("tui", "TrackTableBenchmark.cpp")

# String-literal prefixes: a raw string may use a custom delimiter, an
# encoding prefix may not. Inside registration arguments even these appear
# only as ordinary literals or the invocation is rejected.
RAW_STRING_PREFIXES = frozenset({"R", "u8R", "uR", "UR", "LR"})
ENCODING_STRING_PREFIXES = frozenset({"u8", "u", "U", "L"})

TAG_RE = re.compile(r"\[([^\[\]]+)\]")
# The complete tags argument must be bracketed tags with nonempty bodies.
TAG_ARGUMENT_RE = re.compile(r"(?:\[[^\[\]]+\])+")
# Exactly one ordinary string literal, quotes included.
ORDINARY_LITERAL_RE = re.compile(r'"((?:\\.|[^"\\])*)"')
FUNCTION_LEVEL_NAME_RE = re.compile(r"(?:[a-z][A-Za-z0-9_]*|operator\S+)\s+[a-z].+")

# Translation phase 2 line splice: a backslash, optional horizontal
# whitespace, an optional carriage return, and the newline. Splicing runs
# before tokenization, so it joins identifiers, comments, and literals.
SPLICE_RE = re.compile(r"\\[ \t\v\f]*\r?\n")
SPLICED_INVOCATION_MESSAGE = (
    "registration invocations containing a line splice are not supported;"
    " write the invocation without backslash-newline continuations"
)


@dataclass(frozen=True)
class TestCase:
    path: Path
    line: int
    name: str
    tags: tuple[str, ...]
    template: bool = False
    # Verbatim tags literal contents; None when the invocation omits the
    # argument (the canonical ordinary no-tag form).
    tags_literal: str | None = None
    # (start, end) span of the tags literal including its quotes; None when
    # the argument is omitted. Offsets index the original source text.
    tags_span: tuple[int, int] | None = None


@dataclass(frozen=True)
class Issue:
    path: Path
    line: int
    kind: str
    message: str

    def format(self, root: Path = PROJECT_ROOT) -> str:
        try:
            path = self.path.relative_to(root).as_posix()
        except ValueError:
            path = self.path.as_posix()
        return f"{path}:{self.line}: {self.kind}: {self.message}"


def discover_test_files(root: Path = PROJECT_ROOT) -> list[Path]:
    """Every candidate content-discovery file under test/, fixtures excluded."""
    test_root = absolute_path(root) / "test"
    return sorted(
        absolute_path(path)
        for path in test_root.rglob("*")
        if path.is_file() and _is_discovery_candidate(absolute_path(path), test_root)
    )


def is_audit_candidate(path: str | Path, root: Path = PROJECT_ROOT) -> bool:
    """Whether a selected file belongs to the audit's content-discovery universe."""
    root = absolute_path(root)
    candidate = Path(path)
    if not candidate.is_absolute():
        candidate = root / candidate
    return _is_discovery_candidate(absolute_path(candidate), root / "test")


def resolve_files(paths: Iterable[str], root: Path = PROJECT_ROOT) -> list[Path]:
    """Resolve default, directory, and explicit-file selections to candidates."""
    root = absolute_path(root)
    selected = list(paths)
    if not selected:
        return discover_test_files(root)

    files: list[Path] = []
    seen: set[Path] = set()
    test_root = root / "test"
    for name in selected:
        path = Path(name)
        if not path.is_absolute():
            path = root / path
        path = absolute_path(path)
        if not path.exists():
            raise die(f"explicitly selected audit path does not exist: {name}")
        if path.is_dir():
            candidates = sorted(
                absolute_path(candidate)
                for candidate in path.rglob("*")
                if candidate.is_file() and _is_discovery_candidate(absolute_path(candidate), test_root)
            )
        elif path.is_file() and _is_discovery_candidate(absolute_path(path), test_root):
            candidates = [absolute_path(path)]
        else:
            candidates = []

        for candidate in candidates:
            if candidate in seen:
                continue
            seen.add(candidate)
            files.append(candidate)

    return files


def scan_test_file(path: Path) -> tuple[list[TestCase], list[Issue]]:
    """One lexical pass: detected registrations plus unsupported-syntax findings."""
    return _scan_source(path.read_text(encoding="utf-8"), path)


def parse_test_cases(path: Path) -> list[TestCase]:
    """Registrations detected in one file; unsupported invocations are issues."""
    return scan_test_file(path)[0]


def registration_placement_issues(path: Path, cases: Iterable[TestCase]) -> list[Issue]:
    """Validate registration filenames without rescanning or assigning ownership."""
    if path.suffix in TEST_PLACEMENT_SUFFIXES:
        message = (
            "registration macros are not supported in headers or include fragments;"
            " move the case to a *Test.cpp or *Test.mm registration file"
        )
    elif not path.name.endswith(("Test.cpp", "Test.mm", "Benchmark.cpp")):
        message = "registration macros must live in *Test.cpp, *Test.mm, or *Benchmark.cpp files"
    else:
        return []
    return [Issue(case.path, case.line, "unsupported-placement", message) for case in cases]


def audit_files(files: Iterable[Path], root: Path = PROJECT_ROOT) -> list[Issue]:
    issues: list[Issue] = []
    test_root = absolute_path(root) / "test"
    for path in files:
        path = absolute_path(path)
        cases, lexical_issues = scan_test_file(path)
        issues.extend(lexical_issues)
        placement_issues = registration_placement_issues(path, cases)
        issues.extend(placement_issues)
        if placement_issues:
            continue
        workload = _is_workload_file(path, test_root)
        for case in cases:
            issues.extend(_audit_case(case, workload=workload))
    return issues


def audit_paths(paths: Iterable[str], root: Path = PROJECT_ROOT) -> list[Issue]:
    return audit_files(resolve_files(paths, root), root=root)


def _audit_case(case: TestCase, *, workload: bool = False) -> list[Issue]:
    issues: list[Issue] = []
    if not _valid_name(case.name):
        issues.append(
            Issue(
                case.path,
                case.line,
                "name",
                'test name should use "Component - behavior" or the documented function-level form',
            )
        )

    if case.tags_literal is None:
        if case.template:
            issues.append(
                Issue(case.path, case.line, "tag", "TEMPLATE_TEST_CASE must keep the mandatory empty tags argument")
            )
        # The canonical ordinary no-tag form omits the argument entirely.
        return issues

    if workload and case.tags:
        issues.append(
            Issue(
                case.path,
                case.line,
                "tag",
                "performance workload cases carry no tags; selection uses case names",
            )
        )
        return issues

    if case.tags_literal == "":
        if case.template:
            # The mandatory empty template argument is valid.
            return issues
        issues.append(Issue(case.path, case.line, "tag", "untagged test cases must omit the tags argument"))
        return issues
    if TAG_ARGUMENT_RE.fullmatch(case.tags_literal) is None:
        issues.append(Issue(case.path, case.line, "tag", "the tags argument must contain only bracketed Catch2 tags"))
        return issues

    for tag in case.tags:
        if tag not in ALLOWED_TAGS:
            issues.append(
                Issue(
                    case.path,
                    case.line,
                    "tag",
                    f"tag [{tag}] is not permitted; only [concurrency] and [.manual] are allowed",
                )
            )
    if len(set(case.tags)) != len(case.tags):
        issues.append(Issue(case.path, case.line, "tag-duplicate", "tags must not repeat"))
    return issues


def _valid_name(name: str) -> bool:
    if " - " in name:
        component, behavior = name.split(" - ", 1)
        return bool(component.strip()) and bool(behavior.strip())
    return FUNCTION_LEVEL_NAME_RE.fullmatch(name) is not None


def _is_workload_file(path: Path, test_root: Path) -> bool:
    """Whether path is one of the current tag-free performance workloads."""
    try:
        relative = path.relative_to(test_root)
    except ValueError:
        return False
    if relative.parts[: len(WORKLOAD_DIRECTORY_PARTS)] == WORKLOAD_DIRECTORY_PARTS:
        return True
    return relative.parts == WORKLOAD_BENCHMARK_PARTS


def _is_discovery_candidate(path: Path, test_root: Path) -> bool:
    try:
        relative = path.relative_to(test_root)
    except ValueError:
        return False
    if path.suffix not in TEST_DISCOVERY_SUFFIXES:
        return False
    return relative.parts[: len(LINT_FIXTURE_PARTS)] != LINT_FIXTURE_PARTS


def _scan_source(source: str, path: Path) -> tuple[list[TestCase], list[Issue]]:
    """One lexical pass: detected registrations plus unsupported-syntax findings.

    Reported lines and tags spans index the original source text, the
    retagger contract. Scanning runs on the phase-2 spliced logical view when
    the file has line splices, so comments, identifiers, and literals joined
    by a splice are recognized, while raw string bodies keep their original
    spelling.
    """
    logical, positions = _normalized_source(source)
    return _scan_text(source, path, logical, positions)


def _scan_text(original: str, path: Path, text: str, positions: list[int] | None) -> tuple[list[TestCase], list[Issue]]:
    cases: list[TestCase] = []
    issues: list[Issue] = []
    length = len(text)
    index = 0
    while index < length:
        char = text[index]
        if char == "/":
            if text.startswith("//", index):
                index = _skip_line_comment(text, index)
                continue
            if text.startswith("/*", index):
                index = _skip_block_comment(text, index)
                continue
            index += 1
            continue
        if char == '"':
            index = _skip_string(text, index)
            continue
        if char == "'":
            index = _skip_char_or_separator(text, index)
            continue
        if char.isalpha() or char == "_":
            end = index + 1
            while end < length and (text[end].isalnum() or text[end] == "_"):
                end += 1
            word = text[index:end]
            if end < length and text[end] == '"':
                if word in RAW_STRING_PREFIXES:
                    index = _skip_raw_string(text, end)
                    continue
                if word in ENCODING_STRING_PREFIXES:
                    index = _skip_string(text, end)
                    continue
            if end < length and text[end] == "'" and word in ENCODING_STRING_PREFIXES:
                index = _skip_char(text, end)
                continue
            if word in REGISTRATION_MACROS:
                parenthesis = _skip_invocation_trivia(text, end)
                if parenthesis < length and text[parenthesis] == "(":
                    if word in SUPPORTED_REGISTRATION_MACROS:
                        case, index = _parse_supported_invocation(
                            original, text, positions, path, word, index, parenthesis, issues
                        )
                        if case is not None:
                            cases.append(case)
                        continue
                    issues.append(
                        Issue(
                            path,
                            _original_line(original, positions, index),
                            "unsupported-syntax",
                            f"{word} is not a supported registration macro;"
                            " use TEST_CASE or TEMPLATE_TEST_CASE with literal arguments",
                        )
                    )
            index = end
            continue
        index += 1
    return cases, issues


def _normalized_source(source: str) -> tuple[str, list[int] | None]:
    """The phase-2 spliced logical view of source plus its original-offset map.

    Returns (source, None) when the file has no line splice, so the no-splice
    scan stays byte exact. Otherwise the logical view has every splice
    removed and positions[i] is the offset of logical[i] in the original
    text: lexical joins from the splice become visible to the scan while
    reported positions stay in the original source.
    """
    if SPLICE_RE.search(source) is None:
        return source, None
    logical, positions = _splice_source(source)
    return _protect_raw_strings(logical, positions, source)


def _splice_source(source: str) -> tuple[str, list[int]]:
    logical: list[str] = []
    positions: list[int] = []
    index = 0
    for match in SPLICE_RE.finditer(source):
        logical.append(source[index : match.start()])
        positions.extend(range(index, match.start()))
        index = match.end()
    logical.append(source[index:])
    positions.extend(range(index, len(source)))
    return "".join(logical), positions


def _protect_raw_strings(logical: str, positions: list[int], original: str) -> tuple[str, list[int]]:
    """Restore raw string bodies and delimiters to their original spelling.

    Phase 2 splicing is reverted inside a raw string literal: a
    backslash-newline in the body is content, and a splice cannot forge a
    closing delimiter, so the raw string is located in the original text and
    kept as one protected chunk. The walk uses the same helpers as the
    registration scan, so only a genuine raw string context is protected.
    """
    pieces: list[str] = []
    mapped: list[int] = []
    copied = 0
    index = 0
    length = len(logical)
    while index < length:
        char = logical[index]
        if char == "/":
            if logical.startswith("//", index):
                index = _skip_line_comment(logical, index)
                continue
            if logical.startswith("/*", index):
                index = _skip_block_comment(logical, index)
                continue
            index += 1
            continue
        if char == '"':
            index = _skip_string(logical, index)
            continue
        if char == "'":
            index = _skip_char_or_separator(logical, index)
            continue
        if char.isalpha() or char == "_":
            end = index + 1
            while end < length and (logical[end].isalnum() or logical[end] == "_"):
                end += 1
            word = logical[index:end]
            if end < length and logical[end] == '"':
                if word in RAW_STRING_PREFIXES:
                    orig_quote = positions[end]
                    orig_end = _skip_raw_string(original, orig_quote)
                    logical_end = bisect_left(positions, orig_end)
                    pieces.append(logical[copied:end])
                    mapped.extend(positions[copied:end])
                    pieces.append(original[orig_quote:orig_end])
                    mapped.extend(range(orig_quote, orig_end))
                    copied = logical_end
                    index = logical_end
                    continue
                if word in ENCODING_STRING_PREFIXES:
                    index = _skip_string(logical, end)
                    continue
            if end < length and logical[end] == "'" and word in ENCODING_STRING_PREFIXES:
                index = _skip_char(logical, end)
                continue
            index = end
            continue
        index += 1
    pieces.append(logical[copied:])
    mapped.extend(positions[copied:])
    return "".join(pieces), mapped


def _parse_supported_invocation(
    original: str,
    text: str,
    positions: list[int] | None,
    path: Path,
    macro: str,
    macro_index: int,
    open_paren: int,
    issues: list[Issue],
) -> tuple[TestCase | None, int]:
    """Parse one supported invocation, reporting unsupported argument forms.

    The invocation is parsed in the spliced logical view, while reported
    lines and the tags span index the original source. An invocation whose
    original span contains a line splice is reported once as unsupported
    syntax and yields no case: a spliced registration is unsupported, not
    invisible.

    Returns the detected registration, or None when the invocation is
    rejected, and the index scanning resumes from.
    """
    template = macro == "TEMPLATE_TEST_CASE"
    macro_line = _original_line(original, positions, macro_index)
    arguments: list[tuple[int, int]] = []
    findings: list[Issue] = []
    rejected = False
    depth = 0
    length = len(text)
    index = open_paren + 1
    argument_start = open_paren + 1
    while index < length:
        char = text[index]
        if text.startswith("//", index) or text.startswith("/*", index):
            findings.append(
                Issue(
                    path,
                    _original_line(original, positions, index),
                    "unsupported-syntax",
                    "comments inside a registration invocation are not supported",
                )
            )
            rejected = True
            index = (
                _skip_line_comment(text, index) if text.startswith("//", index) else _skip_block_comment(text, index)
            )
            continue
        if char == '"':
            index = _skip_string(text, index)
            continue
        if char == "'":
            index = _skip_char_or_separator(text, index)
            continue
        if char.isalpha() or char == "_":
            end = index + 1
            while end < length and (text[end].isalnum() or text[end] == "_"):
                end += 1
            word = text[index:end]
            if word in ENCODING_STRING_PREFIXES and end < length and text[end] == "'":
                index = _skip_char(text, end)
                continue
            if word in RAW_STRING_PREFIXES and end < length and text[end] == '"':
                findings.append(
                    Issue(
                        path,
                        _original_line(original, positions, index),
                        "unsupported-syntax",
                        "raw string arguments are not supported",
                    )
                )
                rejected = True
                index = _skip_raw_string(text, end)
                continue
            index = end
            continue
        if char == "(":
            depth += 1
        elif char == ")":
            if depth == 0:
                arguments.append((argument_start, index))
                if _spliced_invocation(original, positions, macro_index, index):
                    issues.append(Issue(path, macro_line, "unsupported-syntax", SPLICED_INVOCATION_MESSAGE))
                    return None, index + 1
                issues.extend(findings)
                if rejected:
                    return None, index + 1
                case = _build_case(text, positions, path, macro, macro_line, template, arguments, issues)
                return case, index + 1
            depth -= 1
        elif char == "," and depth == 0:
            arguments.append((argument_start, index))
            argument_start = index + 1
        index += 1
    issues.extend(findings)
    issues.append(Issue(path, macro_line, "unsupported-syntax", "unterminated registration invocation"))
    return None, length


def _build_case(
    text: str,
    positions: list[int] | None,
    path: Path,
    macro: str,
    macro_line: int,
    template: bool,
    arguments: list[tuple[int, int]],
    issues: list[Issue],
) -> TestCase | None:
    if template:
        expected = "TEMPLATE_TEST_CASE requires a name literal, a tags literal, and at least one type"
        valid = len(arguments) >= 3
    else:
        expected = "TEST_CASE accepts a name literal and an optional tags literal"
        valid = len(arguments) in (1, 2)
    if not valid:
        issues.append(Issue(path, macro_line, "unsupported-syntax", expected))
        return None

    name_match = _literal_match(text, *arguments[0])
    if name_match is None:
        issues.append(
            Issue(path, macro_line, "unsupported-syntax", "the name argument must be a single string literal")
        )
        return None

    tags_match = None
    if len(arguments) > 1:
        tags_match = _literal_match(text, *arguments[1])
        if tags_match is None:
            issues.append(
                Issue(path, macro_line, "unsupported-syntax", "the tags argument must be a single string literal")
            )
            return None

    tags_literal = tags_match[0] if tags_match is not None else None
    tags_span = _original_span(positions, *tags_match[1]) if tags_match is not None else None
    return TestCase(
        path=path,
        line=macro_line,
        name=name_match[0],
        tags=tuple(TAG_RE.findall(tags_literal)) if tags_literal is not None else (),
        template=template,
        tags_literal=tags_literal,
        tags_span=tags_span,
    )


def _literal_match(source: str, start: int, end: int) -> tuple[str, tuple[int, int]] | None:
    """Match one whole ordinary string literal argument; return contents and span."""
    while start < end and source[start].isspace():
        start += 1
    while end > start and source[end - 1].isspace():
        end -= 1
    match = ORDINARY_LITERAL_RE.fullmatch(source, start, end)
    if match is None:
        return None
    return match.group(1), (match.start(1) - 1, match.end(1) + 1)


def _line_of(source: str, index: int) -> int:
    return source.count("\n", 0, index) + 1


def _original_index(positions: list[int] | None, index: int) -> int:
    """The original-source offset of one logical index."""
    if positions is None:
        return index
    return positions[index]


def _original_line(original: str, positions: list[int] | None, index: int) -> int:
    """The original-source line that carries one logical index."""
    return _line_of(original, _original_index(positions, index))


def _original_span(positions: list[int] | None, start: int, end: int) -> tuple[int, int]:
    """Map a logical [start, end) span onto original-source offsets."""
    if positions is None:
        return start, end
    return positions[start], positions[end - 1] + 1


def _spliced_invocation(original: str, positions: list[int] | None, macro_index: int, close_index: int) -> bool:
    """Whether the original invocation span contains a line splice."""
    if positions is None:
        return False
    start = positions[macro_index]
    return SPLICE_RE.search(original, start, positions[close_index] + 1) is not None


def _skip_line_comment(source: str, start: int) -> int:
    newline = source.find("\n", start + 2)
    return len(source) if newline == -1 else newline + 1


def _skip_block_comment(source: str, start: int) -> int:
    end = source.find("*/", start + 2)
    return len(source) if end == -1 else end + 2


def _skip_string(source: str, start: int) -> int:
    """Skip one ordinary string literal, quotes included."""
    return _skip_quoted(source, start, '"')


def _skip_char(source: str, start: int) -> int:
    """Skip one character literal, quotes included."""
    return _skip_quoted(source, start, "'")


def _skip_quoted(source: str, start: int, quote: str) -> int:
    """Skip one ordinary literal of the given quote kind, bounded at a newline.

    An unescaped newline ends the scan instead of letting an unterminated
    literal (a stray quote or apostrophe in prose such as ``isn't``) swallow
    the rest of the file and hide later registrations. Every lexical walk
    (the registration scan, the raw-string protection, and the invocation
    parser) routes through this one primitive, so the bound fixes all
    callers at once. The logical view already has line splices removed, so a
    newline seen here is a genuine line end; a backslash still escapes the
    next character, which keeps a spliced literal open in the walks that
    see the original text.
    """
    index = start + 1
    while index < len(source):
        char = source[index]
        if char == "\\":
            index += 2
            continue
        if char == quote:
            return index + 1
        if char == "\n":
            return index
        index += 1
    return index


def _skip_char_or_separator(source: str, start: int) -> int:
    """Skip a character literal or one digit-separator apostrophe.

    A preprocessing number starts with a digit or ".digit"; an apostrophe
    inside one is a digit separator (1'000, 0x00FF'FFFF, 0xDEAD'BEEF'CAFE,
    1'000ms). An apostrophe after a keyword or identifier opens a character
    literal (return'"'). Encoded literals such as u8'a' are consumed by the
    identifier branch before this rule applies.
    """
    if start > 0 and _inside_pp_number(source, start):
        return start + 1
    return _skip_char(source, start)


def _inside_pp_number(source: str, apostrophe: int) -> bool:
    """Whether the apostrophe sits in a token that starts like a number.

    The backscan crosses earlier digit separators so the classification
    sees the whole number token: in 0xDEAD'BEEF'CAFE the fragment after the
    first separator (BEEF) does not start with a digit, yet the apostrophe
    is a separator of the surrounding number. The token must still start
    like a number and every earlier separator must be followed by a digit
    of the token, so an apostrophe after a keyword or identifier
    (return'"', isn't) keeps opening a character literal.
    """
    index = apostrophe
    while index > 0 and (source[index - 1].isalnum() or source[index - 1] in "._'"):
        index -= 1
    token = source[index:apostrophe]
    if not token or not (token[0].isdigit() or (token[0] == "." and len(token) > 1 and token[1].isdigit())):
        return False
    return all(token[position + 1 : position + 2].isalnum() for position, char in enumerate(token) if char == "'")


def _skip_invocation_trivia(source: str, start: int) -> int:
    """Skip whitespace and comments between a registration macro and its '('.

    Trivia outside the arguments must not hide a registration call; comments
    inside the invocation are still rejected by the argument parser.
    """
    length = len(source)
    index = start
    while index < length:
        if source.startswith("//", index):
            index = _skip_line_comment(source, index)
        elif source.startswith("/*", index):
            index = _skip_block_comment(source, index)
        elif source[index].isspace():
            index += 1
        else:
            break
    return index


def _skip_raw_string(source: str, start: int) -> int:
    """Skip R"delimiter(...)delimiter"; the delimiter may be empty or custom."""
    open_paren = source.find("(", start + 1)
    if open_paren == -1:
        return len(source)
    delimiter = source[start + 1 : open_paren]
    if delimiter and not all(_is_raw_delimiter_character(character) for character in delimiter):
        # Not a well-formed raw string; keep the ordinary string handling.
        return _skip_string(source, start)
    closing = ")" + delimiter + '"'
    end = source.find(closing, open_paren + 1)
    return len(source) if end == -1 else end + len(closing)


def _is_raw_delimiter_character(character: str) -> bool:
    """One valid d-char: printable, and no space, parenthesis, or backslash."""
    return character.isprintable() and not character.isspace() and character not in "()\\"
