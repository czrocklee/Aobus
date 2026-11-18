"""An explicit native cache selection takes precedence over the workflow default."""

import contextlib
import io
import os
import unittest
from unittest import mock

from ao.__main__ import make_parser, parse_arguments


class RemoteCacheOptionTests(unittest.TestCase):
    def test_performance_can_override_the_workflow_cache(self):
        command = ["measure", "performance"]
        fallback = "grpc://127.0.0.1:9092"
        explicit = "https://cache.example"
        with mock.patch.dict(os.environ, {"AOBUS_BAZEL_REMOTE_CACHE": fallback}):
            parser = make_parser()
            self.assertEqual(parse_arguments(parser, command).remote_cache, fallback)
            self.assertEqual(parse_arguments(parser, [*command, "--remote-cache", explicit]).remote_cache, explicit)

    def test_cache_environment_value_is_validated_without_the_flag(self):
        with (
            mock.patch.dict(os.environ, {"AOBUS_BAZEL_REMOTE_CACHE": "ftp://cache.example"}),
            contextlib.redirect_stderr(io.StringIO()),
            self.assertRaises(SystemExit) as exited,
        ):
            parse_arguments(make_parser(), ["measure", "performance"])
        self.assertEqual(exited.exception.code, 2)


if __name__ == "__main__":
    unittest.main()
