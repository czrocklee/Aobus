"""Opt-in per-phase timing records for CI time attribution.

``measure`` wraps one phase of portal work. Without ``AOBUS_PHASE_LOG_DIR`` it
performs no timing and no I/O, so default runs stay byte-identical. With the
variable set, each process appends strictly allowlisted JSON records to its
own UTF-8 JSONL file in that directory, and ``python -m ao.core.phases``
validates the records and prints normalized rows plus a summary. Records
never contain argv, paths, environment values, or exception text;
``source_id`` is a deterministic SHA-256 digest of a project-relative path,
reproducible from known paths rather than secret anonymization.
"""

from __future__ import annotations

import argparse
import json
import os
import re
import sys
import threading
import time
import uuid
from collections.abc import Collection, Iterator
from contextlib import contextmanager
from contextvars import ContextVar
from pathlib import Path
from typing import Any

from . import command_registry, test_suites

SCHEMA_VERSION = 1
LOG_DIR_ENV = "AOBUS_PHASE_LOG_DIR"
_WRITE_WARNING = "Warning: ao phase log write failed; phase logging disabled for this process."

# Fixed phase dimensions; command and suite identities use their lightweight
# registries below rather than a second handwritten list.
PHASES = frozenset(
    (
        "command",
        "launch-preparation",
        "launch-error",
        "application",
        "configure",
        "build",
        "dependency",
        "generated-headers",
        "checker-build",
        "checker-verify",
        "compile-commands",
        "lint-diagnostics",
        "lint-source-policy",
        "lint-compiler-errors",
        "analysis",
        "analysis-item",
        "suite",
    )
)
TREES = frozenset(("native", "winui", "tidy"))
MODES = frozenset(("STRICT", "RELAXED"))
OUTCOMES = frozenset(("ok", "fail"))
SOURCE_PHASE = "analysis-item"
_PHASE_ID_RE = re.compile(r"\A[0-9a-f]{32}\Z")
_SOURCE_ID_RE = re.compile(r"\A[0-9a-f]{64}\Z")

# Startup nonce: one file per process avoids cross-process append races, and a
# fresh UUID per process makes PID reuse harmless.
_session_id = uuid.uuid4().hex
_write_lock = threading.Lock()
_write_failed = False
_prepared_log_dir: str | None = None
# Parent correlation within one process/task. Worker threads started outside a
# phase record no parent_id; a relation is never invented for them.
_parent_phase: ContextVar[str | None] = ContextVar("ao_phase_parent", default=None)
# Root command context: measures without an explicit ``command`` inherit the
# enclosing root command so nested portal work attributes to it.
_command_context: ContextVar[str | None] = ContextVar("ao_phase_command", default=None)


class _Phase:
    """Mutable holder the with-body assigns; ``status`` 0 means success.

    The property records whether the body assigned ``status`` at all, so an
    explicit native failure keeps its code even when an exception wrapper
    (``die`` raising ``SystemExit``) escapes the block afterwards.
    """

    __slots__ = ("_explicit", "_status")

    def __init__(self) -> None:
        self._status: int = 0
        self._explicit: bool = False

    @property
    def status(self) -> int:
        return self._status

    @status.setter
    def status(self, value: int) -> None:
        self._status = value
        self._explicit = True


def _allowed_commands() -> frozenset[str]:
    """Read command identities without importing command implementations."""
    return command_registry.COMMAND_NAMES


def _check_enum(name: str, value: str | None, allowed: Collection[str]) -> None:
    # Generic message: a caller-supplied value is never echoed.
    if value is not None and (not isinstance(value, str) or value not in allowed):
        raise ValueError(f"unknown phases {name}")


def _check_count(name: str, value: int | None, minimum: int) -> None:
    if value is None:
        return
    if isinstance(value, bool) or not isinstance(value, int) or value < minimum:
        raise ValueError(f"invalid phases {name}")


