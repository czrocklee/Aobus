"""Analyzer coverage must not infer a foreign command for Objective-C++ sources."""

import contextlib
import io
import json
import shlex
import tempfile
import unittest
from pathlib import Path
from unittest import mock

from ao.__main__ import make_parser
from ao.command import analyze
from ao.core import builddir


class AnalyzerCompileCoverageTest(unittest.TestCase):
    def setUp(self):
        self.stack = contextlib.ExitStack()
        self.addCleanup(self.stack.close)
        self.root = Path(self.stack.enter_context(tempfile.TemporaryDirectory()))
        self.build = self.root / "build"
        self.build.mkdir()
        self.source = self.root / "lib" / "Probe.mm"
        self.source.parent.mkdir()
        self.source.touch()
        self.other = self.source.with_name("Other.cpp")
        self.other.touch()
        self.entry = {
            "directory": str(self.root),
            "file": str(self.other),
            "arguments": ["clang++", "-c", str(self.other)],
        }
        self.write_database(self.entry)
        self.stack.enter_context(mock.patch.object(analyze, "PROJECT_ROOT", self.root))
        self.stack.enter_context(mock.patch.object(analyze.tidyengine, "ensure_compile_db"))
        self.stack.enter_context(mock.patch.object(analyze.tidyengine, "system_include_args", return_value=[]))
        self.stack.enter_context(mock.patch.object(builddir, "platform_profile", return_value=builddir.LINUX_PROFILE))
        self.scope = self.stack.enter_context(
            mock.patch.object(analyze.tidyengine, "resolve_scope", return_value=([str(self.source)], True))
        )
        self.tool = self.stack.enter_context(mock.patch.object(analyze.subprocess, "call", return_value=0))
        self.stdout = self.stack.enter_context(contextlib.redirect_stdout(io.StringIO()))
        self.stderr = self.stack.enter_context(contextlib.redirect_stderr(io.StringIO()))

    def write_database(self, entry):
        (self.build / "compile_commands.json").write_text(json.dumps([entry]), encoding="utf-8")

    def run_analyzer(self):
        return analyze.run_command(make_parser().parse_args(["analyze", "-p", str(self.build), "-j", "1"]))

    def test_missing_exact_command_fails_for_explicit_and_batch_native_sources(self):
        for explicit in (True, False):
            with self.subTest(explicit=explicit):
                self.scope.return_value = ([str(self.source)], explicit)
                self.assertEqual(self.run_analyzer(), 1)
                self.tool.assert_not_called()
        self.assertIn("requires an exact native compile command", self.stderr.getvalue())
        self.assertIn(str(self.source), self.stderr.getvalue())
        self.assertNotIn("No analyzer diagnostics found", self.stdout.getvalue())

    def test_outside_source_cannot_borrow_an_unrelated_command(self):
        with tempfile.TemporaryDirectory() as outside:
            source = Path(outside) / "Probe.mm"
            source.touch()
            self.scope.return_value = ([str(source)], True)
            self.assertEqual(self.run_analyzer(), 1)
        self.tool.assert_not_called()
        self.assertIn("requires an exact native compile command", self.stderr.getvalue())

    def test_exact_objective_cpp_command_retains_native_flags(self):
        arguments = ["clang++", "-fobjc-arc", "-isysroot", "/native sdk", "-c", str(self.source)]
        for representation in ("arguments", "command"):
            with self.subTest(representation=representation):
                entry = {"directory": str(self.root), "file": str(self.source)}
                entry[representation] = arguments if representation == "arguments" else shlex.join(arguments)
                self.write_database(entry)
                self.assertEqual(self.run_analyzer(), 0)
                command = self.tool.call_args.args[0]
                self.assertEqual(command[command.index("-p") + 1], str(self.build))
                self.assertEqual(command[-1], str(self.source))
                self.assertEqual(json.loads((self.build / "compile_commands.json").read_text()), [entry])

    def test_explicit_foreign_source_fails_without_analyzing(self):
        source = self.source.with_name("ProbeMacos.mm")
        source.touch()
        self.scope.return_value = ([str(source)], True)
        self.assertEqual(self.run_analyzer(), 1)
        self.tool.assert_not_called()
        self.assertIn(str(source), self.stderr.getvalue())

    def test_batch_foreign_source_is_deferred_while_native_cpp_is_analyzed(self):
        source = self.source.with_name("ProbeMacos.mm")
        source.touch()
        self.scope.return_value = ([str(source), str(self.other)], False)
        self.assertEqual(self.run_analyzer(), 0)
        self.tool.assert_called_once()
        self.assertEqual(self.tool.call_args.args[0][-1], str(self.other))
        self.assertIn("not analyzed", self.stderr.getvalue())
        self.assertIn(str(source), self.stderr.getvalue())

    def test_all_foreign_scope_does_not_claim_clean_analysis(self):
        source = self.source.with_name("ProbeMacos.mm")
        source.touch()
        self.scope.return_value = ([str(source)], False)
        self.assertEqual(self.run_analyzer(), 0)
        self.tool.assert_not_called()
        self.assertIn("not analyzed", self.stderr.getvalue())
        self.assertNotIn("No analyzer diagnostics found", self.stdout.getvalue())


if __name__ == "__main__":
    unittest.main()
