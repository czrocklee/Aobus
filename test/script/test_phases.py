"""Phase measurement records and the consumer CLI."""

import json
import os
import subprocess
import sys
import tempfile
import threading
import time
import unittest
from pathlib import Path
from unittest import mock

from ao import command
from ao.core import command_registry, phases, test_suites
from ao.core.proc import die

ROOT = Path(__file__).resolve().parents[2]
ENV = {**os.environ, "PYTHONPATH": str(ROOT / "script")}
CANARY = "CANARY-SECRET-do-not-print"


def child_env(log_dir: str | Path | None) -> dict[str, str]:
    env = dict(ENV)
    if log_dir is None:
        env.pop(phases.LOG_DIR_ENV, None)
    else:
        env[phases.LOG_DIR_ENV] = str(log_dir)
    return env


def run_child(source: str, log_dir: str | Path | None) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        [sys.executable, "-c", source], env=child_env(log_dir), capture_output=True, text=True, timeout=60
    )


def run_cli(*arguments: str | Path) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        [sys.executable, "-m", "ao.core.phases", *[str(argument) for argument in arguments]],
        env=child_env(None),
        capture_output=True,
        text=True,
        timeout=60,
    )


class RecordFixture:
    """Shared helpers for tests that read or produce real records."""

    def enable_logging(self, directory: str | Path) -> None:
        original = os.environ.get(phases.LOG_DIR_ENV)
        os.environ[phases.LOG_DIR_ENV] = str(directory)

        def restore() -> None:
            if original is None:
                os.environ.pop(phases.LOG_DIR_ENV, None)
            else:
                os.environ[phases.LOG_DIR_ENV] = original

        self.addCleanup(restore)

    def disable_logging(self) -> None:
        original = os.environ.pop(phases.LOG_DIR_ENV, None)

        def restore() -> None:
            if original is not None:
                os.environ[phases.LOG_DIR_ENV] = original

        self.addCleanup(restore)

    def log_rows(self, directory: str | Path) -> list[dict]:
        rows = []
        for path in sorted(Path(directory).glob("*.jsonl")):
            for line in path.read_text(encoding="utf-8").splitlines():
                self.assertTrue(line, f"blank line in {path.name}")
                rows.append(json.loads(line))
        return rows


class MeasureDisabledTest(RecordFixture, unittest.TestCase):
    def test_absent_log_dir_leaves_output_byte_identical_and_writes_nothing(self):
        wrapped = (
            "import sys\n"
            "from ao.core import phases\n"
            "with phases.measure('command', command='check', tree='native', jobs=8, count=3,\n"
            "                    mode='STRICT', suite='core') as phase:\n"
            "    print('work output')\n"
            "    phase.status = 0\n"
            "sys.exit(phase.status)\n"
        )
        baseline = run_child("print('work output')", None)
        result = run_child(wrapped, None)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stdout, baseline.stdout)
        self.assertEqual(result.stderr, "")
        self.disable_logging()
        with mock.patch.object(phases, "_write") as write:
            with phases.measure("command", command="check") as phase:
                phase.status = 4
        write.assert_not_called()


