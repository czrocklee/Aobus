"""Parser contracts of the native default commands (check/perf and friends).

Focused regressions for the shared native option surface added by
``bazel_workflows.add_arguments`` and ``add_remote_cache_argument``: the GTK
graph control stays ``--gtk`` for every consumer, feature requests forward to
one graph-owned setting, and the remote-cache option reads the same workflow
variable everywhere it is registered.
"""

import contextlib
import io
import os
import unittest
from unittest import mock

from ao.__main__ import make_parser
from ao.core import bazel_workflows, builddir


def parse(*arguments):
    return make_parser().parse_args(list(arguments))


class GtkOptionTest(unittest.TestCase):
    def test_every_native_consumer_keeps_gtk_as_the_graph_control(self):
        # Every consumer registers the option through the same
        # add_arguments(gtk_option="--gtk") call; two representative commands
        # cover the positional-argument and flag-only shapes.
        for arguments in (
            ("check", "--gtk", "on"),
            ("perf", "release", "--gtk", "off"),
        ):
            with self.subTest(arguments=arguments):
                args = parse(*arguments)
                self.assertEqual(args.gtk, arguments[arguments.index("--gtk") + 1])

    def test_feature_forwarding_uses_one_destination_for_every_consumer(self):
        forwarded = bazel_workflows.feature_options(parse("perf", "--gtk", "off"))
        self.assertEqual(forwarded, ("--//bazel:gtk_enabled=false",))
        forwarded = bazel_workflows.feature_options(parse("perf", "--gtk", "on", "--system-media", "off"))
        self.assertEqual(
            forwarded,
            (
                "--//bazel:gtk_enabled=true",
                "--//bazel:system_media_enabled=false",
            ),
        )
        # No explicit request forwards nothing: the graph defaults apply.
        self.assertEqual(bazel_workflows.feature_options(parse("check")), ())

    def test_gtk_graph_request_is_linux_only(self):
        # Both consumers hit the same host != "linux" branch in
        # validate_request.
        for arguments in (("check", "--gtk", "on"), ("perf", "--gtk", "on")):
            with self.subTest(arguments=arguments):
                with mock.patch.object(builddir, "platform_profile", return_value=builddir.MACOS_PROFILE):
                    with self.assertRaisesRegex(bazel_workflows.native.BazelWorkspaceError, "Linux-only"):
                        bazel_workflows.validate_request(parse(*arguments))


class DefaultFlavorTest(unittest.TestCase):
    """The ordinary completion default is fastbuild; perf stays explicitly Release."""

    def test_check_defaults_to_fastbuild_while_perf_keeps_its_explicit_release_default(self):
        self.assertEqual(parse("check").flavor, "fastbuild")
        self.assertEqual(parse("perf").flavor, "release")
        buffer = io.StringIO()
        with self.assertRaises(SystemExit), contextlib.redirect_stdout(buffer):
            parse("check", "--help")
        self.assertIn("build flavor (default: fastbuild)", buffer.getvalue())


class SharedRemoteCacheOptionTest(unittest.TestCase):
    """The shared remote-cache option across every flat native consumer."""

    def test_the_workflow_cache_variable_selects_remote_cache_where_the_flag_exists(self):
        cache = "grpc://127.0.0.1:9092"
        with mock.patch.dict(os.environ, {"AOBUS_BAZEL_REMOTE_CACHE": cache}):
            for arguments in (
                ("check",),
                ("perf",),
                ("tidy",),
                ("hygiene",),
                ("coverage", "--all"),
            ):
                with self.subTest(arguments=arguments):
                    self.assertEqual(parse(*arguments).remote_cache, cache)
            explicit = parse("perf", "--remote-cache", "https://cache.example")
            self.assertEqual(explicit.remote_cache, "https://cache.example")
        with mock.patch.dict(os.environ, {"AOBUS_BAZEL_REMOTE_CACHE": ""}):
            self.assertIsNone(parse("perf").remote_cache)
        with (
            mock.patch.dict(os.environ, {"AOBUS_BAZEL_REMOTE_CACHE": "ftp://cache.example"}),
            contextlib.redirect_stderr(io.StringIO()) as stderr,
            self.assertRaises(SystemExit),
        ):
            parse("perf")
        self.assertIn("remote cache requires an http(s) or grpc(s) URL", stderr.getvalue())


if __name__ == "__main__":
    unittest.main()
