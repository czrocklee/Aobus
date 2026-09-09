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
    def test_empty_merge_base_reports_a_discovery_error(self):
        with mock.patch.object(gitfiles, "_git_lines", side_effect=[["topic"], []]):
            with mock.patch.object(gitfiles, "_git_ok", return_value=True):
                with contextlib.redirect_stderr(io.StringIO()) as errors, self.assertRaises(SystemExit):
                    gitfiles.diff_base()
        self.assertIn("returned no revision", errors.getvalue())
        self.assertIn("--commit", errors.getvalue())

    def test_windows_git_config_is_process_local_and_checkout_scoped(self):
        self.assertEqual(
            gitfiles._git_command("status", os_name="nt"),
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
            gitfiles._git_command("status", os_name="posix", platform_name="linux"),
            ["git", "status"],
        )

    def test_macos_git_ignores_smb_executable_bit_artifacts_process_locally(self):
        self.assertEqual(
            gitfiles._git_command(
                "status",
                os_name="posix",
                platform_name="darwin",
                checkout_uses_smb=True,
            ),
            ["git", "-c", "core.filemode=false", "status"],
        )

    def test_macos_git_preserves_file_modes_on_local_filesystems(self):
        self.assertEqual(
            gitfiles._git_command(
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
        self.assertEqual(gitfiles.CPP_TRANSLATION_UNIT_SUFFIXES, (".cpp",))
        self.assertIn(".def", gitfiles.CPP_INCLUDE_FRAGMENT_SUFFIXES)
        self.assertNotIn(".def", gitfiles.CPP_TRANSLATION_UNIT_SUFFIXES)
        self.assertTrue("app/include/ao/i18n/MessageInventory.def".endswith(gitfiles.CPP_SUFFIXES))
        self.assertTrue("app/include/ao/i18n/MessageInventory.def".endswith(gitfiles.SOURCE_SUFFIXES))
        self.assertFalse("app/include/ao/i18n/MessageInventory.def".endswith(gitfiles.CPP_TRANSLATION_UNIT_SUFFIXES))

    def test_changed_files_selects_def_include_fragments(self):
        def git_lines(*args: str) -> list[str]:
            if args[:2] == ("diff", "--name-only"):
                return ["app/include/ao/i18n/MessageInventory.def", "README.md"]
            if args[:2] == ("ls-files", "--others"):
                return ["app/i18n/MessageCatalog.cpp"]
            return []

        with mock.patch.object(gitfiles, "diff_base", return_value="main"):
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


class GitWorkflowFixtureTest(unittest.TestCase):
    def setUp(self):
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
            self.assertEqual(setup.run_command(Namespace(component="git-hooks")), 0)
        configured = subprocess.check_output(
            ["git", "-C", str(linked), "config", "--local", "--get", "core.hooksPath"], text=True
        ).strip()
        self.assertEqual(configured, "script/git-hook")

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

    def test_ci_nested_documentation_and_skill_routes(self):
        (self.root / "README.md").write_text("base\n", encoding="utf-8")
        self.commit("base")
        for name, expected in (
            ("doc/development/naming-convention.md", "true"),
            (".agents/skills/example/SKILL.md", "true"),
            (".agents/skills/example/scripts/check.py", "false"),
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
