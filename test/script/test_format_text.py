"""Authored-text selection and independent formatter failures."""

import contextlib
import io
import tempfile
import unittest
from pathlib import Path
from unittest import mock

from ao.portal import git_files, prettier, source_format, source_scope
from ao.portal.source_scope import Selection, SourceScope


class TextFormatTest(unittest.TestCase):
    def setUp(self):
        self.root = Path(self.enterContext(tempfile.TemporaryDirectory()))
        for module in (git_files, source_format, source_scope):
            self.enterContext(mock.patch.object(module, "PROJECT_ROOT", self.root))
        self.enterContext(contextlib.redirect_stdout(io.StringIO()))
        self.diagnostics = self.enterContext(contextlib.redirect_stderr(io.StringIO()))

    def checkout(self, *names):
        for name in names:
            path = self.root / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text("fixture\n", encoding="utf-8")

    def test_text_only_check_propagates_prettier_findings_without_starting_native_tools(self):
        self.checkout("README.md", ".github/workflows/ci.yml")
        scope = SourceScope(("README.md", ".github/workflows/ci.yml"))
        with (
            mock.patch.object(source_format.subprocess, "run", side_effect=AssertionError("unexpected native tool")),
            mock.patch.object(prettier, "run", return_value=1) as text,
        ):
            self.assertEqual(source_format.check(scope), 1)
        text.assert_called_once_with(list(scope.files), check=True, provision=False)

    def test_broken_native_formatter_does_not_hide_text_findings_or_become_an_ordinary_finding(self):
        self.checkout("lib/Foo.cpp", "README.md")

        def unavailable(argv, *, cwd):
            self.assertEqual(argv, ["fixture-clang-format", "--dry-run", "-Werror", "lib/Foo.cpp"])
            self.assertEqual(cwd, self.root)
            raise OSError("fixture-clang-format is unavailable")

        with (
            mock.patch.object(source_format.subprocess, "run", side_effect=unavailable),
            mock.patch.object(prettier, "run", return_value=1) as text,
        ):
            self.assertEqual(
                source_format.check(SourceScope(("lib/Foo.cpp", "README.md")), clang_format="fixture-clang-format"), 2
            )
        text.assert_called_once_with(["README.md"], check=True, provision=False)
        self.assertIn("fixture-clang-format is unavailable", self.diagnostics.getvalue())

    def test_broken_prettier_does_not_hide_later_bazel_rc_diagnostics(self):
        self.checkout("README.md", ".bazelrc")
        with (
            mock.patch.object(prettier, "run", side_effect=prettier.PrettierError("formatter cache is invalid")),
            mock.patch.object(source_format.compiler_rc, "format_msvc_rc", return_value=1) as msvc,
        ):
            self.assertEqual(source_format.check(SourceScope(("README.md", ".bazelrc"))), 2)
        msvc.assert_called_once_with(check=True)
        self.assertIn("formatter cache is invalid", self.diagnostics.getvalue())

    def test_deleted_prettier_control_file_expands_text_without_selecting_unchanged_native_sources(self):
        self.checkout("AGENTS.md", ".agents/skills/example/SKILL.md", "lib/Foo.cpp")
        with mock.patch.object(git_files, "changed_paths", return_value=[".prettierignore"]):
            self.assertEqual(source_scope.resolve(Selection()).files, (".agents/skills/example/SKILL.md", "AGENTS.md"))

    def test_normal_change_does_not_expand_text_scope_or_include_foreign_sources(self):
        self.checkout("AGENTS.md", "doc/guide.md", "bazel/coverage/collect.py", "sdk-cache/Generated.cpp")
        changed = ["doc/guide.md", "bazel/coverage/collect.py", "sdk-cache/Generated.cpp"]
        with mock.patch.object(git_files, "changed_paths", return_value=changed):
            self.assertEqual(source_scope.resolve(Selection()).files, ("bazel/coverage/collect.py", "doc/guide.md"))

    def test_all_scope_uses_git_selection_for_text_but_keeps_owned_native_files(self):
        self.checkout("doc/guide.md", "doc/ignored.md", "lib/Foo.cpp", "bazel/coverage/collect.py")
        with mock.patch.object(git_files, "prettier_sources", return_value=["doc/guide.md"]):
            self.assertEqual(
                source_scope.resolve(Selection(all=True)).files,
                ("bazel/coverage/collect.py", "doc/guide.md", "lib/Foo.cpp"),
            )


if __name__ == "__main__":
    unittest.main()
