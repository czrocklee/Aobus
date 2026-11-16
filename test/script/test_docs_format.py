"""Optional Markdown formatting in the lightweight documentation gate."""

import contextlib
import io
import unittest
from argparse import Namespace
from unittest import mock

from ao.__main__ import make_parser
from ao.command import docs
from ao.core import doccheck, prettier
from ao.core.paths import PROJECT_ROOT


class DocsFormatTest(unittest.TestCase):
    def test_plain_document_check_does_not_prepare_a_formatter(self):
        with (
            mock.patch.object(doccheck, "check_tree", return_value=[]),
            mock.patch.object(doccheck, "discover_markdown", return_value=[PROJECT_ROOT / "AGENTS.md"]),
            mock.patch.object(prettier, "run") as formatter,
            contextlib.redirect_stdout(io.StringIO()),
        ):
            self.assertEqual(docs.run_check(Namespace(format=False)), 0)
        formatter.assert_not_called()

    def test_explicit_format_check_uses_only_the_discovered_markdown_scope(self):
        paths = [PROJECT_ROOT / "AGENTS.md", PROJECT_ROOT / ".agents/skills/example/SKILL.md"]
        args = make_parser().parse_args(["docs", "check", "--format"])
        for status in (0, 1):
            with (
                self.subTest(status=status),
                mock.patch.object(doccheck, "check_tree", return_value=[]),
                mock.patch.object(doccheck, "discover_markdown", return_value=paths),
                mock.patch.object(prettier, "run", return_value=status) as formatter,
                contextlib.redirect_stdout(io.StringIO()) as stdout,
            ):
                self.assertEqual(docs.run_check(args), status)
            formatter.assert_called_once_with(["AGENTS.md", ".agents/skills/example/SKILL.md"], check=True)
            self.assertEqual("Documentation check passed" in stdout.getvalue(), status == 0)

    def test_document_errors_stop_before_formatter_setup(self):
        issue = doccheck.Issue(PROJECT_ROOT / "AGENTS.md", 1, "broken-link", "missing target")
        with (
            mock.patch.object(doccheck, "check_tree", return_value=[issue]),
            mock.patch.object(prettier, "run") as formatter,
            contextlib.redirect_stderr(io.StringIO()),
        ):
            self.assertEqual(docs.run_check(Namespace(format=True)), 1)
        formatter.assert_not_called()

    def test_formatter_operational_failure_is_not_a_documentation_pass(self):
        with (
            mock.patch.object(doccheck, "check_tree", return_value=[]),
            mock.patch.object(doccheck, "discover_markdown", return_value=[]),
            mock.patch.object(prettier, "run", side_effect=prettier.PrettierError("wrong tool version")),
            contextlib.redirect_stdout(io.StringIO()) as stdout,
            contextlib.redirect_stderr(io.StringIO()) as stderr,
        ):
            self.assertEqual(docs.run_check(Namespace(format=True)), 1)
        self.assertIn("wrong tool version", stderr.getvalue())
        self.assertNotIn("Documentation check passed", stdout.getvalue())


if __name__ == "__main__":
    unittest.main()
