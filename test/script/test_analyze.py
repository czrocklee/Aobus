"""The analyze command resolves its scope and delegates to the Bazel analyzer.

These tests fake the portal boundary (``bazel_lint.run_analyzer``), never a
Bazel graph or a clang-tidy process: the analyzer's native-context contracts
live with the action and report modules.
"""

import contextlib
import io
import tempfile
import unittest
from pathlib import Path
from unittest import mock

from ao.__main__ import make_parser
from ao.command import analyze


class AnalyzeCommandTest(unittest.TestCase):
    def setUp(self):
        self.stack = contextlib.ExitStack()
        self.addCleanup(self.stack.close)
        self.root = Path(self.stack.enter_context(tempfile.TemporaryDirectory()))
        self.stack.enter_context(mock.patch.object(analyze, "PROJECT_ROOT", self.root))
        self.candidates: list[str] = []
        self.explicit = False
        self.scope = self.stack.enter_context(
            mock.patch.object(
                analyze.tidyengine,
                "resolve_scope",
                side_effect=lambda args, folders, label: (list(self.candidates), self.explicit),
            )
        )
        self.runner = self.stack.enter_context(mock.patch.object(analyze.bazel_lint, "run_analyzer", return_value=0))
        self.stdout = self.stack.enter_context(contextlib.redirect_stdout(io.StringIO()))
        self.stderr = self.stack.enter_context(contextlib.redirect_stderr(io.StringIO()))

    def source(self, relative):
        path = self.root / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text("// probe\n", encoding="utf-8")
        return path

    def parse(self, *options):
        return make_parser().parse_args(["analyze", *options])

    def run_command(self, *options):
        return analyze.run_command(self.parse(*options))

    def test_explicit_files_delegate_with_the_canonical_scope_and_flag(self):
        probe = self.source("lib/sample/Sample.cpp")
        self.source("lib/sample/Other.cpp")
        # A repeated candidate is analyzed once.
        self.candidates = [str(probe), "lib/sample/Other.cpp", "lib/sample/Other.cpp"]
        self.explicit = True
        args = self.parse()
        self.assertEqual(analyze.run_command(args), 0)
        self.runner.assert_called_once()
        delegated_args, files = self.runner.call_args.args
        self.assertIs(delegated_args, args)
        self.assertEqual(files, [str(probe), str(self.root / "lib/sample/Other.cpp")])
        self.assertTrue(self.runner.call_args.kwargs["explicit"])

    def test_batch_scopes_delegate_without_the_explicit_flag(self):
        self.source("lib/sample/Sample.cpp")
        self.candidates = ["lib/sample/Sample.cpp"]
        self.explicit = False
        self.assertEqual(self.run_command("--all"), 0)
        self.assertFalse(self.runner.call_args.kwargs["explicit"])

    def test_checkout_lint_fixtures_are_excluded_from_every_scope(self):
        kept = self.source("lib/sample/Sample.cpp")
        fixture = self.source("test/lint/fixture/StyleCase.cpp")
        infrastructure = self.source("test/lint/Runner.cpp")
        self.candidates = [str(kept), str(fixture), str(infrastructure)]
        self.explicit = True
        self.assertEqual(self.run_command(), 0)
        delegated = self.runner.call_args.args[1]
        self.assertEqual(delegated, [str(kept)])

    def test_empty_or_non_cpp_scope_does_not_prepare_a_native_context(self):
        document = self.source("README.md")
        for candidates in ([], [str(document)]):
            with self.subTest(candidates=candidates):
                self.candidates = candidates
                self.explicit = True
                self.assertEqual(self.run_command(), 0)
        self.runner.assert_not_called()

    def test_an_explicit_missing_path_fails_before_native_preparation(self):
        self.candidates = [str(self.root / "lib/sample/Missing.cpp")]
        self.explicit = True
        with self.assertRaises(SystemExit) as failure:
            self.run_command()
        self.assertEqual(failure.exception.code, 1)
        self.assertIn("explicitly selected analyzer path", self.stderr.getvalue())
        self.runner.assert_not_called()

    def test_fix_export_and_response_arguments_fail_before_scope_resolution(self):
        # Argparse rejects option-shaped values with the "=" spelling; the
        # command guard remains for a programmatic Namespace without it.
        for argument in ("--fix", "--export-fixes=fixes.yaml", "@responses.txt"):
            with self.subTest(argument=argument):
                with self.assertRaises(SystemExit):
                    self.parse("--tidy-arg=" + argument)
                args = self.parse()
                args.tidy_arg = [argument]
                self.assertEqual(analyze.run_command(args), 1)
        self.scope.assert_not_called()
        self.runner.assert_not_called()
        self.assertEqual(self.stderr.getvalue().count("ERROR:"), 3)

    def test_delegation_propagates_the_public_gate_status(self):
        self.source("lib/sample/Sample.cpp")
        self.candidates = ["lib/sample/Sample.cpp"]
        self.runner.return_value = 1
        self.assertEqual(self.run_command("--fail-on-diagnostics"), 1)


if __name__ == "__main__":
    unittest.main()