def _check_source_id(phase: str, source_id: str | None) -> None:
    if source_id is None:
        return
    if phase != SOURCE_PHASE or _SOURCE_ID_RE.fullmatch(source_id) is None:
        raise ValueError("source_id requires the analysis-item phase and a 64-character lowercase hex digest")


def _final_status(value: object) -> int:
    # Signed codes are preserved verbatim: a crashed native step reports its
    # actual code (for example -11), not a normalized 1. A bool or non-int
    # value is a caller bug recorded as failure 1.
    if isinstance(value, bool) or not isinstance(value, int):
        return 1
    return value


def _wrapped_status(state: _Phase, fallback: int) -> int:
    # An explicitly assigned valid signed nonzero status is the native
    # failure contract: it wins over the exception wrapper's exit code. The
    # untouched default 0 falls back to the wrapper mapping, so a root that
    # never records a phase status still measures the process exit code.
    value = state._status
    if state._explicit and isinstance(value, int) and not isinstance(value, bool) and value != 0:
        return value
    return fallback


def _exit_status(exc: SystemExit) -> int:
    # ``None`` is ``sys.exit()`` success; a valid signed int code is preserved
    # verbatim, anything else (string codes, bools) fails as 1.
    code = exc.code
    if code is None:
        return 0
    if isinstance(code, bool) or not isinstance(code, int):
        return 1
    return code


def _write(log_dir: str, record: dict[str, object]) -> None:
    global _write_failed, _prepared_log_dir
    if _write_failed:
        return
    # Encoding does not need the append-order lock. Keep synchronous per-record
    # close/flush semantics instead of introducing a background writer lifetime.
    encoded = json.dumps(record, sort_keys=True) + "\n"
    with _write_lock:
        if _write_failed:
            return
        try:
            directory = os.path.abspath(log_dir)
            if directory != _prepared_log_dir:
                os.makedirs(directory, exist_ok=True)
                _prepared_log_dir = directory
            path = os.path.join(directory, f"phases-{_session_id}.jsonl")
            with open(path, "a", encoding="utf-8") as sink:
                sink.write(encoded)
        except OSError:
            # One generic warning; the failure never masks the wrapped work.
            _write_failed = True
            print(_WRITE_WARNING, file=sys.stderr)


def validate_metadata(
    phase: str,
    *,
    command: str | None = None,
    tree: str | None = None,
    mode: str | None = None,
    suite: str | None = None,
    jobs: int | None = None,
    count: int | None = None,
    source_id: str | None = None,
) -> None:
    """Validate measurement arguments without timing, I/O, or command imports."""
    _check_enum("phase", phase, PHASES)
    if command is not None:
        _check_enum("command", command, _allowed_commands())
    _check_enum("tree", tree, TREES)
    _check_enum("mode", mode, MODES)
    _check_enum("suite", suite, test_suites.SUITES)
    _check_count("jobs", jobs, 1)
    _check_count("count", count, 0)
    _check_source_id(phase, source_id)


