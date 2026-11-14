"""Native profile preflight, platform policy and fail-closed admission."""

import unittest
from unittest.mock import patch

from ao.core import bazel_native as native
from ao.core import bazel_profiles
from ao.core.bazel_native import BazelWorkspaceError


class ProfilePreflightTest(unittest.TestCase):
    def test_validate_request_is_pure_before_any_host_or_sdk_lookup(self):
        with (
            patch.object(native, "native_platform", side_effect=AssertionError("host lookup")),
            patch.object(native, "require_supported_source", side_effect=AssertionError("source lookup")),
            patch("subprocess.run", side_effect=AssertionError("process")),
        ):
            for config in ("debug", "release", "profile", "asan", "tsan"):
                for platform in ("linux", "macos"):
                    with self.subTest(config=config, platform=platform):
                        self.assertIsNone(bazel_profiles.validate_request(config, platform))
            for config in ("debug", "release", "asan"):
                self.assertIsNone(bazel_profiles.validate_request(config, "windows"))
            for config, platform, message in (
                ("profile", "windows", "sampling profile"),
                ("tsan", "windows", "ThreadSanitizer"),
                ("coverage", "linux", "unknown"),
                ("profile", "unknown", "unsupported"),
            ):
                with (
                    self.subTest(config=config, platform=platform),
                    self.assertRaisesRegex(BazelWorkspaceError, message),
                ):
                    bazel_profiles.validate_request(config, platform)

    def test_compilation_config_keeps_the_closed_enum_and_release_sdk_mapping(self):
        self.assertEqual(bazel_profiles.CONFIGURATIONS, ("debug", "release", "profile", "asan", "tsan"))
        self.assertEqual(bazel_profiles.SANITIZER_CONFIGS, ("asan", "tsan"))
        for config, expected in (
            ("debug", "debug"),
            ("asan", "debug"),
            ("tsan", "debug"),
            ("release", "release"),
            ("profile", "release"),
        ):
            with self.subTest(config=config):
                self.assertEqual(bazel_profiles.compilation_config(config), expected)
        with self.assertRaisesRegex(BazelWorkspaceError, "unknown"):
            bazel_profiles.compilation_config("opt")


if __name__ == "__main__":
    unittest.main()
