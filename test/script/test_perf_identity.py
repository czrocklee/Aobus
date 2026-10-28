"""Regression coverage for source identity in reused performance executables."""

import contextlib
import io
import json
import os
import subprocess
import tempfile
import unittest
from functools import partial
from pathlib import Path
from typing import ClassVar
from unittest import mock

from ao.__main__ import make_parser
from ao.command import perf
from ao.core import builddir


class PerformanceIdentityTest(unittest.TestCase):
    def setUp(self):
        self.enterContext(mock.patch.dict(os.environ))
        for name in ("GIT_DIR", "GIT_WORK_TREE", "GIT_INDEX_FILE", "GIT_COMMON_DIR"):
            os.environ.pop(name, None)
        # Fixture repositories must not inherit injected configuration either.
        os.environ.pop("GIT_CONFIG_PARAMETERS", None)
        os.environ.pop("GIT_CONFIG_COUNT", None)
        for name in [key for key in os.environ if key.startswith(("GIT_CONFIG_KEY_", "GIT_CONFIG_VALUE_"))]:
            os.environ.pop(name, None)
        configuration = Path(self.enterContext(tempfile.TemporaryDirectory())) / "git-config"
        configuration.write_text("", encoding="utf-8")
        os.environ["GIT_CONFIG_GLOBAL"] = str(configuration)
        os.environ["GIT_CONFIG_SYSTEM"] = str(configuration)
        os.environ["GIT_CONFIG_NOSYSTEM"] = "1"

    def test_fixture_git_environment_is_isolated_and_restored(self):
        inherited = {
            "GIT_DIR": "inherited-private",
            "GIT_WORK_TREE": "inherited-worktree",
            "GIT_INDEX_FILE": "inherited-index",
            "GIT_COMMON_DIR": "inherited-common",
            "GIT_CONFIG_PARAMETERS": "'status.showUntrackedFiles=no'",
            "GIT_CONFIG_COUNT": "2",
            "GIT_CONFIG_KEY_0": "status.showUntrackedFiles",
            "GIT_CONFIG_VALUE_0": "no",
            "GIT_CONFIG_KEY_1": "submodule.child.ignore",
            "GIT_CONFIG_VALUE_1": "all",
        }
        configuration_names = ("GIT_CONFIG_GLOBAL", "GIT_CONFIG_SYSTEM", "GIT_CONFIG_NOSYSTEM")
        with mock.patch.dict(os.environ, inherited):
            original = {name: os.environ.get(name) for name in configuration_names}
            fixture = PerformanceIdentityTest()
            try:
                fixture.setUp()
                self.assertTrue(all(name not in os.environ for name in inherited))
                for name in ("GIT_CONFIG_GLOBAL", "GIT_CONFIG_SYSTEM"):
                    self.assertTrue(Path(os.environ[name]).is_file())
                self.assertEqual(os.environ["GIT_CONFIG_NOSYSTEM"], "1")
            finally:
                fixture.doCleanups()
            self.assertEqual({name: os.environ[name] for name in inherited}, inherited)
            self.assertEqual({name: os.environ.get(name) for name in configuration_names}, original)

    def test_provenance_uses_checkout_scoped_git_commands_on_native_share_profiles(self):
        command = perf.gitfiles.git_command
        for profile, options in (
            ("windows", {"os_name": "nt", "platform_name": "win32", "checkout_uses_smb": True}),
            ("macos-smb", {"os_name": "posix", "platform_name": "darwin", "checkout_uses_smb": True}),
            ("linux", {"os_name": "posix", "platform_name": "linux", "checkout_uses_smb": False}),
        ):
            with self.subTest(profile=profile):
                with (
                    mock.patch.object(perf.gitfiles, "git_command", side_effect=partial(command, **options)) as git,
                    mock.patch.object(perf, "capture", side_effect=["revision-id\n", ""]) as capture,
                ):
                    self.assertEqual(perf._revision(), "revision-id")
                self.assertEqual(
                    git.call_args_list,
                    [
                        mock.call("rev-parse", "--verify", "HEAD"),
                        mock.call(
                            "--no-optional-locks",
                            "-c",
                            "status.showUntrackedFiles=normal",
                            "status",
                            "--porcelain",
                            "--ignore-submodules=none",
                        ),
                    ],
                )
                self.assertEqual(
                    capture.call_args_list,
                    [
                        mock.call(command("rev-parse", "--verify", "HEAD", **options), cwd=perf.PROJECT_ROOT),
                        mock.call(
                            command(
                                "--no-optional-locks",
                                "-c",
                                "status.showUntrackedFiles=normal",
                                "status",
                                "--porcelain",
                                "--ignore-submodules=none",
                                **options,
                            ),
                            cwd=perf.PROJECT_ROOT,
                        ),
                    ],
                )

    def test_provenance_ignores_discovery_bindings_in_real_repositories(self):
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)

            def git(root, *arguments):
                return subprocess.check_output(
                    [
                        "git",
                        "-C",
                        str(root),
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

            revisions = {}
            for name in ("source", "alternate"):
                root = directory / name
                root.mkdir()
                git(root, "init", "-b", "main")
                (root / "value.txt").write_text(name + "\n", encoding="utf-8")
                git(root, "add", ".")
                git(root, "commit", "-m", "fixture")
                revisions[name] = git(root, "rev-parse", "HEAD")
            self.assertNotEqual(revisions["source"], revisions["alternate"])
            source = directory / "source"
            for dirty in (False, True):
                (source / "value.txt").write_text("changed\n" if dirty else "source\n", encoding="utf-8")
                for private in (str(directory / "alternate" / ".git"), ""):
                    with (
                        self.subTest(dirty=dirty, private=private),
                        mock.patch.dict(os.environ, {"AOBUS_GIT_DIR": private, "AOBUS_GIT_WORK_TREE": str(source)}),
                        mock.patch.object(perf, "PROJECT_ROOT", source),
                        mock.patch.object(perf.gitfiles, "PROJECT_ROOT", source),
                    ):
                        expected = revisions["source"] + ("+dirty" if dirty else "")
                        self.assertEqual(perf._revision(), expected)

    def test_provenance_does_not_refresh_the_real_index_or_hide_content_changes(self):
        for state in ("stat-only", "binary-stat-only", "staged", "unstaged", "untracked"):
            with self.subTest(state=state), tempfile.TemporaryDirectory() as temporary:
                root = Path(temporary)
                git = [
                    "git",
                    "-C",
                    str(root),
                    "-c",
                    "user.name=Aobus fixture",
                    "-c",
                    "user.email=fixture@example.invalid",
                    "-c",
                    "commit.gpgsign=false",
                    "-c",
                    "core.hooksPath=unused-fixture-hooks",
                ]
                subprocess.run([*git, "init", "-b", "main"], check=True, capture_output=True)
                text = root / "value.txt"
                binary = root / "value.bin"
                text.write_text("original\n", encoding="utf-8")
                binary.write_bytes(b"binary\0content")
                (root / ".gitattributes").write_text("*.bin -diff\n", encoding="utf-8")
                subprocess.run([*git, "add", "."], check=True, capture_output=True)
                subprocess.run([*git, "commit", "-m", "fixture"], check=True, capture_output=True)
                revision = subprocess.check_output([*git, "rev-parse", "HEAD"], text=True).strip()
                if state in ("stat-only", "binary-stat-only"):
                    path = binary if state == "binary-stat-only" else text
                    stat = path.stat()
                    os.utime(path, ns=(stat.st_atime_ns, stat.st_mtime_ns + 1_000_000_000))
                elif state == "untracked":
                    (root / "untracked.txt").write_text("new\n", encoding="utf-8")
                else:
                    text.write_text("changed\n", encoding="utf-8")
                    if state == "staged":
                        subprocess.run([*git, "add", "."], check=True, capture_output=True)
                index = root / ".git/index"
                before = (index.read_bytes(), index.stat().st_mtime_ns)
                with (
                    mock.patch.dict(os.environ, {"GIT_OPTIONAL_LOCKS": "1"}),
                    mock.patch.object(perf, "PROJECT_ROOT", root),
                    mock.patch.object(perf.gitfiles, "PROJECT_ROOT", root),
                ):
                    actual = perf._revision()
                expected = revision + ("" if state.endswith("stat-only") else "+dirty")
                self.assertEqual(actual, expected)
                self.assertEqual((index.read_bytes(), index.stat().st_mtime_ns), before)

    _pinned_status_pollutions: ClassVar[dict[str, dict[str, str]]] = {
        "GIT_CONFIG_COUNT": {
            "GIT_CONFIG_COUNT": "2",
            "GIT_CONFIG_KEY_0": "status.showUntrackedFiles",
            "GIT_CONFIG_VALUE_0": "no",
            "GIT_CONFIG_KEY_1": "submodule.child.ignore",
            "GIT_CONFIG_VALUE_1": "all",
        },
        "GIT_CONFIG_PARAMETERS": {
            "GIT_CONFIG_PARAMETERS": "'status.showUntrackedFiles=no' 'submodule.child.ignore=all'"
        },
    }

    def _build_pinned_fixture(self, root):
        def git(directory, *arguments):
            return subprocess.run(
                [
                    "git",
                    "-C",
                    str(directory),
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
                check=True,
                capture_output=True,
            )

        child = root / "child"
        child.mkdir()
        git(child, "init", "-b", "main")
        (child / "value.txt").write_text("original\n", encoding="utf-8")
        git(child, "add", ".")
        git(child, "commit", "-m", "child")
        git(root, "init", "-b", "main")
        (root / ".gitignore").write_text("ignored.txt\n", encoding="utf-8")
        git(root, "add", ".gitignore")
        git(root, "submodule", "add", "../child", "child")
        git(root, "commit", "-m", "submodule")
        revision = subprocess.check_output(["git", "-C", str(root), "rev-parse", "HEAD"], text=True).strip()
        parent_index = root / ".git/index"
        child_index = Path(
            subprocess.check_output(["git", "-C", str(child), "rev-parse", "--git-path", "index"], text=True).strip()
        )
        if not child_index.is_absolute():
            child_index = child / child_index
        return child, revision, parent_index, child_index

    def _apply_pinned_state(self, state, root, child):
        if state == "parent-untracked":
            (root / "untracked.txt").write_text("new\n", encoding="utf-8")
        elif state == "parent-ignored":
            (root / "ignored.txt").write_text("new\n", encoding="utf-8")
        elif state == "submodule-tracked":
            (child / "value.txt").write_text("changed\n", encoding="utf-8")
        elif state == "submodule-untracked":
            (child / "untracked-inside.txt").write_text("new\n", encoding="utf-8")

    def _assert_pinned_indexes_unchanged(self, parent_index, child_index, before):
        self.assertEqual(
            (
                parent_index.read_bytes(),
                parent_index.stat().st_mtime_ns,
                child_index.read_bytes(),
                child_index.stat().st_mtime_ns,
            ),
            before,
        )

    def test_pinned_status_contract_overrides_polluted_configuration(self):
        # Both env-injected configuration channels can hide content from plain
        # status; each dirty class is applied on its own so the explicit
        # switches must carry every one, and the non-ignored-untracked contract
        # must not over-report an ignored file or a clean tree.
        dirty_states = ("parent-untracked", "submodule-tracked", "submodule-untracked")
        clean_states = ("clean", "parent-ignored")
        for channel, injection in self._pinned_status_pollutions.items():
            for state in (*dirty_states, *clean_states):
                with self.subTest(channel=channel, state=state), tempfile.TemporaryDirectory() as temporary:
                    root = Path(temporary)
                    child, revision, parent_index, child_index = self._build_pinned_fixture(root)
                    self._apply_pinned_state(state, root, child)
                    before = (
                        parent_index.read_bytes(),
                        parent_index.stat().st_mtime_ns,
                        child_index.read_bytes(),
                        child_index.stat().st_mtime_ns,
                    )
                    with (
                        mock.patch.dict(os.environ, {**injection, "GIT_OPTIONAL_LOCKS": "1"}),
                        mock.patch.object(perf, "PROJECT_ROOT", root),
                        mock.patch.object(perf.gitfiles, "PROJECT_ROOT", root),
                    ):
                        expected = revision + ("+dirty" if state in dirty_states else "")
                        self.assertEqual(perf._revision(), expected)
                    self._assert_pinned_indexes_unchanged(parent_index, child_index, before)

    def test_each_pinned_switch_is_load_bearing(self):
        # Scratch control, not a production contract: with either switch
        # stripped from the argv, the polluted configuration hides exactly the
        # change class that switch owns, so removal must report a clean tree.
        removal_cases = (
            (("-c", "status.showUntrackedFiles=normal"), "parent-untracked"),
            (("--ignore-submodules=none",), "submodule-tracked"),
        )
        real_git_command = perf.gitfiles.git_command
        for channel, injection in self._pinned_status_pollutions.items():
            for removed, state in removal_cases:
                with (
                    self.subTest(channel=channel, removed=" ".join(removed)),
                    tempfile.TemporaryDirectory() as temporary,
                ):
                    root = Path(temporary)
                    child, revision, _parent_index, _child_index = self._build_pinned_fixture(root)
                    self._apply_pinned_state(state, root, child)

                    def git_command_without(*arguments, removed_arguments=removed, **keywords):
                        argv = list(arguments)
                        for index in range(len(argv) - len(removed_arguments) + 1):
                            if tuple(argv[index : index + len(removed_arguments)]) == removed_arguments:
                                del argv[index : index + len(removed_arguments)]
                                break
                        return real_git_command(*argv, **keywords)

                    with (
                        mock.patch.dict(os.environ, injection),
                        mock.patch.object(perf.gitfiles, "git_command", git_command_without),
                        mock.patch.object(perf, "PROJECT_ROOT", root),
                        mock.patch.object(perf.gitfiles, "PROJECT_ROOT", root),
                    ):
                        self.assertEqual(perf._revision(), revision)

    def test_meaningful_worktree_changes_still_mark_the_revision_dirty(self):
        with mock.patch.object(perf, "capture", side_effect=["revision-id\n", " M script/ao/command/perf.py\n"]):
            self.assertEqual(perf._revision(), "revision-id+dirty")

    def test_revision_read_failure_is_not_silently_reported_as_verified(self):
        with mock.patch.object(perf, "capture", side_effect=subprocess.CalledProcessError(128, "git")) as capture:
            with self.assertRaises(subprocess.CalledProcessError):
                perf._revision()
            capture.assert_called_once()

    def test_worktree_status_failure_cannot_report_a_clean_revision(self):
        def capture(argv, *, cwd, check=True):
            if "rev-parse" in argv:
                return "revision-id\n"
            if check:
                raise subprocess.CalledProcessError(128, argv)
            return ""

        with mock.patch.object(perf, "capture", side_effect=capture):
            with self.assertRaises(subprocess.CalledProcessError):
                perf._revision()

    @mock.patch.dict(os.environ, {"AOBUS_PERF_BASELINE_JSON": ""})
    def test_reused_executable_cannot_claim_the_current_checkout_revision(self):
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            executable = builddir.executable(directory / "test" / perf.TARGET)
            executable.parent.mkdir()
            original = b"existing executable from a previous revision"
            executable.write_bytes(original)
            report_path = directory / "new-parent" / "report.json"
            args = make_parser().parse_args(["perf", "--no-build", "-p", str(directory), "--output", str(report_path)])

            def write_report(_command, *, env, **_kwargs):
                report = {
                    "schema": perf.REPORT_SCHEMA,
                    "metadata": {
                        "revision": env["AOBUS_PERF_REVISION"],
                        "compiler": env["AOBUS_PERF_COMPILER"],
                        "build_mode": env["AOBUS_PERF_BUILD_MODE"],
                        "platform": env["AOBUS_PERF_PLATFORM"],
                        "icu_version": "78.3",
                    },
                    "measurements": [
                        {
                            "capability": "ordering",
                            "scenario": "construction",
                            "dataset": "none",
                            "input_count": 0,
                            "median_ns": 1,
                            "p95_ns": 2,
                        }
                    ],
                }
                Path(env["AOBUS_PERF_REPORT_JSON"]).write_text(json.dumps(report), encoding="utf-8")
                return 0

            output = io.StringIO()
            with (
                mock.patch.object(perf, "_revision", return_value="new-checkout+dirty") as revision,
                mock.patch.object(perf.build, "do_build") as build,
                mock.patch.object(perf.build, "validate_build_tree", return_value="gcc") as validate,
                mock.patch.object(perf, "run", side_effect=write_report),
                contextlib.redirect_stdout(output),
            ):
                self.assertEqual(perf.run_command(args), 0)

            report = json.loads(report_path.read_text(encoding="utf-8"))
            self.assertEqual(report["metadata"]["revision"], "unverified")
            self.assertIn("Source revision: unverified", output.getvalue())
            self.assertNotIn("new-checkout", output.getvalue())
            self.assertEqual(executable.read_bytes(), original)
            validate.assert_called_once_with(args, directory, expected_build_type="Release")
            self.assertEqual(report["metadata"]["compiler"], "gcc")
            self.assertEqual(report["metadata"]["build_mode"], "release")
            build.assert_not_called()
            revision.assert_not_called()


if __name__ == "__main__":
    unittest.main()
