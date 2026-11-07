"""Behavior tests for the bounded Bazel portal and provider adapter."""

import argparse
import copy
import hashlib
import io
import json
import os
import shutil
import subprocess
import sys
import tempfile
import textwrap
import unittest
from contextlib import contextmanager
from pathlib import Path, PureWindowsPath
from unittest.mock import Mock, patch

from ao.__main__ import make_parser
from ao.command import bazel, coverage
from ao.core import bazel_dependencies as dependencies
from ao.core import bazel_deps as deps
from ao.core import bazel_host_tools as host_tools
from ao.core import bazel_native as native
from ao.core import bazel_providers as providers
from ao.core import bazel_workspace as workspace
from ao.core import buildenv
from ao.core.paths import PROJECT_ROOT


class BazelParserTests(unittest.TestCase):
    def test_actions_select_native_environment_without_setup(self):
        parser = make_parser()
        for action in ("setup", "provision", "deps", "build", "test", "query", "cquery", "aquery", "run", "hygiene"):
            with self.subTest(action=action):
                args = parser.parse_args(["bazel", action])
                self.assertEqual(bazel.requires_build_environment(args), action != "setup")
                # Every action, the commit gate included, keeps the base Python that Bazel records.
                self.assertFalse(buildenv.requires_python_tools("bazel", [action]))

    def test_the_workflow_cache_variable_selects_remote_cache_where_the_flag_exists(self):
        cache = "grpc://127.0.0.1:9092"
        with patch.dict(os.environ, {bazel.REMOTE_CACHE_ENVIRONMENT: cache}):
            parser = make_parser()
            for action in ("build", "test", "run", "query", "lint", "hygiene"):
                with self.subTest(action=action):
                    self.assertEqual(parser.parse_args(["bazel", action]).remote_cache, cache)
            for action in ("deps", "verify"):
                with self.subTest(action=action):
                    self.assertIsNone(parser.parse_args(["bazel", action]).remote_cache)
            explicit = parser.parse_args(["bazel", "test", "--remote-cache", "https://cache.example"])
            self.assertEqual(explicit.remote_cache, "https://cache.example")
        with patch.dict(os.environ, {bazel.REMOTE_CACHE_ENVIRONMENT: ""}):
            self.assertIsNone(make_parser().parse_args(["bazel", "test"]).remote_cache)
        with (
            patch.dict(os.environ, {bazel.REMOTE_CACHE_ENVIRONMENT: "ftp://cache.example"}),
            patch("sys.stderr", new_callable=io.StringIO) as stderr,
            self.assertRaises(SystemExit),
        ):
            make_parser().parse_args(["bazel", "test"])
        self.assertIn("remote cache requires an http(s) or grpc(s) URL", stderr.getvalue())

    def test_bazel_coverage_is_rejected_by_the_parser(self):
        with patch("sys.stderr"), self.assertRaises(SystemExit) as exited:
            make_parser().parse_args(["bazel", "coverage"])
        self.assertEqual(exited.exception.code, 2)

    def test_top_level_coverage_still_selects_the_cmake_command(self):
        args = make_parser().parse_args(["coverage"])
        self.assertIs(args.func, coverage.run_command)

    def test_build_preserves_labels_before_portal_options(self):
        args = make_parser().parse_args(
            [
                "bazel",
                "build",
                "//lib:ao_unicode_text",
                "--config",
                "release",
                "--jobs",
                "2",
                "--remote-cache",
                "https://cache.example",
                "--path",
                "/tmp/bazel-private",
            ]
        )
        self.assertEqual(args.targets, ["//lib:ao_unicode_text"])
        self.assertEqual(args.config, "release")
        self.assertEqual(args.jobs, 2)
        self.assertEqual(args.remote_cache, "https://cache.example")
        for arguments in (["--jobs", "0"], ["--remote-cache", "file:///tmp/cache"], ["--config", "profile"]):
            with self.subTest(arguments=arguments), patch("sys.stderr"), self.assertRaises(SystemExit):
                make_parser().parse_args(["bazel", "test", *arguments])

    def test_actions_default_to_two_jobs_and_require_explicit_option_forwarding(self):
        for action in ("build", "test", "query", "cquery", "aquery", "run"):
            with self.subTest(action=action):
                args = make_parser().parse_args(["bazel", action])
                self.assertEqual(args.jobs, 2)
                self.assertIsNone(args.remote_cache)
                with patch("sys.stderr"), self.assertRaises(SystemExit):
                    make_parser().parse_args(["bazel", action, "--output=jsonproto"])

    def test_ordinary_bazel_options_are_forwarded_unchanged_after_separator(self):
        arguments = [
            "//test:media_test",
            "--test_filter=Decoder*",
            "--test_output=errors",
            "--test_arg=--repo_env=application",
            "--noremote_upload_local_results",
            "--output=jsonproto",
            "--output_filter=warning",
            "--color=yes",
            "--copt=-Wno-unused-parameter",
            "--cxxopt",
            "-DAPPLICATION_FEATURE=1",
            "--test_arg",
            "--config=application",
        ]
        args = make_parser().parse_args(["bazel", "test", "--config", "release", "--", *arguments])
        self.assertEqual(args.config, "release")
        self.assertEqual(args.targets, arguments)
        original = list(args.targets)
        bazel.validate_forwarded_arguments(args.targets)
        self.assertEqual(args.targets, original)

    def test_query_expressions_are_not_restricted_to_target_labels(self):
        for action in ("query", "cquery", "aquery"):
            for expression in ("deps(//test:media_test)", "mnemonic('CppCompile', //...)", "config"):
                with self.subTest(action=action, expression=expression):
                    args = make_parser().parse_args(["bazel", action, "--", expression, "--output", "jsonproto"])
                    self.assertEqual(args.targets, [expression, "--output", "jsonproto"])
                    bazel.validate_forwarded_arguments(args.targets)

    def test_run_preserves_application_arguments_after_second_separator(self):
        arguments = ["//app:program", "--", "--config=application", "--repo_env=application", "-c"]
        args = make_parser().parse_args(["bazel", "run", "--", *arguments])
        self.assertEqual(args.targets, arguments)
        bazel.validate_forwarded_arguments(args.targets)

    def test_managed_options_are_rejected_before_preparation_or_tool_lookup(self):
        for arguments in (
            ["--batch"],
            ["--nobatch"],
            ["--host_jvm_args=-Xmx4g"],
            ["--output_user_root=/tmp/other"],
            ["--output_base", "/tmp/other"],
            ["--bazelrc=/tmp/other.rc"],
            ["--noignore_all_rc_files"],
            ["--noworkspace_rc"],
            ["--config=release"],
            ["--compilation_mode", "opt"],
            ["-c", "opt"],
            ["-copt"],
            ["--action_env=CC=other"],
            ["--host_action_env=CC=other"],
            ["--repo_env", "AOBUS_BAZEL_CONFIG=other"],
            ["--override_repository=evil"],
            ["--inject_repository", "evil=/tmp/other"],
            ["--override_module=rules_cc=/tmp/other"],
            ["--platforms=//:other"],
            ["--extra_toolchains=//:other"],
            ["--compiler=other"],
            ["--crosstool_top=//:other"],
            ["--platform_mappings=/tmp/other"],
            ["--symlink_prefix=/tmp/other"],
            ["--disk_cache=/tmp/other"],
            ["--flagfile=/tmp/other"],
            ["--flag_alias=mode=compilation_mode"],
            ["--invocation_policy=other"],
            ["--@aobus_native_deps//:mode=other"],
            ["--noenable_platform_specific_config"],
            ["--noincompatible_strict_action_env"],
            ["--noexperimental_strict_repo_env"],
            ["--noenable_bzlmod"],
            ["--norun_validations"],
            ["--run_validations=false"],
            ["--run_validations", "false"],
            ["--run_validations=0"],
            ["--noexperimental_run_validations"],
            ["--experimental_run_validations=false"],
            ["--experimental_run_validations", "false"],
            ["--experimental_run_validations=0"],
            ["--noexperimental_use_validation_aspect"],
            ["--experimental_use_validation_aspect=false"],
            ["--experimental_use_validation_aspect", "false"],
        ):
            with (
                self.subTest(arguments=arguments),
                patch.object(workspace, "workspace_path") as select,
                patch.object(workspace, "prepare_workspace") as prepare,
                patch.object(workspace, "bazel_executable") as executable,
                patch.object(workspace, "execute") as execute,
                patch("sys.stderr"),
            ):
                args = make_parser().parse_args(["bazel", "test", "--", *arguments])
                with self.assertRaises(SystemExit):
                    bazel.run(args)
                select.assert_not_called()
                prepare.assert_not_called()
                executable.assert_not_called()
                execute.assert_not_called()

    def test_crt_and_gsl_overrides_are_rejected_in_joined_and_split_compiler_options(self):
        for arguments in (
            ["--copt=/MDd"],
            ["--copt", "/MT"],
            ["--host_copt=-MD"],
            ["--cxxopt=-D_ITERATOR_DEBUG_LEVEL=0"],
            ["--conlyopt=/D_DEBUG"],
            ["--copt=-UNDEBUG"],
            ["--copt=/Ugsl_CONFIG_CONTRACT_CHECKING_ON"],
            ["--copt=-Dgsl_CONFIG_CONTRACT_CHECKING_OFF"],
            ["--copt", "-DGSL_THROW_ON_CONTRACT_VIOLATION"],
            ["--per_file_copt=.*@/MD"],
            ["--features=-dynamic_link_msvcrt"],
            ["--features=static_link_msvcrt"],
            ["--linkopt=/NODEFAULTLIB:msvcrt.lib"],
            ["--copt=-fms-runtime-lib=static"],
            ["--define=gsl_CONFIG_CONTRACT_CHECKING_OFF=1"],
        ):
            with self.subTest(arguments=arguments), self.assertRaisesRegex(native.BazelWorkspaceError, "CRT/GSL"):
                bazel.validate_forwarded_arguments(arguments)


