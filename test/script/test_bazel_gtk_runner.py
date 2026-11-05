"""Bazel test --headless owns the GTK service context for explicit Linux labels."""

import io
import os
import tempfile
import unittest
from contextlib import contextmanager
from pathlib import Path
from unittest.mock import patch

from ao.__main__ import make_parser
from ao.command import bazel
from ao.command import test as test_command
from ao.core import bazel_native as native
from ao.core import bazel_workspace as workspace


class BazelHeadlessParserTests(unittest.TestCase):
    def test_headless_is_a_test_only_option(self):
        args = make_parser().parse_args(["bazel", "test", "--headless", "--", "//test:gtk_test"])
        self.assertIs(args.func, bazel.run)
        self.assertTrue(args.headless)
        self.assertEqual(args.targets, ["//test:gtk_test"])

        for action in ("setup", "deps", "build", "query", "cquery", "aquery", "run", "verify"):
            with self.subTest(action=action), patch("sys.stderr"), self.assertRaises(SystemExit) as exited:
                make_parser().parse_args(["bazel", action, "--headless"])
            self.assertEqual(exited.exception.code, 2)

    def test_plain_test_and_forwarded_headless_do_not_request_services(self):
        plain = make_parser().parse_args(["bazel", "test", "--", "//test:gtk_test"])
        self.assertFalse(plain.headless)
        forwarded = make_parser().parse_args(["bazel", "test", "--", "--headless", "//test:gtk_test"])
        self.assertFalse(forwarded.headless)
        self.assertEqual(forwarded.targets, ["--headless", "//test:gtk_test"])


class BazelHeadlessRunTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix="aobus-bazel-headless-")
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name).resolve()
        self.tree = self.root / "tree"
        self.prepared = self.tree / "configuration"
        self.executable = Path("mock-bazel")
        self.environment = {
            "DISPLAY": ":77",
            "AOBUS_OWNED_GTK_DISPLAY": "1",
            "GTK_A11Y": "test",
            "GTK_IM_MODULE": "simple",
            "GDK_BACKEND": "x11",
            "GDK_DISABLE": "gl,vulkan",
            "GSK_RENDERER": "cairo",
            "G_DEBUG": "fatal-warnings",
            "DBUS_SESSION_BUS_ADDRESS": "unix:path=/tmp/aobus-headless-bus",
            "AOBUS_OWNED_GTK_BUS": "unix:path=/tmp/aobus-headless-bus",
            "AOBUS_HEADLESS_FIXTURE": "literal=value;$(not-a-shell)",
        }

    def service(self, events, *, failure=None):
        @contextmanager
        def context(count):
            self.assertEqual(count, 1)
            events.append("service-enter")
            try:
                if failure is not None:
                    raise failure
                yield [dict(self.environment)]
            finally:
                events.append("service-exit")

        return context

    def test_headless_forwards_the_owned_environment_without_user_endpoint_overrides(self):
        events = []
        args = make_parser().parse_args(
            [
                "bazel",
                "test",
                "--path",
                str(self.tree),
                "--headless",
                "--",
                "//test:gtk_test",
                "--test_filter=Gtk*",
                "--test_env=DISPLAY=:99",
                "--test-env=GDK_BACKEND=wayland",
                "--test_env=USER_ONLY=personal",
                "--test_env",
                "DBUS_SESSION_BUS_ADDRESS=unix:path=/tmp/user-session-bus",
            ]
        )

        original_targets = list(args.targets)

        def prepare(tree, config, *, prefix):
            self.assertEqual((tree, config, prefix), (self.tree, "debug", None))
            self.assertTrue(tree.is_dir())
            events.append("prepare")
            return self.prepared

        def execute(executable, tree, prepared, action, config, targets, *, jobs, remote_cache):
            self.assertEqual(
                (executable, tree, prepared, action, config, jobs, remote_cache),
                (self.executable, self.tree, self.prepared, "test", "debug", 2, None),
            )
            self.assertEqual(events, ["prepare", "service-enter"])
            expected = [f"--test_env={key}={value}" for key, value in self.environment.items()]
            self.assertEqual(
                targets,
                [
                    "//test:gtk_test",
                    "--test_filter=Gtk*",
                    "--test_env=USER_ONLY=personal",
                    *expected,
                ],
            )
            self.assertNotIn("--test_env", targets)
            forwarded = {
                target.removeprefix("--test_env=").split("=", 1)[0]
                for target in targets
                if target.startswith("--test_env=")
            }
            self.assertEqual(forwarded, set(self.environment) | {"USER_ONLY"})
            for user_value in (
                "--test_env=DISPLAY=:99",
                "--test_env=DBUS_SESSION_BUS_ADDRESS=unix:path=/tmp/user-session-bus",
                "DBUS_SESSION_BUS_ADDRESS=unix:path=/tmp/user-session-bus",
                "--test_env=GTK_A11Y=always",
                "--test_env=GDK_BACKEND=wayland",
                "--test-env=GDK_BACKEND=wayland",
                "--test_env=G_DEBUG=gc-friendly",
            ):
                self.assertNotIn(user_value, targets)
            events.append("execute")
            return 7

        with (
            patch.dict(
                os.environ,
                {
                    "DISPLAY": ":99",
                    "DBUS_SESSION_BUS_ADDRESS": "unix:path=/tmp/user-session-bus",
                    "GTK_A11Y": "always",
                    "GDK_BACKEND": "wayland",
                    "G_DEBUG": "gc-friendly",
                },
            ),
            patch.object(native, "native_platform", return_value="linux"),
            patch.object(workspace, "prepare_workspace", side_effect=prepare) as prepare_workspace,
            patch.object(workspace, "bazel_executable", return_value=self.executable) as executable,
            patch.object(workspace, "execute", side_effect=execute) as launch,
            patch.object(test_command, "virtual_gtk_displays", self.service(events)),
            patch("sys.stderr", io.StringIO()),
        ):
            self.assertEqual(bazel.run(args), 7)

        self.assertEqual(events, ["prepare", "service-enter", "execute", "service-exit"])
        prepare_workspace.assert_called_once_with(self.tree, "debug", prefix=None)
        executable.assert_called_once_with()
        launch.assert_called_once()
        self.assertEqual(args.targets, original_targets)

    def test_headless_rejects_unlabelled_and_non_linux_invocations_before_services(self):
        cases = (
            (["--test_filter=Gtk*"], "linux", "explicit target"),
            ([], "linux", "explicit target"),
            (["--test_arg", "value"], "linux", "explicit target"),
            (["--test_arg", "//test:gtk_test"], "linux", "explicit target"),
            (["--test_env", "//test:gtk_test"], "linux", "explicit target"),
            (["//test:gtk_test"], "macos", "native Linux only"),
            (["//test:gtk_test"], "windows", "native Linux only"),
        )
        for targets, host, message in cases:
            args = make_parser().parse_args(["bazel", "test", "--path", str(self.tree), "--headless", "--", *targets])
            diagnostics = io.StringIO()
            with (
                self.subTest(targets=targets, host=host),
                patch.object(native, "native_platform", return_value=host),
                patch.object(workspace, "workspace_path") as select,
                patch.object(workspace, "prepare_workspace") as prepare,
                patch.object(workspace, "bazel_executable") as executable,
                patch.object(workspace, "execute") as launch,
                patch.object(
                    test_command,
                    "virtual_gtk_displays",
                    side_effect=AssertionError("headless service started"),
                ) as service,
                patch("sys.stderr", diagnostics),
                self.assertRaises(SystemExit) as exited,
            ):
                bazel.run(args)
            self.assertNotEqual(exited.exception.code, 0)
            self.assertIn(message, diagnostics.getvalue())
            for operation in (select, prepare, executable, launch, service):
                operation.assert_not_called()

    def test_headless_rejects_managed_options_before_starting_services(self):
        args = make_parser().parse_args(
            [
                "bazel",
                "test",
                "--path",
                str(self.tree),
                "--headless",
                "--",
                "--config=release",
                "//test:gtk_test",
            ]
        )
        diagnostics = io.StringIO()
        with (
            patch.object(workspace, "workspace_path") as select,
            patch.object(workspace, "prepare_workspace") as prepare,
            patch.object(workspace, "bazel_executable") as executable,
            patch.object(workspace, "execute") as launch,
            patch.object(
                test_command,
                "virtual_gtk_displays",
                side_effect=AssertionError("headless service started"),
            ) as service,
            patch("sys.stderr", diagnostics),
            self.assertRaises(SystemExit),
        ):
            bazel.run(args)
        self.assertIn("managed by ao", diagnostics.getvalue())
        for operation in (select, prepare, executable, launch, service):
            operation.assert_not_called()

    def test_headless_service_exits_when_bazel_raises(self):
        events = []
        args = make_parser().parse_args(
            ["bazel", "test", "--path", str(self.tree), "--headless", "--", "//test:gtk_test"]
        )

        def execute(*args, **kwargs):
            self.assertEqual(events, ["service-enter"])
            events.append("execute")
            raise RuntimeError("bazel failed")

        with (
            patch.object(native, "native_platform", return_value="linux"),
            patch.object(workspace, "prepare_workspace", return_value=self.prepared),
            patch.object(workspace, "bazel_executable", return_value=self.executable),
            patch.object(workspace, "execute", side_effect=execute),
            patch.object(test_command, "virtual_gtk_displays", self.service(events)),
            patch("sys.stderr", io.StringIO()),
            self.assertRaisesRegex(RuntimeError, "bazel failed"),
        ):
            bazel.run(args)

        self.assertEqual(events, ["service-enter", "execute", "service-exit"])

    def test_headless_service_startup_failure_prevents_bazel_execution(self):
        events = []
        args = make_parser().parse_args(
            ["bazel", "test", "--path", str(self.tree), "--headless", "--", "//test:gtk_test"]
        )
        with (
            patch.object(native, "native_platform", return_value="linux"),
            patch.object(workspace, "prepare_workspace", return_value=self.prepared),
            patch.object(workspace, "bazel_executable") as executable,
            patch.object(workspace, "execute") as launch,
            patch.object(
                test_command,
                "virtual_gtk_displays",
                self.service(events, failure=RuntimeError("no Xvfb")),
            ),
            patch("sys.stderr", io.StringIO()),
            self.assertRaisesRegex(RuntimeError, "no Xvfb"),
        ):
            bazel.run(args)

        self.assertEqual(events, ["service-enter", "service-exit"])
        executable.assert_not_called()
        launch.assert_not_called()

    def test_plain_actions_do_not_start_headless_services(self):
        for action in ("test", "build"):
            calls = []
            args = make_parser().parse_args(["bazel", action, "--path", str(self.tree), "--", "//test:gtk_test"])

            def execute(*args, calls=calls, **kwargs):
                calls.append((args, kwargs))
                return 0

            with (
                self.subTest(action=action),
                patch.object(native, "native_platform", return_value="linux"),
                patch.object(workspace, "prepare_workspace", return_value=self.prepared),
                patch.object(workspace, "bazel_executable", return_value=self.executable),
                patch.object(workspace, "execute", side_effect=execute),
                patch.object(
                    test_command,
                    "virtual_gtk_displays",
                    side_effect=AssertionError("headless service started"),
                ) as service,
                patch("sys.stderr", io.StringIO()),
            ):
                self.assertEqual(bazel.run(args), 0)
            self.assertEqual(len(calls), 1)
            self.assertEqual(calls[0][0][5], ["//test:gtk_test"])
            service.assert_not_called()


if __name__ == "__main__":
    unittest.main()
