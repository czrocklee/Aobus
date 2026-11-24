"""Global documentation semantics and Markdown formatting are independent checks."""

import contextlib
import io
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

from ao.__main__ import make_parser
from ao.core.paths import PROJECT_ROOT
from ao.portal import doc_check, documentation_checks, prettier, source_scope


class DocsFormatTest(unittest.TestCase):
    def test_documentation_semantics_do_not_prepare_a_formatter(self):
        with (
            mock.patch.object(doc_check, "check_tree", return_value=[]),
            mock.patch.object(documentation_checks, "discover_markdown", return_value=[PROJECT_ROOT / "AGENTS.md"]),
            mock.patch.object(prettier, "run") as formatter,
            contextlib.redirect_stdout(io.StringIO()),
        ):
            self.assertEqual(documentation_checks.check(), 0)
        formatter.assert_not_called()

    def test_docs_format_scope_resolves_without_the_markdown_parser(self):
        probe = (
            "import sys\n"
            "from pathlib import Path\n"
            "sys.path.insert(0, sys.argv[1])\n"
            "sys.modules['markdown_it'] = None\n"
            "from ao.portal import documentation_checks\n"
            "documentation_checks.PROJECT_ROOT = Path(sys.argv[2])\n"
            "print(*documentation_checks.markdown_scope().files, sep='\\n')\n"
        )
        with tempfile.TemporaryDirectory() as folder:
            (Path(folder) / "README.md").write_text("# Readme\n", encoding="utf-8")
            result = subprocess.run(
                [sys.executable, "-I", "-S", "-B", "-c", probe, str(PROJECT_ROOT / "script"), folder],
                capture_output=True,
                text=True,
            )
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stdout.splitlines(), ["README.md"])

    def test_docs_format_checks_only_discovered_markdown_without_global_semantics(self):
        paths = [PROJECT_ROOT / "AGENTS.md", PROJECT_ROOT / ".agents/skills/example/SKILL.md"]
        for status in (0, 1):
            args = make_parser().parse_args(["check", "format", "--docs"])
            with (
                self.subTest(status=status),
                mock.patch.object(doc_check, "check_tree", side_effect=AssertionError("no semantic check")),
                mock.patch.object(documentation_checks, "discover_markdown", return_value=paths),
                mock.patch.object(source_scope, "require_files"),
                mock.patch.object(prettier, "run", return_value=status) as formatter,
                contextlib.redirect_stdout(io.StringIO()),
                contextlib.redirect_stderr(io.StringIO()),
            ):
                self.assertEqual(args.func(args), status)
            formatter.assert_called_once_with(
                ["AGENTS.md", ".agents/skills/example/SKILL.md"], check=True, provision=False
            )

    def test_document_errors_fail_without_formatter_setup(self):
        issue = doc_check.Issue(PROJECT_ROOT / "AGENTS.md", 1, "broken-link", "missing target")
        with (
            mock.patch.object(doc_check, "check_tree", return_value=[issue]),
            mock.patch.object(prettier, "run") as formatter,
            contextlib.redirect_stderr(io.StringIO()),
        ):
            self.assertEqual(documentation_checks.check(), 1)
        formatter.assert_not_called()

    def test_docs_format_cannot_select_a_second_source_inventory(self):
        for options in (["--all"], ["--commit", "HEAD"], ["README.md"]):
            args = make_parser().parse_args(["check", "format", "--docs", *options])
            with (
                self.subTest(options=options),
                mock.patch.object(documentation_checks, "discover_markdown") as discover,
                contextlib.redirect_stderr(io.StringIO()),
            ):
                with self.assertRaises(SystemExit):
                    args.func(args)
                discover.assert_not_called()


if __name__ == "__main__":
    unittest.main()
