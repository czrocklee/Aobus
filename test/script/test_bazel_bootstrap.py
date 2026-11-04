"""Native environment boundaries of the opt-in Bazel route."""

import os
import platform
import re
import shlex
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

from ao import __main__ as portal

ROOT = Path(__file__).resolve().parents[2]


@unittest.skipUnless(shutil.which("bash"), "POSIX bootstrap requires Bash")
class MacosBazelEnvironmentTests(unittest.TestCase):
    def _run(self, *, version: str = "22.1.8", architecture: str = "x86_64") -> subprocess.CompletedProcess[str]:
        with tempfile.TemporaryDirectory(prefix="aobus-bazel-bootstrap-") as temporary:
            root = Path(temporary)
            llvm = root / "llvm"
            (llvm / "bin").mkdir(parents=True)
            compiler = llvm / "bin/clang++"
            bash = str(shutil.which("bash"))
            # Git Bash can live under "Program Files"; a shebang cannot quote
            # that native path. This fixture uses only portable shell builtins.
            compiler.write_text(f"#!/bin/sh\nprintf '%s\\n' {shlex.quote(version)}\n", encoding="utf-8", newline="\n")
            compiler.chmod(0o755)
            calls = root / "calls"
            script = f"""
set -eu
source {shlex.quote(str(ROOT / "script/ao/macos-vcpkg-bootstrap.sh"))}
aobus_macos_load_toolchain_lock() {{
    export AOBUS_MACOS_LLVM_FORMULA=llvm@22
    export AOBUS_MACOS_LLVM_MAJOR=22
    export AOBUS_MACOS_DEPLOYMENT_TARGET=15.0
    export AOBUS_HOMEBREW=/unused/brew
}}
aobus_macos_formula_prefix() {{
    printf '%s\\n' "$2" >> {shlex.quote(str(calls))}
    case "$2" in
        llvm@22) printf '%s\\n' {shlex.quote(str(llvm))} ;;
        pkgconf) printf '%s\\n' {shlex.quote(str(root / "pkgconf"))} ;;
        *) return 91 ;;
    esac
}}
aobus_macos_prepare_expected_shim() {{
    printf 'shim=%s|%s|%s\\n' "$1" "$2" "$3"
}}
uname() {{ printf '%s\\n' {shlex.quote(architecture)}; }}
aobus_macos_prepare_bazel_environment {shlex.quote(str(ROOT))}
printf 'triplet=%s\\ncompiler=%s\\ndeployment=%s\\n' "$AOBUS_VCPKG_TRIPLET" "$CXX" "$MACOSX_DEPLOYMENT_TARGET"
"""
            environment = dict(os.environ, AOBUS_STATE_ROOT=str(root / "state"))
            result = subprocess.run([bash, "-c", script], env=environment, capture_output=True, text=True, check=False)
            queried = calls.read_text(encoding="utf-8").splitlines()
            self.assertEqual(queried, ["llvm@22", "pkgconf"])
            return result

    def test_existing_provider_path_needs_no_cmake_ninja_or_vcpkg_bootstrap(self) -> None:
        result = self._run()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("triplet=x64-aobus-osx", result.stdout)
        self.assertIn("deployment=15.0", result.stdout)
        self.assertIn("|22.1.8|", result.stdout)

    def test_arm64_uses_the_native_project_triplet(self) -> None:
        result = self._run(architecture="arm64")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("triplet=arm64-aobus-osx", result.stdout)

    def test_wrong_llvm_major_fails_before_preparing_the_shim(self) -> None:
        result = self._run(version="21.1.0")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("expected major 22", result.stderr)
        self.assertNotIn("shim=", result.stdout)


