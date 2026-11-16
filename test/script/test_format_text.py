"""Authored-text scope and dispatch for the shared format/hygiene entry."""

import contextlib
import io
import tempfile
import unittest
from argparse import Namespace
from pathlib import Path
from unittest import mock

from ao.command import format as format_command
from ao.command import hygiene
from ao.core import buildenv, gitfiles, prettier

TEXT_FILES = (
    "AGENTS.md",
    ".prettierrc.json",
    "script/ao/prettier.json",
    "doc/guide.md",
    "app/ui.css",
    ".github/workflows/ci.yml",
    "config/bazel/dependencies.json",
    "cmake/ports/example/vcpkg.json",
    ".agents/skills/example/SKILL.md",
)
NATIVE_FILES = ("lib/Foo.cpp", "script/tool.py", "BUILD", "test/lint/Case.cpp")


def _checkout(root: Path, names) -> None:
    for name in names:
        path = root / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text("fixture\n", encoding="utf-8")


class TextFormatTest(unittest.TestCase):
    def args(self):
        return Namespace(files=[], all=False, folder=[], commit=None, check=True)

    def test_text_only_scope_dispatches_to_prettier_without_native_or_python_formatters(self):
        files = ["AGENTS.md", ".agents/skills/example/SKILL.md", "config/input.json", "app/layout.yaml", "app/ui.css"]
        args = Namespace(files=files, all=False, folder=[], commit=None, check=True)
        with (
            mock.patch.object(format_command, "_file_exists", return_value=True),
            mock.patch.object(prettier, "run", return_value=0) as text,
            mock.patch.object(format_command, "run_clang_format", return_value=0) as clang,
            mock.patch.object(format_command, "run_ruff_format", return_value=0) as ruff,
            contextlib.redirect_stdout(io.StringIO()),
        ):
            self.assertEqual(format_command.run_command(args), 0)
        text.assert_called_once_with(files, check=True)
        clang.assert_called_once_with([], check=True, build_dir=None, clang_format=None)
        ruff.assert_called_once_with([], check=True, python=None)

    def test_prettier_failures_cannot_produce_a_clean_format_result(self):
        args = Namespace(files=["README.md"], all=False, folder=[], commit=None, check=True)
        with (
            mock.patch.object(format_command, "_file_exists", return_value=True),
            mock.patch.object(prettier, "run", return_value=1),
            contextlib.redirect_stdout(io.StringIO()),
            contextlib.redirect_stderr(io.StringIO()),
        ):
            self.assertEqual(format_command.run_command(args), 1)
        with (
            mock.patch.object(format_command, "_file_exists", return_value=True),
            mock.patch.object(prettier, "run", side_effect=prettier.PrettierError("invalid formatter cache")),
            contextlib.redirect_stderr(io.StringIO()) as stderr,
            self.assertRaises(SystemExit),
        ):
            format_command.run_command(args)
        self.assertIn("invalid formatter cache", stderr.getvalue())

    def test_changed_and_all_scopes_include_root_docs_hidden_skills_and_text_assets(self):
        files = [
            "AGENTS.md",
            ".prettierrc.json",
            ".agents/skills/example/SKILL.md",
            "doc/guide.md",
            ".github/pull_request_template.md",
            "app/ui.css",
            "cmake/ports/example/vcpkg.json",
            "config/bazel/dependencies.json",
        ]
        # A control edit widens the diff scope, so keep it out of the changed set
        # to prove the plain changed-file selection on its own.
        changed = [name for name in files if name not in format_command.PRETTIER_CONTROL_FILES]
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            _checkout(root, files)
            with (
                mock.patch.object(format_command, "PROJECT_ROOT", root),
                mock.patch.object(gitfiles, "PROJECT_ROOT", root),
                mock.patch.object(gitfiles, "changed_paths", return_value=[*changed, "foreign/README.md"]),
            ):
                self.assertEqual(sorted(format_command.resolve_files(self.args())), sorted(changed))
                all_args = self.args()
                all_args.all = True
                self.assertEqual(format_command.resolve_files(all_args), sorted(files))
                folder_args = self.args()
                folder_args.folder = [".agents/skills"]
                self.assertEqual(format_command.resolve_files(folder_args), [".agents/skills/example/SKILL.md"])

    def test_a_control_change_expands_the_owned_text_family_only(self):
        for control in format_command.PRETTIER_CONTROL_FILES:
            with self.subTest(control=control), tempfile.TemporaryDirectory() as temporary:
                root = Path(temporary)
                _checkout(root, (*TEXT_FILES, *NATIVE_FILES))
                with (
                    mock.patch.object(format_command, "PROJECT_ROOT", root),
                    mock.patch.object(gitfiles, "PROJECT_ROOT", root),
                    mock.patch.object(gitfiles, "changed_paths", return_value=[control]),
                ):
                    self.assertEqual(format_command.resolve_files(self.args()), sorted(TEXT_FILES))

    def test_a_deleted_control_file_still_expands_the_text_scope(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            _checkout(root, ("doc/guide.md", "lib/Foo.cpp"))
            with (
                mock.patch.object(format_command, "PROJECT_ROOT", root),
                mock.patch.object(gitfiles, "PROJECT_ROOT", root),
                mock.patch.object(gitfiles, "changed_paths", return_value=[".prettierrc.json"]),
            ):
                self.assertFalse((root / ".prettierrc.json").exists())
                self.assertEqual(format_command.resolve_files(self.args()), ["doc/guide.md"])

    def test_non_control_changes_never_expand_the_text_scope(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            _checkout(root, (*TEXT_FILES, *NATIVE_FILES))
            with (
                mock.patch.object(format_command, "PROJECT_ROOT", root),
                mock.patch.object(gitfiles, "PROJECT_ROOT", root),
                mock.patch.object(
                    gitfiles, "changed_paths", return_value=[".gitattributes", "doc/guide.md", "lib/Foo.cpp"]
                ),
                mock.patch.object(gitfiles, "prettier_sources", side_effect=AssertionError("scope widened")),
            ):
                self.assertEqual(format_command.resolve_files(self.args()), ["doc/guide.md", "lib/Foo.cpp"])

    def test_explicit_and_folder_selections_never_expand_on_a_control_change(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            _checkout(root, ("README.md", "doc/guide.md"))
            with (
                mock.patch.object(format_command, "PROJECT_ROOT", root),
                mock.patch.object(gitfiles, "PROJECT_ROOT", root),
                mock.patch.object(gitfiles, "changed_paths", side_effect=AssertionError("rescanned")) as changed,
            ):
                explicit = self.args()
                explicit.files = ["README.md"]
                self.assertEqual(format_command.resolve_files(explicit), ["README.md"])
                folder = self.args()
                folder.folder = ["doc"]
                self.assertEqual(format_command.resolve_files(folder), ["doc/guide.md"])
            changed.assert_not_called()

    def test_whole_tree_and_folder_scopes_take_text_from_git_and_native_from_the_scan(self):
        scanned = ["lib/Foo.cpp", "doc/guide.md", "doc/ignored.md"]
        with (
            mock.patch.object(gitfiles, "find_sources", return_value=scanned) as scan,
            mock.patch.object(gitfiles, "root_bazel_files", return_value=["BUILD"]),
            mock.patch.object(gitfiles, "prettier_sources", return_value=["doc/guide.md"]) as text,
        ):
            all_args = self.args()
            all_args.all = True
            # The scan's text portion is dropped: only Git-enumerated text remains.
            self.assertEqual(format_command.resolve_files(all_args), ["BUILD", "doc/guide.md", "lib/Foo.cpp"])
        scan.assert_called_once_with(list(format_command.FORMAT_TOP_DIRS), suffixes=gitfiles.SOURCE_SUFFIXES)
        text.assert_called_once_with(list(format_command.FORMAT_TOP_DIRS), include_root=True)
        with (
            mock.patch.object(gitfiles, "find_sources", return_value=["doc/guide.md", "doc/ignored.md"]),
            mock.patch.object(gitfiles, "prettier_sources", return_value=["doc/guide.md"]) as text,
        ):
            folder_args = self.args()
            folder_args.folder = ["doc"]
            self.assertEqual(format_command.resolve_files(folder_args), ["doc/guide.md"])
        text.assert_called_once_with(["doc"])

    def test_text_only_native_preflight_needs_neither_compiler_nor_managed_python_tools(self):
        for command in ("format", "hygiene"):
            with self.subTest(command=command):
                self.assertFalse(buildenv.requires_build_env(command, ["README.md"]))
                self.assertFalse(buildenv.requires_python_tools(command, ["README.md"]))

    def test_hygiene_requests_check_only_prettier_for_a_text_scope(self):
        files = ["README.md"]
        args = Namespace(files=files, all=False, folder=[], commit=None, path=None, jobs=1)
        with (
            mock.patch.object(hygiene.format_command, "resolve_files", return_value=files),
            mock.patch.object(format_command, "_file_exists", return_value=True),
            mock.patch.object(prettier, "run", return_value=0) as text,
            contextlib.redirect_stdout(io.StringIO()),
        ):
            self.assertEqual(hygiene.run_command(args, lint=("fixture", lambda files, explicit: 0)), 0)
        text.assert_called_once_with(files, check=True)


if __name__ == "__main__":
    unittest.main()
