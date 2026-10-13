"""Objective-C++ compiler-cache activation, ownership and shared-mode admission."""

import hashlib
import os
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

from ao.core import compiler_cache, workspace_cache


class ObjectiveCppCompilerCacheTest(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.executable = self.root / "cache-provider"
        self.executable.write_bytes(b"provider is not executed by activation")
        self.record = {
            "schemaVersion": 1,
            "provider": "ccache",
            "version": "4.13.6",
            "executable": str(self.executable),
            "executableSha256": hashlib.sha256(self.executable.read_bytes()).hexdigest(),
            "cacheSize": "20G",
        }
        self.key = "CMAKE_OBJCXX_COMPILER_LAUNCHER"
        self.marker = "AOBUS_MANAGED_OBJCXX_COMPILER_LAUNCHER"

    def updates(self, provider, system, overrides):
        environment = {
            "SCCACHE_PATH": str(self.executable),
            "AOBUS_MSBUILD_CL_TOOL_EXE": "explicit-wrapper",
            **overrides,
        }
        if provider == "local":
            return compiler_cache.local_environment(self.record, root=self.root, system=system, environ=environment)
        return compiler_cache.ci_environment(environ=environment, system=system, state=self.root)

    def test_only_darwin_activation_manages_objective_cpp(self):
        for provider in ("local", "ci"):
            for system in ("Linux", "Darwin", "Windows"):
                with self.subTest(provider=provider, system=system):
                    updates = self.updates(provider, system, {})
                    for language in ("C", "CXX"):
                        self.assertEqual(updates[f"CMAKE_{language}_COMPILER_LAUNCHER"], str(self.executable.resolve()))
                        self.assertEqual(updates[f"AOBUS_MANAGED_{language}_COMPILER_LAUNCHER"], "1")
                    if system == "Darwin":
                        self.assertEqual(updates[self.key], str(self.executable.resolve()))
                        self.assertEqual(updates[self.marker], "1")
                    else:
                        self.assertNotIn(self.key, updates)
                        self.assertNotIn(self.marker, updates)

    def test_activation_preserves_empty_and_nonempty_explicit_objective_cpp_launchers(self):
        for provider in ("local", "ci"):
            for desired in ("", "/explicit/cache"):
                with self.subTest(provider=provider, desired=desired):
                    updates = self.updates(provider, "Darwin", {self.key: desired})
                    self.assertNotIn(self.key, updates)
                    self.assertNotIn(self.marker, updates)

    def test_sync_adopts_and_rebinds_only_managed_objective_cpp_entries(self):
        environment = {self.key: "/managed/new", self.marker: "1"}
        expected = [f"-D{self.key}=/managed/new", f"-D{self.marker}:BOOL=ON"]
        self.assertEqual(compiler_cache.cmake_launcher_arguments(environment, build_dir=self.root), expected)
        cache = self.root / "CMakeCache.txt"
        cache.write_text(f"{self.key}:STRING=/managed/old\n{self.marker}:BOOL=ON\n", encoding="utf-8")
        self.assertEqual(
            compiler_cache.cmake_launcher_arguments(environment, build_dir=self.root), [f"-D{self.key}=/managed/new"]
        )
        cache.write_text(f"{self.key}:STRING=/managed/new\n{self.marker}:BOOL=ON\n", encoding="utf-8")
        self.assertEqual(compiler_cache.cmake_launcher_arguments(environment, build_dir=self.root), [])

    def test_sync_never_adopts_unmanaged_cached_objective_cpp_even_when_named_ccache(self):
        for value in ("", "/explicit/ccache"):
            for marker in ("", f"{self.marker}:BOOL=OFF\n"):
                with self.subTest(value=value, marker=marker):
                    (self.root / "CMakeCache.txt").write_text(f"{self.key}:STRING={value}\n{marker}", encoding="utf-8")
                    self.assertEqual(
                        compiler_cache.cmake_launcher_arguments(
                            {self.key: "/managed/ccache", self.marker: "1"}, build_dir=self.root
                        ),
                        [],
                    )

    def test_explicit_objective_cpp_override_retires_ownership_and_stale_configuration_is_cleared(self):
        (self.root / "CMakeCache.txt").write_text(
            f"{self.key}:STRING=/managed/ccache\n{self.marker}:BOOL=ON\n", encoding="utf-8"
        )
        for desired in ("", "/explicit/cache"):
            with self.subTest(desired=desired):
                self.assertEqual(
                    compiler_cache.cmake_launcher_arguments({self.key: desired}, build_dir=self.root),
                    [f"-D{self.key}={desired}", "-U", self.marker],
                )
        # Cleanup depends on ownership, not on Darwin activation being active.
        self.assertEqual(
            compiler_cache.cmake_launcher_arguments({}, build_dir=self.root), ["-U", self.key, "-U", self.marker]
        )


@unittest.skipIf(os.name == "nt", "POSIX shared-workspace policy")
class ObjectiveCppSharedCachePolicyTest(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name).resolve()
        (self.root / "ccache").write_bytes(b"policy-only provider stub")
        source = self.root / "workspace"
        (source / "cmake").mkdir(parents=True)
        shutil.copy2(
            workspace_cache.PROJECT_ROOT / "cmake" / "SharedWorkspaceCache.cmake",
            source / "cmake" / "SharedWorkspaceCache.cmake",
        )
        # Policy-only fixture: native probes separately prove actual OBJCXX loading and compilation.
        (source / "CMakeLists.txt").write_text(
            "cmake_minimum_required(VERSION 3.30)\nproject(CachePolicyProbe LANGUAGES NONE)\n"
            "set(CMAKE_C_COMPILER_ID Clang)\nset(CMAKE_CXX_COMPILER_ID Clang)\n"
            "set(CMAKE_OBJCXX_COMPILER_ID Clang)\nset(CMAKE_OBJCXX_COMPILER_LOADED TRUE)\n"
            "include(cmake/SharedWorkspaceCache.cmake)\n"
            'file(WRITE "${CMAKE_BINARY_DIR}/objcxx-launcher.txt" "${CMAKE_OBJCXX_COMPILER_LAUNCHER}")\n',
            encoding="utf-8",
        )
        environment = {
            compiler_cache.SHARED_WORKSPACES_EFFECTIVE: "1",
            compiler_cache.SHARED_WORKSPACES_CCACHE: str(self.root / "ccache"),
        }
        self.build = self.root / "build"
        self.profile = workspace_cache.prepare(self.build, project_root=source, environ=environment, system="Linux")
        self.launchers = {
            f"CMAKE_{language}_COMPILER_LAUNCHER": str(self.root / "ccache") for language in ("C", "CXX", "OBJCXX")
        }
        self.launchers.update(
            {f"AOBUS_MANAGED_{language}_COMPILER_LAUNCHER": "1" for language in ("C", "CXX", "OBJCXX")}
        )

    def configure(self, extra=(), launchers=None):
        cmake = shutil.which("cmake")
        if cmake is None or shutil.which("ninja") is None:
            self.skipTest("CMake and Ninja are required for policy configuration")
        return subprocess.run(
            [
                cmake,
                "-S",
                str(self.profile.source_dir),
                "-B",
                str(self.build),
                "-G",
                "Ninja",
                "-DCMAKE_BUILD_TYPE=Debug",
                *compiler_cache.cmake_launcher_arguments(self.launchers if launchers is None else launchers),
                *self.profile.cmake_arguments,
                *extra,
            ],
            capture_output=True,
            text=True,
            check=False,
            timeout=60,
        )

    def test_objective_cpp_receives_namespace_and_strict_cache_arguments(self):
        result = self.configure()
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        launcher = (self.build / "objcxx-launcher.txt").read_text(encoding="utf-8").split(";")
        namespace = next(
            arg.split("=", 1)[1]
            for arg in self.profile.cmake_arguments
            if arg.startswith("-DAOBUS_SHARED_WORKSPACE_NAMESPACE:")
        )
        self.assertEqual(
            launcher,
            [
                str(self.root / "ccache"),
                f"base_dir={self.build.resolve()}",
                f"namespace={namespace}",
                "hash_dir=true",
                "sloppiness=",
            ],
        )
        self.assertEqual(
            compiler_cache.read_cmake_cache(self.build / "CMakeCache.txt")["CMAKE_OBJCXX_COMPILER_LAUNCHER"],
            str(self.root / "ccache"),
        )

    def test_objective_cpp_user_prefix_maps_are_rejected_in_base_and_active_configuration_flags(self):
        for key in ("CMAKE_OBJCXX_FLAGS", "CMAKE_OBJCXX_FLAGS_DEBUG"):
            for flag in (
                "-ffile-prefix-map=/physical=/logical",
                "-fdebug-prefix-map=/physical=/logical",
                "-fmacro-prefix-map=/physical=/logical",
            ):
                with self.subTest(key=key, flag=flag):
                    result = self.configure(["-DCMAKE_OBJCXX_FLAGS=", "-DCMAKE_OBJCXX_FLAGS_DEBUG=", f"-D{key}={flag}"])
                    self.assertNotEqual(result.returncode, 0)
                    self.assertIn("owns all compiler prefix maps", " ".join((result.stdout + result.stderr).split()))

    def test_objective_cpp_coverage_and_raw_sanitizers_are_rejected(self):
        for key in ("CMAKE_OBJCXX_FLAGS", "CMAKE_OBJCXX_FLAGS_DEBUG"):
            for flag in (
                "--coverage",
                "-fcoverage-mapping",
                "-fsanitize=address",
                "-fsanitize=thread",
                "-fsanitize-coverage=trace-pc-guard",
            ):
                with self.subTest(key=key, flag=flag):
                    result = self.configure(["-DCMAKE_OBJCXX_FLAGS=", "-DCMAKE_OBJCXX_FLAGS_DEBUG=", f"-D{key}={flag}"])
                    self.assertNotEqual(result.returncode, 0)
                    self.assertIn(
                        "does not support coverage or sanitizer builds",
                        " ".join((result.stdout + result.stderr).split()),
                    )

    def test_shared_mode_rejects_missing_empty_and_unmanaged_objective_cpp_launchers(self):
        for desired in (None, "", "/explicit/cache"):
            with self.subTest(desired=desired):
                environment = {key: value for key, value in self.launchers.items() if "OBJCXX" not in key}
                if desired is not None:
                    environment["CMAKE_OBJCXX_COMPILER_LAUNCHER"] = desired
                result = self.configure(launchers=environment)
                self.assertNotEqual(result.returncode, 0)
                self.assertIn(
                    "requires the verified Aobus-managed ccache OBJCXX launcher",
                    " ".join((result.stdout + result.stderr).split()),
                )


if __name__ == "__main__":
    unittest.main()