class MeasureRecordTest(RecordFixture, unittest.TestCase):
    def test_enabled_writes_paired_records_with_only_relevant_dimensions(self):
        with tempfile.TemporaryDirectory() as temporary:
            self.enable_logging(temporary)
            with phases.measure(
                "configure", command="check", tree="native", jobs=8, count=3, mode="STRICT", suite="core"
            ):
                pass
            with phases.measure("build") as phase:
                phase.status = 0
            rows = self.log_rows(temporary)
            self.assertEqual([row["kind"] for row in rows], ["BEGIN", "END", "BEGIN", "END"])
            full, minimal = rows[0], rows[2]
            self.assertEqual(full["phase"], "configure")
            self.assertEqual(full["command"], "check")
            self.assertEqual(full["tree"], "native")
            self.assertEqual(full["jobs"], 8)
            self.assertEqual(full["count"], 3)
            self.assertEqual(full["mode"], "STRICT")
            self.assertEqual(full["suite"], "core")
            self.assertEqual(
                set(minimal), {"schema_version", "kind", "session_id", "pid", "phase_id", "phase", "walltime_start_ns"}
            )
            for row in rows:
                self.assertEqual(row["schema_version"], 1)
                self.assertEqual(row["pid"], os.getpid())
                self.assertRegex(row["session_id"], r"\A[0-9a-f]{32}\Z")
                self.assertRegex(row["phase_id"], r"\A[0-9a-f]{32}\Z")
                self.assertNotIn("parent_id", row)
                self.assertGreaterEqual(row["walltime_start_ns"], 0)
            for end in rows[1::2]:
                self.assertEqual(
                    set(end),
                    {
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
                    },
                )
                self.assertEqual(end["status"], 0)
                self.assertEqual(end["outcome"], "ok")
                self.assertGreaterEqual(end["elapsed_ns"], 0)
            self.assertNotIn("elapsed_ns", rows[0])
            self.assertEqual(rows[1]["phase_id"], rows[0]["phase_id"])
            self.assertNotEqual(rows[3]["phase_id"], rows[1]["phase_id"])

    def test_nonzero_status_flags_failure(self):
        with tempfile.TemporaryDirectory() as temporary:
            self.enable_logging(temporary)
            with phases.measure("build") as phase:
                phase.status = 7
            end = self.log_rows(temporary)[1]
            self.assertEqual(end["status"], 7)
            self.assertEqual(end["outcome"], "fail")

    def test_system_exit_int_code_is_recorded_and_preserved(self):
        with tempfile.TemporaryDirectory() as temporary:
            self.enable_logging(temporary)
            with self.assertRaises(SystemExit) as caught:
                with phases.measure("command"):
                    raise SystemExit(3)
            self.assertEqual(caught.exception.code, 3)
            end = self.log_rows(temporary)[1]
            self.assertEqual(end["status"], 3)
            self.assertEqual(end["outcome"], "fail")

    def test_system_exit_none_and_string_codes_map_to_success_and_failure(self):
        with tempfile.TemporaryDirectory() as temporary:
            self.enable_logging(temporary)
            with self.assertRaises(SystemExit) as caught:
                with phases.measure("command"):
                    sys.exit()
            self.assertIsNone(caught.exception.code)
            with self.assertRaises(SystemExit) as caught:
                with phases.measure("command"):
                    raise SystemExit("message text")
            self.assertEqual(caught.exception.code, "message text")
            ends = [row for row in self.log_rows(temporary) if row["kind"] == "END"]
            self.assertEqual([end["status"] for end in ends], [0, 1])
            self.assertEqual([end["outcome"] for end in ends], ["ok", "fail"])

    def test_keyboard_interrupt_records_130_and_reraises(self):
        with tempfile.TemporaryDirectory() as temporary:
            self.enable_logging(temporary)
            with self.assertRaises(KeyboardInterrupt):
                with phases.measure("suite"):
                    raise KeyboardInterrupt
            end = self.log_rows(temporary)[1]
            self.assertEqual(end["status"], 130)
            self.assertEqual(end["outcome"], "fail")

    def test_base_exception_records_failure_reraises_and_hides_details(self):
        with tempfile.TemporaryDirectory() as temporary:
            self.enable_logging(temporary)
            with self.assertRaises(ValueError):
                with phases.measure("suite"):
                    raise ValueError("boom detail")
            rows = self.log_rows(temporary)
            end = rows[1]
            self.assertEqual(end["status"], 1)
            self.assertEqual(end["outcome"], "fail")
            self.assertNotIn("boom detail", json.dumps(rows))

    def test_nested_measure_links_parent_and_restores_context(self):
        with tempfile.TemporaryDirectory() as temporary:
            self.enable_logging(temporary)
            with phases.measure("command", command="check") as outer:
                with phases.measure("suite") as inner:
                    inner.status = 0
                outer.status = 0
            with phases.measure("suite"):
                pass
            rows = self.log_rows(temporary)
            begins = [row for row in rows if row["kind"] == "BEGIN"]
            outer_begin, inner_begin, sibling = begins
            self.assertNotIn("parent_id", outer_begin)
            self.assertEqual(inner_begin["parent_id"], outer_begin["phase_id"])
            self.assertNotIn("parent_id", sibling)
            self.assertEqual(inner_begin["command"], "check")
            self.assertNotIn("command", sibling)

    def test_unparented_worker_thread_records_no_parent(self):
        with tempfile.TemporaryDirectory() as temporary:
            self.enable_logging(temporary)
            done = threading.Event()
            started = threading.Event()

            def work() -> None:
                started.set()
                with phases.measure("analysis-item"):
                    done.wait(10)
                done.set()

            thread = threading.Thread(target=work)
            thread.start()
            started.wait(10)
            with phases.measure("suite"):
                pass
            done.set()
            thread.join(10)
            self.assertFalse(thread.is_alive())
            begins = [row for row in self.log_rows(temporary) if row["kind"] == "BEGIN"]
            by_phase = {row["phase"]: row for row in begins}
            self.assertNotIn("parent_id", by_phase["analysis-item"])
            self.assertNotIn("parent_id", by_phase["suite"])
            self.assertNotIn("command", by_phase["analysis-item"])

    def test_explicit_native_status_survives_system_exit_wrapper(self):
        # Real caller pattern: winuitidy records phase.status = 3, then die()
        # raises SystemExit(1) inside the measure block. The native failure is
        # the metric; the wrapper exit code is still rethrown unchanged.
        with tempfile.TemporaryDirectory() as temporary:
            self.enable_logging(temporary)
            with self.assertRaises(SystemExit) as caught:
                with phases.measure("compile-commands", tree="winui") as phase:
                    phase.status = 3
                    raise die("native checker failed")
            self.assertEqual(caught.exception.code, 1)
            rows = self.log_rows(temporary)
            end = rows[1]
            self.assertEqual(end["status"], 3)
            self.assertEqual(end["outcome"], "fail")
            self.assertNotIn("native checker failed", json.dumps(rows))

    def test_explicit_status_survives_ordinary_exception(self):
        with tempfile.TemporaryDirectory() as temporary:
            self.enable_logging(temporary)
            with self.assertRaises(ValueError):
                with phases.measure("suite") as phase:
                    phase.status = 5
                    raise ValueError("boom detail")
            end = self.log_rows(temporary)[1]
            self.assertEqual(end["status"], 5)
            self.assertEqual(end["outcome"], "fail")

    def test_explicit_zero_or_invalid_status_falls_back_to_wrapper_code(self):
        with tempfile.TemporaryDirectory() as temporary:
            self.enable_logging(temporary)
            with self.assertRaises(SystemExit):
                with phases.measure("command") as phase:
                    phase.status = 0
                    raise SystemExit(3)
            with self.assertRaises(SystemExit):
                with phases.measure("command") as phase:
                    phase.status = True
                    raise SystemExit(2)
            ends = [row for row in self.log_rows(temporary) if row["kind"] == "END"]
            self.assertEqual([end["status"] for end in ends], [3, 2])

    def test_negative_status_records_actual_signed_code(self):
        with tempfile.TemporaryDirectory() as temporary:
            self.enable_logging(temporary)
            with phases.measure("suite") as phase:
                # -11: a crashed native step reports its actual code, not a normalized 1.
                phase.status = -11
            with self.assertRaises(SystemExit) as caught:
                with phases.measure("build"):
                    raise SystemExit(-11)
            self.assertEqual(caught.exception.code, -11)
            ends = [row for row in self.log_rows(temporary) if row["kind"] == "END"]
            self.assertEqual([end["status"] for end in ends], [-11, -11])
            self.assertEqual([end["outcome"] for end in ends], ["fail", "fail"])

    def test_bool_status_is_invalid_and_recorded_as_failure(self):
        with tempfile.TemporaryDirectory() as temporary:
            self.enable_logging(temporary)
            with phases.measure("suite") as phase:
                phase.status = True
            end = self.log_rows(temporary)[1]
            self.assertEqual(end["status"], 1)
            self.assertEqual(end["outcome"], "fail")

    def test_command_allowlist_projects_from_the_lightweight_registry(self):
        self.assertEqual(command_registry.COMMAND_NAMES, frozenset(module.NAME for module in command.COMMAND_MODULES))
        self.assertIs(phases._allowed_commands(), command_registry.COMMAND_NAMES)
        with tempfile.TemporaryDirectory() as temporary:
            self.enable_logging(temporary)
            with mock.patch.object(
                command_registry, "COMMAND_NAMES", command_registry.COMMAND_NAMES | {"future-command"}
            ):
                with phases.measure("command", command="future-command"):
                    pass
            begin = self.log_rows(temporary)[0]
            self.assertEqual(begin["command"], "future-command")

    def test_new_suite_uses_one_registry_with_logging_enabled_or_disabled(self):
        from ao.command import test as test_command

        self.assertIs(test_command.SUITES, test_suites.SUITES)
        future = test_suites.SuiteSpec("Future", "tooling")
        for enabled in (False, True):
            with self.subTest(logging=enabled), tempfile.TemporaryDirectory() as temporary:
                if enabled:
                    self.enable_logging(temporary)
                else:
                    self.disable_logging()
                with (
                    mock.patch.dict(test_suites.SUITES, {"future-suite": future}),
                    mock.patch.object(test_command, "suite_shards", return_value=1),
                    mock.patch.object(test_command, "run_non_catch2_suite", return_value=0) as runner,
                ):
                    self.assertEqual(test_command.run_suites(("future-suite",), Path(temporary)), 0)
                    runner.assert_called_once()
                    if enabled:
                        rows = phases._load_rows(Path(temporary))
                        self.assertIsNotNone(rows)
                        records = phases._pair_records(rows)
                        self.assertEqual(records[0]["suite"], "future-suite")
                    else:
                        self.assertFalse(list(Path(temporary).glob("*.jsonl")))

    def test_source_id_is_recorded_only_for_analysis_items(self):
        digest = "a" * 64
        with tempfile.TemporaryDirectory() as temporary:
            self.enable_logging(temporary)
            with phases.measure("analysis-item", source_id=digest):
                pass
            with phases.measure("analysis-item"):
                pass
            begins = [row for row in self.log_rows(temporary) if row["kind"] == "BEGIN"]
            self.assertEqual(begins[0]["source_id"], digest)
            self.assertNotIn("source_id", begins[1])

    def test_invalid_arguments_fail_fast_without_logging(self):
        with tempfile.TemporaryDirectory() as temporary:
            log_dir = Path(temporary)
            self.enable_logging(log_dir)
            for name, kwargs in (
                ("phase", {"phase": "bogus"}),
                ("tree", {"phase": "build", "tree": "bogus"}),
                ("mode", {"phase": "build", "mode": "bogus"}),
                ("suite", {"phase": "build", "suite": "bogus"}),
                ("command", {"phase": "build", "command": "bogus"}),
                ("zero jobs", {"phase": "build", "jobs": 0}),
                ("negative jobs", {"phase": "build", "jobs": -1}),
                ("bool jobs", {"phase": "build", "jobs": True}),
                ("negative count", {"phase": "build", "count": -1}),
                ("bool count", {"phase": "build", "count": True}),
                ("source_id phase", {"phase": "build", "source_id": "a" * 64}),
                ("source_id shape", {"phase": "analysis-item", "source_id": "A" * 64}),
            ):
                with self.subTest(name=name), self.assertRaises(ValueError):
                    with phases.measure(**kwargs):
                        pass
            self.assertFalse(list(log_dir.glob("*.jsonl")))


