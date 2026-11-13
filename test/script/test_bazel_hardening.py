"""Nix-owned hardening snapshots and explicit development-mode launch flags."""

import json
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

from ao.core import bazel_compiler_probe as compiler_probe
from ao.core import bazel_contract as contract
from ao.core import bazel_native as native
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
        self.environment = patch.dict(native.os.environ, environment, clear=True)
        self.environment.start()
        self.addCleanup(self.environment.stop)

    def write_report(self, **changes):
        report = {
            "schemaVersion": 1,
            "nixpkgsRevision": native.read_json(native.PROJECT_ROOT / "nixpkgs.json")["rev"],
            "ccWrapper": {"suffixSalt": "fixture_target", "hardeningEnable": self.policy},
        }
        report.update(changes)
        self.report.write_text(json.dumps(report))

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
                with self.assertRaisesRegex(native.BazelWorkspaceError, "pinned shell"):
                    compiler_probe.nix_cc_wrapper()
        self.report.unlink()
        with self.assertRaisesRegex(native.BazelWorkspaceError, "pinned shell"):
            compiler_probe.nix_cc_wrapper()

    def test_unknown_verifier_configuration_is_not_treated_as_an_optimized_profile(self):
        with self.assertRaisesRegex(ValueError, "configuration"):
            contract._expected_hardening_environment("fastbuild")

    def test_prepared_rc_selects_configuration_without_implementing_toolchain_modes(self):
        for config in ("debug", "release"):
            with (
                self.subTest(config=config),
                patch.object(native, "native_platform", return_value="linux"),
                patch.object(native, "executable", side_effect=lambda name, **_: "/nix/store/tools/bin/" + name),
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


if __name__ == "__main__":
    unittest.main()
