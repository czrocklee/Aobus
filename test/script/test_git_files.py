"""Tests for Git-backed source discovery."""

import contextlib
import io
import os
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path
from unittest import mock

from git_support import GitCheckoutTestCase

from ao.portal import git_files, setup_actions


class GitFilesTest(unittest.TestCase):
    def setUp(self):
        environment = mock.patch.dict(os.environ)
        environment.start()
        self.addCleanup(environment.stop)
        os.environ.pop("AOBUS_GIT_DIR", None)
        os.environ.pop("AOBUS_GIT_WORK_TREE", None)

    def test_windows_git_config_is_process_local_and_checkout_scoped(self):
        self.assertEqual(
            git_files.git_command("status", os_name="nt"),
            [
                "git",
                "-c",
                f"safe.directory={git_files.PROJECT_ROOT.as_posix()}",
                "-c",
                "core.filemode=false",
                "status",
            ],
        )
        self.assertEqual(
            git_files.git_command("status", os_name="posix", platform_name="linux"),
            ["git", "status"],
        )

    def test_macos_git_ignores_smb_executable_bit_artifacts_process_locally(self):
        self.assertEqual(
            git_files.git_command(
                "status",
                os_name="posix",
                platform_name="darwin",
                checkout_uses_smb=True,
            ),
            ["git", "-c", "core.filemode=false", "status"],
        )

    def test_macos_git_preserves_file_modes_on_local_filesystems(self):
        self.assertEqual(
            git_files.git_command(
                "status",
                os_name="posix",
                platform_name="darwin",
                checkout_uses_smb=False,
            ),
            ["git", "status"],
        )

    def test_macos_smb_detection_uses_the_mount_containing_the_checkout(self):
        mount_output = """\
/dev/disk3s1 on /System/Volumes/Data (apfs, local, journaled)
//guest:@10.200.200.1/aobus on /Users/alice/mnt/aobus (smbfs, nodev, noowners)
"""

        self.assertTrue(
            git_files._path_is_on_smb_mount(
                Path("/Users/alice/mnt/aobus/script/ao"),
                mount_output,
            )
        )
        self.assertFalse(
            git_files._path_is_on_smb_mount(
                Path("/Users/alice/dev/Aobus"),
                mount_output,
            )
        )

    def test_include_fragment_suffixes_cover_def_without_treating_it_as_a_translation_unit(self):
        self.assertEqual(git_files.CPP_TRANSLATION_UNIT_SUFFIXES, (".cpp", ".mm"))
        self.assertIn(".def", git_files.CPP_INCLUDE_FRAGMENT_SUFFIXES)
        self.assertNotIn(".def", git_files.CPP_TRANSLATION_UNIT_SUFFIXES)
        self.assertTrue("app/include/ao/i18n/MessageInventory.def".endswith(git_files.CPP_SUFFIXES))
        self.assertTrue("app/include/ao/i18n/MessageInventory.def".endswith(git_files.SOURCE_SUFFIXES))
        self.assertFalse("app/include/ao/i18n/MessageInventory.def".endswith(git_files.CPP_TRANSLATION_UNIT_SUFFIXES))

    def test_bazel_file_predicate_matches_only_supported_suffixes_and_exact_basenames(self):
        for name in (
            "BUILD",
            "WORKSPACE",
            "BUILD.bazel",
            "MODULE.bazel",
            "WORKSPACE.bazel",
            "REPO.bazel",
            "VENDOR.bazel",
            "defs.bzl",
            "nested/BUILD",
            "nested/WORKSPACE",
            "space dir/a file.bzl",
            "音楽/module.bazel",
        ):
            with self.subTest(name=name):
                self.assertTrue(git_files.is_bazel_file(name))
        for name in (
            "fooBUILD",
            "nested/fooWORKSPACE",
            "nested/build",
            "BUILD/README.md",
            ".bazelrc",
            ".bazelversion",
            "nested/.bazelrc",
            "MODULE.bazel.lock",
            "WORKSPACE.lock",
            "BUILD.in",
            "BUILD.bazel.in",
            "defs.bzl.tpl",
            "source.cpp",
            "script.py",
        ):
            with self.subTest(name=name):
                self.assertFalse(git_files.is_bazel_file(name))

    def test_root_bazel_files_selects_only_immediate_files_without_scanning_build_trees(self):
        eligible = [
            ".bazelrc",
            "rules.bzl",
            "BUILD",
            "WORKSPACE",
            "MODULE.bazel",
            "REPO.bazel",
            "BUILD.bazel",
            ".bzl",
            ".bazel",
            "space name.bzl",
            "音楽.bazel",
        ]
        excluded = [
            "fooBUILD",
            "fooWORKSPACE",
            ".bazelversion",
            "MODULE.bazel.lock",
            "BUILD.bazel.in",
            "rules.bzl.tpl",
            "source.cpp",
            "script.py",
            "nested/BUILD",
            "nested/defs.bzl",
            # A root BUILD file and a build directory cannot coexist on case-insensitive hosts.
            "build-output/MODULE.bazel",
            "build-output/vendor/BUILD",
            "sdk-cache/WORKSPACE",
            "directory.bzl/BUILD",
            "directory.bazel/defs.bzl",
        ]
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            for name in (*eligible, *excluded):
                path = root / name
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text("fixture\n", encoding="utf-8")
            iterdir = Path.iterdir
            with (
                mock.patch.object(git_files, "PROJECT_ROOT", root),
                mock.patch.object(Path, "iterdir", autospec=True, side_effect=iterdir) as scan,
                mock.patch.object(Path, "rglob", side_effect=AssertionError("root Bazel discovery must not recurse")),
            ):
                self.assertEqual(git_files.root_bazel_files(), sorted(eligible))
                scan.assert_called_once_with(root)

    def test_find_sources_includes_bazel_files_without_widening_language_specific_scopes(self):
        bazel_files = [
            ".bazelrc",
            "BUILD",
            "WORKSPACE",
            "MODULE.bazel",
            "bazel/BUILD",
            "bazel/nested/WORKSPACE",
            "bazel/a file.bzl",
            "bazel/音楽/module.bazel",
            "test/lint/BUILD",
            "test/lint/analysis/BUILD.bazel",
            "test/lint/analysis/generated_header.bzl",
        ]
        cpp_files = ["lib/Player.cpp", "include/Player.hpp", "include/Messages.def", "app/Host.mm"]
        python_files = ["script/check.py"]
        text_files = ["directory.bzl/README.md", "doc/guide.md", "config.json", "app/layout.yaml", "app/style.css"]
        excluded = [
            "fooBUILD",
            "bazel/fooWORKSPACE",
            "bazel/build",
            ".bazelversion",
            "MODULE.bazel.lock",
            "WORKSPACE.in",
            "BUILD.bazel.in",
            "bazel/defs.bzl.tpl",
            "test/lint/Case.cpp",
            "test/lint/driver.py",
        ]
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            for name in (*bazel_files, *cpp_files, *python_files, *text_files, *excluded):
                path = root / name
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text("fixture\n", encoding="utf-8")
            with mock.patch.object(git_files, "PROJECT_ROOT", root):
                self.assertEqual(
                    git_files.find_sources(["."], suffixes=git_files.SOURCE_SUFFIXES),
                    sorted(bazel_files + cpp_files + python_files + text_files),
                )
                self.assertEqual(
                    git_files.find_sources(["."], suffixes=git_files.PRETTIER_SUFFIXES), sorted(text_files)
                )
                self.assertEqual(git_files.find_sources(["."]), sorted(cpp_files))
                self.assertEqual(git_files.find_sources(["."], suffixes=git_files.PYTHON_SUFFIXES), python_files)
                self.assertEqual(
                    git_files.find_sources(["."], suffixes=git_files.BAZEL_SUFFIXES),
                    [
                        "MODULE.bazel",
                        "bazel/a file.bzl",
                        "bazel/音楽/module.bazel",
                        "test/lint/analysis/BUILD.bazel",
                        "test/lint/analysis/generated_header.bzl",
                    ],
                )
                self.assertEqual(
                    git_files.find_sources(["test/lint"], suffixes=git_files.SOURCE_SUFFIXES),
                    ["test/lint/BUILD", "test/lint/analysis/BUILD.bazel", "test/lint/analysis/generated_header.bzl"],
                )

    def test_find_sources_walks_foreign_sdk_folders_only_when_explicitly_selected(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            for name in ("BUILD", "bazel/BUILD", "bazel/rules.bzl", "sdk-cache/nested/BUILD", "sdk-cache/vendor.bzl"):
                path = root / name
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text("fixture\n", encoding="utf-8")
            rglob = Path.rglob
            with (
                mock.patch.object(git_files, "PROJECT_ROOT", root),
                mock.patch.object(Path, "rglob", autospec=True, side_effect=rglob) as scan,
            ):
                self.assertEqual(git_files.find_sources([], suffixes=git_files.SOURCE_SUFFIXES), [])
                scan.assert_not_called()
                self.assertEqual(
                    git_files.find_sources(["bazel"], suffixes=git_files.SOURCE_SUFFIXES),
                    ["bazel/BUILD", "bazel/rules.bzl"],
                )
                scan.assert_called_once_with(root / "bazel", "*")
                scan.reset_mock()
                self.assertEqual(
                    git_files.find_sources(["sdk-cache"], suffixes=git_files.SOURCE_SUFFIXES),
                    ["sdk-cache/nested/BUILD", "sdk-cache/vendor.bzl"],
                )
                scan.assert_called_once_with(root / "sdk-cache", "*")

    def test_changed_files_selects_def_include_fragments(self):
        def git_lines(*args: str, null_terminated: bool = False) -> list[str]:
            if args[:2] == ("diff", "--name-only"):
                return ["app/include/ao/i18n/MessageInventory.def", "README.md"]
            return []

        with (
            mock.patch.object(git_files, "diff_base", return_value="main"),
            mock.patch.object(git_files, "_git_worktree_files", return_value=["app/i18n/MessageCatalog.cpp"]),
        ):
            with mock.patch.object(git_files, "_git_lines", side_effect=git_lines):
                files = git_files.changed_files()

        self.assertEqual(
            files,
            [
                "app/i18n/MessageCatalog.cpp",
                "app/include/ao/i18n/MessageInventory.def",
            ],
        )

    def test_git_discovery_failure_is_not_reported_as_an_empty_scope(self):
        completed = mock.Mock(returncode=128, stdout="", stderr="fatal: dubious ownership")
        with (
            mock.patch.object(git_files.subprocess, "run", return_value=completed),
            self.assertRaisesRegex(ValueError, "dubious ownership"),
        ):
            git_files._git_lines("diff", "--name-only")

    def test_worktree_names_handle_binary_mode_only_and_renamed_paths(self):
        records = [
            " M binary.py",
            " M mode.cpp",
            "R  new name.py",
            "old.py",
            " R unstaged.py",
            "old unstaged.py",
            "C  copy.py",
            "original.py",
            "?? a\tfile.py",
            "A  staged.py",
            " D deleted.py",
            "UU conflict.py",
            " D unpaired-old.py",
            " A unpaired-new.py",
        ]
        with mock.patch.object(git_files, "_git_lines", return_value=records) as lines:
            self.assertEqual(
                git_files._git_worktree_files(),
                [
                    "binary.py",
                    "mode.cpp",
                    "new name.py",
                    "unstaged.py",
                    "copy.py",
                    "a\tfile.py",
                    "staged.py",
                    "deleted.py",
                    "conflict.py",
                    "unpaired-old.py",
                    "unpaired-new.py",
                ],
            )
        lines.assert_called_once_with("status", "--porcelain=v1", "-z", "--untracked-files=all", null_terminated=True)

    def test_failed_discovery_explains_an_ignored_other_root_binding(self):
        root = str(git_files.PROJECT_ROOT)
        other = str(Path(tempfile.gettempdir()) / "another-checkout")
        values = {"AOBUS_GIT_DIR": str(Path(tempfile.gettempdir()) / "private.git"), "AOBUS_GIT_WORK_TREE": other}
        completed = mock.Mock(returncode=128, stdout="", stderr="fatal: not a git repository")
        with (
            mock.patch.dict(os.environ, values),
            mock.patch.object(git_files.subprocess, "run", return_value=completed),
            self.assertRaises(ValueError) as error,
        ):
            git_files._git_lines("branch", "--show-current")
        message = str(error.exception)
        self.assertIn("not a git repository", message)
        self.assertIn("ignored AOBUS_GIT_WORK_TREE", message)
        self.assertIn(repr(other), message)
        self.assertIn(repr(root), message)

    def test_successful_discovery_ignores_other_root_binding_without_warning(self):
        other = str(Path(tempfile.gettempdir()) / "another-checkout")
        values = {"AOBUS_GIT_DIR": str(Path(tempfile.gettempdir()) / "private.git"), "AOBUS_GIT_WORK_TREE": other}
        completed = mock.Mock(returncode=0, stdout="main\n", stderr="")
        with (
            mock.patch.dict(os.environ, values),
            mock.patch.object(git_files.subprocess, "run", return_value=completed) as run,
        ):
            with contextlib.redirect_stderr(io.StringIO()) as errors:
                self.assertEqual(git_files._git_lines("branch", "--show-current"), ["main"])
        self.assertNotIn("--git-dir", run.call_args.args[0])
        self.assertEqual(errors.getvalue(), "")

    def test_null_terminated_git_output_preserves_newlines_and_whitespace_in_paths(self):
        completed = mock.Mock(returncode=0, stdout=b"a\nfile.py\0a\rfile.py\0a\r\nfile.py\0 \0", stderr=b"")
        with mock.patch.object(git_files.subprocess, "run", return_value=completed) as run:
            self.assertEqual(
                git_files._git_lines("ls-files", "-z", null_terminated=True),
                ["a\nfile.py", "a\rfile.py", "a\r\nfile.py", " "],
            )
        self.assertFalse(run.call_args.kwargs["text"])

    def test_source_discovery_binds_only_the_matching_checkout(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            private = root / "private metadata"
            with mock.patch.dict(
                os.environ, {"AOBUS_GIT_DIR": str(private), "AOBUS_GIT_WORK_TREE": str(git_files.PROJECT_ROOT)}
            ):
                self.assertEqual(
                    git_files.git_command("ls-files", source_discovery=True, os_name="posix", platform_name="linux"),
                    [
                        "git",
                        "--no-optional-locks",
                        "-c",
                        "diff.autoRefreshIndex=false",
                        "--git-dir",
                        str(private),
                        "--work-tree",
                        str(git_files.PROJECT_ROOT),
                        "ls-files",
                    ],
                )
                with mock.patch.object(git_files, "PROJECT_ROOT", root):
                    self.assertEqual(
                        git_files.git_command(
                            "ls-files", source_discovery=True, os_name="posix", platform_name="linux"
                        ),
                        ["git", "--no-optional-locks", "-c", "diff.autoRefreshIndex=false", "ls-files"],
                    )

    def test_source_discovery_rejects_incomplete_or_relative_bindings(self):
        root = str(git_files.PROJECT_ROOT)
        private = str(Path(tempfile.gettempdir()) / "private.git")
        for values, expected in (
            ({"AOBUS_GIT_DIR": private}, "set together"),
            ({"AOBUS_GIT_WORK_TREE": root}, "set together"),
            ({"AOBUS_GIT_DIR": "", "AOBUS_GIT_WORK_TREE": root}, "set together"),
            ({"AOBUS_GIT_DIR": private, "AOBUS_GIT_WORK_TREE": ""}, "set together"),
            ({"AOBUS_GIT_DIR": "relative.git", "AOBUS_GIT_WORK_TREE": root}, "absolute paths"),
            ({"AOBUS_GIT_DIR": private, "AOBUS_GIT_WORK_TREE": "relative-root"}, "absolute paths"),
        ):
            with (
                self.subTest(values=values),
                mock.patch.dict(os.environ, values),
                self.assertRaisesRegex(git_files.GitBindingError, expected),
            ):
                git_files.git_command("ls-files", source_discovery=True)

    def test_binding_keeps_platform_git_options_and_does_not_export_standard_selectors(self):
        before = dict(os.environ)
        private = str(Path(tempfile.gettempdir()) / "private.git")
        values = {"AOBUS_GIT_DIR": private, "AOBUS_GIT_WORK_TREE": str(git_files.PROJECT_ROOT)}
        selectors = {
            key: os.environ.get(key) for key in ("GIT_DIR", "GIT_WORK_TREE", "GIT_INDEX_FILE", "GIT_COMMON_DIR")
        }
        with mock.patch.dict(os.environ, values):
            prefix = [
                "git",
                "--no-optional-locks",
                "-c",
                "diff.autoRefreshIndex=false",
                "--git-dir",
                private,
                "--work-tree",
                str(git_files.PROJECT_ROOT),
            ]
            self.assertEqual(
                git_files.git_command("ls-files", source_discovery=True, os_name="nt"),
                [
                    *prefix,
                    "-c",
                    f"safe.directory={git_files.PROJECT_ROOT.as_posix()}",
                    "-c",
                    "core.filemode=false",
                    "ls-files",
                ],
            )
            for shared, options in ((False, []), (True, ["-c", "core.filemode=false"])):
                self.assertEqual(
                    git_files.git_command(
                        "ls-files",
                        source_discovery=True,
                        os_name="posix",
                        platform_name="darwin",
                        checkout_uses_smb=shared,
                    ),
                    [*prefix, *options, "ls-files"],
                )
            self.assertEqual({key: os.environ.get(key) for key in selectors}, selectors)
        self.assertEqual(dict(os.environ), before)

    def test_generic_git_commands_ignore_even_a_malformed_discovery_binding(self):
        with mock.patch.dict(os.environ, {"AOBUS_GIT_DIR": ""}):
            self.assertEqual(
                git_files.git_command(
                    "config", "--local", "core.hooksPath", "script/git-hook", os_name="posix", platform_name="linux"
                ),
                ["git", "config", "--local", "core.hooksPath", "script/git-hook"],
            )

    def test_changed_paths_keep_control_files_before_the_source_suffix_filter(self):
        def git_lines(*args: str, null_terminated: bool = False) -> list[str]:
            if args[:2] == ("diff", "--name-only"):
                return [".prettierignore", "doc/guide.md"]
            return []

        with (
            mock.patch.object(git_files, "diff_base", return_value="main"),
            mock.patch.object(git_files, "_git_worktree_files", return_value=[".gitignore", "script/ao/prettier.json"]),
            mock.patch.object(git_files, "_git_lines", side_effect=git_lines),
        ):
            self.assertEqual(
                git_files.changed_paths(),
                [".gitignore", ".prettierignore", "doc/guide.md", "script/ao/prettier.json"],
            )
            # The suffix filter still owns source selection; control names are
            # only visible to callers that ask for the unfiltered set.
            self.assertEqual(
                git_files.changed_files(suffixes=git_files.SOURCE_SUFFIXES),
                ["doc/guide.md", "script/ao/prettier.json"],
            )
        self.assertTrue(git_files.matches_source(".gitignore", (".gitignore",)))
        self.assertFalse(git_files.matches_source(".gitignore", git_files.SOURCE_SUFFIXES))

    def test_prettier_sources_use_git_names_and_keep_the_fixture_exclusion(self):
        names = [
            "AGENTS.md",
            ".prettierrc.json",
            "app/ui.css",
            "doc/guide.md",
            "doc/deleted.md",
            "script/tool.py",
            "test/lint/fixture/notes.md",
        ]
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            for name in names:
                if name == "doc/deleted.md":
                    continue
                path = root / name
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text("fixture\n", encoding="utf-8")
            with (
                mock.patch.object(git_files, "PROJECT_ROOT", root),
                mock.patch.object(git_files, "_git_lines_if_repository", return_value=names) as git_names,
            ):
                self.assertEqual(
                    git_files.prettier_sources(["doc", "app"], include_root=True),
                    [".prettierrc.json", "AGENTS.md", "app/ui.css", "doc/guide.md"],
                )
                self.assertEqual(git_files.prettier_sources(["doc"]), ["doc/guide.md"])
            git_names.assert_called_with(
                "ls-files", "-z", "--cached", "--others", "--exclude-standard", null_terminated=True
            )

    @unittest.skipIf(os.name == "nt", "native symlink creation needs separate Windows privileges")
    def test_prettier_sources_fall_back_to_a_bounded_filesystem_scan(self):
        with tempfile.TemporaryDirectory() as temporary, tempfile.TemporaryDirectory() as external:
            root = Path(temporary)
            for name in ("AGENTS.md", "doc/guide.md", "doc/plan/local.md", "lib/Foo.cpp"):
                path = root / name
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text("fixture\n", encoding="utf-8")
            outside = Path(external) / "outside.md"
            outside.write_text("outside\n", encoding="utf-8")
            (root / "doc/linked.md").symlink_to(outside)
            with (
                mock.patch.object(git_files, "PROJECT_ROOT", root),
                mock.patch.object(git_files, "_git_lines_if_repository", return_value=None),
            ):
                # Without Git the scan cannot know ignore rules, so ignored-looking
                # text stays (Prettier's own --ignore-path skips it); a discovered
                # link that escapes the checkout is dropped before it can abort.
                self.assertEqual(
                    git_files.prettier_sources(["doc"], include_root=True),
                    ["AGENTS.md", "doc/guide.md", "doc/plan/local.md"],
                )
                self.assertEqual(git_files.prettier_sources(["doc"]), ["doc/guide.md", "doc/plan/local.md"])

    def test_prettier_sources_fall_back_only_when_git_cannot_enumerate_the_checkout(self):
        missing = mock.Mock(
            returncode=128,
            stdout=b"",
            stderr=b"fatal: not a git repository (or any of the parent directories): .git\n",
        )
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            (root / "doc").mkdir()
            (root / "doc/guide.md").write_text("fixture\n", encoding="utf-8")
            binding = {"AOBUS_GIT_DIR": str(root / "metadata"), "AOBUS_GIT_WORK_TREE": str(root)}
            with mock.patch.object(git_files, "PROJECT_ROOT", root):
                with mock.patch.object(git_files.subprocess, "run", return_value=missing):
                    self.assertEqual(git_files.prettier_sources(["doc"]), ["doc/guide.md"])
                with mock.patch.object(
                    git_files.subprocess, "run", side_effect=FileNotFoundError(2, "No such file or directory", "git")
                ):
                    self.assertEqual(git_files.prettier_sources(["doc"]), ["doc/guide.md"])
                # An explicit binding that does not resolve is a misconfiguration,
                # never a plain exported tree.
                with (
                    mock.patch.object(git_files.subprocess, "run", return_value=missing),
                    mock.patch.dict(os.environ, binding),
                    self.assertRaisesRegex(git_files.GitBindingError, "not a git repository"),
                ):
                    git_files.prettier_sources(["doc"])
                # A missing executable cannot honor an explicit selection either.
                with (
                    mock.patch.object(
                        git_files.subprocess,
                        "run",
                        side_effect=FileNotFoundError(2, "No such file or directory", "git"),
                    ),
                    mock.patch.dict(os.environ, binding),
                    self.assertRaisesRegex(git_files.GitBindingError, "No such file or directory"),
                ):
                    git_files.prettier_sources(["doc"])

    def test_prettier_sources_ignore_a_binding_for_another_checkout(self):
        missing = mock.Mock(
            returncode=128,
            stdout=b"",
            stderr=b"fatal: not a git repository (or any of the parent directories): .git\n",
        )
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary) / "checkout"
            (root / "doc").mkdir(parents=True)
            (root / "doc/guide.md").write_text("fixture\n", encoding="utf-8")
            other = Path(temporary) / "another checkout"
            other.mkdir()
            # git_command ignores a well-formed binding for a different work tree,
            # so it cannot make this checkout's Git unavailable.
            binding = {"AOBUS_GIT_DIR": str(other / "metadata"), "AOBUS_GIT_WORK_TREE": str(other)}
            with (
                mock.patch.object(git_files, "PROJECT_ROOT", root),
                mock.patch.dict(os.environ, binding),
            ):
                with mock.patch.object(git_files.subprocess, "run", return_value=missing):
                    self.assertEqual(git_files.prettier_sources(["doc"]), ["doc/guide.md"])
                with mock.patch.object(
                    git_files.subprocess, "run", side_effect=FileNotFoundError(2, "No such file or directory", "git")
                ):
                    self.assertEqual(git_files.prettier_sources(["doc"]), ["doc/guide.md"])

    def test_prettier_sources_report_os_failures_instead_of_falling_back(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            (root / "doc").mkdir()
            (root / "doc/guide.md").write_text("fixture\n", encoding="utf-8")
            for environment in ({}, {"AOBUS_GIT_DIR": str(root / "metadata"), "AOBUS_GIT_WORK_TREE": str(root)}):
                with (
                    self.subTest(environment=environment),
                    mock.patch.object(git_files, "PROJECT_ROOT", root),
                    mock.patch.object(git_files.subprocess, "run", side_effect=PermissionError("permission denied")),
                    mock.patch.dict(os.environ, environment),
                    self.assertRaisesRegex(ValueError, "permission denied"),
                ):
                    git_files.prettier_sources(["doc"])


class GitWorkflowFixtureTest(GitCheckoutTestCase):
    @unittest.skipIf(os.name == "nt", "native symlink creation needs separate Windows privileges")
    def test_prettier_sources_exclude_ignored_compile_commands_links_in_root_and_nested_folders(self):
        (self.root / ".gitignore").write_text("compile_commands.json\ndoc/plan/\n", encoding="utf-8")
        for name in ("README.md", "doc/guide.md", "doc/plan/local.md"):
            path = self.root / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text("fixture\n", encoding="utf-8")
        self.commit("base")
        outside = self.root.parent / "external compile database.json"
        outside.write_text("{}\n", encoding="utf-8")
        (self.root / "compile_commands.json").symlink_to(outside)
        (self.root / "doc/compile_commands.json").symlink_to(outside)
        with mock.patch.object(git_files, "PROJECT_ROOT", self.root):
            self.assertEqual(git_files.prettier_sources(["doc"], include_root=True), ["README.md", "doc/guide.md"])
            self.assertEqual(git_files.prettier_sources(["doc"]), ["doc/guide.md"])

    def test_prettier_sources_accept_absolute_and_normalized_folder_selections(self):
        for name in ("AGENTS.md", "app/ui.css", "doc/guide.md", "doc/deep/nested.md"):
            path = self.root / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text("fixture\n", encoding="utf-8")
        self.commit("base")
        with mock.patch.object(git_files, "PROJECT_ROOT", self.root):
            expected = ["doc/deep/nested.md", "doc/guide.md"]
            self.assertEqual(git_files.prettier_sources(["doc"]), expected)
            self.assertEqual(git_files.prettier_sources([str(self.root / "doc")]), expected)
            self.assertEqual(git_files.prettier_sources(["./doc"]), expected)
            self.assertEqual(git_files.prettier_sources([f"../{self.root.name}/doc"]), expected)
            self.assertEqual(git_files.prettier_sources(["."]), ["AGENTS.md", "app/ui.css", *expected])
            # A selection outside the checkout owns no checkout-relative name and
            # must never widen to the whole tree.
            self.assertEqual(git_files.prettier_sources([str(self.root.parent)]), [])

    @unittest.skipIf(os.name == "nt", "native symlink creation needs separate Windows privileges")
    def test_prettier_sources_keep_tracked_and_unignored_links_for_prettier_containment(self):
        (self.root / "doc").mkdir()
        (self.root / "doc/guide.md").write_text("fixture\n", encoding="utf-8")
        outside = self.root.parent / "external text.md"
        outside.write_text("# outside\n", encoding="utf-8")
        (self.root / "doc/tracked.md").symlink_to(outside)
        self.commit("base")
        (self.root / "doc/untracked.md").symlink_to(outside)
        with mock.patch.object(git_files, "PROJECT_ROOT", self.root):
            # Git-enumerated links stay in discovery so Prettier's own containment
            # error reports them; only the filesystem fallback drops them.
            self.assertEqual(
                git_files.prettier_sources(["doc"]), ["doc/guide.md", "doc/tracked.md", "doc/untracked.md"]
            )
            self.assertTrue(git_files._escapes_checkout("doc/tracked.md"))

    def test_changed_source_scope_includes_committed_and_worktree_bazel_files_only(self):
        for name in ("BUILD", "nested/WORKSPACE"):
            path = self.root / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text("base\n", encoding="utf-8")
        self.commit("base")
        base = self.git("rev-parse", "HEAD")
        for name in ("BUILD", "MODULE.bazel"):
            (self.root / name).write_text("committed\n", encoding="utf-8")
        self.commit("committed Bazel changes")
        for name in (
            "nested/WORKSPACE",
            "nested/BUILD",
            "nested/BUILD.bazel",
            "rules/defs.bzl",
            "WORKSPACE",
            "source.cpp",
            "source.py",
            "fooBUILD",
            "nested/fooWORKSPACE",
            ".bazelrc",
            ".bazelversion",
            "MODULE.bazel.lock",
            "BUILD.bazel.in",
            "rules/defs.bzl.tpl",
        ):
            path = self.root / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text("worktree\n", encoding="utf-8")
        self.git("add", "nested/BUILD")
        index = self.root / ".git" / "index"
        before = index.read_bytes()
        with mock.patch.object(git_files, "PROJECT_ROOT", self.root):
            self.assertEqual(
                git_files.changed_files(base, suffixes=git_files.SOURCE_SUFFIXES),
                [
                    ".bazelrc",
                    "BUILD",
                    "MODULE.bazel",
                    "WORKSPACE",
                    "nested/BUILD",
                    "nested/BUILD.bazel",
                    "nested/WORKSPACE",
                    "rules/defs.bzl",
                    "source.cpp",
                    "source.py",
                ],
            )
            self.assertEqual(git_files.changed_files(base), ["source.cpp"])
            self.assertEqual(git_files.changed_files(base, suffixes=git_files.PYTHON_SUFFIXES), ["source.py"])
            self.assertEqual(
                git_files.changed_files(base, suffixes=git_files.BAZEL_SUFFIXES),
                ["MODULE.bazel", "nested/BUILD.bazel", "rules/defs.bzl"],
            )
            self.assertEqual(git_files.changed_files(base, suffixes=("BUILD",)), ["BUILD", "fooBUILD", "nested/BUILD"])
        self.assertEqual(index.read_bytes(), before)

    def test_diverged_main_does_not_expand_default_topic_scope(self):
        (self.root / "common.cpp").write_text("base\n", encoding="utf-8")
        self.commit("base")
        base = self.git("rev-parse", "HEAD")
        self.git("checkout", "-b", "topic")
        (self.root / "topic.cpp").write_text("topic\n", encoding="utf-8")
        self.commit("topic")
        self.git("checkout", "main")
        (self.root / "common.cpp").write_text("main change\n", encoding="utf-8")
        self.commit("main")
        self.git("checkout", "topic")
        with mock.patch.object(git_files, "PROJECT_ROOT", self.root):
            self.assertEqual(git_files.diff_base(), base)
            self.assertEqual(git_files.changed_files(), ["topic.cpp"])
            self.assertEqual(git_files.changed_files("main"), ["common.cpp", "topic.cpp"])

    def test_standalone_topic_without_local_main_still_uses_previous_commit(self):
        (self.root / "base.cpp").touch()
        self.commit("base")
        self.git("checkout", "-b", "topic")
        (self.root / "earlier.cpp").touch()
        self.commit("earlier topic change")
        (self.root / "latest.cpp").touch()
        tip = self.commit("latest topic change")
        self.git("branch", "-D", "main")
        self.git("tag", "main", tip)
        with mock.patch.object(git_files, "PROJECT_ROOT", self.root):
            self.assertEqual(git_files.changed_files(), ["latest.cpp"])

    def test_topic_without_a_common_ancestor_reports_why_default_scope_is_unavailable(self):
        (self.root / "source.cpp").touch()
        self.commit("main history")
        self.git("checkout", "--orphan", "topic")
        (self.root / "source.cpp").write_text("topic content\n", encoding="utf-8")
        self.commit("independent topic history")
        with mock.patch.object(git_files, "PROJECT_ROOT", self.root):
            with self.assertRaisesRegex(ValueError, "main and HEAD have no common ancestor"):
                git_files.diff_base()
            with self.assertRaisesRegex(ValueError, "no common ancestor.*--commit"):
                git_files.changed_files()

    def test_explicit_hook_setup_supports_linked_worktrees(self):
        (self.root / "README.md").write_text("fixture\n", encoding="utf-8")
        self.commit("base")
        linked = self.root.parent / "linked"
        self.git("worktree", "add", "-b", "linked", str(linked))
        with (
            mock.patch.object(git_files, "PROJECT_ROOT", linked),
            mock.patch.object(setup_actions, "PROJECT_ROOT", linked),
        ):
            self.assertEqual(setup_actions.setup("git-hooks"), 0)
        configured = subprocess.check_output(
            ["git", "-C", str(linked), "config", "--local", "--get", "core.hooksPath"], text=True
        ).strip()
        self.assertEqual(configured, "script/git-hook")

    def test_discovery_uses_separate_metadata_without_rewriting_a_foreign_gitfile(self):
        (self.root / "base.py").write_text("base\n", encoding="utf-8")
        clean = self.root / "clean.py"
        clean.write_text("clean\n", encoding="utf-8")
        (self.root / "binary.py").write_bytes(b"\0old")
        self.commit("base")
        base = self.git("rev-parse", "HEAD")
        self.git("checkout", "-b", "topic")
        (self.root / "topic.py").write_text("topic\n", encoding="utf-8")
        self.commit("topic")
        head = self.git("rev-parse", "HEAD")
        (self.root / "base.py").write_text("unstaged\n", encoding="utf-8")
        (self.root / "binary.py").write_bytes(b"\0new")
        (self.root / "staged.py").write_text("staged\n", encoding="utf-8")
        self.git("add", "staged.py")
        staged = self.git("ls-files", "--stage")
        (self.root / "untracked.py").write_text("untracked\n", encoding="utf-8")
        # A share can change stat identity without changing tracked content.
        # git diff may refresh that entry even with --no-optional-locks.
        previous = clean.stat()
        os.utime(clean, ns=(previous.st_atime_ns, previous.st_mtime_ns + 2_000_000_000))
        private = self.root.parent / "private metadata"
        (self.root / ".git").rename(private)
        marker = self.root / ".git"
        marker.write_text("gitdir: /unavailable-foreign-host/private.git\n", encoding="utf-8")
        preserved = {
            path: path.read_bytes() for path in (marker, private / "HEAD", private / "index", private / "config")
        }
        with (
            mock.patch.object(git_files, "PROJECT_ROOT", self.root),
            mock.patch.dict(os.environ, {"AOBUS_GIT_DIR": str(private), "AOBUS_GIT_WORK_TREE": str(self.root)}),
        ):
            self.assertEqual(git_files._git_lines("rev-parse", "HEAD"), [head])
            self.assertEqual(git_files._git_lines("ls-files", "--stage"), staged.splitlines())
            self.assertEqual(
                git_files._git_lines("ls-files"), ["base.py", "binary.py", "clean.py", "staged.py", "topic.py"]
            )
            self.assertEqual(git_files.diff_base(), base)
            self.assertEqual(
                git_files.changed_files(suffixes=git_files.PYTHON_SUFFIXES),
                ["base.py", "binary.py", "staged.py", "topic.py", "untracked.py"],
            )
            self.assertEqual(
                git_files.changed_files(base, suffixes=git_files.PYTHON_SUFFIXES),
                ["base.py", "binary.py", "staged.py", "topic.py", "untracked.py"],
            )
            other = self.root.parent / "other"
            other.mkdir()
            with mock.patch.object(self, "root", other):
                self.git("init", "-b", "main")
                (other / "independent.py").write_text("independent\n", encoding="utf-8")
                self.commit("independent")
                self.assertEqual(self.git("ls-files"), "independent.py")
                self.assertNotEqual(self.git("rev-parse", "HEAD"), head)
        self.assertEqual({path: path.read_bytes() for path in preserved}, preserved)

    def test_worktree_rename_with_intent_to_add_preserves_the_index(self):
        old = self.root / "old name.py"
        old.write_text("fixture\n" * 40, encoding="utf-8")
        self.commit("base")
        new = self.root / "new name.py"
        old.rename(new)
        self.git("add", "--intent-to-add", new.name)
        index = self.root / ".git" / "index"
        before = index.read_bytes()
        with mock.patch.object(git_files, "PROJECT_ROOT", self.root):
            self.assertEqual(git_files._git_worktree_files(), [new.name])
            self.assertEqual(git_files.changed_files("HEAD", suffixes=git_files.PYTHON_SUFFIXES), [new.name])
        self.assertEqual(index.read_bytes(), before)

    def test_staged_rename_selects_the_destination_and_preserves_the_index(self):
        old = self.root / "old name.py"
        old.write_text("fixture\n" * 40, encoding="utf-8")
        self.commit("base")
        new = self.root / "new name.py"
        old.rename(new)
        self.git("add", "--all")
        # Porcelain must ignore display-oriented color and relative-path settings.
        self.git("config", "color.status", "always")
        self.git("config", "status.relativePaths", "true")
        index = self.root / ".git" / "index"
        before = index.read_bytes()
        with mock.patch.object(git_files, "PROJECT_ROOT", self.root):
            self.assertEqual(git_files.changed_files("HEAD", suffixes=git_files.PYTHON_SUFFIXES), [new.name])
        self.assertEqual(index.read_bytes(), before)

    def assert_discovery_preserves_names(self, spellings, *, extension=".py", suffixes=git_files.PYTHON_SUFFIXES):
        self.git("config", "core.quotePath", "true")
        self.git("config", "status.showUntrackedFiles", "no")
        for state in ("committed", "staged", "untracked", "unstaged"):
            with self.subTest(state=state):
                (self.root / "seed.py").write_text(f"seed {state}\n", encoding="utf-8")
                names = [f"{state}-{spelling}{extension}" for spelling in spellings]
                for name in names:
                    (self.root / name).parent.mkdir(parents=True, exist_ok=True)
                if state == "unstaged":
                    for name in names:
                        (self.root / name).write_text("base\n", encoding="utf-8")
                self.commit(f"base {state}")
                base = self.git("rev-parse", "HEAD")
                for name in names:
                    (self.root / name).write_text("changed\n", encoding="utf-8")
                if state == "committed":
                    self.commit("committed paths")
                elif state == "staged":
                    self.git("add", ".")
                index = self.root / ".git" / "index"
                before = index.read_bytes()
                with mock.patch.object(git_files, "PROJECT_ROOT", self.root):
                    self.assertEqual(git_files.changed_files(base, suffixes=suffixes), sorted(names))
                self.assertEqual(index.read_bytes(), before)

    def test_discovery_preserves_non_ascii_names_in_every_change_state(self):
        self.assert_discovery_preserves_names(["café", "音楽"])

    @unittest.skipIf(os.name == "nt", "Windows forbids control characters in filenames")
    def test_discovery_preserves_control_characters_in_every_posix_change_state(self):
        self.assert_discovery_preserves_names(["tab\tname", "cr\rname", "lf\nname", "crlf\r\nname"])

    def test_stat_only_binary_and_attribute_marked_sources_do_not_expand_scope(self):
        paths = [self.root / "binary.py", self.root / "attribute.cpp"]
        paths[0].write_bytes(b"\0original binary\n")
        paths[1].write_text("int value = 1;\n", encoding="utf-8")
        (self.root / ".gitattributes").write_text("attribute.cpp -diff\n", encoding="utf-8")
        self.commit("base")
        for path in paths:
            previous = path.stat()
            os.utime(path, ns=(previous.st_atime_ns, previous.st_mtime_ns + 2_000_000_000))
        index = self.root / ".git" / "index"
        before = index.read_bytes()
        with mock.patch.object(git_files, "PROJECT_ROOT", self.root):
            self.assertEqual(git_files.changed_files("HEAD", suffixes=git_files.SOURCE_SUFFIXES), [])
            paths[0].write_bytes(b"\0changed binary\n")
            paths[1].write_text("int value = 2;\n", encoding="utf-8")
            self.assertEqual(
                git_files.changed_files("HEAD", suffixes=git_files.SOURCE_SUFFIXES), ["attribute.cpp", "binary.py"]
            )
        self.assertEqual(index.read_bytes(), before)

    @unittest.skipIf(os.name == "nt", "Windows cannot preserve Unix executable bits")
    def test_real_mode_only_change_is_selected_without_refreshing_the_index(self):
        path = self.root / "mode.cpp"
        path.write_text("int value = 1;\n", encoding="utf-8")
        path.chmod(0o644)
        self.git("config", "core.filemode", "true")
        self.commit("base")
        path.chmod(0o755)
        index = self.root / ".git" / "index"
        before = index.read_bytes()
        with mock.patch.object(git_files, "PROJECT_ROOT", self.root):
            # Use local-filesystem semantics even if this fixture runs on an SMB host.
            with mock.patch.object(git_files, "_darwin_checkout_uses_smb", return_value=False):
                self.assertEqual(git_files.changed_files("HEAD"), [path.name])
        self.assertEqual(index.read_bytes(), before)

    def test_hook_setup_does_not_redirect_to_discovery_metadata(self):
        other = self.root.parent / "other"
        other.mkdir()
        with mock.patch.object(self, "root", other):
            self.git("init", "-b", "main")
        config = other / ".git" / "config"
        before = config.read_bytes()
        with (
            mock.patch.object(git_files, "PROJECT_ROOT", self.root),
            mock.patch.object(setup_actions, "PROJECT_ROOT", self.root),
            mock.patch.dict(os.environ, {"AOBUS_GIT_DIR": str(other / ".git"), "AOBUS_GIT_WORK_TREE": str(self.root)}),
        ):
            self.assertEqual(setup_actions.setup("git-hooks"), 0)
        configured = subprocess.check_output(
            ["git", "-C", str(self.root), "config", "--local", "--get", "core.hooksPath"], text=True
        ).strip()
        self.assertEqual(configured, "script/git-hook")
        self.assertEqual(config.read_bytes(), before)

    def test_missing_bound_metadata_fails_discovery_instead_of_returning_an_empty_scope(self):
        with (
            mock.patch.object(git_files, "PROJECT_ROOT", self.root),
            mock.patch.dict(
                os.environ,
                {"AOBUS_GIT_DIR": str(self.root.parent / "absent.git"), "AOBUS_GIT_WORK_TREE": str(self.root)},
            ),
            self.assertRaisesRegex(ValueError, "git branch --show-current failed"),
        ):
            git_files.changed_files()

    def ci_scope(self, base, event="pull_request"):
        bash = shutil.which("bash")
        if bash is None:
            self.skipTest("CI scope fixture requires Bash")
        script = Path(__file__).resolve().parents[2] / ".github/actions/resolve-hygiene-base/resolve.sh"
        output = self.root.parent / "github-output"
        output.write_text("", encoding="utf-8")
        result = subprocess.run(
            [bash, str(script)],
            cwd=self.root,
            env={
                **os.environ,
                "GITHUB_EVENT_NAME": event,
                "GITHUB_OUTPUT": output.as_posix(),
                "GITHUB_STEP_SUMMARY": (self.root.parent / "github-summary").as_posix(),
                "PULL_REQUEST_BASE_SHA": base if event == "pull_request" else "",
                "PUSH_BEFORE_SHA": base if event == "push" else "",
                "DEFAULT_BRANCH": "main",
                "WORKFLOW_REF": "refs/heads/main",
            },
            capture_output=True,
            text=True,
        )
        return result, dict(line.split("=", 1) for line in output.read_text(encoding="utf-8").splitlines())

    def test_ci_pr_scope_includes_every_commit_in_the_final_merge_tree(self):
        (self.root / "base.cpp").write_text("base\n", encoding="utf-8")
        self.commit("base")
        self.git("checkout", "-b", "topic")
        for name in ("first.cpp", "middle.cpp", "last.cpp"):
            (self.root / name).write_text(name, encoding="utf-8")
            self.commit(name)
        self.git("checkout", "main")
        (self.root / "base.cpp").write_text("advanced main\n", encoding="utf-8")
        self.commit("advance main")
        base = self.git("rev-parse", "HEAD")
        self.git("merge", "--no-ff", "topic", "-m", "PR merge tree")

        result, output = self.ci_scope(base)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(output["sha"], base)
        self.assertEqual(output["docs-only"], "false")
        self.assertIn("4 commits, 3 changed paths", result.stdout)
        with mock.patch.object(git_files, "PROJECT_ROOT", self.root):
            self.assertEqual(git_files.changed_files(output["sha"]), ["first.cpp", "last.cpp", "middle.cpp"])
        summary = (self.root.parent / "github-summary").read_text(encoding="utf-8")
        self.assertIn(f"{base}..{self.git('rev-parse', 'HEAD')}", summary)
        self.assertIn("**4 commits**", summary)

    def test_ci_docs_route_requires_a_verified_nonempty_documentation_change(self):
        (self.root / "README.md").write_text("base\n", encoding="utf-8")
        self.commit("base")
        base = self.git("rev-parse", "HEAD")
        (self.root / "README.md").write_text("docs changed\n", encoding="utf-8")
        self.commit("docs")
        for event, expected in (("pull_request", "true"), ("push", "true"), ("workflow_dispatch", "false")):
            result, output = self.ci_scope(base, event)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(output["docs-only"], expected)
        result, output = self.ci_scope(self.git("rev-parse", "HEAD"))
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(output["docs-only"], "false")
        self.git("-c", "tag.gpgsign=false", "tag", "-a", "base-tag", "-m", "fixture tag", base)
        for invalid in ("", "HEAD~1", "f" * 40, self.git("rev-parse", "base-tag")):
            with self.subTest(invalid=invalid):
                result, output = self.ci_scope(invalid)
                self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)
                self.assertEqual(output, {})
        (self.root / "unknown-input").write_text("native gate\n", encoding="utf-8")
        self.commit("unknown input")
        result, output = self.ci_scope(base)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(output["docs-only"], "false")

    def test_ci_pr_rejects_a_nonancestor_base_even_if_only_docs_differ(self):
        readme = self.root / "README.md"
        readme.write_text("common\n", encoding="utf-8")
        self.commit("common history")
        self.git("checkout", "-b", "other")
        readme.write_text("other branch\n", encoding="utf-8")
        self.commit("diverge")
        base = self.git("rev-parse", "HEAD")
        self.git("checkout", "main")

        result, output = self.ci_scope(base)
        self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(output, {})

        # A forced push can legitimately use a nonancestor as its before SHA.
        result, output = self.ci_scope(base, "push")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(output, {"sha": base, "docs-only": "true"})

    def test_ci_deployment_readme_keeps_the_docs_route_only_when_retained(self):
        readme = self.root / "app/windows-winui/deployment/README.md"
        readme.parent.mkdir(parents=True)
        readme.write_text("deployment instructions\n", encoding="utf-8")
        self.commit("add deployment readme")
        original = self.git("rev-parse", "HEAD")

        readme.write_text("updated instructions\n", encoding="utf-8")
        self.commit("edit deployment readme")
        edited = self.git("rev-parse", "HEAD")
        result, output = self.ci_scope(original)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(output["docs-only"], "true")

        moved = readme.with_name("Instructions.md")
        readme.rename(moved)
        self.commit("move deployment readme to another Markdown path")
        result, output = self.ci_scope(edited)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(output["docs-only"], "false")

        moved.unlink()
        self.commit("remove deployment readme entirely")
        result, output = self.ci_scope(edited)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(output["docs-only"], "false")

    def test_ci_code_to_markdown_rename_still_requires_native_validation(self):
        source = self.root / "old.cpp"
        source.write_text("fixture\n", encoding="utf-8")
        self.commit("base")
        base = self.git("rev-parse", "HEAD")
        (self.root / "doc").mkdir()
        source.rename(self.root / "doc" / "old.md")
        self.commit("move code")
        result, output = self.ci_scope(base)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(output["docs-only"], "false")

    def test_ci_component_documentation_mixed_with_code_requires_native_validation(self):
        (self.root / "README.md").write_text("base\n", encoding="utf-8")
        self.commit("base")
        for folder, filename in (("tool", "check.py"), ("app", "Runtime.cpp"), ("asset", "icon.svg")):
            with self.subTest(folder=folder):
                base = self.git("rev-parse", "HEAD")
                directory = self.root / folder / "nested"
                directory.mkdir(parents=True)
                (directory / "README.md").write_text("documentation\n", encoding="utf-8")
                (directory / filename).write_text("non-documentation\n", encoding="utf-8")
                self.commit("mixed documentation and native input")

                result, output = self.ci_scope(base)

                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertEqual(output["docs-only"], "false")

    def test_ci_nested_documentation_and_skill_routes(self):
        (self.root / "README.md").write_text("base\n", encoding="utf-8")
        self.commit("base")
        for name, expected in (
            ("doc/development/naming-convention.md", "true"),
            (".agents/skills/example/SKILL.md", "true"),
            (".agents/skills/example/scripts/check.py", "false"),
            ("tool/lint/nested/README.md", "true"),
            ("tool/lint/check.py", "false"),
            ("app/runtime/nested/README.md", "true"),
            ("app/runtime/Runtime.cpp", "false"),
            ("asset/brand/nested/Soul.md", "true"),
            ("asset/brand/Soul.svg", "false"),
            ("doc/plan/nested/draft.md", "true"),
            ("doc/reference/shell/component-vocabulary.md", "true"),
            ("doc/reference/shell/glossary.md", "true"),
        ):
            with self.subTest(name=name):
                base = self.git("rev-parse", "HEAD")
                path = self.root / name
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text("fixture\n", encoding="utf-8")
                self.commit("nested input")
                result, output = self.ci_scope(base)
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertEqual(output["docs-only"], expected)


if __name__ == "__main__":
    unittest.main()