class BazelHygieneTests(unittest.TestCase):
    STATE = str(Path(tempfile.gettempdir()) / "aobus-bazel-hygiene-state")
    TOOLS = Path(tempfile.gettempdir()) / "aobus-bazel-hygiene-tools" / "bin" / "python"

    def gate(self, arguments, host="linux", environment=None, files=None):
        """Run the gate with its shared stages and tool bootstrap faked; return the gate's selections."""
        if files is None:
            files = ["script/ao/command/bazel.py", "lib/utility/Base64.cpp"]
        args = make_parser().parse_args(["bazel", "hygiene", *arguments])
        with (
            patch.object(native, "native_platform", return_value=host),
            patch.dict(os.environ, {"AOBUS_STATE_ROOT": self.STATE, **(environment or {})}),
            patch("ao.core.gitfiles.changed_files", return_value=files),
            patch("ao.core.pythonenv.ensure_environment", return_value=self.TOOLS) as tools,
            patch("ao.command.hygiene.run_command", return_value=0) as gate,
        ):
            self.assertEqual(bazel.run(args), 0)
        gate.assert_called_once()
        self.assertIs(gate.call_args.args[0], args)
        # The Nix shell's interpreter already carries Ruff and mypy.
        if host == "linux" or not any(name.endswith(".py") for name in files):
            tools.assert_not_called()
            self.assertIsNone(gate.call_args.kwargs["python"])
        else:
            tools.assert_called_once_with(PROJECT_ROOT, Path(self.STATE), platform_name=host)
            self.assertEqual(gate.call_args.kwargs["python"], self.TOOLS)
        self.assertEqual(args._resolved_sources, files)
        return gate.call_args.kwargs

    def test_lint_stage_routes_python_to_its_checks_and_cpp_to_bazel_lint_in_the_gate_tree(self):
        tree = str(Path(tempfile.gettempdir()) / "aobus-bazel-hygiene")
        stage = self.gate(
            [
                *("--config", "release", "--path", tree, "--vcpkg-prefix", "/vcpkg", "-j", "6"),
                *("--remote-cache", "https://cache.example"),
            ]
        )
        self.assertIsNone(stage["clang_format"])
        label, lint = stage["lint"]
        self.assertEqual(label, "bazel lint")
        files = ["script/ao/command/bazel.py", "lib/utility/Base64.cpp", "lib/utility/Missing.cpp"]
        # The gate parses lint's own registration, as ao hygiene does for its stages.
        parser = argparse.ArgumentParser()
        bazel.register(parser.add_subparsers())
        defaults = vars(parser.parse_args(["bazel", "lint"]))
        for explicit in (True, False):
            with (
                self.subTest(explicit=explicit),
                patch("ao.core.pythoncheck.run_paths", return_value=0) as python,
                patch("ao.core.bazel_lint.run", return_value=1) as native_lint,
                patch("sys.stdout", new_callable=io.StringIO),
            ):
                self.assertEqual(lint(files, explicit), 1)
            python.assert_called_once_with(["script/ao/command/bazel.py"], python=None)
            selected = vars(native_lint.call_args.args[0])
            self.assertEqual(selected.pop("_resolved_sources"), ["lib/utility/Base64.cpp"])
            self.assertEqual(selected.keys(), defaults.keys())
            expected = {
                **defaults,
                "config": "release",
                "path": tree,
                "vcpkg_prefix": "/vcpkg",
                "jobs": 6,
                "remote_cache": "https://cache.example",
                "files": ["lib/utility/Base64.cpp"] if explicit else [],
            }
            self.assertEqual(selected, expected)

    def test_python_failure_fails_the_stage_without_starting_bazel_for_no_cpp_scope(self):
        _, lint = self.gate([])["lint"]
        with (
            patch("ao.core.pythoncheck.run_paths", return_value=1),
            patch("ao.core.bazel_lint.run", side_effect=AssertionError("no C++ scope")),
            patch("sys.stdout", new_callable=io.StringIO),
        ):
            self.assertEqual(lint(["script/ao/command/bazel.py"], True), 1)

    def test_native_hosts_run_only_ruff_and_mypy_in_the_managed_tool_environment(self):
        for host in ("macos", "windows"):
            with (
                self.subTest(host=host),
                patch("ao.core.windows_llvm_sdk.ready_root", return_value=(Path("sdk"), True)),
            ):
                _, lint = self.gate([], host=host)["lint"]
                with (
                    patch("ao.core.pythoncheck.run_paths", return_value=0) as python,
                    patch("sys.stdout", new_callable=io.StringIO),
                ):
                    self.assertEqual(lint(["script/ao/command/bazel.py"], True), 0)
                python.assert_called_once_with(["script/ao/command/bazel.py"], python=self.TOOLS)

    def test_empty_scope_skips_native_tool_requirements(self):
        for host in ("windows", "macos"):
            args = make_parser().parse_args(["bazel", "hygiene"])
            with (
                self.subTest(host=host),
                patch.object(native, "native_platform", return_value=host),
                patch("ao.core.gitfiles.changed_files", return_value=[]) as changed,
                patch("ao.core.windows_llvm_sdk.ready_root", side_effect=AssertionError("no SDK for empty scope")),
                patch("ao.core.pythonenv.ensure_environment", side_effect=AssertionError("no empty tooling stage")),
                patch("ao.command.format.run_command", side_effect=AssertionError("no formatting stage")),
                patch("sys.stdout", new_callable=io.StringIO) as stdout,
            ):
                self.assertEqual(bazel.run(args), 0)
            changed.assert_called_once()
            self.assertEqual(args._resolved_sources, [])
            self.assertIn("No files to check.", stdout.getvalue())

    def test_python_only_windows_scope_needs_no_llvm_sdk(self):
        source = "script/ao/core/pythoncheck.py"
        args = make_parser().parse_args(["bazel", "hygiene", source])
        with (
            patch.object(native, "native_platform", return_value="windows"),
            patch.dict(os.environ, {"AOBUS_STATE_ROOT": self.STATE}),
            patch("ao.core.windows_llvm_sdk.ready_root", side_effect=AssertionError("SDK not provisioned")),
            patch("ao.core.pythonenv.ensure_environment", return_value=self.TOOLS) as tools,
            patch("ao.command.format.run_ruff_format", return_value=0) as formatting,
            patch("ao.core.pythoncheck.run_paths", return_value=0) as python,
            patch("ao.core.bazel_lint.run", side_effect=AssertionError("no C++ stage")),
            patch("sys.stdout", new_callable=io.StringIO),
        ):
            self.assertEqual(bazel.run(args), 0)
        tools.assert_called_once_with(PROJECT_ROOT, Path(self.STATE), platform_name="windows")
        formatting.assert_called_once_with([source], check=True, python=self.TOOLS)
        python.assert_called_once_with([source], python=self.TOOLS)

    def test_cpp_only_native_scope_does_not_prepare_python_tools(self):
        source = "lib/utility/Base64.cpp"
        for host in ("windows", "macos"):
            args = make_parser().parse_args(["bazel", "hygiene", source])
            with (
                self.subTest(host=host),
                patch.object(native, "native_platform", return_value=host),
                patch("ao.core.windows_llvm_sdk.ready_root", return_value=(Path("sdk"), True)) as sdk,
                patch("ao.core.pythonenv.ensure_environment", side_effect=AssertionError("no Python scope")),
                patch("ao.command.format.run_clang_format", return_value=0) as formatting,
                patch("ao.command.name_audit.run_command", return_value=0),
                patch("ao.core.pythoncheck.run_paths", side_effect=AssertionError("no Python stage")),
                patch("ao.core.bazel_lint.run", return_value=0) as native_lint,
                patch("sys.stdout", new_callable=io.StringIO),
            ):
                self.assertEqual(bazel.run(args), 0)
            formatting.assert_called_once_with(
                [source],
                check=True,
                build_dir=None,
                clang_format=str(Path("sdk/bin/clang-format.exe")) if host == "windows" else None,
            )
            self.assertEqual(native_lint.call_args.args[0]._resolved_sources, [source])
            if host == "windows":
                sdk.assert_called_once()
            else:
                sdk.assert_not_called()

    def test_scope_is_reused_and_managed_tools_leave_bazel_on_the_portal_interpreter(self):
        sources = ["script/ao/core/pythoncheck.py", "lib/utility/Base64.cpp"]
        args = make_parser().parse_args(["bazel", "hygiene"])
        portal_python = sys.executable

        def lint(selected):
            self.assertEqual(selected._resolved_sources, [sources[1]])
            self.assertEqual(sys.executable, portal_python)
            return 0

        with (
            patch.object(native, "native_platform", return_value="windows"),
            patch.dict(os.environ, {"AOBUS_STATE_ROOT": self.STATE}),
            patch("ao.core.gitfiles.changed_files", return_value=sources) as changed,
            patch("ao.core.windows_llvm_sdk.ready_root", return_value=(Path("sdk"), True)),
            patch("ao.core.pythonenv.ensure_environment", return_value=self.TOOLS) as tools,
            patch("ao.command.format.run_clang_format", return_value=0),
            patch("ao.command.format.run_ruff_format", return_value=0) as formatting,
            patch("ao.command.name_audit.run_command", return_value=0),
            patch("ao.core.pythoncheck.run_paths", return_value=0) as python,
            patch("ao.core.bazel_lint.run", side_effect=lint) as native_lint,
            patch("sys.stdout", new_callable=io.StringIO),
        ):
            self.assertEqual(bazel.run(args), 0)
        changed.assert_called_once()
        tools.assert_called_once_with(PROJECT_ROOT, Path(self.STATE), platform_name="windows")
        formatting.assert_called_once_with([sources[0]], check=True, python=self.TOOLS)
        python.assert_called_once_with([sources[0]], python=self.TOOLS)
        native_lint.assert_called_once()
        self.assertEqual(sys.executable, portal_python)

    def test_tool_environment_failure_stops_before_any_stage(self):
        args = make_parser().parse_args(["bazel", "hygiene", "script/ao/command/bazel.py"])
        with (
            patch.object(native, "native_platform", return_value="macos"),
            patch.dict(os.environ, {"AOBUS_STATE_ROOT": self.STATE}),
            patch("ao.core.pythonenv.ensure_environment", side_effect=RuntimeError("no locked wheel")),
            patch("ao.command.hygiene.run_command", side_effect=AssertionError("no gate without tools")),
            patch("sys.stderr", new_callable=io.StringIO) as stderr,
            self.assertRaises(SystemExit),
        ):
            bazel.run(args)
        self.assertIn("Python tooling bootstrap failed: no locked wheel", stderr.getvalue())

    def test_windows_formats_with_the_provisioned_sdk_or_names_provisioning(self):
        from ao.core import windows_llvm_sdk

        with tempfile.TemporaryDirectory(prefix="aobus-bazel-hygiene-sdk-") as temporary:
            root = Path(temporary)
            for relative in windows_llvm_sdk.read_manifest(windows_llvm_sdk.MANIFEST).required_files:
                (root / relative).parent.mkdir(parents=True, exist_ok=True)
                (root / relative).write_bytes(b"")
            stage = self.gate(
                [], host="windows", environment={"AOBUS_LLVM_SDK_ROOT": temporary}, files=["lib/utility/Base64.cpp"]
            )
            self.assertEqual(stage["clang_format"], str(root / "bin/clang-format.exe"))
            (root / "bin/clang-format.exe").unlink()
            args = make_parser().parse_args(["bazel", "hygiene", "lib/utility/Base64.cpp"])
            with (
                patch.object(native, "native_platform", return_value="windows"),
                patch.dict(os.environ, {"AOBUS_LLVM_SDK_ROOT": temporary}),
                patch("ao.command.hygiene.run_command", side_effect=AssertionError("no gate without an SDK")),
                patch("sys.stderr", new_callable=io.StringIO) as stderr,
                self.assertRaises(SystemExit),
            ):
                bazel.run(args)
        self.assertIn("ao bazel provision", stderr.getvalue())


class NixDependencyMetadataTests(unittest.TestCase):
    def setUp(self):
        self.bin = Path("/nix/store/package-bin")
        self.out = Path("/nix/store/package-out")
        self.dev = Path("/nix/store/package-dev")
        self.entry = {
            "version": "1.2.3",
            "storePath": self.bin.as_posix(),
            "drvPath": "/nix/store/package.drv",
            "outputs": {
                "out": self.out.as_posix(),
                "bin": self.bin.as_posix(),
                "dev": self.dev.as_posix(),
                "doc": "/nix/store/package-doc",
            },
        }
        self.report = {"dependencies": {"fixture": self.entry}}
        self.installed = {self.bin, self.out, self.dev}
        for patcher in (
            patch.object(Path, "is_dir", lambda path: path in self.installed),
            patch.object(providers, "nix_root", side_effect=lambda path: path),
            patch.object(native, "executable", side_effect=AssertionError("no Nix CLI discovery")),
            patch.object(providers.subprocess, "run", side_effect=AssertionError("no Nix CLI query")),
        ):
            patcher.start()
            self.addCleanup(patcher.stop)

    def test_reported_selected_bin_and_sibling_outputs_do_not_require_derivation_file(self):
        with patch.object(Path, "is_file", side_effect=AssertionError("drv file presence is not authority")):
            root, version, derivation, outputs = providers.nix_dependency(self.report, "fixture")
        self.assertEqual(root, self.bin)
        self.assertEqual(version, "1.2.3")
        self.assertEqual(derivation, "/nix/store/package.drv")
        self.assertEqual(outputs, [self.bin, self.dev, self.out])

    def test_missing_or_malformed_report_membership_fails_closed(self):
        reports = [{}, *({"dependencies": value} for value in (None, [], True, {}))]
        reports.extend({"dependencies": {"fixture": value}} for value in (None, [], False))
        for key in self.entry:
            entry = copy.deepcopy(self.entry)
            del entry[key]
            reports.append({"dependencies": {"fixture": entry}})
        for report in reports:
            with self.subTest(report=report), self.assertRaisesRegex(native.BazelWorkspaceError, "re-enter"):
                providers.nix_dependency(report, "fixture")

    def test_all_metadata_types_and_output_names_are_validated(self):
        for key, value in (
            ("version", None),
            ("version", 1),
            ("version", ""),
            ("version", "1\n2"),
            ("storePath", None),
            ("storePath", []),
            ("drvPath", False),
            ("outputs", []),
            ("outputs", True),
            ("outputs", {}),
            ("outputs", {"out": None}),
            ("outputs", {"out": [str(self.out)]}),
            ("outputs", {"": str(self.out)}),
            ("outputs", {"out/bad": str(self.out)}),
            ("outputs", {"out\n": str(self.out)}),
            ("outputs", {1: str(self.out)}),
        ):
            report = copy.deepcopy(self.report)
            report["dependencies"]["fixture"][key] = value
            with self.subTest(key=key, value=value), self.assertRaisesRegex(native.BazelWorkspaceError, "re-enter"):
                providers.nix_dependency(report, "fixture")

    def test_raw_store_roots_are_validated_before_canonicalization_or_absence_filtering(self):
        for value in (
            "relative",
            "/tmp/provider",
            "/nix/store",
            "/nix/store/",
            "/nix/store/.",
            "/nix/store/..",
            "/nix/store//package",
            "/nix/store/package/",
            "/nix/store/package/include",
            "/nix/store/package/../other",
            "/nix/store/package\n",
            "/nix/store/package\x00",
        ):
            for field in ("storePath", "drvPath", "outputs"):
                report = copy.deepcopy(self.report)
                entry = report["dependencies"]["fixture"]
                entry[field] = {**entry["outputs"], "doc": value} if field == "outputs" else value
                with (
                    self.subTest(field=field, value=value),
                    patch.object(providers, "nix_root", side_effect=AssertionError("invalid raw root")),
                    self.assertRaisesRegex(native.BazelWorkspaceError, "re-enter"),
                ):
                    providers.nix_dependency(report, "fixture")
        for value in ("/nix/store/package", "/nix/store/.drv", "/nix/store/package.drv/nested"):
            self.entry["drvPath"] = value
            with self.subTest(derivation=value), self.assertRaisesRegex(native.BazelWorkspaceError, "re-enter"):
                providers.nix_dependency(self.report, "fixture")

    def test_selected_root_must_be_declared_and_installed(self):
        for installed in (True, False):
            report = copy.deepcopy(self.report)
            if installed:
                del report["dependencies"]["fixture"]["outputs"]["bin"]
            else:
                self.installed.remove(self.bin)
            with self.subTest(installed=installed), self.assertRaisesRegex(native.BazelWorkspaceError, "re-enter"):
                providers.nix_dependency(report, "fixture")

    def test_canonical_output_aliases_are_deduplicated_and_selected_root_is_retained(self):
        alias = Path("/nix/store/package-alias")
        self.installed.add(alias)
        self.entry["storePath"] = alias.as_posix()
        self.entry["outputs"]["alias"] = alias.as_posix()
        with patch.object(providers, "nix_root", side_effect=lambda path: self.bin if path == alias else path):
            root, _, _, outputs = providers.nix_dependency(self.report, "fixture")
        self.assertEqual(root, self.bin)
        self.assertEqual(outputs, [self.bin, self.dev, self.out])


class BazelWorkspaceTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="aobus-bazel-test-")
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name).resolve()
        self.small_policy = native.read_json(providers.POLICY)
        self.small_policy["packages"]["native"] = [self.small_policy["packages"]["native"][0]]
        self.small_policy["packages"]["test"] = [self.small_policy["packages"]["test"][0]]
        self.small_policy_path = self.root / "policy.json"
        self.write_json(self.small_policy_path, self.small_policy)

    def write_json(self, path, value):
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(json.dumps(value), encoding="utf-8")

    def test_checkout_paths_and_existing_cmake_trees_are_rejected(self):
        for path in (native.PROJECT_ROOT, native.PROJECT_ROOT / "build/bazel", native.PROJECT_ROOT.parent):
            with self.subTest(path=path), self.assertRaisesRegex(native.BazelWorkspaceError, "outside"):
                workspace.workspace_path(str(path), "debug")
        (self.root / "CMakeCache.txt").touch()
        with self.assertRaisesRegex(native.BazelWorkspaceError, "CMake tree"):
            workspace.workspace_path(str(self.root), "debug")

    @unittest.skipIf(os.name == "nt", "Native Windows aliases are covered by the canonical-anchor regression")
    def test_workspace_path_rejects_checkout_paths_via_source_symlink_alias(self):
        source = self.root / "source"
        source.mkdir()
        alias = self.root / "source-alias"
        alias.symlink_to(source, target_is_directory=True)
        with patch.object(native, "PROJECT_ROOT", alias):
            for selected in (source, source / "build", source.parent):
                with self.subTest(selected=selected), self.assertRaises(native.BazelWorkspaceError):
                    workspace.workspace_path(str(selected), "debug")

    def test_workspace_path_compares_mapped_and_unc_source_anchors_canonically(self):
        canonical = self.root / "canonical-source"
        original_resolve = Path.resolve
        for alias in (Path("Y:/checkout"), Path("//server/share/checkout")):

            def resolve(path, *args, source_alias=alias, **kwargs):
                return canonical if path == source_alias else original_resolve(path, *args, **kwargs)

            with (
                self.subTest(alias=alias),
                patch.object(native, "PROJECT_ROOT", alias),
                patch.object(Path, "resolve", resolve),
            ):
                for selected in (canonical, canonical / "build", canonical.parent):
                    with self.subTest(selected=selected), self.assertRaises(native.BazelWorkspaceError):
                        workspace.workspace_path(str(selected), "debug")

    def test_build_dir_override_is_exact_and_external(self):
        with patch.dict(os.environ, {"BUILD_DIR": str(self.root)}):
            self.assertEqual(workspace.workspace_path(None, "release"), self.root)

    def test_child_environment_drops_all_target_role_flags_not_compiler_identity(self):
        source = {
            "NIX_CFLAGS_COMPILE": "bad",
            "NIX_CFLAGS_COMPILE_FOR_TARGET": "bad",
            "NIX_LDFLAGS_FOR_BUILD": "bad",
            "CPLUS_INCLUDE_PATH": "bad",
            "CCACHE_DIR": "bad",
            "CXXFLAGS": "bad",
            "LIBRARY_PATH": "bad",
            "CXX": "/nix/store/clang++",
            "PATH": "/tools",
        }
        self.assertEqual(native.child_environment(source), {"CXX": "/nix/store/clang++", "PATH": "/tools"})
        self.assertEqual(source["NIX_CFLAGS_COMPILE_FOR_TARGET"], "bad")

    def test_tool_admission_requires_a_successful_exact_version_report(self):
        executable = Path("/tools/bazel")
        for reported, status, accepted in (("9.2.0", 0, True), ("8.0.0", 0, False), ("9.2.0", 1, False)):
            with (
                self.subTest(reported=reported, status=status),
                patch.object(
                    workspace.subprocess,
                    "run",
                    return_value=subprocess.CompletedProcess([], status, f"bazel {reported}\n"),
                ) as query,
            ):
                if accepted:
                    workspace._tool_version(executable, "9.2.0")
                else:
                    with self.assertRaisesRegex(native.BazelWorkspaceError, "9.2.0"):
                        workspace._tool_version(executable, "9.2.0")
                self.assertEqual(query.call_args.args[0], [str(executable), "--version"])

    @unittest.skipIf(os.name == "nt", "Nix store path identity is not a Windows provider boundary")
    def test_nix_source_build_version_suffix_requires_nix_store_path(self):
        result = subprocess.CompletedProcess([], 0, "bazel 9.2.0- (@non-git)\n")
        with patch.object(workspace.subprocess, "run", return_value=result):
            workspace._tool_version(Path("/nix/store/hash-bazel/bin/bazel"), "9.2.0")
            with self.assertRaises(native.BazelWorkspaceError):
                workspace._tool_version(Path("/ambient/bazel"), "9.2.0")

    def test_native_setup_rejects_download_hash_before_install(self):
        executable = self.root / "managed/bazel"
        lock = {
            "schemaVersion": 1,
            "version": "9.2.0",
            "artifacts": {
                "darwin-arm64": {"url": "https://example.invalid/bazel", "sha256": "0" * 64},
            },
        }

        def download(url, path):
            Path(path).write_bytes(b"wrong")

        with (
            patch.object(native, "read_json", return_value=lock),
            patch.object(native, "native_platform", return_value="macos"),
            patch.object(workspace, "_artifact_key", return_value="darwin-arm64"),
            patch.object(workspace, "_managed_tool", return_value=executable),
            patch.object(workspace.urllib.request, "urlretrieve", side_effect=download),
            patch.dict(os.environ, {}, clear=True),
        ):
            with self.assertRaisesRegex(native.BazelWorkspaceError, "SHA-256"):
                workspace.bazel_executable(setup=True)
        self.assertFalse(executable.exists())

    def test_native_setup_installs_only_verified_bytes_and_checks_version(self):
        executable = self.root / "managed/bazel"
        payload = b"verified executable"
        lock = {
            "schemaVersion": 1,
            "version": "9.2.0",
            "artifacts": {
                "darwin-arm64": {"url": "https://example.invalid/bazel", "sha256": hashlib.sha256(payload).hexdigest()},
            },
        }
        with (
            patch.object(native, "read_json", return_value=lock),
            patch.object(native, "native_platform", return_value="macos"),
            patch.object(workspace, "_artifact_key", return_value="darwin-arm64"),
            patch.object(workspace, "_managed_tool", return_value=executable),
            patch.object(
                workspace.urllib.request, "urlretrieve", side_effect=lambda url, path: Path(path).write_bytes(payload)
            ),
            patch.object(workspace, "_tool_version") as version,
            patch.object(workspace.os, "replace", wraps=os.replace) as replace,
            patch.dict(os.environ, {}, clear=True),
        ):
            self.assertEqual(workspace.bazel_executable(setup=True), executable)
            self.assertEqual(workspace.bazel_executable(), executable)
        self.assertEqual(executable.read_bytes(), payload)
        self.assertEqual(version.call_count, 2)
        replace.assert_called_once()
        staged, destination = replace.call_args.args
        self.assertEqual(destination, executable)
        self.assertEqual(staged.parent, executable.parent)
        self.assertNotEqual(staged, executable)
        self.assertFalse(staged.exists())

    def test_tool_publication_bad_checksum_preserves_old_or_missing_target(self):
        download = self.root / "download"
        download.write_bytes(b"bad download")
        for existing in (False, True):
            executable = self.root / f"bazel-{existing}"
            if existing:
                executable.write_bytes(b"old binary")
            with patch.object(workspace.os, "replace") as replace:
                with self.assertRaisesRegex(native.BazelWorkspaceError, "SHA-256"):
                    workspace._publish_tool(download, executable, "0" * 64)
            replace.assert_not_called()
            if existing:
                self.assertEqual(executable.read_bytes(), b"old binary")
            else:
                self.assertFalse(executable.exists())
        self.assertEqual(list(self.root.glob(".bazel-stage-*")), [])

    def test_tool_publication_reuses_matching_concurrent_winner(self):
        payload = b"verified binary"
        download, executable = self.root / "download", self.root / "bazel"
        download.write_bytes(payload)

        def competing_publish(staged, destination):
            destination.write_bytes(payload)
            raise PermissionError("concurrent setup started the executable")

        with patch.object(workspace.os, "replace", side_effect=competing_publish):
            workspace._publish_tool(download, executable, hashlib.sha256(payload).hexdigest())
        self.assertEqual(executable.read_bytes(), payload)
        with patch.object(workspace.os, "replace") as replace:
            workspace._publish_tool(download, executable, hashlib.sha256(payload).hexdigest())
        replace.assert_not_called()
        self.assertEqual(list(self.root.glob(".bazel-stage-*")), [])

    def test_write_json_preserves_content_and_does_not_rewrite_equivalent_values(self):
        path = self.root / "nested/target.json"
        payload = {"zeta": "café \u2713\n\x01", "alpha": {"b": 2, "a": [1, 2]}}
        native.write_json(path, payload)
        self.assertEqual(json.loads(path.read_text(encoding="utf-8")), payload)
        first = path.read_bytes()
        old = 1_000_000_000_000_000_000
        os.utime(path, ns=(old, old))
        sentinel = path.stat().st_mtime_ns
        equivalent = {"alpha": {"a": [1, 2], "b": 2}, "zeta": payload["zeta"]}
        native.write_json(path, equivalent)
        self.assertEqual(path.read_bytes(), first)
        self.assertEqual(path.stat().st_mtime_ns, sentinel)

    def test_write_json_rewrites_changed_values_with_new_bytes_and_timestamp(self):
        path = self.root / "target.json"
        native.write_json(path, {"value": 1})
        first = path.read_bytes()
        old = 1_000_000_000_000_000_000
        os.utime(path, ns=(old, old))
        sentinel = path.stat().st_mtime_ns
        native.write_json(path, {"value": 2})
        self.assertEqual(json.loads(path.read_text(encoding="utf-8")), {"value": 2})
        self.assertNotEqual(path.read_bytes(), first)
        self.assertNotEqual(path.stat().st_mtime_ns, sentinel)

    def test_write_json_read_failure_fails_closed_without_writing(self):
        path = self.root / "target.json"
        path.write_text("{}", encoding="utf-8")
        first = path.read_bytes()
        with patch.object(Path, "read_bytes", side_effect=PermissionError("denied")):
            with self.assertRaises(PermissionError):
                native.write_json(path, {"value": 1})
        self.assertEqual(path.read_bytes(), first)

    def provider(self):
        prefix = self.root / "installed/x64-windows-static-md"
        (prefix / "include").mkdir(parents=True)
        for config in ("debug", "release"):
            selected = prefix / ("debug" if config == "debug" else "")
            (selected / "lib/pkgconfig").mkdir(parents=True)
            (selected / "bin").mkdir()
        (prefix.parent / "vcpkg").mkdir()
        (prefix.parent / "vcpkg/status").write_text(
            "Package: icu\nVersion: 78.3#1\nArchitecture: x64-windows-static-md\nStatus: install ok installed\n\n"
            "Package: icu\nFeature: tools\nArchitecture: x64-windows-static-md\nStatus: install ok installed\n\n"
            "Package: catch2\nVersion: 3.99.1\nArchitecture: x64-windows-static-md\nStatus: install ok installed\n\n"
            "Package: icu\nVersion: 1.0\nArchitecture: x64-linux\nStatus: install ok installed\n",
            encoding="utf-8",
        )
        host = prefix.parent / "x64-windows"
        tool = host / "tools/pkgconf/pkgconf.exe"
        tool.parent.mkdir(parents=True)
        tool.touch()
        tool.chmod(0o755)
        tool.with_name("pkgconf-8.dll").write_bytes(b"host pkgconf runtime")
        status = prefix.parent / "vcpkg/status"
        with status.open("a", encoding="utf-8") as stream:
            stream.write(
                "\n\nPackage: pkgconf\nVersion: 3.0.3\nArchitecture: x64-windows\nStatus: install ok installed\n"
            )
        info = prefix.parent / "vcpkg/info"
        info.mkdir()
        (info / "pkgconf_3.0.3_x64-windows.list").write_text(
            "x64-windows/tools/pkgconf/pkgconf.exe\nx64-windows/tools/pkgconf/pkgconf-8.dll\n", encoding="utf-8"
        )
        for relative, text in {
            "unicode/uvernum.h": "#define U_ICU_VERSION_MAJOR_NUM 78\n#define U_ICU_VERSION_MINOR_NUM 3\n",
            "unicode/platform.h": "#if defined(U_STATIC_IMPLEMENTATION)\n#elif 1\n#   define U_EXPORT\n#endif\n",
            "unicode/utypes.h": (
                "#if defined(U_COMBINED_IMPLEMENTATION)\n#define U_DATA_API U_EXPORT\n#elif 1\n"
                "#define U_DATA_API\n#define U_COMMON_API\n#define U_I18N_API\n#define U_LAYOUT_API\n"
                "#define U_LAYOUTEX_API\n#define U_IO_API\n#define U_TOOLUTIL_API\n"
                "#elif defined(U_COMMON_IMPLEMENTATION)\n#endif\n"
            ),
            "catch2/catch_version_macros.hpp": (
                "#define CATCH_VERSION_MAJOR 3\n#define CATCH_VERSION_MINOR 99\n#define CATCH_VERSION_PATCH 1\n"
            ),
        }.items():
            marker = prefix / "include" / relative
            marker.parent.mkdir(parents=True, exist_ok=True)
            marker.write_text(text)
        return prefix

    def nix_entry(self, root, version, outputs):
        return {
            "version": version,
            "storePath": "/nix/store/" + root.name,
            "drvPath": "/nix/store/" + root.name + ".drv",
            "outputs": {name: "/nix/store/" + path.name for name, path in outputs.items()},
        }

    @contextmanager
    def nix_paths(self, roots):
        paths = {Path("/nix/store") / root.name: root for root in roots}
        original_is_dir = Path.is_dir

        def is_dir(path):
            if path.parent == Path("/nix/store"):
                return path in paths and original_is_dir(paths[path])
            return original_is_dir(path)

        def store_root(path):
            resolved = paths.get(path, path).resolve()
            return next(root for root in roots if resolved.is_relative_to(root))

        with patch.object(Path, "is_dir", is_dir), patch.object(providers, "nix_root", side_effect=store_root):
            yield

    def linux_provider(self, *, wrong_metadata=False, split_library_output=False):
        icu, dev, catch, other = (
            self.root / name for name in ("icu-out", "icu-dev", "catch2", "other-icu-same-version")
        )
        report_path = self.root / "nix-report.json"
        report = {
            "schemaVersion": 1,
            "nixpkgsRevision": native.read_json(native.PROJECT_ROOT / "nixpkgs.json")["rev"],
            "dependencies": {
                "icu": self.nix_entry(icu, "78.3", {"out": icu, "dev": dev}),
                "catch2": self.nix_entry(catch, "3.16.0", {"out": catch}),
            },
        }
        self.write_json(report_path, report)
        roots = [icu, dev, catch, other]
        for root in roots:
            (root / "lib").mkdir(parents=True, exist_ok=True)
        for root in (dev, catch, other):
            (root / "lib/pkgconfig").mkdir(exist_ok=True)

        def query(argv, **kwargs):
            self.assertEqual(argv[0], "/tools/pkg-config")
            variable, module = argv[1].removeprefix("--variable="), argv[2]
            if module == "catch2-with-main":
                root = catch
            elif variable == "pcfiledir":
                root = catch if wrong_metadata == "catch2" else other if wrong_metadata else dev
            elif variable == "libdir":
                root = dev if split_library_output else icu
            else:
                root = dev
            suffix = {"pcfiledir": "lib/pkgconfig", "libdir": "lib", "includedir": "include"}[variable]
            return subprocess.CompletedProcess(argv, 0, str(root / suffix))

        with (
            patch.dict(
                os.environ,
                {
                    "AOBUS_NIX_DEPENDENCY_REPORT": str(report_path),
                    "PKG_CONFIG_PATH_FOR_TARGET": str(other / "lib/pkgconfig"),
                },
            ),
            patch.object(
                native,
                "executable",
                side_effect=lambda name, **kwargs: {"pkg-config": "/tools/pkg-config", "readelf": "/tools/readelf"}[
                    name
                ],
            ),
            self.nix_paths(roots),
            patch.object(providers.subprocess, "run", side_effect=query),
        ):
            return providers.linux_provider(self.small_policy, identity_path=self.root / "identity.json")

    def test_linux_provider_whitelists_only_package_report_outputs_and_metadata(self):
        base, versions, expected = self.linux_provider()
        self.assertEqual(versions["icu"], "78.3")
        self.assertEqual(expected["icu"], [self.root / "icu-dev", self.root / "icu-out"])
        self.assertNotIn(str(self.root / "other-icu-same-version"), base["allowed_roots"])
        self.assertEqual(
            base["pkg_config"]["pc_dirs"],
            sorted(
                [
                    str(self.root / "icu-dev/lib/pkgconfig"),
                    str(self.root / "catch2/lib/pkgconfig"),
                ]
            ),
        )
        identity = json.loads((self.root / "identity.json").read_text())
        self.assertEqual(identity["packages"]["icu"]["deriver"], "/nix/store/icu-out.drv")
        self.assertEqual(identity["packages"]["icu"]["outputs"], [str(path) for path in expected["icu"]])
        self.assertEqual(identity["packages"]["icu"]["storePath"], str(self.root / "icu-out"))

    def test_linux_provider_rejects_metadata_outside_each_packages_reported_outputs(self):
        for wrong_metadata in (True, "catch2"):
            with (
                self.subTest(wrong_metadata=wrong_metadata),
                self.assertRaisesRegex(native.BazelWorkspaceError, "does not match the Nix report"),
            ):
                self.linux_provider(wrong_metadata=wrong_metadata)

    def test_linux_provider_accepts_split_dev_library_directory_from_reported_outputs(self):
        base, _, expected = self.linux_provider(split_library_output=True)
        self.assertIn(str(self.root / "icu-dev/lib"), base["library_dirs"])
        self.assertEqual(expected["icu"], [self.root / "icu-dev", self.root / "icu-out"])

    def test_linux_resolution_rejects_actual_library_outside_report_output(self):
        expected = self.root / "reported"
        library = self.root / "same-version-other-output/libicu.so"
        library.parent.mkdir()
        library.touch()
        manifest = {
            "modules": {},
            "packages": [
                {
                    "name": "icu",
                    "interface": {"header_dirs": []},
                    "resolved_libraries": [{"kind": "shared", "file": str(library)}],
                }
            ],
        }
        with patch.object(providers, "nix_root", side_effect=lambda path: path.parent):
            with self.assertRaisesRegex(native.BazelWorkspaceError, "resolved library"):
                providers.verify_linux_resolution(manifest, {"icu": [expected]})

    def test_linux_resolution_accepts_siblings_but_rejects_other_packages_headers_and_libraries(self):
        root, dev, other = (self.root / name for name in ("package-out", "package-dev", "other-package"))
        manifest = {
            "modules": {},
            "packages": [
                {
                    "name": "icu",
                    "interface": {"header_dirs": [str(dev / "include")]},
                    "resolved_libraries": [{"kind": "shared", "file": str(root / "lib/libicu.so")}],
                }
            ],
        }
        expected = {"icu": [root, dev], "lmdb": [other]}
        with self.nix_paths([root, dev, other]):
            providers.verify_linux_resolution(manifest, expected)
            for role in ("header", "library"):
                foreign = copy.deepcopy(manifest)
                package = foreign["packages"][0]
                if role == "header":
                    package["interface"]["header_dirs"] = [str(other / "include")]
                else:
                    package["resolved_libraries"][0]["file"] = str(other / "lib/libicu.so")
                with self.subTest(role=role), self.assertRaisesRegex(native.BazelWorkspaceError, "library/header"):
                    providers.verify_linux_resolution(foreign, expected)

    def test_linux_resolution_metadata_uses_only_selected_group_output_union(self):
        root, transitive, other_group = (self.root / name for name in ("icu", "lmdb", "catch2"))
        manifest = {"packages": [], "modules": {"transitive": {"metadata": str(transitive / "lib/pkgconfig/lmdb.pc")}}}
        with self.nix_paths([root, transitive, other_group]):
            providers.verify_linux_resolution(manifest, {"icu": [root], "lmdb": [transitive]})
            manifest["modules"]["transitive"]["metadata"] = str(other_group / "lib/pkgconfig/catch2.pc")
            with self.assertRaisesRegex(native.BazelWorkspaceError, "resolved metadata"):
                providers.verify_linux_resolution(manifest, {"icu": [root], "lmdb": [transitive]})

    @unittest.skipUnless(sys.platform == "linux", "Nix multicall dispatch is a Linux provider boundary")
    def test_executable_preserves_pkg_config_multicall_symlink_invocation_name(self):
        executable = self.root / "tools/nix"
        executable.parent.mkdir()
        executable.write_text(
            f"#!{sys.executable}\nimport pathlib, sys\n"
            "assert pathlib.Path(sys.argv[0]).name == 'pkg-config'\n"
            "assert sys.argv[1:] == ['--version']\n"
            "print('1.0')\n",
            encoding="utf-8",
        )
        executable.chmod(0o755)
        alias = executable.with_name("pkg-config")
        alias.symlink_to(executable.name)
        with patch.object(native.shutil, "which", return_value=str(alias)):
            selected = native.executable("pkg-config")
        self.assertEqual(selected, str(alias))
        result = subprocess.run([selected, "--version"], capture_output=True, text=True, check=True)
        self.assertEqual(result.stdout, "1.0\n")

    def test_vcpkg_explicit_triplet_requires_status_and_headers(self):
        prefix = self.provider()
        self.assertEqual(providers.vcpkg_prefix(str(prefix), None), prefix)
        with self.assertRaisesRegex(native.BazelWorkspaceError, "triplet"):
            providers.vcpkg_prefix(str(prefix.parent), None)
        with self.assertRaisesRegex(native.BazelWorkspaceError, "prepared vcpkg prefix"):
            providers.vcpkg_prefix(None, None)

    def test_tree_selects_its_provisioned_triplet_and_never_cmake_metadata(self):
        prefix = self.provider()
        cmake = self.root / "cmake-debug"
        cmake.mkdir()
        (cmake / "CMakeCache.txt").write_text(
            f"VCPKG_INSTALLED_DIR:PATH={prefix.parent}\nVCPKG_TARGET_TRIPLET:STRING={prefix.name}\n", encoding="utf-8"
        )
        tree = self.root / "bazel-debug"
        with (
            patch.object(native, "native_platform", return_value="windows"),
            patch.object(workspace.builddir, "build_dir", return_value=cmake),
        ):
            with self.assertRaisesRegex(native.BazelWorkspaceError, "run `ao bazel provision`"):
                providers.vcpkg_prefix(None, tree)
            tree.mkdir()
            prefix.parent.rename(tree / "vcpkg_installed")
            self.assertEqual(
                providers.vcpkg_prefix(None, tree), (tree / "vcpkg_installed/x64-windows-static-md").resolve()
            )

    def test_vcpkg_triplets_follow_the_native_presets(self):
        with patch.object(native, "native_platform", return_value="windows"):
            self.assertEqual(providers.vcpkg_triplets(), ("x64-windows-static-md", "x64-windows"))
        with patch.object(native, "native_platform", return_value="macos"):
            with patch.dict(providers.os.environ, {"AOBUS_VCPKG_TRIPLET": "arm64-aobus-osx"}):
                self.assertEqual(providers.vcpkg_triplets(), ("arm64-aobus-osx", "arm64-aobus-osx"))
            with patch.dict(providers.os.environ, clear=True):
                with self.assertRaisesRegex(native.BazelWorkspaceError, "AOBUS_VCPKG_TRIPLET"):
                    providers.vcpkg_triplets()
        with patch.object(native, "native_platform", return_value="linux"):
            with self.assertRaisesRegex(native.BazelWorkspaceError, "Nix"):
                providers.vcpkg_triplets()
        presets = {
            preset["name"]: preset.get("cacheVariables", {})
            for preset in json.loads((native.PROJECT_ROOT / "CMakePresets.json").read_text(encoding="utf-8"))[
                "configurePresets"
            ]
        }
        windows = presets["windows-base"]
        self.assertEqual(
            (windows["VCPKG_TARGET_TRIPLET"], windows["VCPKG_HOST_TRIPLET"]), ("x64-windows-static-md", "x64-windows")
        )
        macos = presets["macos-base"]
        self.assertEqual(
            (macos["VCPKG_TARGET_TRIPLET"], macos["VCPKG_HOST_TRIPLET"]),
            ("$env{AOBUS_VCPKG_TRIPLET}", "$env{AOBUS_VCPKG_TRIPLET}"),
        )

    def test_native_provider_filters_host_triplet_and_selects_debug_paths(self):
        prefix = self.provider()
        base, versions = providers.native_provider(
            prefix, "debug", "windows", identity_path=self.root / "provider-identity.json", policy=self.small_policy
        )
        self.assertEqual(versions, {"icu": "78.3", "catch2": "3.99.1"})
        self.assertEqual(base["library_dirs"], [str(prefix / "debug/lib")])
        self.assertEqual(base["library_roots"], [str(prefix / "debug/lib")])
        self.assertEqual(base["runtime_roots"], [str(prefix / "debug/bin")])
        self.assertNotIn(str(prefix), base["allowed_roots"])
        self.assertIn(str(prefix / "include"), base["allowed_roots"])
        self.assertNotIn(str(prefix / "lib"), base["allowed_roots"])
        self.assertEqual(base["pkg_config"]["executable"], str(prefix.parent / "x64-windows/tools/pkgconf/pkgconf.exe"))
        self.assertTrue(base["provider"]["version"].startswith(f"{prefix}:"))
        identity = json.loads((self.root / "provider-identity.json").read_text())
        self.assertEqual(identity["packages"]["icu"]["features"], ["core", "tools"])
        self.assertEqual(identity["prefix"], str(prefix))

    def symlink_or_skip(self, link, target, *, directory=False):
        try:
            link.symlink_to(target, target_is_directory=directory)
        except OSError as exc:
            self.skipTest(f"host lacks symlink privileges: {exc}")

    def test_windows_provider_requires_static_target_triplet_and_declared_host_pkgconf(self):
        prefix = self.provider()

        def provider():
            return providers.native_provider(
                prefix, "debug", "windows", identity_path=self.root / "provider.json", policy=self.small_policy
            )

        # Static SDKs need not create target bin directories; host tool inputs stay explicit.
        shutil.rmtree(prefix / "bin")
        shutil.rmtree(prefix / "debug/bin")
        base, _ = provider()
        self.assertEqual(base["runtime_roots"], [])
        host = prefix.parent / "x64-windows"
        self.assertNotIn(str(host), base["allowed_roots"])
        self.assertIn(str(host / "tools/pkgconf/pkgconf-8.dll"), base["allowed_roots"])
        with self.assertRaisesRegex(native.BazelWorkspaceError, "x64-windows-static-md"):
            providers.native_provider(
                host, "debug", "windows", identity_path=self.root / "wrong.json", policy=self.small_policy
            )
        owned = prefix.parent / "vcpkg/info/pkgconf_3.0.3_x64-windows.list"
        original = owned.read_text(encoding="utf-8")
        owned.write_text(original.replace("x64-windows/tools/pkgconf/pkgconf.exe\n", ""), encoding="utf-8")
        with self.assertRaisesRegex(native.BazelWorkspaceError, "unowned|ownership"):
            provider()
        owned.write_text(original, encoding="utf-8")
        runtime = host / "tools/pkgconf/pkgconf-8.dll"
        # An installed DLL remains a tool input even if its ownership entry disappears.
        owned.write_text(original.replace("x64-windows/tools/pkgconf/pkgconf-8.dll\n", ""), encoding="utf-8")
        with self.assertRaisesRegex(native.BazelWorkspaceError, "unowned"):
            provider()
        owned.write_text(original, encoding="utf-8")
        extra = runtime.with_name("unowned.dll")
        extra.touch()
        with self.assertRaisesRegex(native.BazelWorkspaceError, "unowned"):
            provider()
        extra.unlink()
        runtime.unlink()
        with self.assertRaisesRegex(native.BazelWorkspaceError, "missing|runtime"):
            provider()

    def test_native_selected_directory_alias_cannot_authorize_opposite_configuration(self):
        prefix = self.provider()
        for config in ("debug", "release"):
            selected = prefix / ("debug" if config == "debug" else "")
            opposite = prefix / ("" if config == "debug" else "debug")
            for directory in ("lib", "bin"):
                path = selected / directory
                shutil.rmtree(path)
                self.symlink_or_skip(path, opposite / directory, directory=True)
                with (
                    self.subTest(config=config, directory=directory),
                    self.assertRaisesRegex(native.BazelWorkspaceError, "configuration|native provider root"),
                ):
                    providers.native_provider(
                        prefix,
                        config,
                        "windows",
                        identity_path=self.root / "alias-identity.json",
                        policy=self.small_policy,
                    )
                path.unlink()
                path.mkdir()
                if directory == "lib":
                    (path / "pkgconfig").mkdir()

    def test_native_header_and_tool_roots_cannot_escape_prefix_by_symlink(self):
        prefix = self.provider()
        for path in (prefix / "include", prefix.parent / "x64-windows/tools/pkgconf/pkgconf.exe"):
            target = self.root / ("outside-headers" if path.is_dir() else "outside-tool")
            directory = path.is_dir()
            if directory:
                shutil.copytree(path, target)
                shutil.rmtree(path)
            else:
                shutil.copyfile(path, target)
                path.unlink()
            self.symlink_or_skip(path, target, directory=directory)
            with self.subTest(path=path), self.assertRaisesRegex(native.BazelWorkspaceError, "native provider"):
                providers.native_provider(
                    prefix,
                    "debug",
                    "windows",
                    identity_path=self.root / "escape-identity.json",
                    policy=self.small_policy,
                )
            path.unlink()
            if directory:
                shutil.copytree(target, path)
            else:
                shutil.copyfile(target, path)

    def test_native_metadata_cannot_publish_opposite_configuration_catch2_archive(self):
        prefix = self.provider()
        (prefix / "lib/Catch2.lib").write_bytes(b"release static archive")
        (prefix / "debug/lib/Catch2d.lib").write_bytes(b"debug suffixed archive")
        for config in ("debug", "release"):
            selected = prefix / ("debug" if config == "debug" else "")
            opposite = prefix / ("" if config == "debug" else "debug")
            metadata = selected / "lib/pkgconfig/catch2-with-main.pc"
            metadata.write_text("Name: Catch2\nVersion: 3.99.1\n")
            base, _ = providers.native_provider(
                prefix, config, "windows", identity_path=self.root / "pc-identity.json", policy=self.small_policy
            )
            spec = {
                **base,
                "schema": 1,
                "platform": "windows",
                "config": config.title(),
                "linkage": "static",
                "packages": [
                    {
                        "name": "catch2",
                        "modules": ["catch2-with-main"],
                        "expected_versions": {"catch2-with-main": "3.99.1"},
                    }
                ],
            }
            query_state = {
                "pc_dir": str(metadata.parent),
                "library_dir": selected / "lib",
                "library_name": "Catch2d" if config == "debug" else "Catch2",
            }

            def query(argv, *, state=query_state, **kwargs):
                option = argv[1:]
                if "--modversion" in option:
                    text = "3.99.1"
                elif "--variable=pcfiledir" in option:
                    text = state["pc_dir"]
                elif "--cflags" in option:
                    text = f'-I"{prefix / "include"}"'
                elif "--libs" in option:
                    text = f'-L"{state["library_dir"]}" -l{state["library_name"]}'
                else:
                    text = ""
                return subprocess.CompletedProcess(argv, 0, text, "")

            with patch.object(deps.subprocess, "run", side_effect=query):
                adapter = deps._Adapter(spec, {"schema": 1, "patches": []})
                adapter.prepare()
                good = adapter.resolve()
                self.assertGreater(good["manifest"]["packages"][0]["declared_headers"], 0)
                self.assertEqual(
                    good["packages"][0]["libraries"][0]["source"],
                    str(selected / "lib" / (query_state["library_name"] + ".lib")),
                )
                query_state["library_dir"] = opposite / "lib"
                query_state["library_name"] = "Catch2" if config == "debug" else "Catch2d"
                with self.subTest(config=config), self.assertRaisesRegex(deps.BazelDependencyError, "outside"):
                    deps._Adapter(spec, {"schema": 1, "patches": []}).prepare()

    def test_native_provider_rejects_missing_manifest_features(self):
        prefix = self.provider()
        status = prefix.parent / "vcpkg/status"
        status.write_text(status.read_text().replace("Feature: tools", "Feature: unexpected"))
        with self.assertRaisesRegex(native.BazelWorkspaceError, "features"):
            providers.native_provider(
                prefix, "debug", "windows", identity_path=self.root / "identity.json", policy=self.small_policy
            )

    def test_windows_static_icu_patch_guards_archive_order_and_translates_only_system_tail(self):
        prefix = self.provider()
        policy = native.read_json(providers.POLICY)["windows_icu"]
        systems = ["kernel32", "user32", "gdi32", "winspool", "shell32", "ole32", "oleaut32", "comdlg32", "advapi32"]
        for config, suffix in (("Debug", "d"), ("Release", "")):
            spec = {
                "platform": "windows",
                "config": config,
                "linkage": "static",
                "system_libraries": systems,
                "provider": {"name": "vcpkg", "version": "exact-status"},
            }
            archives = ["icuin" + suffix, "icuuc" + suffix, "icudt" + suffix]
            flags = ["-l" + name for name in archives + systems]
            with patch.object(dependencies, "_metadata_flags", return_value=flags):
                result = dependencies._windows_icu_patch(spec, policy, prefix)
            self.assertEqual(result["id"], "windows-icu-system-links")
            self.assertEqual(result["when"]["provider_version"], "exact-status")
            self.assertEqual(result["when"]["config"], config)
            self.assertEqual(result["remove"], {"libraries": [{"name": name, "kind": "system"} for name in systems]})
            self.assertEqual(result["add"], {"linkopts": [name + ".lib" for name in systems]})
            self.assertEqual(result["expect"]["missing"], result["add"])
            self.assertEqual(
                result["expect"]["present"]["libraries"],
                [{"name": name, "kind": "static"} for name in archives] + result["remove"]["libraries"],
            )
            # The unicode-only import shares the identity gate with a narrower closure.
            uc_archives = ["icuuc" + suffix, "icudt" + suffix]
            uc_flags = ["-l" + name for name in uc_archives + systems]
            with patch.object(dependencies, "_metadata_flags", return_value=uc_flags):
                uc_result = dependencies._windows_icu_patch(spec, policy, prefix, package="icu_uc")
            self.assertEqual(uc_result["when"]["package"], "icu_uc")
            self.assertEqual(
                [item["name"] for item in uc_result["expect"]["present"]["libraries"] if item["kind"] == "static"],
                uc_archives,
            )
            for invalid in (flags[:-1], [*flags, "-lunknown"], flags[3:4] + flags[:3] + flags[4:], flags[1:]):
                with (
                    self.subTest(config=config, flags=invalid),
                    patch.object(dependencies, "_metadata_flags", return_value=invalid),
                ):
                    with self.assertRaisesRegex(native.BazelWorkspaceError, "sequence/interleaving"):
                        dependencies._windows_icu_patch(spec, policy, prefix)
            # The structural map admits any coherent installed release; only the triplet stays fixed.
            with self.assertRaisesRegex(native.BazelWorkspaceError, "unsupported Windows static ICU provider"):
                dependencies._windows_icu_patch(spec, policy, prefix.parent / "x64-windows")

    def test_macos_runtime_patch_removes_only_reviewed_trailing_implicit_libraries(self):
        policy = native.read_json(providers.POLICY)["macos_icu_runtime_remove"]
        spec = {"platform": "macos", "config": "Release", "provider": {"name": "vcpkg", "version": "prefix:status"}}
        with patch.object(dependencies, "_metadata_flags", return_value=["-licuuc", "-lc++", "-lpthread", "-lm"]):
            result = dependencies._macos_icu_runtime_patch(spec, policy)
        self.assertEqual(result["when"]["provider_version"], "prefix:status")
        self.assertEqual(
            result["remove"]["libraries"],
            [
                {"name": "c++", "kind": "system"},
                {"name": "pthread", "kind": "system"},
                {"name": "m", "kind": "system"},
            ],
        )
        self.assertEqual(result["expect"]["present"], result["remove"])
        with patch.object(dependencies, "_metadata_flags", return_value=["-licuuc", "-lunknown"]):
            self.assertIsNone(dependencies._macos_icu_runtime_patch(spec, policy))
        with patch.object(dependencies, "_metadata_flags", return_value=["-lc++", "-licuuc"]):
            with self.assertRaisesRegex(native.BazelWorkspaceError, "interleaving"):
                dependencies._macos_icu_runtime_patch(spec, policy)

    def test_refresh_rechecks_header_membership_against_current_ownership_inventory(self):
        prefix = self.provider()
        for name in ("manual-link/Catch2Maind.lib", "Catch2d.lib"):
            path = prefix / "debug/lib" / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.touch()
        info = prefix.parent / "vcpkg/info"
        info.mkdir(exist_ok=True)
        inventory = info / f"catch2_3.99.1_{prefix.name}.list"
        inventory.write_text(f"{prefix.name}/include/catch2/catch_version_macros.hpp\n", encoding="utf-8")
        tree = self.root / "bazel"
        observed = []
        with (
            patch.object(native, "native_platform", return_value="windows"),
            patch.object(providers, "POLICY", self.small_policy_path),
            native.capture_probes(observed.append),
        ):
            first = dependencies.resolve_dependencies(tree, "debug", prefix=str(prefix), group="test")
            added = prefix / "include/new-header.h"
            added.touch()
            unchanged = dependencies.resolve_dependencies(tree, "debug", prefix=str(prefix), group="test")
            self.assertEqual(first["manifest"], unchanged["manifest"])
            self.assertEqual(first["packages"], unchanged["packages"])
            with inventory.open("a", encoding="utf-8") as stream:
                stream.write(f"{prefix.name}/include/new-header.h\n")
            resolution = dependencies.resolve_dependencies(tree, "debug", prefix=str(prefix), group="test")
        manifest = resolution["manifest"]
        self.assertEqual(manifest["packages"][0]["declared_headers"], 2)
        self.assertEqual(manifest["applied_patches"], ["catch2-static-fallback"])
        self.assertEqual(
            manifest["packages"][0]["resolved_libraries"][0]["file"],
            str(prefix / "debug/lib/manual-link/Catch2Maind.lib"),
        )
        self.assertIn(
            str(added),
            [
                file["source"]
                for package in resolution["packages"]
                for header in package["headers"]
                for file in header["files"]
            ],
        )
        self.assertIn(str(inventory), resolution["watch_files"])
        self.assertIn(prefix / "debug/lib/pkgconfig/catch2-with-main.pc", observed)
        self.assertIn(str(prefix / "debug/lib"), resolution["watch_directories"])
        self.assertFalse((tree / "generated").exists())

    def test_policy_requires_explicit_package_arrays_without_singleton_coercion(self):
        for group in ("native", "test"):
            policy = native.read_json(providers.POLICY)
            policy["packages"][group] = policy["packages"][group][0]
            with (
                self.subTest(group=group),
                self.assertRaisesRegex(native.BazelWorkspaceError, "unsupported Bazel dependency groups"),
            ):
                providers.package_groups(policy)

    def test_shipped_policy_is_accepted_by_the_version_free_loader(self):
        providers.load_policy()

    def test_policy_loader_rejects_the_previous_review_schema(self):
        policy = native.read_json(providers.POLICY)
        policy["schema"] = 2
        self.write_json(self.root / "old-schema.json", policy)
        with patch.object(providers, "POLICY", self.root / "old-schema.json"):
            with self.assertRaisesRegex(native.BazelWorkspaceError, "unsupported Bazel dependency policy schema"):
                providers.load_policy()

    def test_policy_loader_recursively_rejects_reintroduced_version_fields(self):
        # A hand-edited schema number must not silently ignore legacy version pins.
        injections = (
            (("packages", "native", 0), "version", "78.4"),
            (("library_fallbacks", "cli11", "windows"), "status_version", "2.6.2"),
            (("header_fallbacks", "fakeit", "linux"), "provider_version", "2.5.0"),
            (("windows_audio", "flac"), "status_versions", {"libflac": "1.5.0"}),
            ((), "metadata_versions", {"mimalloc": {}}),
        )
        for path, field, value in injections:
            policy = native.read_json(providers.POLICY)
            target = policy
            for key in path:
                target = target[key]
            target[field] = value
            self.write_json(self.root / "policy.json", policy)
            with (
                self.subTest(path=path, field=field),
                patch.object(providers, "POLICY", self.root / "policy.json"),
                self.assertRaisesRegex(native.BazelWorkspaceError, "dependency version policy belongs to the existing"),
            ):
                providers.load_policy()

    def test_native_provider_records_all_ports_and_rejects_incoherent_boost(self):
        prefix = self.provider()
        status = prefix.parent / "vcpkg/status"
        additions = {
            "lmdb": "1.0.1",
            "ryml": "0.16.0",
            "c4core": "0.5.0",
            "boost-headers": "1.91.0",
            "boost-pfr": "1.91.0",
            # Installed transitive ports in the compiled Windows Process closure.
            "boost-date-time": "1.91.0",
            "boost-context": "1.91.0",
            "boost-atomic": "1.91.0",
            "gsl-lite": "1.1.1",
            "fast-float": "8.2.10",
            "xxhash": "0.8.3",
            "foonathan-lexy": "2025.5.0",
            "libflac": "1.5.0",
            "libogg": "1.3.6",
            "alac": "2017-11-03-c38887c5",
            "fdk-aac": "2.0.3",
            "mpg123": "1.33.7",
            "opus": "1.6.1",
            "fakeit": "2.5.0",
            "cli11": "2.6.2",
            "spdlog": "1.17.0",
            "ftxui": "7.0.3",
            "mimalloc": "fixture",
            "stb": "fixture",
        }
        for name in native.read_json(native.PROJECT_ROOT / "vcpkg.json")["dependencies"]:
            if isinstance(name, str) and name.startswith("boost-"):
                additions[name] = "1.91.0"
        with status.open("a", encoding="utf-8") as stream:
            for name, version in additions.items():
                stream.write(
                    f"\n\nPackage: {name}\nVersion: {version}\n"
                    f"Architecture: {prefix.name}\nStatus: install ok installed\n"
                )
            stream.write(
                f"\n\nPackage: spdlog\nFeature: tz-offset\nArchitecture: {prefix.name}\nStatus: install ok installed\n"
            )
        _, versions = providers.native_provider(
            prefix, "release", "windows", identity_path=self.root / "full-identity.json"
        )
        self.assertEqual(versions["rapidyaml"], "0.16.0")
        self.assertEqual(versions["boost"], "1.91.0")
        self.assertEqual(versions["lexy"], "2025.5.0")
        identity = native.read_json(self.root / "full-identity.json")
        self.assertEqual(identity["packages"]["c4core"]["version"], "0.5.0")
        status.write_text(
            status.read_text().replace("Package: boost-pfr\nVersion: 1.91.0", "Package: boost-pfr\nVersion: 1.90.0")
        )
        with self.assertRaisesRegex(native.BazelWorkspaceError, "inconsistent"):
            providers.native_provider(prefix, "release", "windows", identity_path=self.root / "full-identity.json")

    def test_metadata_fallback_rejects_partial_or_ambiguous_metadata(self):
        first, second = self.root / "pc-one", self.root / "pc-two"
        first.mkdir()
        second.mkdir()
        spec = {"pkg_config": {"pc_dirs": [str(first), str(second)]}}
        self.assertFalse(dependencies._has_metadata(spec, ["ryml"]))
        (first / "ryml.pc").touch()
        with self.assertRaisesRegex(native.BazelWorkspaceError, "incomplete"):
            dependencies._has_metadata(spec, ["ryml", "c4core"])
        (second / "ryml.pc").touch()
        with self.assertRaisesRegex(native.BazelWorkspaceError, "ambiguous"):
            dependencies._has_metadata(spec, ["ryml"])

    def installed_artifacts(self, root, names, *, shared=True):
        (root / "include").mkdir(parents=True, exist_ok=True)
        for config in ("debug", "release"):
            selected = root / ("debug" if config == "debug" else "")
            for name in names:
                artifacts = [f"lib/{name}.lib", f"bin/{name}.dll"] if shared else [f"lib/{name}.lib"]
                for relative in artifacts:
                    path = selected / relative
                    path.parent.mkdir(parents=True, exist_ok=True)
                    path.touch()

    def test_windows_reviewed_static_fallback_selects_exact_config_and_rejects_old_dll_layout(self):
        root = self.root / "triplet"
        self.installed_artifacts(root, ["lmdb"], shared=False)
        (root / "include/lmdb.h").touch()
        policy = native.read_json(providers.POLICY)
        for config in ("debug", "release"):
            interface = dependencies._library_interface({"name": "lmdb"}, [root], config, "windows", policy)
            selected = root / ("debug" if config == "debug" else "")
            self.assertEqual(
                interface["libraries"], [{"name": "lmdb", "file": str(selected / "lib/lmdb.lib"), "kind": "static"}]
            )
            runtime = selected / "bin/lmdb.dll"
            runtime.parent.mkdir(parents=True, exist_ok=True)
            runtime.touch()
            with (
                self.subTest(config=config),
                self.assertRaisesRegex(native.BazelWorkspaceError, "unsupported static provider shape"),
            ):
                dependencies._library_interface({"name": "lmdb"}, [root], config, "windows", policy)
            runtime.unlink()
        (root / "lib/lmdb.lib").unlink()
        with self.assertRaisesRegex(native.BazelWorkspaceError, "missing/ambiguous"):
            dependencies._library_interface({"name": "lmdb"}, [root], "release", "windows", policy)

    def test_windows_rapidyaml_fallback_uses_reviewed_static_c4core_without_cmake(self):
        root = self.root / "triplet"
        self.installed_artifacts(root, ["ryml", "c4core"], shared=False)
        (root / "include/ryml").mkdir()
        (root / "include/ryml/ryml.hpp").touch()
        policy = native.read_json(providers.POLICY)
        for config in ("debug", "release"):
            with (
                self.subTest(config=config),
                patch.object(
                    Path, "read_text", side_effect=AssertionError("artifact fallback must not read CMake exports")
                ),
            ):
                interface = dependencies._library_interface({"name": "rapidyaml"}, [root], config, "windows", policy)
            selected = root / ("debug" if config == "debug" else "")
            self.assertEqual(interface["header_dirs"], [str(root / "include"), str(root / "include/ryml")])
            self.assertEqual(interface["defines"], ["C4_CPP=17"])
            self.assertEqual(
                interface["libraries"],
                [
                    {"name": name, "kind": "static", "file": str(selected / "lib" / f"{name}.lib")}
                    for name in ("ryml", "c4core")
                ],
            )

    def test_windows_static_rapidyaml_rejects_import_or_alternate_archive_shapes(self):
        root = self.root / "triplet"
        self.installed_artifacts(root, ["ryml", "c4core"], shared=False)
        (root / "include/ryml.hpp").touch()
        policy = native.read_json(providers.POLICY)
        for config in ("debug", "release"):
            selected = root / ("debug" if config == "debug" else "")
            for library in ("ryml", "c4core"):
                for relative in (f"bin/{library}.dll", f"bin/{library}d.dll", f"lib/{library}d.lib"):
                    path = selected / relative
                    path.parent.mkdir(parents=True, exist_ok=True)
                    path.touch()
                    with (
                        self.subTest(config=config, relative=relative),
                        self.assertRaisesRegex(native.BazelWorkspaceError, "unsupported static provider shape"),
                    ):
                        dependencies._library_interface({"name": "rapidyaml"}, [root], config, "windows", policy)
                    path.unlink()
        (root / "debug/lib/c4core.lib").unlink()
        with self.assertRaisesRegex(native.BazelWorkspaceError, "missing/ambiguous"):
            dependencies._library_interface({"name": "rapidyaml"}, [root], "debug", "windows", policy)

    def test_native_manual_archive_symlink_cannot_cross_configurations(self):
        policy = native.read_json(providers.POLICY)
        for host, package, marker, libraries in (
            ("windows", "rapidyaml", "ryml.hpp", ("ryml", "c4core")),
            ("macos", "rapidyaml", "ryml.hpp", ("ryml", "c4core")),
            ("windows", "lmdb", "lmdb.h", ("lmdb",)),
        ):
            for config in ("debug", "release"):
                root = self.root / f"archive-{host}-{package}-{config}"
                (root / "include").mkdir(parents=True)
                (root / "include" / marker).touch()
                selected = root / ("debug" if config == "debug" else "")
                opposite = root / ("" if config == "debug" else "debug")
                for current in (selected, opposite):
                    (current / "lib").mkdir(parents=True)
                    for name in libraries:
                        filename = f"{name}.lib" if host == "windows" else f"lib{name}.a"
                        (current / "lib" / filename).touch()
                dependencies._library_interface({"name": package}, [root], config, host, policy)
                filename = libraries[0] + ".lib" if host == "windows" else "lib" + libraries[0] + ".a"
                archive = selected / "lib" / filename
                archive.unlink()
                self.symlink_or_skip(archive, opposite / "lib" / filename)
                with (
                    self.subTest(host=host, package=package, config=config),
                    self.assertRaisesRegex(native.BazelWorkspaceError, "configuration"),
                ):
                    dependencies._library_interface({"name": package}, [root], config, host, policy)

    def test_catch2_manual_archive_symlink_cannot_cross_configurations(self):
        policy = native.read_json(providers.POLICY)["catch2_fallback"]
        for host in ("windows", "macos"):
            for config in ("debug", "release"):
                root = self.root / f"catch2-{host}-{config}"
                selected = root / ("debug" if config == "debug" else "")
                opposite = root / ("" if config == "debug" else "debug")
                for template in policy[host]:
                    file = selected / "lib" / template.format(debug_suffix="d" if config == "debug" else "")
                    file.parent.mkdir(parents=True, exist_ok=True)
                    file.touch()
                file = selected / "lib" / policy[host][0].format(debug_suffix="d" if config == "debug" else "")
                other = opposite / "lib/other-configuration-archive"
                other.parent.mkdir(parents=True, exist_ok=True)
                other.touch()
                file.unlink()
                self.symlink_or_skip(file, other)
                spec = {
                    "platform": host,
                    "config": config.title(),
                    "library_dirs": [str(selected / "lib")],
                    "provider": {"name": "vcpkg", "version": "fixture"},
                }
                with (
                    self.subTest(host=host, config=config),
                    self.assertRaisesRegex(native.BazelWorkspaceError, "configuration"),
                ):
                    dependencies._catch_fallback(spec, policy, root)

    def test_macos_complete_fallback_requires_selected_static_archives(self):
        root = self.root / "triplet"
        (root / "include").mkdir(parents=True)
        (root / "include/ryml.hpp").touch()
        for name in ("ryml", "c4core"):
            file = root / "debug/lib" / f"lib{name}.a"
            file.parent.mkdir(parents=True, exist_ok=True)
            file.touch()
        interface = dependencies._library_interface(
            {"name": "rapidyaml"}, [root], "debug", "macos", native.read_json(providers.POLICY)
        )
        self.assertEqual(interface["defines"], ["C4_CPP=17"])
        self.assertTrue(
            all(
                item["kind"] == "static" and Path(item["file"]).is_relative_to(root / "debug/lib")
                for item in interface["libraries"]
            )
        )
        with self.assertRaisesRegex(native.BazelWorkspaceError, "missing/ambiguous"):
            dependencies._library_interface(
                {"name": "rapidyaml"}, [root], "release", "macos", native.read_json(providers.POLICY)
            )

    def test_header_fallback_requires_exact_provider_version_and_unique_root(self):
        root = self.root / "headers"
        marker = root / "include/boost/version.hpp"
        marker.parent.mkdir(parents=True)
        marker.write_text("#define BOOST_VERSION 109100\n")
        self.assertEqual(native.header_root([root], "boost/version.hpp"), root / "include")
        native.verify_header_version(marker, "1.91.0", ["BOOST_VERSION"], encoded=True)
        with self.assertRaisesRegex(native.BazelWorkspaceError, "header version mismatch"):
            native.verify_header_version(marker, "1.90.0", ["BOOST_VERSION"], encoded=True)
        other = self.root / "other/include/boost/version.hpp"
        other.parent.mkdir(parents=True)
        other.touch()
        with self.assertRaisesRegex(native.BazelWorkspaceError, "missing/ambiguous"):
            native.header_root([root, other.parents[2]], "boost/version.hpp")

    def gperf_report(self):
        root = self.root / "nix-gperf"
        executable = root / "bin/gperf"
        executable.parent.mkdir(parents=True)
        executable.write_bytes(b"gperf bytes")
        executable.chmod(0o755)
        report = self.root / "nix-report.json"
        self.write_json(
            report,
            {
                "schemaVersion": 1,
                "nixpkgsRevision": native.read_json(native.PROJECT_ROOT / "nixpkgs.json")["rev"],
                "dependencies": {"gperf": self.nix_entry(root, "3.3", {"out": root})},
            },
        )
        return root, executable, report

    def test_linux_gperf_uses_report_root_and_deriver_without_ambient_lookup(self):
        root, executable, report = self.gperf_report()
        with (
            patch.dict(os.environ, {"AOBUS_NIX_DEPENDENCY_REPORT": str(report)}),
            self.nix_paths([root]),
            patch.object(native.shutil, "which", side_effect=AssertionError("no ambient gperf lookup")),
        ):
            identity, selected, runtime, evidence = host_tools._linux_gperf()
            self.assertEqual(selected, executable)
            self.assertEqual(runtime, [])
            self.assertEqual(identity["deriver"], "/nix/store/nix-gperf.drv")
            self.assertEqual(identity["version"], "3.3")
            self.assertIn(report, evidence)
            executable.unlink()
            with self.assertRaisesRegex(native.BazelWorkspaceError, "does not match the Nix report"):
                host_tools._linux_gperf()
        with patch.dict(os.environ, {}, clear=True), self.assertRaisesRegex(native.BazelWorkspaceError, "report"):
            host_tools._linux_gperf()
        value = native.read_json(report)
        value["dependencies"].clear()
        self.write_json(report, value)
        with (
            patch.dict(os.environ, {"AOBUS_NIX_DEPENDENCY_REPORT": str(report)}),
            self.assertRaisesRegex(native.BazelWorkspaceError, "gperf output identity.*re-enter"),
        ):
            host_tools._linux_gperf()

    def test_linux_gperf_rejects_foreign_symlink_target_and_stale_report(self):
        root, executable, report = self.gperf_report()
        other = self.root / "other-gperf-same-version"
        target = other / "bin/gperf"
        target.parent.mkdir(parents=True)
        target.write_bytes(executable.read_bytes())
        executable.unlink()
        self.symlink_or_skip(executable, target)
        with (
            patch.dict(os.environ, {"AOBUS_NIX_DEPENDENCY_REPORT": str(report)}),
            self.nix_paths([root, other]),
            self.assertRaisesRegex(native.BazelWorkspaceError, "does not match the Nix report"),
        ):
            host_tools._linux_gperf()
        value = native.read_json(report)
        value["nixpkgsRevision"] = "stale"
        self.write_json(report, value)
        with (
            patch.dict(os.environ, {"AOBUS_NIX_DEPENDENCY_REPORT": str(report)}),
            self.assertRaisesRegex(native.BazelWorkspaceError, "does not match nixpkgs"),
        ):
            host_tools._linux_gperf()

    def test_linux_gperf_accepts_only_its_reported_sibling_output_symlink(self):
        root, executable, report = self.gperf_report()
        sibling = self.root / "nix-gperf-bin"
        target = sibling / "bin/gperf"
        target.parent.mkdir(parents=True)
        target.write_bytes(executable.read_bytes())
        target.chmod(0o755)
        executable.unlink()
        self.symlink_or_skip(executable, target)
        value = native.read_json(report)
        value["dependencies"]["gperf"]["outputs"]["bin"] = "/nix/store/nix-gperf-bin"
        self.write_json(report, value)
        with (
            patch.dict(os.environ, {"AOBUS_NIX_DEPENDENCY_REPORT": str(report)}),
            self.nix_paths([root, sibling]),
            patch.object(native.shutil, "which", side_effect=AssertionError("no ambient gperf lookup")),
            patch.object(host_tools.subprocess, "run", side_effect=AssertionError("no Nix queries")),
        ):
            identity, selected, _, _ = host_tools._linux_gperf()
            self.assertEqual(selected, executable)
            self.assertNotEqual(selected, target)
            self.assertEqual(identity["deriver"], "/nix/store/nix-gperf.drv")
            del value["dependencies"]["gperf"]["outputs"]["bin"]
            self.write_json(report, value)
            with self.assertRaisesRegex(native.BazelWorkspaceError, "does not match the Nix report"):
                host_tools._linux_gperf()

    def native_gperf(self, host="windows"):
        installed = self.root / "installed"
        target = installed / ("x64-windows-static-md" if host == "windows" else "arm64-target")
        triplet = "x64-windows" if host == "windows" else "x64-aobus-osx"
        directory = installed / triplet / "tools/gperf"
        directory.mkdir(parents=True)
        executable = directory / ("gperf.exe" if host == "windows" else "gperf")
        executable.write_bytes(b"host gperf")
        executable.chmod(0o755)
        status = installed / "vcpkg/status"
        status.parent.mkdir()
        status.write_text(
            f"Package: gperf\nVersion: 3.3#1\nArchitecture: {triplet}\nStatus: install ok installed\n\n"
            "Package: gperf\nVersion: 3.1\nArchitecture: unused-triplet\nStatus: deinstall ok not-installed\n",
            encoding="utf-8",
        )
        info = installed / "vcpkg/info"
        info.mkdir()
        (info / f"gperf_3.3_{triplet}.list").write_text(f"{triplet}/tools/gperf/{executable.name}\n", encoding="utf-8")
        return target, executable, status

    def test_native_gperf_uses_unique_host_port_status_not_target_triplet_or_path(self):
        for host in ("windows", "macos"):
            with self.subTest(host=host):
                target, executable, status = self.native_gperf(host)
                runtime = executable.with_name("adjacent.dll")
                if host == "windows":
                    runtime.write_bytes(b"required runtime")
                    ownership = status.parent / "info/gperf_3.3_x64-windows.list"
                    with ownership.open("a", encoding="utf-8") as stream:
                        stream.write("x64-windows/tools/gperf/adjacent.dll\n")
                unrelated = executable.parents[2] / "bin/ambient.dll"
                unrelated.parent.mkdir()
                unrelated.touch()
                with (
                    patch.object(native.shutil, "which", side_effect=AssertionError("no ambient fallback")),
                    patch.object(native, "host_machine", return_value="AMD64"),
                    patch.object(native.platform, "machine", side_effect=AssertionError("no uname/WMI query")),
                ):
                    identity, selected, dlls, evidence = host_tools._native_gperf(target, host)
                self.assertEqual(identity["host_machine"], "AMD64")
                self.assertEqual(selected, executable)
                self.assertNotEqual(identity["triplet"], target.name)
                self.assertEqual(identity["version"], "3.3")
                self.assertEqual(identity["status_version"], "3.3#1")
                self.assertIn("unverified", identity["provenance"])
                self.assertEqual(dlls, [runtime] if host == "windows" else [])
                self.assertIn(status, evidence)
                shutil.rmtree(self.root / "installed")

    def test_native_gperf_rejects_ambiguous_or_missing_installed_host_identity(self):
        target, _, status = self.native_gperf()
        original = status.read_text()
        status.write_text(
            original + "\nPackage: gperf\nVersion: 3.2\nArchitecture: x64-windows\nStatus: install ok installed\n"
        )
        with self.assertRaisesRegex(native.BazelWorkspaceError, "ambiguous installed gperf host"):
            host_tools._native_gperf(target, "windows")
        status.write_text(original.replace("install ok installed", "deinstall ok not-installed"))
        with self.assertRaisesRegex(native.BazelWorkspaceError, "host port is not installed"):
            host_tools._native_gperf(target, "windows")

    def test_native_gperf_requires_manifest_host_request_and_exact_installed_executable(self):
        target, executable, _ = self.native_gperf()
        with patch.object(native, "read_json", return_value={"dependencies": [{"name": "gperf", "host": False}]}):
            with self.assertRaisesRegex(native.BazelWorkspaceError, "host:true"):
                host_tools._native_gperf(target, "windows")
        executable.unlink()
        with self.assertRaisesRegex(native.BazelWorkspaceError, "host executable is missing"):
            host_tools._native_gperf(target, "windows")

    def test_native_gperf_tool_and_adjacent_runtime_cannot_escape_narrow_directory(self):
        target, executable, _ = self.native_gperf()
        outside = executable.parents[2] / "bin/other.exe"
        outside.parent.mkdir()
        outside.write_bytes(b"not selected")
        executable.unlink()
        self.symlink_or_skip(executable, outside)
        with self.assertRaisesRegex(native.BazelWorkspaceError, "escapes intended"):
            host_tools._native_gperf(target, "windows")
        executable.unlink()
        executable.write_bytes(b"host tool")
        runtime = executable.with_name("runtime.dll")
        self.symlink_or_skip(runtime, outside)
        ownership = target.parent / "vcpkg/info/gperf_3.3_x64-windows.list"
        with ownership.open("a", encoding="utf-8") as stream:
            stream.write("x64-windows/tools/gperf/runtime.dll\n")
        with self.assertRaisesRegex(native.BazelWorkspaceError, "escapes intended"):
            host_tools._native_gperf(target, "windows")

    def resolve_gperf(self, executable, runtime, evidence):
        request = self.root / "request.json"
        self.write_json(request, {"schema": 1, "host": "linux"})
        output = self.root / "repository/_resolution"
        fixtures = self.root / "tool-inputs"
        for name in (
            "bin/genrb",
            "bin/pkgdata",
            "bin/dbus-daemon",
            "bin/glib-compile-resources",
            "lib/libglib-2.0.so",
            "glib-report.json",
            "lib/libdbus-1.so",
            "stdlib.py",
            "dbus-report.json",
            "catalog-sdk.json",
            "debug/libicuuc.a",
            "debug/status",
            "release/libicuuc.a",
            "release/status",
        ):
            path = fixtures / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(b"independent tool SDK fixture\n")
        for config in ("debug", "release"):
            (fixtures / config / "pkgconfig").mkdir(exist_ok=True)
        # Keep the resolver's audit hook isolated from the unittest process.
        # Each new consumer has an authored subplan/SDK selection, independent
        # of gperf. Production provider guards are not changed for this fixture.
        controller = textwrap.dedent(
            """\
            import json
            import sys
            from pathlib import Path
            from unittest.mock import patch
            sys.path.insert(0, sys.argv.pop(1))
            from ao.core import bazel_resolver
            from ao.core import bazel_native as native
            from ao.core import bazel_dependencies as dependencies
            from ao.core import bazel_host_tools as host_tools
            selection = ({'version': '3.3'}, Path(sys.argv.pop(1)),
                         [Path(p) for p in sys.argv.pop(1).split('|') if p], [Path(sys.argv.pop(1))])
            fixtures = Path(sys.argv.pop(1))

            def catalog(tree, config, *, prefix, group):
                assert prefix is None and group == 'catalog'
                return {
                    'packages': [{'name': 'icu', 'headers': [], 'includes': [], 'defines': [],
                                  'linkopts': [], 'runtime': [], 'libraries': [
                                      {'index': 0, 'kind': 'static', 'source': str(fixtures / config / 'libicuuc.a'),
                                       'basename': 'libicuuc.a', 'runtime': None}]}],
                    'manifest': {'config': config, 'package': 'icu'},
                    'watch_files': [str(fixtures / config / 'status')],
                    'watch_directories': [str(fixtures / config / 'pkgconfig')],
                }

            sdk = (
                {'python': sys.executable, 'genrb': str(fixtures / 'bin/genrb'),
                 'pkgdata': str(fixtures / 'bin/pkgdata')},
                [Path(sys.executable), fixtures / 'stdlib.py', fixtures / 'bin/genrb',
                 fixtures / 'bin/pkgdata', fixtures / 'catalog-sdk.json'],
                [fixtures / 'catalog-sdk.json'],
            )
            dbus = ({'provider': 'fixture', 'outputs': [str(fixtures)]}, fixtures / 'bin/dbus-daemon',
                    [fixtures / 'lib/libdbus-1.so'], [fixtures / 'dbus-report.json'])
            resource = ({'provider': 'fixture', 'outputs': [str(fixtures)]}, fixtures / 'bin/glib-compile-resources',
                        [fixtures / 'lib/libglib-2.0.so'], [fixtures / 'glib-report.json'])
            with (
                patch.object(native, 'native_platform', return_value='linux'),
                patch.object(host_tools, '_linux_gperf', return_value=selection),
                patch.object(dependencies, 'resolve_dependencies', side_effect=catalog) as imports,
                patch.object(host_tools, '_catalog_sdk', return_value=sdk),
                patch.object(host_tools, '_linux_dbus', return_value=dbus),
                patch.object(host_tools, '_linux_gresource', return_value=resource),
                patch.object(host_tools, '_app_version', return_value='1.2.3'),
            ):
                status = bazel_resolver.main()
                if status == 0:
                    assert [call.args[1] for call in imports.call_args_list] == ['debug', 'release']
                raise SystemExit(status)
            """
        )
        result = subprocess.run(
            [
                sys.executable,
                "-I",
                "-B",
                "-c",
                controller,
                str(native.PROJECT_ROOT / "script"),
                str(executable),
                "|".join(map(str, runtime)),
                str(evidence),
                str(fixtures),
                "--request",
                str(request),
                "--group",
                "tools",
                "--output",
                str(output),
                "--environment-json",
                json.dumps({name: os.environ[name] for name in ("PATH", "SYSTEMROOT", "WINDIR") if name in os.environ}),
            ],
            capture_output=True,
            text=True,
            check=False,
        )
        self.assertEqual(result.stderr, "")
        return result.returncode, native.read_json(output / "repository.json")

    @unittest.skipIf(os.name == "nt", "POSIX executable fixture; native Windows tools need native validation")
    def test_gperf_repository_plan_declares_runtime_and_evidence_watches_without_copying(self):
        _, executable, report = self.gperf_report()
        executable.write_text(
            f"#!{sys.executable}\nimport sys\nassert sys.argv[1:] == ['--version']\nprint('GNU gperf 3.3')\n"
        )
        runtime = executable.with_name("dependency.dll")
        runtime.write_bytes(b"runtime one")
        status, plan = self.resolve_gperf(executable, [runtime], report)
        self.assertEqual(status, 0, plan.get("error"))
        facts = plan["host_tools"]
        self.assertEqual(facts["gperf"]["executable"], str(executable))
        self.assertEqual(facts["gperf"]["runtime"], [str(runtime)])
        self.assertEqual(facts["gperf"]["evidence"], [str(report)])
        self.assertEqual(facts["gperf"]["identity"], {"version": "3.3"})
        self.assertNotIn("links", plan)
        self.assertNotIn("files", plan)
        self.assertLessEqual({str(executable), str(runtime), str(report)}, set(plan["watch_files"]))
        self.assertIn(str(executable.parent), plan["watch_directories"])
        fixtures = self.root / "tool-inputs"
        for config in ("debug", "release"):
            admitted = plan["imports"][config]
            self.assertNotIn("config", admitted)
            self.assertEqual(admitted["packages"][0]["libraries"][0]["source"], str(fixtures / config / "libicuuc.a"))
            self.assertIn(str(fixtures / config / "status"), plan["watch_files"])
            self.assertIn(str(fixtures / config / "pkgconfig"), plan["watch_directories"])
            self.assertEqual(
                admitted["manifest"],
                {"config": config, "package": "icu"},
            )
        self.assertLessEqual(
            {str(fixtures / "stdlib.py"), str(fixtures / "catalog-sdk.json")},
            set(facts["catalog"]["artifacts"]),
        )
        self.assertEqual(facts["catalog"]["evidence"], [str(fixtures / "catalog-sdk.json")])
        self.assertEqual(
            set(facts["dbus"]["artifacts"]),
            {str(fixtures / name) for name in ("bin/dbus-daemon", "lib/libdbus-1.so", "dbus-report.json")},
        )
        self.assertEqual(facts["app_version"], "1.2.3")
        self.assertEqual(
            facts["test_tools"],
            {
                "dbus_daemon": str(fixtures / "bin/dbus-daemon"),
                "dbus_daemon_directory": str(fixtures / "bin"),
            },
        )
        self.assertFalse((self.root / "repository/bin").exists())
        status, plan = self.resolve_gperf(executable, [], report)
        self.assertEqual(status, 0, plan.get("error"))
        self.assertEqual(plan["host_tools"]["gperf"]["executable"], str(executable))
        self.assertEqual(plan["host_tools"]["gperf"]["runtime"], [])
        self.assertNotIn("links", plan)
        self.assertNotIn("files", plan)
        self.assertNotIn(str(runtime), plan["watch_files"])

    @unittest.skipIf(os.name == "nt", "POSIX executable fixture; native Windows tools need native validation")
    def test_gperf_repository_rejects_non_gnu_version_nonexecution_and_runtime_basename_collision(self):
        _, executable, report = self.gperf_report()
        for version in ("GNU gperf 3.2", "gperf 3.3"):
            executable.write_text(f"#!{sys.executable}\nprint({version!r})\n")
            status, plan = self.resolve_gperf(executable, [], report)
            self.assertEqual(status, 1)
            self.assertIn("gperf executable version differs", plan["error"])
        executable.chmod(0o644)
        status, plan = self.resolve_gperf(executable, [], report)
        self.assertEqual(status, 1)
        self.assertIn("PermissionError", plan["error"])
        executable.chmod(0o755)
        executable.write_text(f"#!{sys.executable}\nprint('GNU gperf 3.3')\n")
        duplicate = executable.parent.parent / "GPERF"
        duplicate.touch()
        status, plan = self.resolve_gperf(executable, [duplicate], report)
        self.assertEqual(status, 1)
        self.assertIn("ambiguous gperf runtime basenames", plan["error"])

    def test_lexy_complete_manual_interface_records_truthful_version_evidence(self):
        root = self.root / "lexy"
        header = root / "include/lexy/_detail/config.hpp"
        header.parent.mkdir(parents=True)
        header.write_text("#define LEXY_DETAIL_CONFIG_HPP_INCLUDED\n")
        evidence = self.root / "lexy-identity.json"
        interface = dependencies._lexy_interface([root], "linux", "2025.05.0", evidence)
        self.assertEqual(interface, {"header_dirs": [str(root / "include")]})
        identity = native.read_json(evidence)
        self.assertEqual(identity["provider_version"], "2025.05.0")
        self.assertIn("no upstream version macro", identity["version_evidence"])
        self.assertEqual(identity["header"]["sha256"], hashlib.sha256(header.read_bytes()).hexdigest())
        spec = {
            "schema": 1,
            "platform": "linux",
            "config": "Debug",
            "linkage": "static",
            "provider": {"name": "nix", "version": "deriver", "evidence": native.evidence([evidence])},
            "allowed_roots": [str(root), str(evidence)],
            "packages": [{"name": "lexy", "modules": []}],
        }
        with patch.object(deps.subprocess, "run", side_effect=AssertionError("no metadata or CMake needed")):
            adapter = deps._Adapter(
                spec, {"schema": 1, "patches": [dependencies._manual_patch(spec, "lexy", interface)]}
            )
            adapter.prepare()
            resolution = adapter.resolve()
            manifest = resolution["manifest"]
        self.assertEqual(
            resolution["packages"][0]["headers"][0]["files"],
            [{"source": str(header), "path": "lexy/_detail/config.hpp"}],
        )
        self.assertEqual(manifest["counts"], {"automatic": 0, "manual_fallback": 1})
        self.assertEqual(manifest["packages"][0]["name"], "lexy")
        self.assertEqual(manifest["packages"][0]["resolved_libraries"], [])
        self.assertEqual(manifest["packages"][0]["declared_headers"], 1)
        self.assertEqual(resolution["packages"][0]["name"], "lexy")
        self.assertFalse((self.root / "lexy-projection").exists())
        header.write_text("not lexy")
        with self.assertRaisesRegex(native.BazelWorkspaceError, "unrecognized lexy"):
            dependencies._lexy_interface([root], "linux", "2025.05.0", evidence)

    def test_lexy_native_header_marker_cannot_escape_provider(self):
        root = self.root / "lexy"
        marker = root / "include/lexy/_detail/config.hpp"
        marker.parent.mkdir(parents=True)
        outside = self.root / "outside.hpp"
        outside.write_text("#define LEXY_DETAIL_CONFIG_HPP_INCLUDED\n")
        self.symlink_or_skip(marker, outside)
        with self.assertRaisesRegex(native.BazelWorkspaceError, "escapes intended"):
            dependencies._lexy_interface([root], "windows", "2025.5.0", self.root / "identity.json")

    def test_preparation_failure_prevents_bazel_execution(self):
        args = argparse.Namespace(
            bazel_action="test",
            path=str(self.root),
            config="debug",
            vcpkg_prefix=None,
            targets=[],
            jobs=2,
            remote_cache=None,
        )
        with (
            patch.object(workspace, "prepare_workspace", side_effect=native.BazelWorkspaceError("stale input")),
            patch.object(workspace, "execute") as execute,
            patch("sys.stderr"),
            self.assertRaises(SystemExit),
        ):
            bazel.run(args)
        execute.assert_not_called()

    def test_cli_prepares_and_executes_every_native_action_without_outer_refresh(self):
        prepared = self.root / "configuration"
        for action in ("deps", "build", "test", "query", "cquery", "aquery", "run"):
            arguments = ["bazel", action, "--path", str(self.root), "--config", "release"]
            if action != "deps":
                arguments.extend(["--", "deps(//test:media_test)" if action.endswith("query") else "//test:media_test"])
            args = make_parser().parse_args(arguments)
            with (
                self.subTest(action=action),
                patch.object(workspace, "prepare_workspace", return_value=prepared) as prepare,
                patch.object(
                    dependencies, "resolve_dependencies", side_effect=AssertionError("outer resolution is forbidden")
                ),
                patch.object(workspace, "bazel_executable", return_value=Path("bazel")),
                patch.object(workspace, "execute", return_value=7) as execute,
                patch("sys.stderr"),
            ):
                self.assertEqual(bazel.run(args), 7)
            prepare.assert_called_once_with(self.root, "release", prefix=None)
            execute.assert_called_once_with(
                Path("bazel"), self.root, prepared, action, "release", args.targets, jobs=2, remote_cache=None
            )

    def test_cli_preserves_analysis_stdout_for_machine_readable_output(self):
        for action in ("query", "cquery", "aquery"):
            args = make_parser().parse_args(
                ["bazel", action, "--path", str(self.root), "--", "//...", "--output=jsonproto"]
            )
            output, diagnostics = io.StringIO(), io.StringIO()

            def execute(*args, **kwargs):
                print('{"results": []}')
                return 0

            with (
                self.subTest(action=action),
                patch.object(workspace, "prepare_workspace", return_value=self.root / "configuration"),
                patch.object(workspace, "bazel_executable", return_value=Path("bazel")),
                patch.object(workspace, "execute", side_effect=execute),
                patch("sys.stdout", output),
                patch("sys.stderr", diagnostics),
            ):
                self.assertEqual(bazel.run(args), 0)
            self.assertEqual(output.getvalue(), '{"results": []}\n')
            self.assertIn("Prepared Bazel configuration:", diagnostics.getvalue())

    def test_setup_only_initializes_bazel_without_preparing_a_workspace(self):
        args = make_parser().parse_args(["bazel", "setup"])
        with (
            patch.object(workspace, "bazel_executable", return_value=Path("bazel")) as executable,
            patch.object(workspace, "prepare_workspace") as prepare,
            patch.object(workspace, "execute") as execute,
            patch("sys.stdout"),
        ):
            self.assertEqual(bazel.run(args), 0)
        executable.assert_called_once_with(setup=True)
        prepare.assert_not_called()
        execute.assert_not_called()

    def test_windows_execute_rejects_network_sources_before_launch_or_log_truncation(self):
        log = self.root / "build.log"
        cases = (
            (r"\\server\share\checkout", r"\\server\share\checkout"),
            (r"Y:\checkout", r"\\server\share\checkout"),
            (r"Y:\checkout", r"\\?\UNC\server\share\checkout"),
        )
        for declared, canonical in cases:
            for action in ("build", "test"):
                source = Mock(spec=Path, name=declared)
                source.resolve.return_value = PureWindowsPath(canonical)
                log.write_bytes(b"preserved failure evidence")
                with (
                    self.subTest(declared=declared, canonical=canonical, action=action),
                    patch.object(native, "native_platform", return_value="windows"),
                    patch.object(native, "PROJECT_ROOT", source),
                    patch.object(workspace.subprocess, "run") as run,
                    self.assertRaisesRegex(native.BazelWorkspaceError, "require a local source checkout"),
                ):
                    workspace.execute(
                        Path("bazel.exe"), self.root, self.root / "no-generated-inputs", action, "debug", []
                    )
                run.assert_not_called()
                self.assertEqual(log.read_bytes(), b"preserved failure evidence")

    def test_windows_execute_allows_normal_and_extended_length_local_source_paths(self):
        generated = self.root / "configuration"
        for canonical in (r"C:\checkout", r"\\?\C:\checkout"):
            source = Mock(spec=Path)
            source.resolve.return_value = PureWindowsPath(canonical)
            with (
                self.subTest(canonical=canonical),
                patch.object(native, "native_platform", return_value="windows"),
                patch.object(native, "PROJECT_ROOT", source),
                patch.object(workspace.subprocess, "run", return_value=subprocess.CompletedProcess([], 0)) as run,
            ):
                self.assertEqual(workspace.execute(Path("bazel.exe"), self.root, generated, "test", "debug", []), 0)
            run.assert_called_once()

    def test_windows_execute_only_launches_bazel_and_forwards_its_status_and_log(self):
        generated = self.root / "configuration"
        # Stale peer outputs must not cause the launcher to inspect any executable.
        binary_dir = self.root / "bazel-bin/test"
        binary_dir.mkdir(parents=True)
        (binary_dir / "utility_test.exe").write_bytes(b"stale peer")
        for action in ("build", "test"):
            for status in (0, 7):

                def launch(argv, status=status, **kwargs):
                    kwargs["stdout"].write(b"Bazel owns manifest validation\n")
                    return subprocess.CompletedProcess(argv, status)

                with (
                    self.subTest(action=action, status=status),
                    patch.object(native, "native_platform", return_value="windows"),
                    patch.object(native, "PROJECT_ROOT", self.root),
                    patch.object(workspace.subprocess, "run", side_effect=launch) as run,
                    patch("sys.stdout", new_callable=io.StringIO) as output,
                ):
                    self.assertEqual(
                        workspace.execute(Path("bazel.exe"), self.root, generated, action, "debug", ["//custom:test"]),
                        status,
                    )
                run.assert_called_once()
                self.assertEqual(run.call_args.args[0][0], "bazel.exe")
                self.assertEqual(run.call_args.args[0][-1], "//custom:test")
                self.assertEqual(run.call_args.kwargs["cwd"], self.root)
                self.assertEqual(run.call_args.kwargs["stderr"], subprocess.STDOUT)
                self.assertEqual((self.root / "build.log").read_bytes(), b"Bazel owns manifest validation\n")
                self.assertEqual("Bazel owns manifest validation" in output.getvalue(), bool(status))
        self.assertEqual((binary_dir / "utility_test.exe").read_bytes(), b"stale peer")

    def test_execute_inserts_configured_defaults_only_without_explicit_targets(self):
        generated = self.root / "configuration"
        default_labels = ("//fixture:first", "//fixture:second")
        for action in ("build", "test"):
            for targets in ([], ["--test_filter=Decoder*"], ["//..."], ["//lib/library:library", "//test:query_test"]):
                original = list(targets)
                with (
                    self.subTest(action=action, targets=targets),
                    patch.object(workspace, "DEFAULT_TEST_TARGETS", default_labels),
                    patch.object(native, "native_platform", return_value="windows"),
                    patch.object(native, "PROJECT_ROOT", self.root),
                    patch.object(workspace.subprocess, "run", return_value=subprocess.CompletedProcess([], 0)) as run,
                ):
                    self.assertEqual(
                        workspace.execute(Path("bazel.exe"), self.root, generated, action, "debug", targets), 0
                    )
                argv = run.call_args.args[0]
                if not targets or targets[0].startswith("--"):
                    self.assertEqual([arg for arg in argv if arg.startswith("//")], list(default_labels))
                if targets:
                    self.assertEqual(argv[-len(targets) :], targets)
                self.assertEqual(targets, original)
                run.assert_called_once()

    def test_execute_uses_prepared_rc_and_clean_environment_without_global_toolchain_action_env(self):
        generated = self.root / "configuration"
        executable = Path("/tools/bazel")
        with (
            patch.object(native, "native_platform", return_value="linux"),
            patch.object(
                native,
                "executable",
                side_effect=AssertionError("execution must use prepared toolchain configuration"),
            ),
            patch.object(workspace.subprocess, "run", return_value=subprocess.CompletedProcess([], 7)) as run,
            patch.dict(os.environ, {"NIX_CFLAGS_COMPILE_FOR_TARGET": "bad"}),
        ):
            status = workspace.execute(executable, self.root, generated, "test", "release", [])
        self.assertEqual(status, 7)
        argv = run.call_args.args[0]
        self.assertEqual(argv[:4], [str(executable), f"--bazelrc={generated / 'aobus.bazelrc'}", "test", "--jobs=2"])
        self.assertNotIn("--batch", argv)
        self.assertFalse(
            any(arg.startswith(("--repo_env=", "--action_env=", "--config=", "--output_user_root=")) for arg in argv)
        )
        self.assertFalse(any(arg.startswith("--remote_cache") for arg in argv))
        self.assertNotIn("NIX_CFLAGS_COMPILE_FOR_TARGET", run.call_args.kwargs["env"])

    def test_execute_deps_fetches_only_governed_repositories_with_prepared_rc(self):
        prepared = self.root / "configuration"
        with (
            patch.object(native, "native_platform", return_value="linux"),
            patch.object(workspace.subprocess, "run", return_value=subprocess.CompletedProcess([], 0)) as run,
            patch("sys.stdout"),
        ):
            self.assertEqual(workspace.execute(Path("bazel"), self.root, prepared, "deps", "debug", []), 0)
        self.assertEqual(
            run.call_args.args[0],
            [
                "bazel",
                f"--bazelrc={prepared / 'aobus.bazelrc'}",
                "fetch",
                "--repo=@aobus_native_deps",
                "--repo=@aobus_test_deps",
                "--repo=@aobus_native_tools",
                "--repo=@aobus_toolchain",
            ],
        )

    def test_execute_preserves_forwarded_options_and_remote_cache(self):
        prepared = self.root / "configuration"
        for action in ("build", "test"):
            arguments = ["--test_filter=Decoder*", "--noremote_upload_local_results"]
            with (
                self.subTest(action=action),
                patch.object(native, "native_platform", return_value="linux"),
                patch.object(workspace.subprocess, "run", return_value=subprocess.CompletedProcess([], 0)) as run,
                patch("sys.stdout"),
            ):
                self.assertEqual(
                    workspace.execute(
                        Path("bazel"),
                        self.root,
                        prepared,
                        action,
                        "debug",
                        arguments,
                        jobs=3,
                        remote_cache="grpcs://cache.example",
                    ),
                    0,
                )
            argv = run.call_args.args[0]
            self.assertIn("--jobs=3", argv)
            self.assertIn("--remote_cache=grpcs://cache.example", argv)
            self.assertEqual(argv[-2:], arguments)
            self.assertEqual(arguments, ["--test_filter=Decoder*", "--noremote_upload_local_results"])

    def test_execute_analysis_prints_only_bazel_stdout_and_logs_diagnostics(self):
        prepared = self.root / "configuration"
        for action in ("query", "cquery", "aquery"):
            output, diagnostics = io.StringIO(), io.StringIO()
            arguments = ["deps(//test:media_test)", "--output=jsonproto"]
            with (
                self.subTest(action=action),
                patch.object(native, "native_platform", return_value="linux"),
                patch.object(
                    workspace.subprocess,
                    "run",
                    return_value=subprocess.CompletedProcess([], 0, b'{"results": []}\n', b"analysis diagnostic\n"),
                ) as run,
                patch("sys.stdout", output),
                patch("sys.stderr", diagnostics),
            ):
                self.assertEqual(workspace.execute(Path("bazel"), self.root, prepared, action, "debug", arguments), 0)
            argv = run.call_args.args[0]
            self.assertEqual(argv[:3], ["bazel", f"--bazelrc={prepared / 'aobus.bazelrc'}", action])
            self.assertEqual(argv[-2:], arguments)
            self.assertEqual(any(value.startswith("--jobs=") for value in argv), action != "query")
            self.assertEqual(output.getvalue(), '{"results": []}\n')
            self.assertIn(f"Bazel {action}", diagnostics.getvalue())
            self.assertEqual((self.root / "build.log").read_bytes(), b'{"results": []}\nanalysis diagnostic\n')

    def test_execute_query_and_run_require_an_explicit_expression_or_target_before_launch(self):
        for action in ("query", "cquery", "aquery", "run"):
            with (
                self.subTest(action=action),
                patch.object(native, "native_platform", return_value="linux"),
                patch.object(workspace.subprocess, "run") as run,
                self.assertRaisesRegex(native.BazelWorkspaceError, "requires an explicit"),
            ):
                workspace.execute(Path("bazel"), self.root, self.root / "configuration", action, "debug", [])
            run.assert_not_called()

    @unittest.skipIf(os.name == "nt", "POSIX executable process boundary; Windows needs native Bazel validation")
    def test_real_child_receives_clean_environment_and_failure_is_logged(self):
        generated = self.root / "configuration"
        executable = self.root / "fake-bazel"
        executable.write_text(
            f"#!{sys.executable}\nimport os, sys\n"
            "assert 'NIX_CFLAGS_COMPILE_FOR_TARGET' not in os.environ\n"
            "print('actual child ran')\nsys.exit(9)\n",
            encoding="utf-8",
        )
        executable.chmod(0o755)
        with patch.dict(os.environ, {"NIX_CFLAGS_COMPILE_FOR_TARGET": "bad"}):
            self.assertEqual(workspace.execute(executable, self.root, generated, "test", "debug", []), 9)
        self.assertEqual((self.root / "build.log").read_text(), "actual child ran\n")


if __name__ == "__main__":
    unittest.main()
