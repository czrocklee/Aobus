"""Worker-thread phase attribution for the tidy runner and the lint fix stage.

Pool workers do not inherit the submitting thread's ``ContextVar`` state, so
the tidy runner and the lint fixture map take a fresh ``copy_context()``
snapshot per submitted work item. These tests run the real phase writer and
real thread pools through the actual ``tidy`` runner and the actual
``linttest.run`` fix route, assert the worker records keep the root command
and their exact parent phase ids, and run the strict consumer over the
produced logs. They also pin the two boundaries that propagation must not
cross: a reused worker thread never leaks one task's context into the next,
and a bare pool worker started outside any phase stays unparented.
"""

import argparse
import concurrent.futures
import contextlib
import hashlib
import io
import json
import os
import subprocess
import sys
import tempfile
import threading
import unittest
from pathlib import Path
from unittest import mock

from ao.command import tidy as tidy_command
from ao.core import builddir, linttest, phases, tidyengine

ROOT = Path(__file__).resolve().parents[2]

ORIGINAL_FIXTURE = "// POSITIVE: FIX-TO: int replacement;\nint original;\n"
FIXED_FIXTURE = "// POSITIVE: FIX-TO: int replacement;\nint replacement;\n"
SYNTAX_COMMAND = ["unused-compiler", "-fsyntax-only"]


def make_tidy_workspace(root: Path) -> tuple[argparse.Namespace, Path]:
    """Build two in-root sources, one outside-root source, and a real compile database."""
    source = root / "checkout"
    database = root / "database"
    database.mkdir(parents=True)
    selected = [source / "lib" / "audio" / f"Selected{index}.cpp" for index in range(2)]
    outside = root / "external" / "Outside.cpp"
    for path in (*selected, outside):
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text("int original;\n", encoding="utf-8")
    entries = [
        {"directory": str(database), "file": str(path), "arguments": ["clang++", "-std=c++26", "-c", str(path)]}
        for path in (*selected, outside)
    ]
    (database / "compile_commands.json").write_text(json.dumps(entries), encoding="utf-8")
    args = argparse.Namespace(
        files=[str(path) for path in (*selected, outside)],
        all=False,
        folder=[],
        commit=None,
        check=None,
        debug=False,
        output=None,
        jobs=2,
        path=str(database),
        fix=False,
        no_build=True,
        tidy_arg=[],
        header_filter=None,
    )
    return args, source


class PhaseLogFixture(unittest.TestCase):
    """Per-test ``AOBUS_PHASE_LOG_DIR`` handling and record readers."""

    def set_phase_log_dir(self, value: str | None) -> None:
        original = os.environ.get(phases.LOG_DIR_ENV)
        if value is None:
            os.environ.pop(phases.LOG_DIR_ENV, None)
        else:
            os.environ[phases.LOG_DIR_ENV] = value

        def restore() -> None:
            if original is None:
                os.environ.pop(phases.LOG_DIR_ENV, None)
            else:
                os.environ[phases.LOG_DIR_ENV] = original

        self.addCleanup(restore)

    def enable_phase_logging(self, directory: Path) -> None:
        self.set_phase_log_dir(str(directory))

    def disable_phase_logging(self) -> None:
        self.set_phase_log_dir(None)

    def phase_rows(self, directory: Path) -> list[dict]:
        rows = []
        for path in sorted(Path(directory).glob("*.jsonl")):
            for line in path.read_text(encoding="utf-8").splitlines():
                rows.append(json.loads(line))
        return rows

    def strict_consumer(self, directory: Path) -> subprocess.CompletedProcess:
        """Run the real strict record consumer in a fresh process."""
        env = dict(os.environ)
        env.pop(phases.LOG_DIR_ENV, None)
        env["PYTHONPATH"] = str(ROOT / "script")
        return subprocess.run(
            [sys.executable, "-m", "ao.core.phases", str(directory)],
            env=env,
            capture_output=True,
            text=True,
            timeout=60,
        )