class MeasureProcessTest(unittest.TestCase):
    def test_system_exit_propagates_through_process_boundary(self):
        child = "import sys\nfrom ao.core import phases\nwith phases.measure('command'):\n    sys.exit(3)\n"
        with tempfile.TemporaryDirectory() as temporary:
            result = run_child(child, temporary)
            self.assertEqual(result.returncode, 3, result.stderr)
            rows = [
                json.loads(line)
                for path in sorted(Path(temporary).glob("*.jsonl"))
                for line in path.read_text(encoding="utf-8").splitlines()
            ]
            self.assertEqual([row["kind"] for row in rows], ["BEGIN", "END"])
            self.assertEqual(rows[1]["status"], 3)
            self.assertEqual(rows[1]["outcome"], "fail")

    def test_end_write_failure_preserves_success_but_the_report_stays_strict(self):
        child = """
import subprocess
import sys
from unittest import mock
from ao.core import phases
real_open = open
attempts = 0
def fail_end(*args, **kwargs):
    global attempts
    attempts += 1
    if attempts == 2:
        raise OSError('private END append failure')
    return real_open(*args, **kwargs)
with mock.patch.object(phases, 'open', side_effect=fail_end, create=True):
    with phases.measure('command', command='check') as phase:
        phase.status = subprocess.run([sys.executable, '-c', 'raise SystemExit(0)']).returncode
    with phases.measure('build'):
        pass
assert attempts == 2
sys.exit(phase.status)
"""
        with tempfile.TemporaryDirectory() as temporary:
            result = run_child(child, temporary)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(result.stderr.count(phases._WRITE_WARNING), 1)
            self.assertNotIn("private END append failure", result.stderr)
            rows = [
                json.loads(line)
                for path in Path(temporary).glob("*.jsonl")
                for line in path.read_text(encoding="utf-8").splitlines()
            ]
            self.assertEqual([row["kind"] for row in rows], ["BEGIN"])
            report = run_cli(temporary)
            self.assertEqual(report.returncode, 1)
            self.assertIn("1 phase record(s) missing END", report.stderr)

    def test_disk_failure_does_not_mask_work_or_leak_paths(self):
        with tempfile.TemporaryDirectory() as temporary:
            blocked = Path(temporary) / "blocked"
            blocked.write_text("not a directory\n", encoding="utf-8")
            child = (
                "import sys\n"
                "from ao.core import phases\n"
                "with phases.measure('command') as phase:\n"
                "    print('work output')\n"
                "    phase.status = 7\n"
                "sys.exit(phase.status)\n"
            )
            result = run_child(child, str(blocked))
            self.assertEqual(result.returncode, 7, result.stderr)
            self.assertEqual(result.stdout, "work output\n")
            self.assertEqual(result.stderr.count("Warning"), 1)
            self.assertNotIn(str(blocked), result.stderr)
            self.assertNotIn("OSError", result.stderr)

    def test_two_processes_write_distinct_files_and_sessions(self):
        with tempfile.TemporaryDirectory() as temporary:
            first = "from ao.core import phases\nwith phases.measure('build'):\n    pass\n"
            second = "from ao.core import phases\nwith phases.measure('configure'):\n    pass\n"
            self.assertEqual(run_child(first, temporary).returncode, 0)
            self.assertEqual(run_child(second, temporary).returncode, 0)
            files = sorted(Path(temporary).glob("*.jsonl"))
            self.assertEqual(len(files), 2)
            sessions = set()
            for path in files:
                rows = [json.loads(line) for line in path.read_text(encoding="utf-8").splitlines()]
                self.assertEqual([row["kind"] for row in rows], ["BEGIN", "END"])
                sessions.add(rows[0]["session_id"])
            self.assertEqual(len(sessions), 2)
            self.assertEqual(run_cli(temporary).returncode, 0)


