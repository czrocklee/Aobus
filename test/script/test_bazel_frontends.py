"""Declared real CLI/TUI graph parity, not native compilation or loader evidence."""

import unittest
from unittest import mock

from ao.__main__ import make_parser
from ao.core import builddir
from ao.core.bazel_workflows import feature_options


class BazelFrontendGraphTest(unittest.TestCase):
    def test_gtk_suite_selection_is_independent_of_graph_feature_requests(self):
        with mock.patch.object(builddir, "platform_profile", return_value=builddir.LINUX_PROFILE):
            parser = make_parser()
        suite = parser.parse_args(["test", "--gtk"])
        self.assertEqual(suite.suite, "gtk")
        self.assertEqual(feature_options(suite), ())
        features = parser.parse_args(["test", "--core", "--gtk-feature", "off", "--system-media", "off"])
        self.assertEqual(features.suite, "core")
        self.assertEqual(
            feature_options(features), ("--//bazel:gtk_enabled=false", "--//bazel:system_media_enabled=false")
        )

        # Actual ABI acceptance/rejection belongs to FrontendProjectionTest in
        # test_bazel_frontend_deps.py, not a source-spelling check of its guard.


if __name__ == "__main__":
    unittest.main()