class TidyRunnerWorkerContextTest(PhaseLogFixture):
    def test_tidy_workers_inherit_command_parent_and_stable_source_ids(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            args, source = make_tidy_workspace(root)
            log_dir = root / "phases"
            self.enable_phase_logging(log_dir)
            native_calls = []

            def native_call(command, **_keywords):
                native_calls.append(
                    {
                        "thread": threading.get_ident(),
                        "command": phases._command_context.get(),
                        "parent": phases._parent_phase.get(),
                    }
                )
                return 0

            with contextlib.ExitStack() as stack:
                stack.enter_context(mock.patch.object(tidy_command, "PROJECT_ROOT", source))
                stack.enter_context(
                    mock.patch.object(
                        tidy_command,
                        "prepare_toolchain",
                        return_value=tidy_command.TidyToolchain("unused-clang-tidy", None, None),
                    )
                )
                stack.enter_context(
                    mock.patch.object(tidy_command.workspace_cache, "validated_source_roots", return_value=(source,))
                )
                stack.enter_context(mock.patch.object(tidy_command.tidyengine, "system_include_args", return_value=[]))
                stack.enter_context(mock.patch.object(tidy_command.subprocess, "call", side_effect=native_call))
                source_id_spy = stack.enter_context(
                    mock.patch.object(tidy_command, "_analysis_source_id", wraps=tidy_command._analysis_source_id)
                )
                stack.enter_context(contextlib.redirect_stdout(io.StringIO()))
                stack.enter_context(contextlib.redirect_stderr(io.StringIO()))
                with phases.measure("command", command="tidy"):
                    status = tidy_command.run_command(args)

            self.assertEqual(status, 0)
            self.assertEqual(source_id_spy.call_count, 3)

            rows = self.phase_rows(log_dir)
            begins = [row for row in rows if row["kind"] == "BEGIN"]
            ends = [row for row in rows if row["kind"] == "END"]
            self.assertEqual(
                [row["phase"] for row in begins],
                ["command", "analysis", "analysis-item", "analysis-item", "analysis-item"],
            )
            command_begin, analysis_begin = begins[0], begins[1]
            items = begins[2:]
            self.assertEqual(analysis_begin["parent_id"], command_begin["phase_id"])
            for item in items:
                self.assertEqual(item["command"], "tidy")
                self.assertEqual(item["parent_id"], analysis_begin["phase_id"])
                self.assertEqual(item["tree"], "tidy")
                self.assertEqual(item["mode"], "STRICT")
                self.assertEqual(item["count"], 1)

            # The digest is over the repository-relative spelling, so it is
            # stable across checkout locations; the outside-root source keeps
            # no identifier at all.
            expected_digests = {
                hashlib.sha256(f"lib/audio/Selected{index}.cpp".encode()).hexdigest() for index in range(2)
            }
            recorded_digests = {item["source_id"] for item in items if "source_id" in item}
            self.assertEqual(recorded_digests, expected_digests)
            self.assertEqual(len([item for item in items if "source_id" not in item]), 1)

            # Every native invocation ran on a pool worker inside its own
            # analysis-item record, never on the submitting thread.
            main_thread = threading.get_ident()
            self.assertEqual(len(native_calls), 3)
            self.assertTrue(all(call["thread"] != main_thread for call in native_calls))
            self.assertTrue(all(call["command"] == "tidy" for call in native_calls))
            self.assertEqual({call["parent"] for call in native_calls}, {item["phase_id"] for item in items})

            # The executor joins inside the enclosing phase: both item ENDs
            # precede the analysis END, which precedes the command END.
            analysis_end = next(row for row in ends if row["phase"] == "analysis")
            command_end = next(row for row in ends if row["phase"] == "command")
            item_ends = [row for row in ends if row["phase"] == "analysis-item"]
            self.assertEqual(len(item_ends), 3)
            self.assertTrue(all(rows.index(end) < rows.index(analysis_end) for end in item_ends))
            self.assertLess(rows.index(analysis_end), rows.index(command_end))

            consumer = self.strict_consumer(log_dir)
            self.assertEqual(consumer.returncode, 0, consumer.stderr)
            self.assertIn("5 phase record(s) measured.", consumer.stdout)

    def test_disabled_logging_skips_the_source_id_helper(self):
        for name, value in (("absent", None), ("empty", "")):
            with self.subTest(name=name), tempfile.TemporaryDirectory() as temporary:
                root = Path(temporary)
                args, source = make_tidy_workspace(root)
                log_dir = root / "phases"

                def native_call(command, **_keywords):
                    return 0

                self.set_phase_log_dir(value)
                with contextlib.ExitStack() as stack:
                    stack.enter_context(mock.patch.object(tidy_command, "PROJECT_ROOT", source))
                    stack.enter_context(
                        mock.patch.object(
                            tidy_command,
                            "prepare_toolchain",
                            return_value=tidy_command.TidyToolchain("unused-clang-tidy", None, None),
                        )
                    )
                    stack.enter_context(
                        mock.patch.object(
                            tidy_command.workspace_cache, "validated_source_roots", return_value=(source,)
                        )
                    )
                    stack.enter_context(
                        mock.patch.object(tidy_command.tidyengine, "system_include_args", return_value=[])
                    )
                    stack.enter_context(mock.patch.object(tidy_command.subprocess, "call", side_effect=native_call))
                    source_id_mock = stack.enter_context(mock.patch.object(tidy_command, "_analysis_source_id"))
                    stack.enter_context(contextlib.redirect_stdout(io.StringIO()))
                    stack.enter_context(contextlib.redirect_stderr(io.StringIO()))
                    with phases.measure("command", command="tidy"):
                        status = tidy_command.run_command(args)

                self.assertEqual(status, 0)
                source_id_mock.assert_not_called()
                self.assertEqual(list(log_dir.glob("*.jsonl")), [])


class LintFixWorkerContextTest(PhaseLogFixture):
    def test_lint_fix_workers_inherit_the_test_command_and_stage_parent(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            fixture_root = root / "lint-fixture"
            database = fixture_root / "database"
            database.mkdir(parents=True)
            (database / "compile_commands.json").write_text("[]", encoding="utf-8")
            plugin = database / "tool" / "lint" / "libAobusLintPlugin.so"
            plugin.parent.mkdir(parents=True)
            plugin.touch()
            helper_dir = fixture_root / "fixture-root"
            helper_dir.mkdir()
            (helper_dir / "TestHelpers.h").write_text("// placeholder\n", encoding="utf-8")
            fixtures = []
            for name in ("BasicFixture", "SecondFixture"):
                source = helper_dir / name / f"{name}.cpp"
                source.parent.mkdir()
                source.write_text(ORIGINAL_FIXTURE, encoding="utf-8")
                fixtures.append(linttest.Fixture(source, "aobus-example"))
            smoke_log = fixture_root / "smoke.log"
            smoke_log.touch()
            log_dir = root / "phases"
            self.enable_phase_logging(log_dir)

            syntax_contexts = []

            def native_run(command, **_keywords):
                if command == SYNTAX_COMMAND:
                    syntax_contexts.append(
                        (threading.get_ident(), phases._command_context.get(), phases._parent_phase.get())
                    )
                    return subprocess.CompletedProcess(command, 0, stdout="")
                copied = Path(command[-1])
                copied.write_text(FIXED_FIXTURE, encoding="utf-8")
                return subprocess.CompletedProcess(
                    command, 1, stdout=f"{copied}:2:1: warning: expected [aobus-example]\n"
                )

            with contextlib.ExitStack() as stack:
                stack.enter_context(
                    mock.patch.object(linttest.builddir, "platform_profile", return_value=builddir.LINUX_PROFILE)
                )
                stack.enter_context(mock.patch.object(linttest, "FIXTURE_DIR", helper_dir))
                stack.enter_context(mock.patch.object(linttest, "discover_fixtures", return_value=fixtures))
                stack.enter_context(mock.patch.object(linttest, "_syntax_command", return_value=SYNTAX_COMMAND))
                stack.enter_context(mock.patch.object(linttest.subprocess, "run", side_effect=native_run))
                stack.enter_context(
                    mock.patch.object(linttest, "_run_replacement_smoke", return_value=(True, smoke_log))
                )
                stack.enter_context(
                    mock.patch.object(linttest, "_run_compiler_error_smoke", return_value=(True, smoke_log))
                )
                stack.enter_context(contextlib.redirect_stdout(io.StringIO()))
                with phases.measure("command", command="test"):
                    with phases.measure("suite", suite="lint"):
                        status = linttest.run(database, jobs=2)

            self.assertEqual(status, 0)
            rows = self.phase_rows(log_dir)
            begins = [row for row in rows if row["kind"] == "BEGIN"]
            ends = [row for row in rows if row["kind"] == "END"]
            command_begin = next(row for row in begins if row["phase"] == "command")
            suite_begin = next(row for row in begins if row["phase"] == "suite")
            fixes_begin = next(row for row in begins if row["phase"] == "lint-fixes")
            syntax_begins = [row for row in begins if row["phase"] == "lint-syntax"]
            self.assertEqual(suite_begin["parent_id"], command_begin["phase_id"])
            self.assertEqual(fixes_begin["parent_id"], suite_begin["phase_id"])
            self.assertEqual(fixes_begin["command"], "test")
            self.assertEqual(len(syntax_begins), 2)
            for syntax in syntax_begins:
                self.assertEqual(syntax["command"], "test")
                self.assertEqual(syntax["parent_id"], fixes_begin["phase_id"])
                self.assertEqual(syntax["tree"], "tidy")
                self.assertEqual(syntax["count"], 1)

            # Both syntax compiles ran on pool workers inside the open
            # lint-fixes stage, and the executor joins before that stage ends.
            main_thread = threading.get_ident()
            self.assertEqual(len(syntax_contexts), 2)
            self.assertTrue(all(thread != main_thread for thread, _, _ in syntax_contexts))
            self.assertTrue(all(command == "test" for _, command, _ in syntax_contexts))
            self.assertEqual({parent for _, _, parent in syntax_contexts}, {s["phase_id"] for s in syntax_begins})
            fixes_end = next(row for row in ends if row["phase"] == "lint-fixes")
            syntax_ends = [row for row in ends if row["phase"] == "lint-syntax"]
            self.assertTrue(all(rows.index(end) < rows.index(fixes_end) for end in syntax_ends))

            consumer = self.strict_consumer(log_dir)
            self.assertEqual(consumer.returncode, 0, consumer.stderr)
            self.assertIn("8 phase record(s) measured.", consumer.stdout)


class RunParallelTaskReuseTest(PhaseLogFixture):
    def test_a_reused_worker_thread_gets_a_fresh_context_per_task(self):
        with tempfile.TemporaryDirectory() as temporary:
            log_dir = Path(temporary)
            self.enable_phase_logging(log_dir)
            main_thread = threading.get_ident()
            observed = []

            def runner(item: int, _log: Path) -> int:
                observed.append(
                    (item, threading.get_ident(), phases._parent_phase.get(), phases._command_context.get())
                )
                with phases.measure("analysis-item", tree="tidy", count=1) as item_phase:
                    item_phase.status = 0
                if item == 0:
                    # Deliberately leave a poisoned parent behind in this
                    # worker's copied context; a fresh per-item snapshot must
                    # keep it out of the next task on the same reused thread.
                    phases._parent_phase.set("poisoned-parent")
                return 0

            with phases.measure("command", command="tidy"):
                with phases.measure("analysis", tree="tidy", jobs=1, count=2) as phase:
                    submitted_parent = phases._parent_phase.get()
                    with contextlib.redirect_stderr(io.StringIO()):
                        result = tidyengine.run_parallel([0, 1], 1, log_dir, runner)
                    phase.status = 0

            self.assertFalse(result.failed)
            self.assertEqual(len(result.logs), 2)
            self.assertEqual(len(observed), 2)
            worker_threads = {thread for _, thread, _, _ in observed}
            self.assertEqual(len(worker_threads), 1, "jobs=1 must reuse one worker thread for both tasks")
            self.assertNotIn(main_thread, worker_threads)
            for _item, _thread, parent, command in observed:
                self.assertEqual(parent, submitted_parent)
                self.assertNotEqual(parent, "poisoned-parent")
                self.assertEqual(command, "tidy")

            # The submitting thread's own context is restored after the join.
            self.assertIsNone(phases._parent_phase.get())
            self.assertIsNone(phases._command_context.get())

            rows = self.phase_rows(log_dir)
            begins = [row for row in rows if row["kind"] == "BEGIN"]
            analysis_begin = next(row for row in begins if row["phase"] == "analysis")
            items = [row for row in begins if row["phase"] == "analysis-item"]
            self.assertEqual(len(items), 2)
            for item in items:
                self.assertEqual(item["command"], "tidy")
                self.assertEqual(item["parent_id"], analysis_begin["phase_id"])
            self.assertNotIn("poisoned-parent", json.dumps(rows))

            consumer = self.strict_consumer(log_dir)
            self.assertEqual(consumer.returncode, 0, consumer.stderr)


class UnparentedWorkerTest(PhaseLogFixture):
    def test_a_bare_pool_worker_outside_any_phase_stays_unparented(self):
        with tempfile.TemporaryDirectory() as temporary:
            log_dir = Path(temporary)
            self.enable_phase_logging(log_dir)
            started = threading.Event()
            done = threading.Event()

            def work() -> None:
                started.set()
                with phases.measure("analysis-item", tree="tidy", count=1):
                    done.wait(10)

            with concurrent.futures.ThreadPoolExecutor(max_workers=1) as pool:
                worker = pool.submit(work)
                self.assertTrue(started.wait(10))
                with phases.measure("suite", suite="lint"):
                    pass
                done.set()
            self.assertTrue(worker.done())

            begins = [row for row in self.phase_rows(log_dir) if row["kind"] == "BEGIN"]
            by_phase = {row["phase"]: row for row in begins}
            # Propagation is per submitted item, never thread-wide: a worker
            # started outside any phase invents no parent and no command,
            # even while the submitting thread measures its own phase.
            self.assertNotIn("parent_id", by_phase["analysis-item"])
            self.assertNotIn("command", by_phase["analysis-item"])
            self.assertNotIn("parent_id", by_phase["suite"])
            self.assertNotIn("command", by_phase["suite"])

            consumer = self.strict_consumer(log_dir)
            self.assertEqual(consumer.returncode, 0, consumer.stderr)


if __name__ == "__main__":
    unittest.main()