class PhaseWriterTest(RecordFixture, unittest.TestCase):
    def test_directory_preparation_is_amortized_and_records_are_closed(self):
        with tempfile.TemporaryDirectory() as temporary:
            self.enable_logging(temporary)
            with mock.patch.object(phases.os, "makedirs", wraps=os.makedirs) as mkdir:
                for _ in range(3):
                    with phases.measure("analysis-item"):
                        pass
            self.assertEqual(mkdir.call_count, 1)
            rows = self.log_rows(temporary)
            self.assertEqual(len(rows), 6)
            self.assertIsNotNone(phases._pair_records(rows))
            self.assertEqual(run_cli(temporary).returncode, 0)

    def test_serialization_does_not_hold_the_append_order_lock(self):
        class ObservedLock:
            held = False

            def __enter__(self):
                self.held = True

            def __exit__(self, *_):
                self.held = False

        lock = ObservedLock()
        encode = json.dumps

        def encode_without_lock(*args, **kwargs):
            self.assertFalse(lock.held)
            return encode(*args, **kwargs)

        with tempfile.TemporaryDirectory() as temporary:
            self.enable_logging(temporary)
            with (
                mock.patch.object(phases, "_write_lock", lock),
                mock.patch.object(phases.json, "dumps", side_effect=encode_without_lock),
            ):
                with phases.measure("analysis-item"):
                    pass
            self.assertEqual(len(self.log_rows(temporary)), 2)

    def test_directory_changes_do_not_reuse_another_destinations_preparation(self):
        with tempfile.TemporaryDirectory() as temporary:
            first = Path(temporary) / "first"
            second = Path(temporary) / "second"
            for directory in (first, second, first):
                self.enable_logging(directory)
                with phases.measure("build"):
                    pass
            self.assertEqual(len(self.log_rows(first)), 4)
            self.assertEqual(len(self.log_rows(second)), 2)
            self.assertEqual(run_cli(first).returncode, 0)
            self.assertEqual(run_cli(second).returncode, 0)


