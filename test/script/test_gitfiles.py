"""Tests for Git-backed source discovery."""

import contextlib
import io
import os
import shutil
import subprocess
import tempfile
import unittest
from argparse import Namespace
from pathlib import Path
from unittest import mock

from ao.command import setup
from ao.core import gitfiles


class GitFilesTest(unittest.TestCase):
    def setUp(self):
        environment = mock.patch.dict(os.environ)
        environment.start()
        self.addCleanup(environment.stop)
        os.environ.pop("AOBUS_GIT_DIR", None)
        os.environ.pop("AOBUS_GIT_WORK_TREE", None)

    def test_empty_merge_base_reports_a_discovery_error(self):
        with mock.patch.object(gitfiles, "_git_lines", side_effect=[["topic"], []]):
            with mock.patch.object(gitfiles, "_git_ok", return_value=True):
                with contextlib.redirect_stderr(io.StringIO()) as errors, self.assertRaises(SystemExit):
                    gitfiles.diff_base()
        self.assertIn("returned no revision", errors.getvalue())
        self.assertIn("--commit", errors.getvalue())

    def test_windows_git_config_is_process_local_and_checkout_scoped(self):
        self.assertEqual(
            gitfiles.git_command("status", os_name="nt"),
            [
                "git",
                "-c",
                f"safe.directory={gitfiles.PROJECT_ROOT.as_posix()}",
                "-c",
                "core.filemode=false",
                "status",
            ],
        )
        self.assertEqual(
            gitfiles.git_command("status", os_name="posix", platform_name="linux"),
            ["git", "status"],
        )

    def test_macos_git_ignores_smb_executable_bit_artifacts_process_locally(self):
        self.assertEqual(
            gitfiles.git_command(
                "status",
                os_name="posix",
                platform_name="darwin",
                checkout_uses_smb=True,
            ),
            ["git", "-c", "core.filemode=false", "status"],
        )

    def test_macos_git_preserves_file_modes_on_local_filesystems(self):
        self.assertEqual(
            gitfiles.git_command(
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
            gitfiles._path_is_on_smb_mount(
                Path("/Users/alice/mnt/aobus/script/ao"),
                mount_output,
            )
        )
        self.assertFalse(
            gitfiles._path_is_on_smb_mount(
                Path("/Users/alice/dev/Aobus"),
                mount_output,
            )
        )

    def test_include_fragment_suffixes_cover_def_without_treating_it_as_a_translation_unit(self):
        self.assertEqual(gitfiles.CPP_TRANSLATION_UNIT_SUFFIXES, (".cpp", ".mm"))
        self.assertIn(".def", gitfiles.CPP_INCLUDE_FRAGMENT_SUFFIXES)
        self.assertNotIn(".def", gitfiles.CPP_TRANSLATION_UNIT_SUFFIXES)
        self.assertTrue("app/include/ao/i18n/MessageInventory.def".endswith(gitfiles.CPP_SUFFIXES))
        self.assertTrue("app/include/ao/i18n/MessageInventory.def".endswith(gitfiles.SOURCE_SUFFIXES))
        self.assertFalse("app/include/ao/i18n/MessageInventory.def".endswith(gitfiles.CPP_TRANSLATION_UNIT_SUFFIXES))

    def test_changed_files_selects_def_include_fragments(self):
        def git_lines(*args: str, null_terminated: bool = False) -> list[str]:
            if args[:2] == ("diff", "--name-only"):
                return ["app/include/ao/i18n/MessageInventory.def", "README.md"]
            return []

        with (
            mock.patch.object(gitfiles, "diff_base", return_value="main"),
            mock.patch.object(gitfiles, "_git_worktree_files", return_value=["app/i18n/MessageCatalog.cpp"]),
        ):
            with mock.patch.object(gitfiles, "_git_lines", side_effect=git_lines):
                files = gitfiles.changed_files()

        self.assertEqual(
            files,
            [
                "app/i18n/MessageCatalog.cpp",
                "app/include/ao/i18n/MessageInventory.def",
            ],
        )

    def test_git_discovery_failure_is_not_reported_as_an_empty_scope(self):
        completed = mock.Mock(returncode=128, stdout="", stderr="fatal: dubious ownership")
        errors = io.StringIO()

        with mock.patch.object(gitfiles.subprocess, "run", return_value=completed):
            with contextlib.redirect_stderr(errors):
                with self.assertRaises(SystemExit):
                    gitfiles._git_lines("diff", "--name-only")

        self.assertIn("dubious ownership", errors.getvalue())

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
        with mock.patch.object(gitfiles, "_git_lines", return_value=records) as lines:
            self.assertEqual(
                gitfiles._git_worktree_files(),
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

    def test_malformed_status_reports_a_discovery_error_without_a_traceback(self):
        for records in (["bad"], [" M "], ["XX file.py"], [" Mfile.py"], ["R  new.py"]):
            with self.subTest(records=records), mock.patch.object(gitfiles, "_git_lines", return_value=records):
                with contextlib.redirect_stderr(io.StringIO()) as errors, self.assertRaises(SystemExit):
                    gitfiles._git_worktree_files()
                self.assertIn("malformed git status", errors.getvalue())

    def test_failed_discovery_explains_an_ignored_other_root_binding(self):
        root = str(gitfiles.PROJECT_ROOT)
        other = str(Path(tempfile.gettempdir()) / "another-checkout")
        values = {"AOBUS_GIT_DIR": str(Path(tempfile.gettempdir()) / "private.git"), "AOBUS_GIT_WORK_TREE": other}
        completed = mock.Mock(returncode=128, stdout="", stderr="fatal: not a git repository")
        with mock.patch.dict(os.environ, values), mock.patch.object(gitfiles.subprocess, "run", return_value=completed):
            with contextlib.redirect_stderr(io.StringIO()) as errors, self.assertRaises(SystemExit):
                gitfiles._git_lines("branch", "--show-current")
        self.assertIn("not a git repository", errors.getvalue())
        self.assertIn("ignored AOBUS_GIT_WORK_TREE", errors.getvalue())
        self.assertIn(repr(other), errors.getvalue())
        self.assertIn(repr(root), errors.getvalue())

    def test_successful_discovery_ignores_other_root_binding_without_warning(self):
        other = str(Path(tempfile.gettempdir()) / "another-checkout")
        values = {"AOBUS_GIT_DIR": str(Path(tempfile.gettempdir()) / "private.git"), "AOBUS_GIT_WORK_TREE": other}
        completed = mock.Mock(returncode=0, stdout="main\n", stderr="")
        with (
            mock.patch.dict(os.environ, values),
            mock.patch.object(gitfiles.subprocess, "run", return_value=completed) as run,
        ):
            with contextlib.redirect_stderr(io.StringIO()) as errors:
                self.assertEqual(gitfiles._git_lines("branch", "--show-current"), ["main"])
        self.assertNotIn("--git-dir", run.call_args.args[0])
        self.assertEqual(errors.getvalue(), "")

    def test_null_terminated_git_output_preserves_newlines_and_whitespace_in_paths(self):
        completed = mock.Mock(returncode=0, stdout=b"a\nfile.py\0a\rfile.py\0a\r\nfile.py\0 \0", stderr=b"")
        with mock.patch.object(gitfiles.subprocess, "run", return_value=completed) as run:
            self.assertEqual(
                gitfiles._git_lines("ls-files", "-z", null_terminated=True),
                ["a\nfile.py", "a\rfile.py", "a\r\nfile.py", " "],
            )
        self.assertFalse(run.call_args.kwargs["text"])

    def test_source_discovery_binds_only_the_matching_checkout(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            private = root / "private metadata"
            with mock.patch.dict(
                os.environ, {"AOBUS_GIT_DIR": str(private), "AOBUS_GIT_WORK_TREE": str(gitfiles.PROJECT_ROOT)}
            ):
                self.assertEqual(
                    gitfiles.git_command("ls-files", source_discovery=True, os_name="posix", platform_name="linux"),
                    [
                        "git",
                        "--no-optional-locks",
                        "-c",
                        "diff.autoRefreshIndex=false",
                        "--git-dir",
                        str(private),
                        "--work-tree",
                        str(gitfiles.PROJECT_ROOT),
                        "ls-files",
                    ],
                )
                with mock.patch.object(gitfiles, "PROJECT_ROOT", root):
                    self.assertEqual(
                        gitfiles.git_command("ls-files", source_discovery=True, os_name="posix", platform_name="linux"),
                        ["git", "--no-optional-locks", "-c", "diff.autoRefreshIndex=false", "ls-files"],
                    )

    def test_source_discovery_rejects_incomplete_or_relative_bindings(self):
        root = str(gitfiles.PROJECT_ROOT)
        private = str(Path(tempfile.gettempdir()) / "private.git")
        for values, expected in (
            ({"AOBUS_GIT_DIR": private}, "set together"),
            ({"AOBUS_GIT_WORK_TREE": root}, "set together"),
            ({"AOBUS_GIT_DIR": "", "AOBUS_GIT_WORK_TREE": root}, "set together"),
            ({"AOBUS_GIT_DIR": private, "AOBUS_GIT_WORK_TREE": ""}, "set together"),
            ({"AOBUS_GIT_DIR": "relative.git", "AOBUS_GIT_WORK_TREE": root}, "absolute paths"),
            ({"AOBUS_GIT_DIR": private, "AOBUS_GIT_WORK_TREE": "relative-root"}, "absolute paths"),
        ):
            with self.subTest(values=values), mock.patch.dict(os.environ, values):
                errors = io.StringIO()
                with contextlib.redirect_stderr(errors), self.assertRaises(SystemExit):
                    gitfiles.git_command("ls-files", source_discovery=True)
                self.assertIn(expected, errors.getvalue())

    def test_binding_keeps_platform_git_options_and_does_not_export_standard_selectors(self):
        before = dict(os.environ)
        private = str(Path(tempfile.gettempdir()) / "private.git")
        values = {"AOBUS_GIT_DIR": private, "AOBUS_GIT_WORK_TREE": str(gitfiles.PROJECT_ROOT)}
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
                str(gitfiles.PROJECT_ROOT),
            ]
            self.assertEqual(
                gitfiles.git_command("ls-files", source_discovery=True, os_name="nt"),
                [
                    *prefix,
                    "-c",
                    f"safe.directory={gitfiles.PROJECT_ROOT.as_posix()}",
                    "-c",
                    "core.filemode=false",
                    "ls-files",
                ],
            )
            for shared, options in ((False, []), (True, ["-c", "core.filemode=false"])):
                self.assertEqual(
                    gitfiles.git_command(
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
                gitfiles.git_command(
                    "config", "--local", "core.hooksPath", "script/git-hook", os_name="posix", platform_name="linux"
                ),
                ["git", "config", "--local", "core.hooksPath", "script/git-hook"],
            )


class GitWorkflowFixtureTest(unittest.TestCase):
    def setUp(self):
        environment = mock.patch.dict(os.environ)
        environment.start()
        self.addCleanup(environment.stop)
        os.environ.pop("AOBUS_GIT_DIR", None)
        os.environ.pop("AOBUS_GIT_WORK_TREE", None)
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name) / "checkout"
        self.root.mkdir()
        self.git("init", "-b", "main")

    def git(self, *arguments):
        return subprocess.check_output(
            [
                "git",
                "-C",
                str(self.root),
                "-c",
                "user.name=Aobus fixture",
                "-c",
                "user.email=fixture@example.invalid",
                "-c",
                "commit.gpgsign=false",
                "-c",
                "core.hooksPath=unused-fixture-hooks",
                *arguments,
            ],
            stderr=subprocess.STDOUT,
            text=True,
        ).strip()

    def commit(self, name):
        self.git("add", ".")
        self.git("commit", "-m", name)

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
        with mock.patch.object(gitfiles, "PROJECT_ROOT", self.root):
            self.assertEqual(gitfiles.diff_base(), base)
            self.assertEqual(gitfiles.changed_files(), ["topic.cpp"])
            self.assertEqual(gitfiles.changed_files("main"), ["common.cpp", "topic.cpp"])

    def test_explicit_hook_setup_supports_linked_worktrees(self):
        (self.root / "README.md").write_text("fixture\n", encoding="utf-8")
        self.commit("base")
        linked = self.root.parent / "linked"
        self.git("worktree", "add", "-b", "linked", str(linked))
        with mock.patch.object(gitfiles, "PROJECT_ROOT", linked), mock.patch.object(setup, "PROJECT_ROOT", linked):
            self.assertEqual(setup.run_command(Namespace(component="git-hooks", shared_workspaces=None)), 0)
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
            mock.patch.object(gitfiles, "PROJECT_ROOT", self.root),
            mock.patch.dict(os.environ, {"AOBUS_GIT_DIR": str(private), "AOBUS_GIT_WORK_TREE": str(self.root)}),
        ):
            self.assertEqual(gitfiles._git_lines("rev-parse", "HEAD"), [head])
            self.assertEqual(gitfiles._git_lines("ls-files", "--stage"), staged.splitlines())
            self.assertEqual(
                gitfiles._git_lines("ls-files"), ["base.py", "binary.py", "clean.py", "staged.py", "topic.py"]
            )
            self.assertEqual(gitfiles.diff_base(), base)
            self.assertEqual(
                gitfiles.changed_files(suffixes=gitfiles.PYTHON_SUFFIXES),
                ["base.py", "binary.py", "staged.py", "topic.py", "untracked.py"],
            )
            self.assertEqual(
                gitfiles.changed_files(base, suffixes=gitfiles.PYTHON_SUFFIXES),
                ["base.py", "binary.py", "staged.py", "topic.py", "untracked.py"],
            )
            self.assertTrue(gitfiles._git_ok("rev-parse", "--verify", "HEAD"))
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
        with mock.patch.object(gitfiles, "PROJECT_ROOT", self.root):
            self.assertEqual(gitfiles._git_worktree_files(), [new.name])
            self.assertEqual(gitfiles.changed_files("HEAD", suffixes=gitfiles.PYTHON_SUFFIXES), [new.name])
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
        with mock.patch.object(gitfiles, "PROJECT_ROOT", self.root):
            self.assertEqual(gitfiles.changed_files("HEAD", suffixes=gitfiles.PYTHON_SUFFIXES), [new.name])
        self.assertEqual(index.read_bytes(), before)

    def assert_discovery_preserves_names(self, spellings):
        self.git("config", "core.quotePath", "true")
        self.git("config", "status.showUntrackedFiles", "no")
        for state in ("committed", "staged", "untracked", "unstaged"):
            with self.subTest(state=state):
                (self.root / "seed.py").write_text(f"seed {state}\n", encoding="utf-8")
                names = [f"{state}-{spelling}.py" for spelling in spellings]
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
                with mock.patch.object(gitfiles, "PROJECT_ROOT", self.root):
                    self.assertEqual(gitfiles.changed_files(base, suffixes=gitfiles.PYTHON_SUFFIXES), sorted(names))
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
        with mock.patch.object(gitfiles, "PROJECT_ROOT", self.root):
            self.assertEqual(gitfiles.changed_files("HEAD", suffixes=gitfiles.SOURCE_SUFFIXES), [])
            paths[0].write_bytes(b"\0changed binary\n")
            paths[1].write_text("int value = 2;\n", encoding="utf-8")
            self.assertEqual(
                gitfiles.changed_files("HEAD", suffixes=gitfiles.SOURCE_SUFFIXES), ["attribute.cpp", "binary.py"]
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
        with mock.patch.object(gitfiles, "PROJECT_ROOT", self.root):
            # Use local-filesystem semantics even if this fixture runs on an SMB host.
            with mock.patch.object(gitfiles, "_darwin_checkout_uses_smb", return_value=False):
                self.assertEqual(gitfiles.changed_files("HEAD"), [path.name])
        self.assertEqual(index.read_bytes(), before)

    def test_hook_setup_does_not_redirect_to_discovery_metadata(self):
        other = self.root.parent / "other"
        other.mkdir()
        with mock.patch.object(self, "root", other):
            self.git("init", "-b", "main")
        config = other / ".git" / "config"
        before = config.read_bytes()
        with (
            mock.patch.object(gitfiles, "PROJECT_ROOT", self.root),
            mock.patch.object(setup, "PROJECT_ROOT", self.root),
            mock.patch.dict(os.environ, {"AOBUS_GIT_DIR": str(other / ".git"), "AOBUS_GIT_WORK_TREE": str(self.root)}),
        ):
            self.assertEqual(setup.run_command(Namespace(component="git-hooks", shared_workspaces=None)), 0)
        configured = subprocess.check_output(
            ["git", "-C", str(self.root), "config", "--local", "--get", "core.hooksPath"], text=True
        ).strip()
        self.assertEqual(configured, "script/git-hook")
        self.assertEqual(config.read_bytes(), before)

    def test_missing_bound_metadata_fails_discovery_instead_of_returning_an_empty_scope(self):
        with (
            mock.patch.object(gitfiles, "PROJECT_ROOT", self.root),
            mock.patch.dict(
                os.environ,
                {"AOBUS_GIT_DIR": str(self.root.parent / "absent.git"), "AOBUS_GIT_WORK_TREE": str(self.root)},
            ),
        ):
            errors = io.StringIO()
            with contextlib.redirect_stderr(errors), self.assertRaises(SystemExit):
                gitfiles.changed_files()
            self.assertIn("git branch --show-current failed", errors.getvalue())

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
        with mock.patch.object(gitfiles, "PROJECT_ROOT", self.root):
            self.assertEqual(gitfiles.changed_files(output["sha"]), ["first.cpp", "last.cpp", "middle.cpp"])
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
        result, output = self.ci_scope("nonexistent-base")
        self.assertNotEqual(result.returncode, 0)
        self.assertNotEqual(output.get("docs-only"), "true")
        (self.root / "unknown-input").write_text("native gate\n", encoding="utf-8")
        self.commit("unknown input")
        result, output = self.ci_scope(base)
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