@contextmanager
def measure(
    phase: str,
    *,
    command: str | None = None,
    tree: str | None = None,
    mode: str | None = None,
    suite: str | None = None,
    jobs: int | None = None,
    count: int | None = None,
    source_id: str | None = None,
) -> Iterator[_Phase]:
    """Measure one phase; inert without ``AOBUS_PHASE_LOG_DIR``.

    All keyword arguments are optional; callers pass only the dimensions
    relevant to the phase. A measure without ``command`` inherits the command
    context of the enclosing phase, attributing nested work to the root
    command. An explicitly assigned nonzero ``status`` survives an exception
    wrapper (a ``die`` ``SystemExit``), while the untouched default falls back
    to the exception's own code. Invalid values fail fast before any work
    starts.
    """
    validate_metadata(
        phase, command=command, tree=tree, mode=mode, suite=suite, jobs=jobs, count=count, source_id=source_id
    )
    state = _Phase()
    log_dir = os.environ.get(LOG_DIR_ENV, "")
    if not log_dir:
        yield state
        return

    phase_id = uuid.uuid4().hex
    parent_id = _parent_phase.get()
    effective_command = command if command is not None else _command_context.get()
    start_wall = time.time_ns()
    start_mono = time.monotonic_ns()
    token = _parent_phase.set(phase_id)
    command_token = _command_context.set(effective_command)
    begin: dict[str, object] = {
        "schema_version": SCHEMA_VERSION,
        "kind": "BEGIN",
        "session_id": _session_id,
        "pid": os.getpid(),
        "phase_id": phase_id,
        "phase": phase,
        "walltime_start_ns": start_wall,
    }
    if parent_id is not None:
        begin["parent_id"] = parent_id
    for name, value in (
        ("command", effective_command),
        ("tree", tree),
        ("mode", mode),
        ("suite", suite),
        ("jobs", jobs),
        ("count", count),
        ("source_id", source_id),
    ):
        if value is not None:
            begin[name] = value
    _write(log_dir, begin)
    status = 0
    try:
        yield state
        status = _final_status(state.status)
    except SystemExit as exc:
        status = _wrapped_status(state, _exit_status(exc))
        raise
    except KeyboardInterrupt:
        status = 130
        raise
    except BaseException:
        status = _wrapped_status(state, 1)
        raise
    finally:
        _parent_phase.reset(token)
        _command_context.reset(command_token)
        _write(
            log_dir,
            {
                "schema_version": SCHEMA_VERSION,
                "kind": "END",
                "session_id": _session_id,
                "pid": os.getpid(),
                "phase_id": phase_id,
                "phase": phase,
                "walltime_start_ns": start_wall,
                "elapsed_ns": time.monotonic_ns() - start_mono,
                "status": status,
                "outcome": "ok" if status == 0 else "fail",
            },
        )


def _is_hex(value: object, length: int) -> bool:
    return (
        isinstance(value, str) and len(value) == length and all(character in "0123456789abcdef" for character in value)
    )


def _bad_int(value: object, minimum: int) -> bool:
    return isinstance(value, bool) or not isinstance(value, int) or value < minimum


_BEGIN_REQUIRED = frozenset(("schema_version", "kind", "session_id", "pid", "phase_id", "phase", "walltime_start_ns"))
_BEGIN_OPTIONAL = frozenset(("parent_id", "command", "tree", "mode", "suite", "jobs", "count", "source_id"))
_END_REQUIRED = frozenset(
    (
        "schema_version",
        "kind",
        "session_id",
        "pid",
        "phase_id",
        "phase",
        "walltime_start_ns",
        "elapsed_ns",
        "status",
        "outcome",
    )
)


def _row_problem(row: object) -> str | None:
    """Return the contract violation of one parsed row, or None when valid."""
    if not isinstance(row, dict):
        return "row is not a JSON object"
    kind = row.get("kind")
    if kind == "BEGIN":
        required, optional = _BEGIN_REQUIRED, _BEGIN_OPTIONAL
    elif kind == "END":
        required, optional = _END_REQUIRED, frozenset()
    else:
        return "unknown kind"
    keys = set(row)
    if keys - required - optional:
        return "unknown field"
    if required - keys:
        return "missing field"
    version = row["schema_version"]
    if isinstance(version, bool) or not isinstance(version, int) or version != SCHEMA_VERSION:
        return "unsupported schema_version"
    for name in ("session_id", "phase_id"):
        if not _is_hex(row[name], 32):
            return f"invalid {name}"
    if "parent_id" in keys and not _is_hex(row["parent_id"], 32):
        return "invalid parent_id"
    if _bad_int(row["pid"], 1):
        return "invalid pid"
    if not isinstance(row["phase"], str) or row["phase"] not in PHASES:
        return "unknown phase"
    if _bad_int(row["walltime_start_ns"], 0):
        return "invalid walltime_start_ns"
    # Type before membership: unhashable or nested values are invalid rows,
    # never an uncaught TypeError.
    for name, allowed in (("tree", TREES), ("mode", MODES), ("suite", test_suites.SUITES)):
        if name in keys:
            value = row[name]
            if not isinstance(value, str) or value not in allowed:
                return f"unknown {name}"
    if "command" in keys:
        value = row["command"]
        if not isinstance(value, str) or value not in _allowed_commands():
            return "unknown command"
    if "jobs" in keys and _bad_int(row["jobs"], 1):
        return "invalid jobs"
    if "count" in keys and _bad_int(row["count"], 0):
        return "invalid count"
    if "source_id" in keys and (row["phase"] != SOURCE_PHASE or not _is_hex(row["source_id"], 64)):
        return "invalid source_id"
    if kind == "END":
        if _bad_int(row["elapsed_ns"], 0):
            return "invalid elapsed_ns"
        status = row["status"]
        # Signed codes are valid outcomes of crashed native steps; only the
        # type is checked here, never the sign.
        if isinstance(status, bool) or not isinstance(status, int):
            return "invalid status"
        outcome = row["outcome"]
        if not isinstance(outcome, str) or outcome not in OUTCOMES or (status == 0) != (outcome == "ok"):
            return "invalid outcome"
    return None