def fixture_row(kind: str, phase: str = "suite", phase_id: str = "a" * 32, **extra: object) -> dict:
    row: dict[str, object] = {
        "schema_version": 1,
        "kind": kind,
        "session_id": "b" * 32,
        "pid": 4242,
        "phase_id": phase_id,
        "phase": phase,
        "walltime_start_ns": 1_000_000_000,
    }
    if kind == "END":
        row.update({"elapsed_ns": 5_000_000, "status": 0, "outcome": "ok"})
    row.update(extra)
    return row


def write_rows(directory: Path, *rows: dict) -> None:
    content = "".join(json.dumps(row, sort_keys=True) + "\n" for row in rows)
    (directory / "phases-fixture.jsonl").write_text(content, encoding="utf-8")


class PhaseReportTest(RecordFixture, unittest.TestCase):
    def test_reports_complete_measurements_with_failed_phases(self):
        with tempfile.TemporaryDirectory() as temporary:
            self.enable_logging(temporary)
            with phases.measure("build", command="check", tree="native", mode="STRICT", suite="core") as outer:
                with phases.measure("suite") as inner:
                    inner.status = 3
                outer.status = 0
            result = run_cli(temporary)
            self.assertEqual(result.returncode, 0, result.stderr)
            records = [
                json.loads(line.removeprefix("AO_PHASE_RECORD "))
                for line in result.stdout.splitlines()
                if line.startswith("AO_PHASE_RECORD ")
            ]
            self.assertEqual(len(records), 2)
            self.assertTrue(all("kind" not in record for record in records))
            by_phase = {record["phase"]: record for record in records}
            self.assertEqual(by_phase["suite"]["status"], 3)
            self.assertEqual(by_phase["suite"]["outcome"], "fail")
            self.assertEqual(by_phase["suite"]["parent_id"], by_phase["build"]["phase_id"])
            self.assertIn(
                "| command | phase | tree | mode | suite | records | items | failed | total ms | max ms |",
                result.stdout,
            )
            self.assertIn("| check | suite | - | - | - | 1 | - | 1 |", result.stdout)
            self.assertIn("not additive job time", result.stdout)
            self.assertIn("2 phase record(s) measured.", result.stdout)

    def test_summary_separates_records_from_processed_items(self):
        with tempfile.TemporaryDirectory() as temporary:
            self.enable_logging(temporary)
            with phases.measure("analysis", command="tidy", tree="tidy", count=500):
                pass
            result = run_cli(temporary)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertIn("| tidy | analysis | tidy | - | - | 1 | 500 | 0 |", result.stdout)
            records = [
                json.loads(line.removeprefix("AO_PHASE_RECORD "))
                for line in result.stdout.splitlines()
                if line.startswith("AO_PHASE_RECORD ")
            ]
            self.assertEqual(records[0]["count"], 500)

    def test_summary_omits_partial_item_totals_and_retains_zero(self):
        with tempfile.TemporaryDirectory() as temporary:
            self.enable_logging(temporary)
            for count in (3, None):
                with phases.measure("analysis", command="tidy", count=count):
                    pass
            with phases.measure("generated-headers", command="tidy", count=0):
                pass
            result = run_cli(temporary)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertIn("| tidy | analysis | - | - | - | 2 | - | 0 |", result.stdout)
            self.assertIn("| tidy | generated-headers | - | - | - | 1 | 0 | 0 |", result.stdout)

    def test_report_does_not_import_command_implementations(self):
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            write_rows(directory, fixture_row("BEGIN", command="check", suite="core"), fixture_row("END"))
            child = (
                "import builtins, sys\n"
                "original = builtins.__import__\n"
                "def guarded(name, *args, **kwargs):\n"
                "    if name == 'command' or name.startswith('ao.command'):\n"
                "        raise AssertionError('reporter imported command implementations')\n"
                "    return original(name, *args, **kwargs)\n"
                "builtins.__import__ = guarded\n"
                "from ao.core import phases\n"
                "assert not any(name.startswith('ao.command') for name in sys.modules)\n"
                f"raise SystemExit(phases.main([{str(directory)!r}]))\n"
            )
            result = run_child(child, None)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertIn("AO_PHASE_RECORD", result.stdout)

    def test_summary_lists_slowest_analysis_items_by_source_id(self):
        digests = {delay: f"{index:064x}" for index, delay in enumerate((0.02, 0.06, 0.12), 1)}
        with tempfile.TemporaryDirectory() as temporary:
            self.enable_logging(temporary)
            with phases.measure("analysis"):
                for delay in (0.02, 0.06, 0.12):
                    with phases.measure("analysis-item", source_id=digests[delay]):
                        time.sleep(delay)
                with phases.measure("analysis-item"):
                    time.sleep(0.02)
            result = run_cli(temporary)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertIn("### Slowest analysis items", result.stdout)
            table = result.stdout.split("### Slowest analysis items", 1)[1]
            listed = [line for line in table.splitlines() if line.startswith("| ") and "source_id" not in line]
            self.assertEqual(len(listed), 3)
            self.assertTrue(listed[0].startswith(f"| {digests[0.12]} |"))
            self.assertTrue(listed[2].startswith(f"| {digests[0.02]} |"))
            self.assertIn(f"| {digests[0.06]} | ", result.stdout)

    def test_fails_when_directory_is_missing_or_empty(self):
        with tempfile.TemporaryDirectory() as temporary:
            missing = run_cli(Path(temporary) / "absent")
            self.assertNotEqual(missing.returncode, 0)
            empty = run_cli(temporary)
            self.assertNotEqual(empty.returncode, 0)

    def test_accepts_negative_status_from_crashed_native_steps(self):
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            write_rows(directory, fixture_row("BEGIN"), fixture_row("END", status=-11, outcome="fail"))
            result = run_cli(directory)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertIn('"status": -11', result.stdout)

    def test_fails_on_truncated_and_malformed_rows_without_echoing_them(self):
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            write_rows(directory, fixture_row("BEGIN"), fixture_row("END"))
            with (directory / "phases-fixture.jsonl").open("a", encoding="utf-8") as sink:
                sink.write('{"schema_version": 1, "kind": "BEGIN", "canary": "' + CANARY)
            terminal = run_cli(directory)
            self.assertNotEqual(terminal.returncode, 0)
            self.assertIn("truncated terminal record", terminal.stderr)
            self.assertNotIn(CANARY, terminal.stdout)
            self.assertNotIn(CANARY, terminal.stderr)
            write_rows(directory, fixture_row("BEGIN"), fixture_row("END"))
            with (directory / "phases-fixture.jsonl").open("a", encoding="utf-8") as sink:
                sink.write('{"canary": "' + CANARY + "\n")
                sink.write(json.dumps(fixture_row("END", phase_id="d" * 32), sort_keys=True) + "\n")
            malformed = run_cli(directory)
            self.assertNotEqual(malformed.returncode, 0)
            self.assertIn("malformed phase record", malformed.stderr)
            self.assertNotIn(CANARY, malformed.stdout)
            self.assertNotIn(CANARY, malformed.stderr)

    def test_fails_on_empty_or_blank_log_files(self):
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            write_rows(directory, fixture_row("BEGIN"), fixture_row("END"))
            blank_file = directory / "phases-empty.jsonl"
            blank_file.write_text("", encoding="utf-8")
            empty = run_cli(directory)
            self.assertNotEqual(empty.returncode, 0)
            self.assertIn("empty phase log file", empty.stderr)
            blank_file.write_text("\n", encoding="utf-8")
            blank = run_cli(directory)
            self.assertNotEqual(blank.returncode, 0)
            self.assertIn("blank line", blank.stderr)

    def test_fails_on_unpaired_begin(self):
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            write_rows(directory, fixture_row("BEGIN"), fixture_row("END"), fixture_row("BEGIN", phase_id="c" * 32))
            result = run_cli(directory)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("missing END", result.stderr)

    def test_fails_on_invalid_timing_and_status_fields(self):
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            cases = (
                ("string status", fixture_row("END", status="7"), "invalid status"),
                ("bool status", fixture_row("END", status=True), "invalid status"),
                ("negative elapsed", fixture_row("END", elapsed_ns=-1), "invalid elapsed_ns"),
                ("outcome mismatch", fixture_row("END", status=3, outcome="ok"), "invalid outcome"),
                ("bool jobs", fixture_row("BEGIN", jobs=True), "invalid jobs"),
                ("zero jobs", fixture_row("BEGIN", jobs=0), "invalid jobs"),
                ("negative count", fixture_row("BEGIN", count=-1), "invalid count"),
                ("count on END", fixture_row("END", count=1), "unknown field"),
                ("float schema", fixture_row("BEGIN", schema_version=1.0), "unsupported schema_version"),
            )
            for name, row, reason in cases:
                with self.subTest(name=name):
                    write_rows(directory, fixture_row("BEGIN"), row)
                    result = run_cli(directory)
                    self.assertNotEqual(result.returncode, 0)
                    self.assertIn(reason, result.stderr)

    def test_fails_on_untrusted_rows_without_echoing_secrets(self):
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            cases = (
                ("unknown field", fixture_row("BEGIN", canary=CANARY)),
                ("canary session", fixture_row("BEGIN", session_id=CANARY)),
                ("canary phase", fixture_row("BEGIN", phase=CANARY)),
                ("canary source", fixture_row("BEGIN", phase="analysis-item", source_id=CANARY)),
                ("source off phase", fixture_row("BEGIN", phase="build", source_id="a" * 64)),
                ("canary parent", fixture_row("BEGIN", parent_id=CANARY)),
                ("bad schema", fixture_row("BEGIN", schema_version=2)),
                ("end with begin field", fixture_row("END", command=CANARY)),
                ("unhashable phase", fixture_row("BEGIN", phase=[CANARY])),
                ("nested command", fixture_row("BEGIN", command={CANARY: 1})),
                ("unhashable source", fixture_row("BEGIN", phase="analysis-item", source_id=[CANARY])),
            )
            for name, row in cases:
                with self.subTest(name=name):
                    write_rows(directory, fixture_row("BEGIN"), row)
                    result = run_cli(directory)
                    self.assertNotEqual(result.returncode, 0)
                    self.assertNotIn(CANARY, result.stdout)
                    self.assertNotIn(CANARY, result.stderr)

    def test_fails_on_duplicate_phase_ids_even_after_end(self):
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            write_rows(directory, fixture_row("BEGIN"), fixture_row("END"), fixture_row("BEGIN"))
            repeated_begin = run_cli(directory)
            self.assertNotEqual(repeated_begin.returncode, 0)
            self.assertIn("duplicate BEGIN", repeated_begin.stderr)
            write_rows(directory, fixture_row("BEGIN"), fixture_row("END"), fixture_row("END"))
            repeated_end = run_cli(directory)
            self.assertNotEqual(repeated_end.returncode, 0)
            self.assertIn("duplicate END", repeated_end.stderr)

    def test_fails_on_pid_mismatch_between_begin_and_end(self):
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            write_rows(directory, fixture_row("BEGIN"), fixture_row("END", pid=9999))
            result = run_cli(directory)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("END does not match BEGIN", result.stderr)

    def test_fails_on_unknown_or_self_referencing_parent(self):
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            write_rows(directory, fixture_row("BEGIN", parent_id="c" * 32), fixture_row("END"))
            unknown = run_cli(directory)
            self.assertNotEqual(unknown.returncode, 0)
            self.assertIn("unknown parent", unknown.stderr)
            write_rows(directory, fixture_row("BEGIN", parent_id="a" * 32), fixture_row("END"))
            self_parent = run_cli(directory)
            self.assertNotEqual(self_parent.returncode, 0)
            self.assertIn("self-referencing parent", self_parent.stderr)

    def test_fails_on_end_before_begin(self):
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            write_rows(directory, fixture_row("END"), fixture_row("BEGIN"))
            result = run_cli(directory)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("END without BEGIN", result.stderr)

    def test_fails_on_parent_cycle(self):
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            write_rows(
                directory,
                fixture_row("BEGIN", phase="build", phase_id="a" * 32, parent_id="b" * 32),
                fixture_row("BEGIN", phase="suite", phase_id="b" * 32, parent_id="a" * 32),
            )
            result = run_cli(directory)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("unknown parent", result.stderr)

    def test_fails_on_closed_parent(self):
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            write_rows(
                directory,
                fixture_row("BEGIN", phase_id="a" * 32),
                fixture_row("END", phase_id="a" * 32),
                fixture_row("BEGIN", phase_id="e" * 32, parent_id="a" * 32),
            )
            result = run_cli(directory)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("closed parent", result.stderr)

    def test_consumer_errors_never_echo_provided_paths_or_tracebacks(self):
        with tempfile.TemporaryDirectory() as temporary:
            base = Path(temporary)
            secret_dir = base / f"phases-{CANARY}"
            missing = run_cli(secret_dir)
            self.assertNotEqual(missing.returncode, 0)
            self.assertIn("phase log directory not found", missing.stderr)
            directory = secret_dir / "logs"
            directory.mkdir(parents=True)
            (directory / f"phases-{CANARY}.jsonl").write_text("{not json\n", encoding="utf-8")
            named = run_cli(directory)
            self.assertNotEqual(named.returncode, 0)
            self.assertIn("malformed phase record in phase log file 1:1", named.stderr)
            valid = base / "valid"
            valid.mkdir()
            write_rows(valid, fixture_row("BEGIN"), fixture_row("END"))
            summary = base / f"summary-{CANARY}"
            summary.mkdir()
            blocked = run_cli(valid, "--github-summary", summary)
            self.assertNotEqual(blocked.returncode, 0)
            self.assertIn("cannot write summary file", blocked.stderr)
            for result in (missing, named, blocked):
                self.assertNotIn(CANARY, result.stdout)
                self.assertNotIn(CANARY, result.stderr)
                self.assertNotIn("Traceback", result.stderr)

    def test_github_summary_appends_markdown_without_raw_records(self):
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            write_rows(directory, fixture_row("BEGIN"), fixture_row("END"))
            summary = directory / "summary.md"
            for _ in range(2):
                result = run_cli(directory, "--github-summary", summary)
                self.assertEqual(result.returncode, 0, result.stderr)
            text = summary.read_text(encoding="utf-8")
            self.assertEqual(text.count("## ao phase measurements"), 2)
            self.assertNotIn("AO_PHASE_RECORD", text)
            self.assertIn(
                "| command | phase | tree | mode | suite | records | items | failed | total ms | max ms |", text
            )


if __name__ == "__main__":
    unittest.main()
