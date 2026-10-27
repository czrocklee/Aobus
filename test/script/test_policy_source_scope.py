"""Tests for the real-Clang source-policy scope smoke stage."""

import contextlib
import io
import json
import os
import subprocess
import tempfile
import threading
import unittest
from pathlib import Path
from unittest import mock

from ao.core import builddir, linttest, phases


class SourcePolicyScopeSmokeTest(unittest.TestCase):
    def native_result(self, command, **_kwargs):
        if "--dump-config" in command:
            config = json.loads(next(arg.removeprefix("-config=") for arg in command if arg.startswith("-config=")))
            roots = config["CheckOptions"].get("AobusSourceRoots", "[]")
            config["CheckOptions"] = {f"{alias}.AobusSourceRoots": roots for alias in linttest._SOURCE_POLICY_CHECKS}
            return subprocess.CompletedProcess(command, 0, stdout=json.dumps(config))
        path = Path(command[command.index("--") - 1])
        output = ""
        config_text = next(arg.removeprefix("-config=") for arg in command if arg.startswith("-config="))
        if '"CheckOptions"' in config_text:
            options = json.loads(config_text)["CheckOptions"]
            value = options.get("AobusSourceRoots", "[]")
            try:
                roots = json.loads(value)
                invalid = not isinstance(roots, list) or any(not isinstance(root, str) or not root for root in roots)
            except json.JSONDecodeError:
                invalid = True
            if invalid:
                output += "warning: invalid AobusSourceRoots [clang-tidy-config]\n"
        for line_number, line in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
            if "// POSITIVE:" in line:
                check = line.split("// POSITIVE:", 1)[1]
                output += f"{path}:{line_number}:1: warning: expected [{check}]\n"
        return subprocess.CompletedProcess(command, 0, stdout=output)

    def test_scope_smoke_passes_only_with_all_required_diagnostics_and_explicit_roots(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            with (
                mock.patch.object(linttest.tidyengine, "clang_tool", return_value="pinned-clang-tidy"),
                mock.patch.object(linttest.subprocess, "run", side_effect=self.native_result) as run,
            ):
                success, _log = linttest._run_source_policy_smoke(
                    root, root, plugin=root / "tool/lint/libAobusLintPlugin.so"
                )
            self.assertTrue(success)
            self.assertEqual(run.call_count, linttest._source_policy_case_count(windows=False))
            all_roots = []
            for call in run.call_args_list:
                command = call.args[0]
                self.assertEqual(command[0], "pinned-clang-tidy")
                self.assertIn(f"-load={root / 'tool/lint/libAobusLintPlugin.so'}", command)
                config = next(
                    argument.removeprefix("-config=") for argument in command if argument.startswith("-config=")
                )
                if "key: 'AobusSourceRoots', value: " not in config:
                    continue  # Raw metadata and dump round trips intentionally bypass config_for.
                encoded = config.split("key: 'AobusSourceRoots', value: ", 1)[1]
                roots_json, _end = json.JSONDecoder().raw_decode(encoded)
                all_roots.append(json.loads(roots_json))
            self.assertTrue(all(all_roots))
            self.assertTrue(any(len(roots) == 2 for roots in all_roots))
            self.assertTrue(any("space;and checkout" in root for roots in all_roots for root in roots))

    def test_missing_or_unexpected_warnings_and_compiler_errors_fail_the_smoke(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            for mode in (
                "missing",
                "unexpected",
                "compiler-error",
                "crash",
                "dump-missing-option",
                "dump-error",
                "configuration-warning-missing",
                "configuration-warning-unexpected",
            ):

                def result(command, mode=mode, **_kwargs):
                    if "--dump-config" in command:
                        if mode == "dump-missing-option":
                            return subprocess.CompletedProcess(command, 0, stdout="CheckOptions: {}\n")
                        if mode == "dump-error":
                            return subprocess.CompletedProcess(command, 1, stdout="error: cannot dump config\n")
                        return self.native_result(command)
                    if mode.startswith("dump-"):
                        return self.native_result(command)
                    if mode.startswith("configuration-warning-"):
                        completed = self.native_result(command)
                        if mode.endswith("missing"):
                            completed.stdout = "\n".join(
                                line for line in completed.stdout.splitlines() if "[clang-tidy-config]" not in line
                            )
                        else:
                            completed.stdout += "warning: invalid AobusSourceRoots [clang-tidy-config]\n"
                        return completed
                    path = command[command.index("--") - 1]
                    if mode == "missing":
                        return subprocess.CompletedProcess(command, 0, stdout="")
                    if mode == "unexpected":
                        completed = self.native_result(command)
                        if "// NEGATIVE:" in Path(path).read_text(encoding="utf-8"):
                            completed.stdout += (
                                f"{path}:3:1: warning: unexpected [aobus-readability-forbid-raw-throw]\n"
                            )
                        return completed
                    if mode == "compiler-error":
                        return subprocess.CompletedProcess(
                            command, 1, stdout=f"{path}:1:1: error: invalid fixture [clang-diagnostic-error]\n"
                        )
                    return subprocess.CompletedProcess(command, -6, stdout="")

                with (
                    self.subTest(mode=mode),
                    mock.patch.object(linttest.tidyengine, "clang_tool", return_value="pinned-clang-tidy"),
                    mock.patch.object(linttest.subprocess, "run", side_effect=result),
                ):
                    success, log = linttest._run_source_policy_smoke(
                        root, root, plugin=root / "tool/lint/libAobusLintPlugin.so"
                    )
                    self.assertFalse(success)
                    self.assertIn("ERROR:", log.read_text(encoding="utf-8"))

    def test_losing_any_one_selected_check_fails_the_smoke(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            for dropped in linttest._SOURCE_POLICY_CHECKS:

                def partial(command, dropped=dropped, **_kwargs):
                    completed = self.native_result(command)
                    if "--dump-config" not in command:
                        completed.stdout = "\n".join(
                            line for line in completed.stdout.splitlines() if f"[{dropped}]" not in line
                        )
                    return completed

                with (
                    self.subTest(dropped=dropped),
                    mock.patch.object(linttest.tidyengine, "clang_tool", return_value="pinned-clang-tidy"),
                    mock.patch.object(linttest.subprocess, "run", side_effect=partial),
                ):
                    success, log = linttest._run_source_policy_smoke(
                        root, root, plugin=root / "tool/lint/libAobusLintPlugin.so", jobs=4
                    )
                    self.assertFalse(success)
                    self.assertIn(f"[{dropped}]", log.read_text(encoding="utf-8"))

    def test_metadata_faults_and_dump_round_trips_use_the_real_host_boundary(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            with (
                mock.patch.object(linttest.tidyengine, "clang_tool", return_value="pinned-clang-tidy"),
                mock.patch.object(linttest.subprocess, "run", side_effect=self.native_result) as run,
            ):
                success, _log = linttest._run_source_policy_smoke(
                    root, root, plugin=root / "tool/lint/libAobusLintPlugin.so"
                )
            self.assertTrue(success)
            payloads = []
            dumps = []
            round_trips = []
            for call in run.call_args_list:
                command = call.args[0]
                config = next(arg.removeprefix("-config=") for arg in command if arg.startswith("-config="))
                if '"CheckOptions"' not in config:
                    continue
                options = json.loads(config)["CheckOptions"]
                if "--dump-config" in command:
                    dumps.append(options)
                elif "AobusSourceRoots" not in options and any(".AobusSourceRoots" in key for key in options):
                    round_trips.append(options)
                else:
                    payloads.append(options.get("AobusSourceRoots"))
            self.assertIn("{", payloads)
            self.assertIn("{}", payloads)
            self.assertIn("[]", payloads)
            self.assertIn('[null,17,false,{},""]', payloads)
            self.assertTrue(any(value and value.startswith('["') and not value.endswith("]") for value in payloads))
            self.assertEqual(len(dumps), 2)
            self.assertEqual(len(round_trips), 2)
            self.assertIn({}, dumps)
            self.assertTrue(any(set(options.values()) == {"[]"} for options in round_trips))

    def test_posix_case_mismatches_keep_policy_without_changing_windows_semantics(self):
        for windows in (False, True):
            cases = linttest._source_policy_cases(Path("source-policy"), windows=windows)
            case_rules = {name: expected for name, _path, _roots, expected in cases}
            self.assertEqual(case_rules["case-varied-root"], not windows)
            self.assertEqual(case_rules["case-varied-test"], not windows)

    def test_windows_smoke_uses_both_path_profiles_and_the_prepared_static_host(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            tools = linttest.WindowsFixtureTools("native-static-host", ("--driver-mode=cl", "/EHsc"))
            with mock.patch.object(linttest.subprocess, "run", side_effect=self.native_result) as run:
                success, _log = linttest._run_source_policy_smoke(root, root, plugin=None, tools=tools)
            self.assertTrue(success)
            self.assertEqual(run.call_count, linttest._source_policy_case_count(windows=True))
            for call in run.call_args_list:
                command = call.args[0]
                self.assertEqual(command[0], tools.tidy)
                self.assertIn("/clang:-fdiagnostics-format=clang", command)
                self.assertIn("/clang:-std=c++26", command)
                self.assertFalse(any(argument.startswith("-load=") for argument in command))

    def test_parallel_smoke_bounds_children_and_freezes_distinct_case_inputs(self):
        for windows in (False, True):
            with self.subTest(windows=windows), tempfile.TemporaryDirectory() as temporary:
                root = Path(temporary)
                lock = threading.Lock()
                release = threading.Event()
                active = 0
                peak = 0
                source_bytes = {}
                contexts = []

                def native(command, lock=lock, release=release, source_bytes=source_bytes, contexts=contexts, **kwargs):
                    nonlocal active, peak
                    path = Path(next(argument for argument in command if argument.endswith(".cpp")))
                    before = path.read_bytes()
                    with lock:
                        active += 1
                        peak = max(peak, active)
                        source_bytes.setdefault(path, before)
                        contexts.append((phases._command_context.get(), phases._parent_phase.get()))
                        if active == 4:
                            release.set()
                    try:
                        self.assertTrue(release.wait(timeout=5), "four source-policy workers did not start")
                        self.assertEqual(path.read_bytes(), before)
                        result = self.native_result(command, **kwargs)
                        self.assertEqual(path.read_bytes(), before)
                        return result
                    finally:
                        with lock:
                            active -= 1

                tools = linttest.WindowsFixtureTools("static-host", ()) if windows else None
                with (
                    mock.patch.dict(os.environ, {phases.LOG_DIR_ENV: str(root / "phases")}),
                    mock.patch.object(linttest.tidyengine, "clang_tool", return_value="pinned-clang-tidy"),
                    mock.patch.object(linttest.subprocess, "run", side_effect=native) as run,
                    phases.measure("command", command="test"),
                    phases.measure("lint-source-policy", tree="tidy", jobs=4) as phase,
                ):
                    parent = phases._parent_phase.get()
                    success, log = linttest._run_source_policy_smoke(
                        root, root, plugin=None if windows else root / "verified.so", tools=tools, jobs=4
                    )
                    phase.status = 0 if success else 1
                self.assertTrue(success, log.read_text(encoding="utf-8"))
                self.assertEqual(peak, 4)
                self.assertEqual(active, 0)
                self.assertEqual(run.call_count, linttest._source_policy_case_count(windows=windows))
                self.assertEqual(len(source_bytes), linttest._source_policy_task_count(windows=windows))
                self.assertEqual(set(contexts), {("test", parent)})
                self.assertEqual({path: path.read_bytes() for path in source_bytes}, source_bytes)
                labels = [name for name, *_ in linttest._source_policy_cases(root, windows=windows)]
                positions = [log.read_text(encoding="utf-8").index(f"=== {name}/native ===") for name in labels]
                self.assertEqual(positions, sorted(positions))

    def test_source_policy_failure_fails_the_lint_gate_and_preserves_its_workspace(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            (root / "compile_commands.json").write_text("[]", encoding="utf-8")
            plugin = root / "tool/lint/libAobusLintPlugin.so"
            plugin.parent.mkdir(parents=True)
            plugin.touch()
            source = root / "Fixture.cpp"
            source.write_text("int value;\n", encoding="utf-8")
            log = root / "failure.log"
            log.write_text("source policy control failed\n", encoding="utf-8")
            with (
                mock.patch.object(linttest.builddir, "platform_profile", return_value=builddir.LINUX_PROFILE),
                mock.patch.object(
                    linttest, "discover_fixtures", return_value=[linttest.Fixture(source, "aobus-example")]
                ),
                mock.patch.object(linttest, "_run_diagnostic", return_value=(True, log)),
                mock.patch.object(linttest, "_run_source_policy_smoke", return_value=(False, log)),
                mock.patch.object(linttest, "_run_compiler_error_smoke", return_value=(True, log)),
                mock.patch.object(linttest.shutil, "rmtree") as cleanup,
                contextlib.redirect_stdout(io.StringIO()),
            ):
                self.assertEqual(linttest.run(root, jobs=1), 1)
            cleanup.assert_not_called()


if __name__ == "__main__":
    unittest.main()
