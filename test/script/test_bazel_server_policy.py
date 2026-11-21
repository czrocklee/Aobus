"""Prepared Bazel launches share native caches but keep independent per-tree state."""

import os
import shlex
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

from ao.core import bazel_native as native
from ao.portal import bazel_providers as providers
from ao.portal import bazel_workspace as workspace


def _read_rc(path: Path) -> list[list[str]]:
    return [
        words for line in path.read_text(encoding="utf-8").splitlines() if (words := shlex.split(line, comments=True))
    ]


def _disk_cache_options(commands: list[list[str]]) -> list[tuple[str, str]]:
    return [
        (scope, option)
        for scope, *options in commands
        for option in options
        if option.partition("=")[0] == "--disk_cache" or option.startswith("--experimental_disk_cache_gc_")
    ]


class BazelServerPolicyTest(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix="aobus-bazel-server-policy-")
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)

    def prepare(self, tree, config="debug", host="linux", environment=None):
        environment = {"AOBUS_STATE_ROOT": str(self.root / "state")} if environment is None else environment
        if host == "windows":
            environment = {
                "VSINSTALLDIR": "C:/selected-vs/",
                "VCToolsVersion": "14.44.35207",
                "WindowsSDKVersion": "10.0.26100.0/",
                **environment,
            }
        with (
            patch.dict(os.environ, environment, clear=True),
            patch.object(native, "native_platform", return_value=host),
            patch.object(providers, "vcpkg_prefix", return_value=self.root / "provider"),
            patch.object(workspace.Path, "home", return_value=self.root / "home"),
            patch.object(workspace.build_dir, "windows_state_root", return_value=self.root / "windows-state"),
        ):
            prepared = workspace.prepare_workspace(tree, config)
        return _read_rc(prepared / "aobus.bazelrc")

    def test_repository_rc_selects_a_home_disk_cache_for_each_native_host(self):
        rc = _read_rc(native.PROJECT_ROOT / ".bazelrc")
        self.assertIn(["build", "--enable_platform_specific_config"], rc)
        self.assertCountEqual(
            _disk_cache_options(rc),
            [
                ("build:linux", "--disk_cache=~/.cache/Aobus/cache/bazel"),
                ("build:macos", "--disk_cache=~/Library/Caches/Aobus/cache/bazel"),
                ("build:windows", "--disk_cache=~/AppData/Local/Aobus/cache/bazel"),
                ("build", "--experimental_disk_cache_gc_max_size=50G"),
            ],
        )

    def test_ordinary_test_results_do_not_depend_on_the_callers_search_path_or_portal_state(self):
        # Both launchers read the repository rc; only the portal adds the prepared
        # rc. A global passthrough silently makes these otherwise green tests miss.
        commands = _read_rc(native.PROJECT_ROOT / ".bazelrc") + self.prepare(self.root / "tree")
        for scope, *options in commands:
            with self.subTest(scope=scope, options=options):
                self.assertNotIn("--test_env=PATH", options)
                self.assertNotIn("--test_env=AOBUS_STATE_ROOT", options)
                if scope in {"build", "test", "build:windows", "test:windows"}:
                    # Bazel's Windows tw.exe requires a nonempty PATH to start the test.
                    self.assertNotIn("--test_env=PATH=", options)

    def test_explicit_disk_cache_overrides_only_the_cache_and_can_be_unset(self):
        tree, state = self.root / "tree", self.root / "managed"
        cache = self.root / "other volume #2" / "缓存"
        host = native.native_platform()
        for value in (str(cache), "", None):
            with self.subTest(value=value):
                environment = {"AOBUS_STATE_ROOT": str(state)}
                if value is not None:
                    environment["AOBUS_BAZEL_DISK_CACHE"] = value
                commands = self.prepare(tree, host=host, environment=environment)
                self.assertEqual(
                    _disk_cache_options(commands), [("build", "--disk_cache=" + cache.as_posix())] if value else []
                )
                self.assertIn(["startup", "--output_user_root=" + (state / "bazel").as_posix()], commands)
                self.assertIn(["startup", "--output_base=" + (tree / "state/output").as_posix()], commands)
        self.assertFalse(cache.exists())  # Preparation selects storage; Bazel creates it on use.

    def test_invalid_disk_cache_path_does_not_silently_select_another_cache(self):
        paths = ["relative/cache", "~/cache", str(self.root / "cache") + "\nbuild --test_filter=none"]
        if os.name == "nt":
            paths.extend(["C:cache", r"\\server\share\cache"])
        for path in paths:
            with self.subTest(path=path), self.assertRaisesRegex(native.BazelWorkspaceError, "AOBUS_BAZEL_DISK_CACHE"):
                self.prepare(
                    self.root / "tree", host=native.native_platform(), environment={"AOBUS_BAZEL_DISK_CACHE": path}
                )

    def test_prepared_trees_use_separate_output_bases_and_bounded_idle_lifetime(self):
        # The pinned AOBUS_STATE_ROOT short-circuits state selection before
        # any host branch, so one prepared tree carries the whole policy;
        # host state fallbacks are owned by the state-selection test below.
        tree = self.root / "tree"
        tokens = self.prepare(tree)
        startup = [line[1] for line in tokens if line[0] == "startup"]
        self.assertEqual(
            startup,
            [
                "--output_user_root=" + (self.root / "state/bazel").as_posix(),
                "--output_base=" + (tree / "state/output").as_posix(),
                "--max_idle_secs=900",
            ],
        )

    def test_shared_install_root_shares_repository_downloads_and_keeps_servers_per_tree(self):
        commands = [self.prepare(self.root / name) for name in ("first", "second")]
        roots, bases = [], []
        for name, command in zip(("first", "second"), commands, strict=True):
            tree = self.root / name
            self.assertIn(["common", "--experimental_install_base_gc_max_age=0"], command)
            self.assertIn(["build", "--symlink_prefix=" + tree.as_posix() + "/bazel-"], command)
            roots.extend(line[1] for line in command if line[1].startswith("--output_user_root="))
            bases.extend(line[1] for line in command if line[1].startswith("--output_base="))
            # Bazel's default download cache lives under the shared output root.
            self.assertFalse(any("repository_cache" in token for line in command for token in line))
            self.assertFalse(any("--install_base=" in token for line in command for token in line))
        self.assertEqual(roots[0], roots[1])
        self.assertNotEqual(bases[0], bases[1])

    def test_repreparing_a_legacy_tree_preserves_its_old_output_and_install_directories(self):
        tree = self.root / "legacy"
        old_output = tree / "state/old-workspace-hash/server"
        old_install = tree / "state/install/old-install-key"
        old_output.mkdir(parents=True)
        old_install.mkdir(parents=True)
        old_output.joinpath("sentinel").write_text("old server state", encoding="utf-8")
        old_install.joinpath("sentinel").write_text("old installation", encoding="utf-8")
        tokens = self.prepare(tree)
        self.assertIn(["startup", "--output_base=" + (tree / "state/output").as_posix()], tokens)
        self.assertEqual(old_output.joinpath("sentinel").read_text(encoding="utf-8"), "old server state")
        self.assertEqual(old_install.joinpath("sentinel").read_text(encoding="utf-8"), "old installation")
        # Preparation writes configuration only; Bazel creates its native state.
        self.assertFalse((tree / "state/output").exists())

    def test_state_selection_retains_overrides_without_redirecting_the_native_disk_cache(self):
        for host, environment, expected in (
            ("linux", {}, self.root / "home/.cache/Aobus"),
            ("linux", {"XDG_CACHE_HOME": str(self.root / "xdg cache")}, self.root / "xdg cache/Aobus"),
            ("macos", {}, self.root / "home/Library/Caches/Aobus"),
            ("windows", {}, self.root / "windows-state"),
            # The override is honored before any host branch, so one host proves it.
            ("linux", {"AOBUS_STATE_ROOT": str(self.root / "Åsa 音乐")}, self.root / "Åsa 音乐"),
        ):
            with self.subTest(host=host, environment=environment):
                tree = self.root / "tree with spaces 音乐"
                tokens = self.prepare(tree, host=host, environment=environment)
                self.assertIn(["startup", "--output_user_root=" + (expected / "bazel").as_posix()], tokens)
                self.assertIn(["startup", "--output_base=" + (tree / "state/output").as_posix()], tokens)
                # Prepared launches inherit the repository cache policy, even with a separate state root.
                self.assertEqual(_disk_cache_options(tokens), [])


if __name__ == "__main__":
    unittest.main()
