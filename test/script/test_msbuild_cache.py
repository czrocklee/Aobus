"""Guard MSBuild incrementality without making custom caches Clean targets."""

import hashlib
import io
import subprocess
import unittest
from pathlib import Path
from unittest import mock

from ao.core import msbuild_cache

LOCAL = r"C:\Users\dev\AppData\Local"
CONFIG = {
    "cache_dir": LOCAL + r"\Aobus\cache",
    "temporary_dir": LOCAL + r"\Aobus\cache\tmp",
    "debug": "false",
    "debug_dir": "",
    "log_file": "",
    "stats_log": "",
    "remote_storage": "",
    "msvc_utf8": "true",
    "prefix_command": "",
    "prefix_command_cpp": "",
}


class MSBuildCacheGuardTest(unittest.TestCase):
    def reason(self, config=None, *, roots=("S:\\", r"R:\work")):
        return msbuild_cache.unsafe_reason(
            CONFIG if config is None else config, excluded_roots=(LOCAL,), project_roots=roots
        )

    def test_effective_config_accepts_all_origins_and_empty_values(self):
        text = "\n".join(f"(C:\\settings (team)\\ccache.conf) {k} = {v}" for k, v in CONFIG.items())
        self.assertEqual(msbuild_cache.parse_config(text), CONFIG)

    def test_missing_malformed_and_duplicate_config_fail_closed(self):
        for text in ("", "ccache failure", "(default) cache_dir = x", "(default) debug = false\n" * 2):
            with self.subTest(text=text), self.assertRaises(ValueError):
                msbuild_cache.parse_config(text)

    def test_normal_local_cache_is_protected(self):
        self.assertIsNone(self.reason())

    def test_unc_source_share_root_does_not_disable_valid_local_cache(self):
        self.assertIsNone(self.reason(roots=(r"\\server\aobus", r"R:\work")))

    def test_extended_unc_source_prefix_is_case_insensitive(self):
        for prefix in ("UNC", "unc", "UnC"):
            with self.subTest(prefix=prefix):
                source = "\\\\?\\" + prefix + r"\server\aobus"
                self.assertIsNone(self.reason(roots=(source, r"R:\work")))

    def test_configuration_file_selected_custom_paths_are_not_protected(self):
        for key in ("cache_dir", "temporary_dir", "log_file", "stats_log"):
            with self.subTest(key=key):
                self.assertIsNotNone(self.reason(CONFIG | {key: r"D:\shared-cache"}))

    def test_debug_directory_is_checked_only_when_active(self):
        self.assertIsNone(self.reason(CONFIG | {"debug_dir": r"D:\debug"}))
        for directory in ("", r"D:\debug"):
            self.assertIsNotNone(self.reason(CONFIG | {"debug": "true", "debug_dir": directory}))
        self.assertIsNone(self.reason(CONFIG | {"debug": "true", "debug_dir": LOCAL + r"\debug"}))

    def test_path_component_and_absolute_path_boundaries(self):
        for path in (LOCAL + "-outside\\cache", r"C:relative", "relative", r"\rooted", ""):
            with self.subTest(path=path):
                self.assertIsNotNone(self.reason(CONFIG | {"cache_dir": path}))
        self.assertIsNone(self.reason(CONFIG | {"cache_dir": CONFIG["cache_dir"].upper()}))

    def test_source_build_and_parent_overlap_retain_legacy_mode(self):
        for roots in ((LOCAL,), (CONFIG["cache_dir"],), (CONFIG["cache_dir"] + r"\build",)):
            with self.subTest(roots=roots):
                self.assertIsNotNone(self.reason(roots=roots))

    def test_unknown_remote_prefix_or_missing_native_inputs_retain_legacy_mode(self):
        for key in ("remote_storage", "prefix_command", "prefix_command_cpp"):
            with self.subTest(key=key):
                self.assertIsNotNone(self.reason(CONFIG | {key: "unverified"}))
        self.assertIsNotNone(msbuild_cache.unsafe_reason(CONFIG, excluded_roots=(), project_roots=("S:\\",)))
        self.assertIsNotNone(self.reason(roots=()))

    def mode(self, **changes):
        arguments = dict(
            executable=Path(r"C:\managed\ccache.exe"),
            managed_wrapper=Path(r"C:\managed\msbuild\cl.exe"),
            expected_sha256=hashlib.sha256(b"verified executable").hexdigest(),
            environ={"AOBUS_MSBUILD_CL_TOOL_EXE": r"C:\managed\msbuild\cl.exe"},
            source_root=Path("S:\\"),
            build_roots=(Path(r"R:\work"),),
            compiler_roots=(Path("S:\\"), Path(r"R:\work")),
        )
        return msbuild_cache.tracking_mode(**(arguments | changes))

    def test_runtime_queries_real_executable_once_with_effective_environment(self):
        output = "\n".join(f"(default) {k} = {v}" for k, v in CONFIG.items())
        with (
            mock.patch.object(Path, "open", side_effect=lambda *args: io.BytesIO(b"verified executable")),
            mock.patch.object(msbuild_cache, "native_excluded_roots", return_value=(LOCAL,)),
            mock.patch.object(msbuild_cache.os.path, "realpath", side_effect=lambda value: value),
            mock.patch.object(
                msbuild_cache.subprocess, "run", return_value=subprocess.CompletedProcess([], 0, output)
            ) as run,
        ):
            self.assertEqual(self.mode(), ("1", None))
        run.assert_called_once()
        self.assertEqual(run.call_args.args[0], [r"C:\managed\ccache.exe", "--show-config"])
        self.assertEqual(run.call_args.kwargs["timeout"], 5)
        self.assertEqual(run.call_args.kwargs["cwd"], Path("S:\\"))

    def test_default_physical_build_is_excluded_but_fixed_compiler_views_are_not(self):
        with (
            mock.patch.object(Path, "open", side_effect=lambda *args: io.BytesIO(b"verified executable")),
            mock.patch.object(msbuild_cache, "native_excluded_roots", return_value=(LOCAL,)),
            mock.patch.object(msbuild_cache.subprocess, "run") as run,
        ):
            mode, reason = self.mode(compiler_roots=(Path("S:\\"), Path(LOCAL + r"\Aobus\build")))
        self.assertEqual(mode, "0")
        self.assertIn("tracker exclusions", reason)
        run.assert_not_called()

    def test_custom_wrapper_sccache_and_changed_binary_never_enable_tracking(self):
        with mock.patch.object(msbuild_cache.subprocess, "run") as run:
            self.assertEqual(self.mode(environ={})[0], "0")
            self.assertEqual(self.mode(environ={"AOBUS_MSBUILD_CL_TOOL_EXE": r"C:\custom\cl.exe"})[0], "0")
            with mock.patch.object(Path, "open", side_effect=lambda *args: io.BytesIO(b"sccache or changed bytes")):
                self.assertEqual(self.mode()[0], "0")
            run.assert_not_called()

    def test_native_folder_failure_and_config_timeout_are_nonfatal(self):
        with (
            mock.patch.object(Path, "open", side_effect=lambda *args: io.BytesIO(b"verified executable")),
            mock.patch.object(msbuild_cache, "native_excluded_roots", side_effect=OSError("unavailable")),
        ):
            self.assertEqual(self.mode()[0], "0")
        with (
            mock.patch.object(Path, "open", side_effect=lambda *args: io.BytesIO(b"verified executable")),
            mock.patch.object(msbuild_cache, "native_excluded_roots", return_value=(LOCAL,)),
            mock.patch.object(msbuild_cache.subprocess, "run", side_effect=subprocess.TimeoutExpired("ccache", 5)),
        ):
            self.assertEqual(self.mode()[0], "0")

    def test_junction_outside_excluded_roots_is_not_trusted(self):
        output = "\n".join(f"(default) {k} = {v}" for k, v in CONFIG.items())

        def physical(value):
            return value.replace(LOCAL + r"\Aobus\cache", r"D:\cache")

        with (
            mock.patch.object(Path, "open", side_effect=lambda *args: io.BytesIO(b"verified executable")),
            mock.patch.object(msbuild_cache, "native_excluded_roots", return_value=(LOCAL,)),
            mock.patch.object(msbuild_cache.os.path, "realpath", side_effect=physical),
            mock.patch.object(msbuild_cache.subprocess, "run", return_value=subprocess.CompletedProcess([], 0, output)),
        ):
            self.assertEqual(self.mode()[0], "0")


if __name__ == "__main__":
    unittest.main()
