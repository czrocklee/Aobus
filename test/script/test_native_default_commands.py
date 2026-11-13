"""Parser contracts of the native default commands (build/check/test/run/perf).

Focused regressions for the shared native option surface added by
``bazel_workflows.add_arguments``: the ``ao test`` suite shortcut ``--gtk``
must stay a suite selection, while the GTK graph control is exposed under
``--gtk-feature`` there and under ``--gtk`` for every other consumer.
"""

import argparse
import contextlib
import io
import os
import tempfile
import unittest
from pathlib import Path
from types import SimpleNamespace
from unittest import mock

from ao.__main__ import make_parser
from ao.command import build as build_command
from ao.core import bazel_native, bazel_workflows, builddir
from ao.core.bazel_native import BazelWorkspaceError


def parse(*arguments):
    return make_parser().parse_args(list(arguments))


class GtkOptionCollisionTest(unittest.TestCase):
    def test_test_gtk_remains_a_suite_shortcut_not_a_graph_control(self):
        with mock.patch.object(builddir, "platform_profile", return_value=builddir.LINUX_PROFILE):
            args = parse("test", "--gtk")
        self.assertEqual(args.suite, "gtk")
        self.assertIsNone(args.gtk)

    def test_test_graph_control_moves_to_gtk_feature(self):
        for value in ("on", "off"):
            with self.subTest(value=value):
                args = parse("test", "--gtk-feature", value)
                self.assertEqual(args.gtk, value)
                self.assertEqual(args.suite, "default")
        # The suite shortcut and the graph control compose.
        with mock.patch.object(builddir, "platform_profile", return_value=builddir.LINUX_PROFILE):
            args = parse("test", "--gtk", "--gtk-feature", "off")
        self.assertEqual(args.suite, "gtk")
        self.assertEqual(args.gtk, "off")

    def test_windows_has_no_gtk_suite_shortcut_but_keeps_the_exact_graph_option(self):
        with mock.patch.object(builddir, "platform_profile", return_value=builddir.WINDOWS_PROFILE):
            parser = make_parser()
        subparsers = next(action for action in parser._actions if isinstance(action, argparse._SubParsersAction))
        options = subparsers.choices["test"]._option_string_actions
        self.assertNotIn("--gtk", options)
        self.assertIn("--gtk-feature", options)
        args = parser.parse_args(["test", "--core", "--gtk-feature", "off"])
        self.assertEqual(args.suite, "core")
        self.assertEqual(args.gtk, "off")
        for arguments in (["test", "--gtk"], ["test", "--suite", "gtk"]):
            with self.subTest(arguments=arguments), contextlib.redirect_stderr(io.StringIO()):
                with self.assertRaises(SystemExit) as failure:
                    parser.parse_args(arguments)
                self.assertEqual(failure.exception.code, 2)

    def test_other_native_commands_keep_gtk_as_the_graph_control(self):
        # Every consumer registers the option through the same
        # add_arguments(gtk_option="--gtk") call; two representative commands
        # cover the positional-argument and flag-only shapes.
        for arguments in (
            ("build", "--gtk", "on"),
            ("run", "cli", "--gtk", "off"),
        ):
            with self.subTest(arguments=arguments):
                args = parse(*arguments)
                self.assertEqual(args.gtk, arguments[arguments.index("--gtk") + 1])

    def test_feature_forwarding_uses_one_destination_for_both_spellings(self):
        forwarded = bazel_workflows.feature_options(parse("test", "--gtk-feature", "off"))
        self.assertEqual(forwarded, ("--//bazel:gtk_enabled=false",))
        forwarded = bazel_workflows.feature_options(parse("build", "--gtk", "on", "--system-media", "off"))
        self.assertEqual(
            forwarded,
            (
                "--//bazel:gtk_enabled=true",
                "--//bazel:system_media_enabled=false",
            ),
        )
        # No explicit request forwards nothing: the graph defaults apply.
        self.assertEqual(bazel_workflows.feature_options(parse("build")), ())

    def test_gtk_graph_request_is_linux_only(self):
        # Both consumers hit the same host != "linux" branch in
        # validate_request; the test command's --gtk-feature spelling is the
        # distinct registration worth keeping.
        with mock.patch.object(builddir, "platform_profile", return_value=builddir.MACOS_PROFILE):
            with self.assertRaisesRegex(bazel_workflows.native.BazelWorkspaceError, "Linux-only"):
                bazel_workflows.validate_request(parse("test", "--gtk-feature", "on"))


class SharedRemoteCacheOptionTest(unittest.TestCase):
    """The shared remote-cache option across every flat native consumer."""

    def test_the_workflow_cache_variable_selects_remote_cache_where_the_flag_exists(self):
        cache = "grpc://127.0.0.1:9092"
        with mock.patch.dict(os.environ, {"AOBUS_BAZEL_REMOTE_CACHE": cache}):
            for arguments in (
                ("build",),
                ("test",),
                ("tidy",),
                ("hygiene",),
                ("coverage", "--all"),
            ):
                with self.subTest(arguments=arguments):
                    self.assertEqual(parse(*arguments).remote_cache, cache)
            with mock.patch.object(builddir, "platform_profile", return_value=builddir.LINUX_PROFILE):
                self.assertEqual(parse("run", "cli").remote_cache, cache)
            explicit = parse("build", "--remote-cache", "https://cache.example")
            self.assertEqual(explicit.remote_cache, "https://cache.example")
        with mock.patch.dict(os.environ, {"AOBUS_BAZEL_REMOTE_CACHE": ""}):
            self.assertIsNone(parse("build").remote_cache)
        with (
            mock.patch.dict(os.environ, {"AOBUS_BAZEL_REMOTE_CACHE": "ftp://cache.example"}),
            contextlib.redirect_stderr(io.StringIO()) as stderr,
            self.assertRaises(SystemExit),
        ):
            parse("build")
        self.assertIn("remote cache requires an http(s) or grpc(s) URL", stderr.getvalue())


