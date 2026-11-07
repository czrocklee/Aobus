"""Shared Bazel installations retain independent per-tree server/output state."""

import os
import shlex
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

from ao.command.bazel import validate_forwarded_arguments
from ao.core import bazel_native as native
from ao.core import bazel_providers as providers
from ao.core import bazel_workspace as workspace


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
            patch.object(workspace.builddir, "windows_state_root", return_value=self.root / "windows-state"),
        ):
            prepared = workspace.prepare_workspace(tree, config)
        return [
            shlex.split(line) for line in (prepared / "aobus.bazelrc").read_text(encoding="utf-8").splitlines() if line
        ]

    def test_every_host_and_profile_uses_separate_output_bases_and_bounded_idle_lifetime(self):
        for host in ("linux", "macos", "windows"):
            for config in ("debug", "release"):
                with self.subTest(host=host, config=config):
                    tree = self.root / host / config
                    tokens = self.prepare(tree, config, host)
                    startup = [line[1] for line in tokens if line[0] == "startup"]
                    self.assertEqual(
                        startup,
                        [
                            "--output_user_root=" + (self.root / "state/bazel").as_posix(),
                            "--output_base=" + (tree / "state/output").as_posix(),
                            "--max_idle_secs=900",
                        ],
                    )
                    self.assertIn(["build", "--experimental_disk_cache_gc_max_size=50G"], tokens)

    def test_shared_install_root_shares_repository_downloads_and_keeps_servers_per_tree(self):
        commands = [self.prepare(self.root / name) for name in ("first", "second")]
        roots, bases = [], []
        for name, command in zip(("first", "second"), commands, strict=True):
            tree = self.root / name
            self.assertIn(["common", "--experimental_install_base_gc_max_age=0"], command)
            self.assertIn(["build", "--disk_cache=" + (self.root / "state/cache/bazel").as_posix()], command)
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

    def test_state_selection_retains_overrides_and_native_fallbacks(self):
        for host, environment, expected in (
            ("linux", {}, self.root / "home/.cache/Aobus"),
            ("linux", {"XDG_CACHE_HOME": str(self.root / "xdg cache")}, self.root / "xdg cache/Aobus"),
            ("macos", {}, self.root / "home/Library/Caches/Aobus"),
            ("windows", {}, self.root / "windows-state"),
            *(
                (host, {"AOBUS_STATE_ROOT": str(self.root / "Åsa 音乐")}, self.root / "Åsa 音乐")
                for host in ("linux", "macos", "windows")
            ),
        ):
            with self.subTest(host=host, environment=environment):
                tree = self.root / "tree with spaces 音乐"
                tokens = self.prepare(tree, host=host, environment=environment)
                self.assertIn(["startup", "--output_user_root=" + (expected / "bazel").as_posix()], tokens)
                self.assertIn(["startup", "--output_base=" + (tree / "state/output").as_posix()], tokens)
                self.assertIn(["build", "--disk_cache=" + (expected / "cache/bazel").as_posix()], tokens)

    def test_server_and_cache_policies_remain_managed(self):
        for option in (
            "max_idle_secs",
            "idle_server_tasks",
            "experimental_install_base_gc_max_age",
            "experimental_disk_cache_gc_max_size",
            "repository_cache",
            "experimental_repository_cache",
            "repository_contents_cache",
            "output_user_root",
            "output_base",
            "install_base",
        ):
            for spelling in (option, option.replace("_", "-")):
                for arguments in ([f"--{spelling}=value"], [f"--{spelling}", "value"]):
                    with self.subTest(arguments=arguments):
                        with self.assertRaisesRegex(native.BazelWorkspaceError, "managed by ao"):
                            validate_forwarded_arguments(arguments)


if __name__ == "__main__":
    unittest.main()