class BazelWorkflowTests(unittest.TestCase):
    def test_linux_ci_checks_the_slice_before_the_complete_native_gate(self) -> None:
        workflow = (ROOT / ".github/workflows/ci.yml").read_text(encoding="utf-8")
        linux = workflow.split("  linux:\n", 1)[1].split("  macos:\n", 1)[0]
        name = "      - name: Check Bazel portable slice\n"
        self.assertEqual(linux.count(name), 1)
        step = linux.split(name, 1)[1].split("      - name:", 1)[0]
        self.assertIn(
            'run: ./ao bazel test --path "$RUNNER_TEMP/aobus-bazel-debug" --jobs "$CMAKE_BUILD_PARALLEL_LEVEL"',
            step,
        )
        self.assertIn("        continue-on-error: true\n", step)
        self.assertNotIn("        if:", step)
        self.assertLess(linux.index(name), linux.index("      - name: Build and test with private PipeWire"))
        self.assertIn("./ao check", linux)
        self.assertIn("      - name: Run hygiene", linux)

    def test_linux_ci_verifies_both_configurations_before_the_unchanged_complete_native_gate(self) -> None:
        workflow = (ROOT / ".github/workflows/ci.yml").read_text(encoding="utf-8")
        linux = workflow.split("  linux:\n", 1)[1].split("  macos:\n", 1)[0]
        name = "      - name: Verify Bazel compile contracts\n"
        self.assertEqual(linux.count(name), 1)
        step = linux.split(name, 1)[1].split("      - name:", 1)[0]
        self.assertIn("        continue-on-error: true\n", step)
        self.assertNotIn("        if:", step)
        commands = [shlex.split(line.strip()) for line in step.splitlines() if line.strip().startswith("./ao")]
        self.assertEqual(
            commands,
            [
                [
                    "./ao",
                    "bazel",
                    "verify",
                    "--path",
                    "$RUNNER_TEMP/aobus-bazel-debug",
                    "--jobs",
                    "$CMAKE_BUILD_PARALLEL_LEVEL",
                ],
                [
                    "./ao",
                    "bazel",
                    "verify",
                    "--config",
                    "release",
                    "--path",
                    "$RUNNER_TEMP/aobus-bazel-release",
                    "--jobs",
                    "$CMAKE_BUILD_PARALLEL_LEVEL",
                ],
            ],
        )
        native_name = "      - name: Build and test with private PipeWire"
        self.assertLess(linux.index("      - name: Check Bazel portable slice"), linux.index(name))
        self.assertLess(linux.index(name), linux.index(native_name))
        native = linux.split(native_name, 1)[1].split("      - name:", 1)[0]
        self.assertNotIn("continue-on-error", native)
        self.assertNotIn("        if:", native)
        self.assertEqual([line.strip() for line in native.splitlines() if "./ao check" in line], ["./ao check"])
        self.assertIn("export AOBUS_REQUIRE_PIPEWIRE=1", native)
        self.assertIn('dbus-run-session --config-file="$runtime/dbus.conf"', native)
        self.assertLess(linux.index(native_name), linux.index("      - name: Run hygiene"))

    def test_bazel_only_jobs_are_optional_cmake_free_and_never_publish_caches(self) -> None:
        workflow = (ROOT / ".github/workflows/ci.yml").read_text(encoding="utf-8")
        for job_name, launcher, prerequisites in (
            ("macos_bazel", "./ao", []),
            ("windows_bazel", ".\\ao.bat", [["doctor", "winui"], ["setup", "winui-runtime"]]),
        ):
            with self.subTest(job=job_name):
                job = re.split(r"\n(?=  \w+:\n)", workflow.split(f"\n  {job_name}:\n", 1)[1], maxsplit=1)[0]
                self.assertIn("\n    continue-on-error: true\n", job)
                self.assertNotIn("actions/cache/save@", job)
                commands = [
                    shlex.split(line.strip().removeprefix("run: "), posix=False)
                    for line in job.splitlines()
                    if line.strip().removeprefix("run: ").startswith(launcher + " ")
                ]
                self.assertEqual(
                    [command[1:3] for command in commands],
                    [
                        *prerequisites,
                        ["bazel", "setup"],
                        ["bazel", "provision"],
                        ["bazel", "test"],
                        ["bazel", "verify"],
                        ["bazel", "hygiene"],
                    ],
                )
                trees = {command[command.index("--path") + 1] for command in commands if "--path" in command}
                self.assertEqual(len(trees), 1)


class BazelPortalEnvironmentTests(unittest.TestCase):
    def test_native_bazel_does_not_activate_the_cmake_compiler_cache(self) -> None:
        with (
            patch.object(portal.buildenv, "requires_parsed_build_env", return_value=True),
            patch.object(portal.compiler_cache, "activate_local") as activate,
            patch.object(portal.workspace_cache, "command_context") as context,
            patch("ao.command.bazel.run", return_value=0),
        ):
            # Parser registration stores the handler at registration time.
            self.assertEqual(portal.main(["bazel", "deps", "--path", str(ROOT.parent / "bazel-test")]), 0)
        activate.assert_not_called()
        context.assert_not_called()

    def test_non_bazel_commands_still_activate_cache_and_enter_workspace_context(self) -> None:
        with (
            patch.object(portal.buildenv, "requires_parsed_build_env", return_value=True),
            patch.object(portal.compiler_cache, "activate_local") as activate,
            patch.object(portal.workspace_cache, "command_context") as context,
            patch("ao.command.build.run_command", return_value=7) as run,
        ):
            self.assertEqual(portal.main(["build"]), 7)
        activate.assert_called_once_with()
        context.assert_called_once_with(run.call_args.args[0])
        context.return_value.__enter__.assert_called_once_with()
        context.return_value.__exit__.assert_called_once()

    def test_all_bazel_actions_skip_cmake_context_discovery_and_platform_inventory(self) -> None:
        for action in ("setup", "provision", "deps", "build", "test", "verify", "hygiene"):
            with (
                self.subTest(action=action),
                patch.object(portal.buildenv, "requires_parsed_build_env", side_effect=AssertionError("CMake lookup")),
                patch.object(
                    portal.workspace_cache, "command_uses_context", side_effect=AssertionError("CMake lookup")
                ),
                patch.object(portal.workspace_cache, "command_context", side_effect=AssertionError("CMake context")),
                patch.object(portal.compiler_cache, "activate_local", side_effect=AssertionError("CMake cache")),
                patch.object(platform, "system", side_effect=AssertionError("no OS inventory")),
                patch.object(platform, "uname", side_effect=AssertionError("no OS inventory")),
                patch.object(platform, "machine", side_effect=AssertionError("no implicit ISA inventory")),
                patch("ao.command.bazel.run", return_value=0) as run,
            ):
                self.assertEqual(portal.main(["bazel", action]), 0)
            run.assert_called_once()


if __name__ == "__main__":
    unittest.main()