def _fail(message: str) -> int:
    print(f"Error: {message}", file=sys.stderr)
    return 1


def _load_rows(directory: Path) -> list[dict[str, Any]] | None:
    # Errors identify files by scan ordinal only; a provided path or filename
    # is never echoed because it can carry untrusted content.
    files = sorted(directory.glob("*.jsonl"))
    if not files:
        _fail("no phase log files found")
        return None
    rows: list[dict[str, Any]] = []
    for ordinal, path in enumerate(files, 1):
        try:
            text = path.read_text(encoding="utf-8")
        except (OSError, UnicodeDecodeError):
            _fail(f"unreadable phase log file {ordinal}")
            return None
        if not text:
            _fail(f"empty phase log file {ordinal}")
            return None
        if not text.endswith("\n"):
            _fail(f"truncated terminal record in phase log file {ordinal}")
            return None
        for number, line in enumerate(text.splitlines(), 1):
            if not line:
                _fail(f"blank line in phase log file {ordinal}:{number}")
                return None
            try:
                row = json.loads(line)
            except json.JSONDecodeError:
                _fail(f"malformed phase record in phase log file {ordinal}:{number}")
                return None
            problem = _row_problem(row)
            if problem is not None:
                _fail(f"invalid phase record in phase log file {ordinal}:{number}: {problem}")
                return None
            rows.append(row)
    return rows


def _pair_records(rows: list[dict[str, Any]]) -> list[dict[str, Any]] | None:
    """Correlate BEGIN/END events in file order; None on a broken set.

    Rows are events: a BEGIN must precede its END, a phase id is used at most
    once, and a parent must be an already-begun, still-open phase of the same
    session. Forward-only parent links make cycles unrepresentable; one file
    per session keeps this order authoritative without wallclock checks.
    """
    records: list[dict[str, Any]] = []
    open_begins: dict[tuple[str, str], dict[str, Any]] = {}
    ended: set[tuple[str, str]] = set()
    for row in rows:
        key = (row["session_id"], row["phase_id"])
        if row["kind"] == "BEGIN":
            if key in open_begins or key in ended:
                _fail(f"duplicate BEGIN for phase {row['phase_id']}")
                return None
            parent_id = row.get("parent_id")
            if parent_id is not None:
                if parent_id == row["phase_id"]:
                    _fail(f"self-referencing parent for phase {row['phase_id']}")
                    return None
                parent_key = (row["session_id"], parent_id)
                if parent_key not in open_begins:
                    reason = "closed parent" if parent_key in ended else "unknown parent"
                    _fail(f"{reason} for phase {row['phase_id']}")
                    return None
            open_begins[key] = row
            continue
        if key in ended:
            _fail(f"duplicate END for phase {row['phase_id']}")
            return None
        if key not in open_begins:
            _fail(f"END without BEGIN for phase {row['phase_id']}")
            return None
        begin = open_begins.pop(key)
        ended.add(key)
        if (
            begin["phase"] != row["phase"]
            or begin["pid"] != row["pid"]
            or begin["walltime_start_ns"] != row["walltime_start_ns"]
        ):
            _fail(f"END does not match BEGIN for phase {row['phase_id']}")
            return None
        records.append(
            {name: value for name, value in begin.items() if name != "kind"}
            | {"elapsed_ns": row["elapsed_ns"], "status": row["status"], "outcome": row["outcome"]}
        )
    if open_begins:
        _fail(f"{len(open_begins)} phase record(s) missing END")
        return None
    if not records:
        _fail("no phase records found")
        return None
    return records


