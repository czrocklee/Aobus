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
            "ccWrappers": {
                "gcc": self.wrapper(suffixSalt="fixture_target_gcc"),
                # Clang links with lld and carries no LTO make helper.
                "clang": {
                    key: value for key, value in self.wrapper(suffixSalt="fixture_target").items() if key != "ltoMake"
                },
            },
        }
        report.update(changes)
        self.report.write_text(json.dumps(report))

    def wrapper(self, **changes):
        wrapper = {
            "suffixSalt": "fixture_target_gcc",
            "hardeningEnable": self.policy,
            "version": "16.2.0",
            "tools": {
                role: "/nix/store/fixture-tools/bin/" + name
                for role, name in (
                    ("cc", "gcc"),
                    ("cxx", "g++"),
                    ("ar", "gcc-ar"),
                    ("strip", "strip"),
                    ("objcopy", "objcopy"),
                    ("gcov", "gcov"),
                )
            },
            "builtinRoots": ["/nix/store/fixture-gcc"],
            "sanitizerIncludeDirectory": "/nix/store/fixture-gcc/include",
            "ltoMake": "/nix/store/fixture-tools/bin/make",
        }
        wrapper.update(changes)
        return wrapper

    def test_missing_stale_or_malformed_policy_fails_closed(self):
        for changes in (
            {"schemaVersion": 2},
            {"nixpkgsRevision": "stale"},
            {"ccWrappers": {}},
            {"ccWrappers": []},
            {"ccWrappers": {"gcc": []}},
            {"ccWrappers": {"gcc": self.wrapper(suffixSalt="target=invalid")}},
            {"ccWrappers": {"gcc": self.wrapper(hardeningEnable="fortify")}},
            {"ccWrappers": {"gcc": self.wrapper(hardeningEnable=[None])}},
            {"ccWrappers": {"gcc": self.wrapper(hardeningEnable=["flag\nsecond-line"])}},
            {"ccWrappers": {"gcc": self.wrapper(version="")}},
            {"ccWrappers": {"gcc": self.wrapper(tools={"cc": "/nix/store/fixture-tools/bin/gcc"})}},
            {"ccWrappers": {"gcc": self.wrapper(builtinRoots=[None])}},
            {"ccWrappers": {"gcc": self.wrapper(sanitizerIncludeDirectory="")}},
            {"ccWrappers": {"gcc": self.wrapper(ltoMake="")}},
            {"ccWrappers": {"gcc": {k: v for k, v in self.wrapper().items() if k != "ltoMake"}}},
        ):
            with self.subTest(changes=changes):
                self.write_report(**changes)
                with self.assertRaisesRegex(native.BazelWorkspaceError, "pinned shell"):
                    compiler_probe.nix_cc_wrapper()
        self.report.unlink()
        with self.assertRaisesRegex(native.BazelWorkspaceError, "pinned shell"):
            compiler_probe.nix_cc_wrapper()

    def test_unknown_verifier_configuration_is_not_treated_as_an_optimized_profile(self):
        # "fast" is not an alias: only the exact fastbuild name is admitted.
        with self.assertRaisesRegex(ValueError, "configuration"):
            contract._expected_hardening_environment("fast")

    def test_fastbuild_hardening_keeps_the_unoptimized_policy(self):
        # Fastbuild owns its unoptimized mode: fortify and fortify3 require
        # optimization and stay excluded exactly as for Debug, never Release.
        expected = contract._expected_hardening_environment("fastbuild")
        self.assertEqual(expected, contract._expected_hardening_environment("debug"))
        self.assertEqual(
            next(iter(expected.values())).split(), ["stackprotector", "strictoverflow", "relro", "bindnow"]
        )
        release = contract._expected_hardening_environment("release")
        self.assertEqual(next(iter(release.values())).split(), self.policy)

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
