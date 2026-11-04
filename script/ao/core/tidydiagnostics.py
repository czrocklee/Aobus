"""Classify clang and clang-tidy diagnostic headers in captured tool output.

The lint fixture runner, the tidy engine and cacheable Bazel lint reports share
this parsing, so a classified log means the same thing in every route.
"""

import re
from dataclasses import dataclass
from pathlib import Path

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
    r"^(?P<path>.+?):\d+:\d+:\s*(?P<severity>fatal error|error|warning|note|remark)(?![\w-])"
)
_MSVC_LOCATION_SEVERITY_RE = re.compile(
    r"^(?P<path>.+?)\(\d+(?:,\d+)?\):\s*(?P<severity>fatal error|error|warning|note|remark)(?![\w-])"
)


@dataclass(frozen=True)
class DiagnosticHeader:
    """The first diagnostic header starting one line of captured tool output."""

    severity: str
    text: str
    # Only the selected first header supplies a location. Bare/driver messages
    # can quote locations without becoming located diagnostics themselves.
    path: str | None = None

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
    return DiagnosticHeader(severity=match.group("severity"), text=line, path=match.groupdict().get("path"))


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


_COMPILE_COMMAND_ERROR_RE = re.compile(
    r"(compile command not found|error while trying to load a compilation database|"
    r"could not auto-detect compilation database)",
    re.IGNORECASE,
)


def log_has_compile_command_error(log: Path) -> bool:
    """Detect clang-tidy database failures even when the tool exits successfully."""
    try:
        return bool(_COMPILE_COMMAND_ERROR_RE.search(log.read_text(encoding="utf-8", errors="replace")))
    except OSError:
        return False