def _ms(elapsed_ns: int) -> str:
    return f"{elapsed_ns / 1_000_000:.1f}"


def _summary(records: list[dict[str, Any]]) -> str:
    stats: dict[tuple[str, ...], list[int]] = {}
    for record in records:
        key = (
            record.get("command", "-"),
            record["phase"],
            record.get("tree", "-"),
            record.get("mode", "-"),
            record.get("suite", "-"),
        )
        entry = stats.setdefault(key, [0, 0, 0, 0, 0, 0])
        entry[0] += 1
        entry[1] += 1 if record["outcome"] == "fail" else 0
        entry[2] += record["elapsed_ns"]
        entry[3] = max(entry[3], record["elapsed_ns"])
        if "count" in record:
            entry[4] += record["count"]
            entry[5] += 1
    lines = [
        "## ao phase measurements",
        "",
        "| command | phase | tree | mode | suite | records | items | failed | total ms | max ms |",
        "|---|---|---|---|---|---:|---:|---:|---:|---:|",
    ]
    for group in sorted(stats):
        records_count, failed, total_ns, max_ns, total_items, counted_records = stats[group]
        item_count = str(total_items) if counted_records == records_count else "-"
        lines.append(
            f"| {' | '.join(group)} | {records_count} | {item_count} | {failed} | {_ms(total_ns)} | {_ms(max_ns)} |"
        )
    lines.extend(
        (
            "",
            "Phase rows overlap through nesting and parallel workers; they are wall-clock",
            "measurements, not additive job time.",
            "The records column counts phase spans; items sums work counts only when every",
            "record in the group specifies one. A dash means unspecified or partial counts.",
            "",
            f"{len(records)} phase record(s) measured.",
        )
    )
    items = [record for record in records if record["phase"] == SOURCE_PHASE and "source_id" in record]
    items.sort(key=lambda record: (-record["elapsed_ns"], record["source_id"]))
    if items:
        lines.extend(("", "### Slowest analysis items", "", "| source_id | ms | outcome |", "|---|---:|---|"))
        for record in items[:10]:
            lines.append(f"| {record['source_id']} | {_ms(record['elapsed_ns'])} | {record['outcome']} |")
    return "\n".join(lines) + "\n"


def _report(directory: Path, summary_path: Path | None) -> int:
    if not directory.is_dir():
        return _fail("phase log directory not found")
    rows = _load_rows(directory)
    if rows is None:
        return 1
    records = _pair_records(rows)
    if records is None:
        return 1
    records.sort(key=lambda record: (record["walltime_start_ns"], record["phase_id"]))
    for record in records:
        print("AO_PHASE_RECORD " + json.dumps(record, sort_keys=True))
    summary = _summary(records)
    print(summary, end="")
    if summary_path is not None:
        try:
            with summary_path.open("a", encoding="utf-8") as sink:
                sink.write(summary)
        except OSError:
            return _fail("cannot write summary file")
    return 0


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        prog="python -m ao.core.phases",
        description="Validate phase measurement logs and print normalized records plus a summary.",
    )
    parser.add_argument("directory", type=Path, help="directory containing per-process phases-*.jsonl logs")
    parser.add_argument(
        "--github-summary",
        type=Path,
        default=None,
        help="append the Markdown summary to this file (for GITHUB_STEP_SUMMARY)",
    )
    arguments = parser.parse_args(argv)
    return _report(arguments.directory, arguments.github_summary)


if __name__ == "__main__":
    sys.exit(main())
