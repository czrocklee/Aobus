"""Buildifier's semantic and formatting verdicts stay separate."""

import contextlib
import io
import json
import subprocess
import tempfile
import unittest
from pathlib import Path
from unittest import mock

from ao.portal import buildifier, source_format, source_lint, source_scope
from ao.portal.source_scope import SourceScope


class BuildifierCommandTest(unittest.TestCase):
    def test_unformatted_starlark_fails_format_check_but_not_semantic_lint(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            source = root / "BUILD.bazel"
            original = 'exports_files([ "fixture.txt" ])\n'
            source.write_text(original, encoding="utf-8")
            tool = root / "ready-buildifier"
            lint_settings = []

            def invoke(argv):
                self.assertEqual(argv[0], str(tool))
                self.assertEqual(Path(argv[-1]), source)
                self.assertIn("-mode=check", argv)
                (lint,) = [argument for argument in argv if argument.startswith("-lint=")]
                lint_settings.append(lint)
                response = {
                    "success": False,
                    "files": [{"filename": str(source), "valid": True, "formatted": False, "warnings": []}],
                }
                return subprocess.CompletedProcess(argv, 0, json.dumps(response), "")

            scope = SourceScope(("BUILD.bazel",))
            with (
                mock.patch.object(source_scope, "PROJECT_ROOT", root),
                mock.patch.object(source_lint, "PROJECT_ROOT", root),
                mock.patch.object(buildifier, "PROJECT_ROOT", root),
                mock.patch.object(buildifier, "executable", return_value=tool) as executable,
                mock.patch.object(buildifier, "_invoke", side_effect=invoke),
                mock.patch.object(source_lint.bazel_lint, "run", side_effect=AssertionError("no native analysis")),
                contextlib.redirect_stdout(io.StringIO()),
                contextlib.redirect_stderr(io.StringIO()),
            ):
                self.assertEqual(source_format.check(scope), 1)
                self.assertEqual(source_lint.run(scope, source_lint.LintRequest()), 0)
            self.assertEqual(lint_settings, ["-lint=off", "-lint=warn"])
            self.assertEqual(executable.call_args_list, [mock.call(provision=False)] * 2)
            self.assertEqual(source.read_text(encoding="utf-8"), original)


if __name__ == "__main__":
    unittest.main()
