"""The explicit loaded WinUI naming gate never silently joins headless suites."""

import argparse
import tempfile
import unittest
from contextlib import contextmanager
from pathlib import Path
from types import SimpleNamespace
from unittest import mock

from ao.command import test
from ao.core import builddir


class WinUiSuiteTest(unittest.TestCase):
    def setUp(self):
        self.enterContext(mock.patch.object(builddir, "platform_profile", return_value=builddir.WINDOWS_PROFILE))
        self.enterContext(mock.patch.object(test.winui, "require_runtime"))
        self.enterContext(mock.patch.object(test.winui, "require_interactive_session", return_value=1))

    @staticmethod
    def options(path, **overrides):
        values = dict(
            suite="winui",
            path=str(path),
            no_build=True,
            filter="",
            list=False,
            repeat=1,
            clang=False,
            asan=False,
            tsan=False,
        )
        values.update(overrides)
        return argparse.Namespace(**values)

    @staticmethod
    def probe(tree):
        path = tree / "app/windows-winui/automation-probe/Debug/ao_winui_automation_probe.exe"
        path.parent.mkdir(parents=True)
        path.touch()
        return path

    def test_is_explicit_windows_only_not_an_all_suite(self):
        self.assertIn("winui", test.selectable_suites())
        self.assertNotIn("winui", test.suites_for("all"))
        self.assertFalse(test.requires_build_environment(self.options(Path("unused"))))
        self.assertTrue(test.requires_build_environment(self.options(Path("unused"), no_build=False)))
        with mock.patch.object(builddir, "platform_profile", return_value=builddir.LINUX_PROFILE):
            self.assertNotIn("winui", test.selectable_suites())
            with self.assertRaises(SystemExit):
                test.suites_for("winui")

    def test_no_build_uses_exact_winui_tree_and_propagates_phase_status(self):
        with tempfile.TemporaryDirectory() as directory:
            tree = Path(directory)
            probe = self.probe(tree)
            measured = []

            @contextmanager
            def measure(kind, **fields):
                record = SimpleNamespace(status=0)
                measured.append((kind, fields, record))
                yield record

            with (
                mock.patch.object(test.phases, "measure", side_effect=measure),
                mock.patch.object(test, "run", return_value=7) as run,
                mock.patch.object(test.build, "do_build") as build,
            ):
                self.assertEqual(test.run_command(self.options(tree)), 7)
            build.assert_not_called()
            run.assert_called_once_with([str(probe)], cwd=probe.parent, log=tree / "build.log", append=True)
            self.assertEqual(measured[0][:2], ("suite", {"tree": "winui", "count": 1, "suite": "winui"}))
            self.assertEqual(measured[0][2].status, 7)

    def test_build_uses_portal_winui_validation_target_and_returned_tree(self):
        self.assertEqual(test.SUITES["winui"].target, test.build.WINUI_CHECK_TARGET)
        with tempfile.TemporaryDirectory() as directory:
            tree = Path(directory)
            self.probe(tree)
            with (
                mock.patch.object(test.build, "do_build", return_value=SimpleNamespace(build_dir=tree)) as build,
                mock.patch.object(test, "run", return_value=0),
            ):
                self.assertEqual(test.run_command(self.options(tree, no_build=False)), 0)
            self.assertEqual(build.call_args.kwargs, {"targets": [test.build.WINUI_CHECK_TARGET]})
            self.assertEqual(build.call_args.args[0].flavor, "debug")
            self.assertFalse(build.call_args.args[0].clean)

    def test_missing_probe_and_desktop_admission_fail_closed(self):
        with tempfile.TemporaryDirectory() as directory:
            with mock.patch.object(test, "run") as run:
                with self.assertRaises(SystemExit):
                    test.run_command(self.options(Path(directory)))
                with mock.patch.object(
                    test.winui, "require_interactive_session", side_effect=RuntimeError("session 0")
                ):
                    with self.assertRaises(SystemExit):
                        test.run_command(self.options(Path(directory)))
            run.assert_not_called()

    def test_rejects_catch2_and_other_build_modes(self):
        for options in ({"filter": "[winui]"}, {"list": True}, {"repeat": 2}, {"clang": True}, {"asan": True}):
            with self.subTest(options=options), self.assertRaises(SystemExit):
                test._run_winui_audit(self.options(Path("unused"), **options))

    def test_ci_requires_the_desktop_gate_without_ignoring_failure(self):
        workflow = (Path(__file__).parents[2] / ".github/workflows/ci.yml").read_text()
        windows = workflow.split("  windows:\n", 1)[1]
        build = windows.index("      - name: Build and test\n")
        audit = windows.index("      - name: WinUI naming gate\n")
        hygiene = windows.index("      - name: Run hygiene\n")
        self.assertLess(build, audit)
        self.assertLess(audit, hygiene)
        block = windows[audit:].split("\n      - name:", 1)[0]
        self.assertIn(r".\ao.bat test --winui --no-build", block)
        self.assertNotIn("continue-on-error", block)
