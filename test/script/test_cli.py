"""CLI dispatch and preflight behavior at consumer-visible boundaries."""

import contextlib
import io
import os
import shlex
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

from ao.__main__ import main
from ao.core.paths import PROJECT_ROOT
from ao.portal import bazel_coverage, bazel_workflows


class NativePortalTest(unittest.TestCase):
    def test_macos_bootstrap_maps_native_architectures_to_project_triplets(self):
        helper = PROJECT_ROOT / "script/ao/macos-vcpkg-bootstrap.sh"
        bash = shutil.which("bash")
        if bash is None:
            self.skipTest("bash is unavailable on this host")

        result = subprocess.run(
            [bash, "-c", 'source "$1"; aobus_macos_triplet x86_64; aobus_macos_triplet arm64', "fixture", str(helper)],
            capture_output=True,
            text=True,
            timeout=15,
        )

        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stdout.splitlines(), ["x64-aobus-osx", "arm64-aobus-osx"])

    def test_macos_expected_shim_disables_the_libcxx_gates(self):
        helper = PROJECT_ROOT / "script/ao/macos-vcpkg-bootstrap.sh"
        bash = shutil.which("bash")
        if bash is None:
            self.skipTest("bash is unavailable on this host")

        with tempfile.TemporaryDirectory() as temp_dir:
            temp = Path(temp_dir)
            header = temp / "llvm/include/c++/v1/__expected/expected.h"
            header.parent.mkdir(parents=True)
            header.write_text("#  if _LIBCPP_STD_VER >= 26\n" * 5, encoding="utf-8")
            result = subprocess.run(
                [
                    bash,
                    "-ec",
                    'source "$1"; aobus_macos_prepare_expected_shim "$2" 22.1.8 "$3"; '
                    'cat "$AOBUS_LIBCXX_EXPECTED_SHIM/__expected/expected.h"',
                    "fixture",
                    str(helper),
                    str(temp / "llvm"),
                    str(temp / "state"),
                ],
                capture_output=True,
                text=True,
                timeout=15,
            )
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(result.stdout, "#  if 0\n" * 5)

    @unittest.skipUnless(sys.platform == "linux", "Linux portal fixture")
    def test_portal_help_preserves_existing_hook_configuration(self):
        with tempfile.TemporaryDirectory() as temporary:
            fixture = Path(temporary)
            shutil.copy2(PROJECT_ROOT / "ao", fixture / "ao")
            package = fixture / "script/ao"
            package.mkdir(parents=True)
            (package / "__init__.py").touch()
            (package / "__main__.py").write_text("print('help fixture')", encoding="utf-8")
            subprocess.run(["git", "init", "-q", str(fixture)], check=True)
            subprocess.run(["git", "-C", str(fixture), "config", "core.hooksPath", "custom-hooks"], check=True)
            (package / "portal").mkdir()
            (package / "portal/__init__.py").touch()
            (package / "portal/build_env.py").write_text("print('0')", encoding="utf-8")
            (package / "macos-vcpkg-bootstrap.sh").write_text(
                f"aobus_macos_prepare_python() {{ AOBUS_PYTHON={shlex.quote(sys.executable)}; }}\n", encoding="utf-8"
            )
            for platform in ("Linux", "Darwin"):
                with self.subTest(platform=platform):
                    temp_state = fixture / f"temporary-{platform}"
                    temp_state.mkdir()
                    result = subprocess.run(
                        [
                            "bash",
                            "-c",
                            f'uname() {{ echo {platform}; }}; export -f uname; bash "$1" help',
                            "fixture",
                            str(fixture / "ao"),
                        ],
                        env={**os.environ, "AO_IN_NIX_PORTAL": "1", "TMPDIR": str(temp_state)},
                        capture_output=True,
                        text=True,
                        timeout=15,
                    )
                    self.assertEqual(result.returncode, 0, result.stderr)
                    self.assertEqual(list(temp_state.iterdir()), [])
                    configured = subprocess.check_output(
                        ["git", "-C", str(fixture), "config", "--get", "core.hooksPath"], text=True
                    ).strip()
                    self.assertEqual(configured, "custom-hooks")


class CliDispatchTest(unittest.TestCase):
    def test_coverage_measures_the_selected_scope_but_instrumentation_only_analyzes(self):
        with (
            mock.patch.object(bazel_coverage.native, "native_platform", return_value="linux"),
            mock.patch.dict(os.environ, {"GITHUB_ACTIONS": "true"}),
            mock.patch("ao.core.jobs.available_cores", return_value=8),
            mock.patch.object(bazel_coverage, "run", return_value=7) as producer,
        ):
            self.assertEqual(main(["measure", "coverage", "--suite", "application", "--scope", "app/runtime"]), 7)
            measured = producer.call_args.args[0]
            self.assertEqual(
                (measured.coverage_suite, measured.scope, measured.jobs), ("application", ("app/runtime",), 8)
            )
            self.assertFalse(measured.analysis_only)
            self.assertEqual(main(["check", "instrumentation", "-j", "3"]), 7)
            checked = producer.call_args.args[0]
            self.assertEqual((checked.scope, checked.jobs), ((), 3))
            self.assertTrue(checked.analysis_only)

    def test_missing_performance_library_fails_before_native_tree_selection(self):
        with (
            tempfile.TemporaryDirectory() as temporary,
            mock.patch.object(bazel_workflows.bazel_workspace, "workspace_path") as select_tree,
            contextlib.redirect_stderr(io.StringIO()),
            self.assertRaises(SystemExit) as failure,
        ):
            main(["measure", "performance", "--library-root", str(Path(temporary) / "missing")])
        self.assertEqual(failure.exception.code, 1)
        select_tree.assert_not_called()

    def test_partial_check_rejects_parent_options_instead_of_silently_discarding_them(self):
        # Lint has its own --config, so argparse alone can accept the parent's
        # value and silently replace it with lint's default.
        with contextlib.redirect_stderr(io.StringIO()), self.assertRaises(SystemExit) as failure:
            main(["check", "--config", "release", "lint"])
        self.assertEqual(failure.exception.code, 2)

    def test_instrumentation_does_not_accept_a_measurement_scope(self):
        with contextlib.redirect_stderr(io.StringIO()), self.assertRaises(SystemExit) as failure:
            main(["check", "instrumentation", "--scope", "lib"])
        self.assertEqual(failure.exception.code, 2)

    def test_invalid_options_are_rejected_before_portal_or_preflight_work(self):
        for module, options, argv in (
            ("ao", (), ("check", "lint", "--tidy-arg=--fix")),
            ("ao.portal.build_env", ("--exit-code",), ("check", "analyzer", "--tidy-arg=@private-input.rsp")),
            ("ao.portal.windows_portal", (), ("check", "docs", "--fresh")),
        ):
            with self.subTest(module=module), tempfile.TemporaryDirectory() as temporary:
                scope = Path(temporary) / "scope.json"
                result = subprocess.run(
                    [sys.executable, "-B", "-m", module, *options, *argv],
                    cwd=PROJECT_ROOT,
                    env={**os.environ, "PYTHONPATH": str(PROJECT_ROOT / "script"), "AOBUS_PREFLIGHT_SCOPE": str(scope)},
                    capture_output=True,
                    text=True,
                    timeout=15,
                )
                self.assertEqual(result.returncode, 2, result.stderr)
                self.assertEqual(result.stdout, "")
                self.assertNotIn("private-input.rsp", result.stderr)
                self.assertFalse(scope.exists())


if __name__ == "__main__":
    unittest.main()