class FlatBuildRouteTest(unittest.TestCase):
    """The flat build entrypoint's target selection and execution handoff."""

    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix="aobus-flat-build-")
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name).resolve()
        self.tree = self.root / "tree"
        self.prepared = self.tree / "configuration"
        self.executable = Path("mock-bazel")

    def route(self, *cli):
        """Run the real command behind a faked workspace boundary."""
        args = parse("build", *cli)
        executed = []
        self.stderr = io.StringIO()

        def execute(executable, tree, prepared, action, config, targets, **keywords):
            executed.append((executable, tree, prepared, action, config, list(targets), keywords))
            return 0

        workspace = SimpleNamespace(
            workspace_path=lambda path, config: self.tree,
            prepare_workspace=lambda tree, config, prefix=None: self.prepared,
            bazel_executable=lambda: self.executable,
            execute=execute,
        )
        with (
            mock.patch.object(build_command, "bazel_workspace", workspace),
            mock.patch.object(build_command, "build_tree_lock", contextlib.nullcontext),
            mock.patch.object(bazel_native, "require_supported_source"),
            contextlib.redirect_stderr(self.stderr),
        ):
            self.assertEqual(build_command.run_command(args), 0)
        return args, executed

    def test_scoped_options_parse_and_invalid_values_are_rejected(self):
        args = parse(
            "build",
            "release",
            "--target",
            "//lib:ao_unicode_text",
            "--jobs",
            "2",
            "--remote-cache",
            "https://cache.example",
            "--path",
            "/tmp/bazel-private",
        )
        self.assertEqual(args.target, ["//lib:ao_unicode_text"])
        self.assertEqual(args.flavor, "release")
        self.assertEqual(args.jobs, 2)
        self.assertEqual(args.remote_cache, "https://cache.example")
        self.assertEqual(args.path, "/tmp/bazel-private")
        self.assertIsInstance(parse("build").jobs, int)
        for arguments in (["--jobs", "0"], ["--remote-cache", "file:///tmp/cache"], ["unknown"]):
            with (
                self.subTest(arguments=arguments),
                contextlib.redirect_stderr(io.StringIO()),
                self.assertRaises(SystemExit) as failure,
            ):
                parse("build", *arguments)
            self.assertEqual(failure.exception.code, 2)

    def test_default_targets_are_the_check_roots_and_explicit_labels_survive_verbatim(self):
        args, executed = self.route()
        self.assertEqual(args.target, [])
        self.assertEqual(executed[0][3], "build")
        self.assertEqual(
            executed[0][5],
            [
                "//:check_products",
                "//:check_tests",
                "//:check_native_tests",
                "--lockfile_mode=error",
            ],
        )
        _, executed = self.route("--target", "//lib:ao_unicode_text")
        self.assertEqual(executed[0][5], ["//lib:ao_unicode_text", "--lockfile_mode=error"])
        # An existing product name is a convenience, not a second source graph.
        _, executed = self.route("--target", "aobus-tui")
        self.assertEqual(executed[0][5], ["//app/tui:aobus-tui", "--lockfile_mode=error"])
        with self.assertRaises(SystemExit):
            self.route("--target", "cmake-only-target")
        self.assertIn("unknown native target", self.stderr.getvalue())

    def test_event_log_retains_the_scoped_build_event_artifact(self):
        log = self.root / "attempt.bep.json"
        _, executed = self.route("--target", "//lib:ao_unicode_text", "--event-log", str(log))
        self.assertIn("--lockfile_mode=error", executed[0][5])
        self.assertIn(f"--build_event_json_file={log}", executed[0][5])

    def test_preparation_failure_stops_the_build_before_execution(self):
        args = parse("build", "--path", str(self.tree))
        workspace = SimpleNamespace(
            workspace_path=lambda path, config: self.tree,
            prepare_workspace=lambda *arguments, **keywords: (_ for _ in ()).throw(BazelWorkspaceError("stale input")),
            bazel_executable=lambda: self.executable,
            execute=lambda *arguments, **keywords: self.fail("no execution after a failed preparation"),
        )
        with (
            mock.patch.object(build_command, "bazel_workspace", workspace),
            mock.patch.object(build_command, "build_tree_lock", contextlib.nullcontext),
            mock.patch.object(bazel_native, "require_supported_source"),
            contextlib.redirect_stderr(io.StringIO()) as stderr,
            self.assertRaises(SystemExit),
        ):
            build_command.run_command(args)
        self.assertIn("stale input", stderr.getvalue())


if __name__ == "__main__":
    unittest.main()
