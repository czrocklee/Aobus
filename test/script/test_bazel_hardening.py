"""Nix-owned hardening snapshots and explicit development-mode launch flags."""

import json
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

from ao.core import bazel_workspace as workspace


class LinuxHardeningTest(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix="aobus-nix-hardening-")
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        self.report = self.root / "nix-report.json"
        self.policy = ["fortify", "fortify3", "stackprotector", "strictoverflow", "relro", "bindnow"]
        self.write_report()
        environment = {
            "AOBUS_NIX_DEPENDENCY_REPORT": str(self.report),
            "AOBUS_STATE_ROOT": str(self.root / "state"),
            "NIX_HARDENING_ENABLE": "must-not-replace-the-pinned-report",
            "NIX_CFLAGS_COMPILE": "-isystem /ambient/dependency/include",
            "NIX_LDFLAGS": "-L/ambient/dependency/lib",
        }
        self.environment = patch.dict(workspace.os.environ, environment, clear=True)
        self.environment.start()
        self.addCleanup(self.environment.stop)

    def write_report(self, **changes):
        report = {
            "schemaVersion": 1,
            "nixpkgsRevision": workspace._json(workspace.PROJECT_ROOT / "nixpkgs.json")["rev"],
            "ccWrapper": {"suffixSalt": "fixture_target", "hardeningEnable": self.policy},
        }
        report.update(changes)
        self.report.write_text(json.dumps(report))

    def test_debug_excludes_only_optimization_dependent_fortify(self):
        self.assertEqual(
            workspace.linux_hardening_environment("debug"),
            {"NIX_HARDENING_ENABLE_fixture_target": "stackprotector strictoverflow relro bindnow"},
        )

    def test_release_preserves_nix_policy_including_new_supported_names(self):
        self.policy.append("futurehardening")
        self.write_report()
        self.assertEqual(
            workspace.linux_hardening_environment("release"),
            {"NIX_HARDENING_ENABLE_fixture_target": " ".join(self.policy)},
        )

    def test_explicit_empty_nix_policy_is_not_replaced_by_ambient_defaults(self):
        self.policy.clear()
        self.write_report()
        self.assertEqual(workspace.linux_hardening_environment("release"), {"NIX_HARDENING_ENABLE_fixture_target": ""})

    def test_missing_stale_or_malformed_policy_fails_closed(self):
        for changes in (
            {"schemaVersion": 2},
            {"nixpkgsRevision": "stale"},
            {"ccWrapper": {}},
            {"ccWrapper": []},
            {"ccWrapper": {"suffixSalt": "target=invalid", "hardeningEnable": []}},
            {"ccWrapper": {"suffixSalt": "target", "hardeningEnable": "fortify"}},
            {"ccWrapper": {"suffixSalt": "target", "hardeningEnable": [None]}},
            {"ccWrapper": {"suffixSalt": "target", "hardeningEnable": ["flag\nsecond-line"]}},
        ):
            with self.subTest(changes=changes):
                self.write_report(**changes)
                with self.assertRaisesRegex(workspace.BazelWorkspaceError, "pinned shell"):
                    workspace.linux_hardening_environment("debug")
        self.report.unlink()
        with self.assertRaisesRegex(workspace.BazelWorkspaceError, "pinned shell"):
            workspace.linux_hardening_environment("debug")

    def test_unknown_mode_is_not_treated_as_an_optimized_profile(self):
        with self.assertRaisesRegex(workspace.BazelWorkspaceError, "configuration"):
            workspace.linux_hardening_environment("fastbuild")

    def test_prepared_rc_selects_configuration_without_implementing_toolchain_modes(self):
        for config in ("debug", "release"):
            with (
                self.subTest(config=config),
                patch.object(workspace, "native_platform", return_value="linux"),
                patch.object(workspace, "_executable", side_effect=lambda name, **_: "/nix/store/tools/bin/" + name),
            ):
                prepared = workspace.prepare_workspace(self.root / config, config)
                rc = (prepared / "aobus.bazelrc").read_text()
                self.assertIn("--config=" + config, rc)
                self.assertNotIn("--copt=", rc)
                self.assertNotIn("--action_env=", rc)
                self.assertNotIn("must-not-replace", rc)
                self.assertNotIn("/ambient/", rc)
                self.assertNotIn("--action_env=NIX_HARDENING_ENABLE=", rc)
                original = (prepared / "aobus.bazelrc").read_bytes()
                workspace.prepare_workspace(self.root / config, config)
                self.assertEqual((prepared / "aobus.bazelrc").read_bytes(), original)

    def test_linux_uses_nix_defaults_instead_of_rules_cc_hardening_overrides(self):
        rc = (workspace.PROJECT_ROOT / ".bazelrc").read_text().splitlines()
        self.assertIn("build:linux --extra_toolchains=@aobus_toolchain//:registered_nix_clang", rc)
        self.assertFalse(any("--features=-default_compile_flags" in line for line in rc))
        self.assertFalse(any(line.startswith("build:linux --copt=") for line in rc))
        source = (workspace.PROJECT_ROOT / "shell.nix").read_text()
        self.assertIn("hardeningFlags = pkgs.stdenv.cc.defaultHardeningFlags;", source)
        self.assertIn("suffixSalt = pkgs.llvmPackages_22.clang.suffixSalt;", source)
        self.assertEqual(source.count("hardeningEnable = hardeningFlags;"), 2)


if __name__ == "__main__":
    unittest.main()
