"""Persisted native CMake Debug hardening, without product dependencies."""

import hashlib
import json
import os
import shlex
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

PROJECT_ROOT = Path(__file__).resolve().parents[2]
REPORT_VARIABLE = "AOBUS_NIX_DEPENDENCY_REPORT"
LAUNCHER_SNAPSHOT = """
file(WRITE "${CMAKE_BINARY_DIR}/cmake-command.txt" "${CMAKE_COMMAND}")
foreach(language IN ITEMS C CXX)
  get_property(raw CACHE CMAKE_${language}_COMPILER_LAUNCHER PROPERTY VALUE)
  file(WRITE "${CMAKE_BINARY_DIR}/raw-${language}.txt" "${raw}")
  get_property(managed CACHE AOBUS_MANAGED_${language}_COMPILER_LAUNCHER PROPERTY VALUE)
  file(WRITE "${CMAKE_BINARY_DIR}/managed-${language}.txt" "${managed}")
  file(GENERATE OUTPUT "${CMAKE_BINARY_DIR}/launcher-${language}-$<CONFIG>.txt"
    CONTENT "${CMAKE_${language}_COMPILER_LAUNCHER}")
endforeach()
"""
# Answers only the configure-time query for extra files configured below the environment.
FAKE_CCACHE = """
import os, sys
if sys.argv[1:] != ["--get-config", "extra_files_to_hash"] or "CCACHE_EXTRAFILES" in os.environ:
    sys.exit(f"unexpected ccache invocation: {sys.argv[1:]}")
if "AOBUS_FIXTURE_CCACHE_ERROR" in os.environ:
    sys.exit(os.environ["AOBUS_FIXTURE_CCACHE_ERROR"])
print(os.environ.get("AOBUS_FIXTURE_CONFIGURED_FILES", ""))
"""
EXTRA_FILES = "CCACHE_EXTRAFILES=path_list_append:"
BUSTER = "SCCACHE_C_CUSTOM_CACHE_BUSTER=string_append:"


def tool(name: str) -> str:
    executable = shutil.which(name)
    if executable is None:
        raise unittest.SkipTest(f"{name} is unavailable")
    return Path(executable).as_posix()


def modifications(launcher: list[str]) -> list[str]:
    """Environment modifications in a Debug prefix, in their application order."""
    return [launcher[index + 1] for index, part in enumerate(launcher[: launcher.index("--")]) if part == "--modify"]


def contains(obj: Path, marker: str) -> bool:
    """Compare object bytes without printing the object on failure."""
    return marker.encode() in obj.read_bytes()


def run(argv: list[str], *, cwd: Path, environment: dict[str, str]) -> str:
    result = subprocess.run(
        argv, executable=argv[0], cwd=cwd, env=environment, capture_output=True, text=True, check=False
    )
    output = result.stdout + result.stderr
    if result.returncode:
        raise AssertionError(f"command failed ({result.returncode}): {shlex.join(argv)}\n{output}")
    return output


class NixHardeningLauncherTest(unittest.TestCase):
    """No compiler is enabled: the salts below are authored policy-parser inputs."""

    def setUp(self):
        self.cmake = tool("cmake")
        tool("ninja")
        temporary = tempfile.TemporaryDirectory(prefix="aobus-cmake-hardening-")
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        self.source = self.root / "source"
        self.source.mkdir()
        self.report = self.root / "report.json"
        self.report.write_text(
            json.dumps(
                {
                    "schemaVersion": 1,
                    "ccWrapper": {
                        "suffixSalt": "fixture_clang",
                        "gccSuffixSalt": "fixture_gcc",
                        "hardeningEnable": ["fortify", "fortify3", "stackprotector", "relro", "futurehardening"],
                    },
                }
            ),
            encoding="utf-8",
        )
        self.environment = {**os.environ, REPORT_VARIABLE: str(self.report)}
        self.source.joinpath("CMakeLists.txt").write_text(
            "cmake_minimum_required(VERSION 3.30)\n"
            "project(HardeningLauncherProbe LANGUAGES NONE)\n"
            "set(LINUX ${AOBUS_FIXTURE_LINUX})\n"
            f'include("{PROJECT_ROOT.as_posix()}/cmake/NixHardening.cmake")\n'
            "aobus_configure_nix_hardening()\n" + LAUNCHER_SNAPSHOT,
            encoding="utf-8",
        )

    def configure(
        self, build: Path, launcher: list[str], *, mode="Debug", linux=True, sanitizer="", initial=True, managed=True
    ):
        argv = [
            self.cmake,
            "-S",
            str(self.source),
            "-B",
            str(build),
            "-G",
            "Ninja",
            f"-DCMAKE_BUILD_TYPE={mode}",
            f"-DAOBUS_FIXTURE_LINUX={'ON' if linux else 'OFF'}",
            f"-DAOBUS_ENABLE_ASAN={'ON' if sanitizer == 'ASAN' else 'OFF'}",
            f"-DAOBUS_ENABLE_TSAN={'ON' if sanitizer == 'TSAN' else 'OFF'}",
        ]
        if initial:
            for language in ("C", "CXX"):
                argv.extend(
                    [
                        f"-DCMAKE_{language}_COMPILER_LAUNCHER:STRING={';'.join(launcher)}",
                        f"-DAOBUS_MANAGED_{language}_COMPILER_LAUNCHER:BOOL={'ON' if managed else 'OFF'}",
                    ]
                )
        run(argv, cwd=self.source, environment=self.environment)

    def fake_ccache(self) -> str:
        if os.name == "nt":
            self.skipTest("the fixture ccache is a POSIX script")
        executable = self.root / "fake bin" / "ccache"
        executable.parent.mkdir(exist_ok=True)
        executable.write_text(f"#!{sys.executable}\n{FAKE_CCACHE}", encoding="utf-8")
        executable.chmod(0o755)
        return executable.as_posix()

    def assert_launchers(self, build: Path, mode: str, raw: list[str], expected: list[str], *, managed=True):
        for language in ("C", "CXX"):
            self.assertEqual((build / f"raw-{language}.txt").read_text(encoding="utf-8"), ";".join(raw))
            self.assertEqual(
                (build / f"managed-{language}.txt").read_text(encoding="utf-8"), "ON" if managed else "OFF"
            )
            effective = (build / f"launcher-{language}-{mode}.txt").read_text(encoding="utf-8")
            self.assertEqual(effective.split(";") if effective else [], expected)

    def policy_file(self, build: Path) -> str:
        """The policy file as the Debug prefix spells it, the last extra file it appends."""
        named = modifications((build / "launcher-CXX-Debug.txt").read_text(encoding="utf-8").split(";"))[-2]
        self.assertTrue(named.startswith(EXTRA_FILES))
        named = named.removeprefix(EXTRA_FILES)
        self.assertEqual(Path(named).resolve(), (build / "nix-debug-hardening.policy").resolve())
        return named

    def debug_prefix(self, build: Path, command: str, configured=()) -> list[str]:
        """Expected injected Debug policy, appended to both caches' caller inputs."""
        policy = "stackprotector relro futurehardening"
        assignments = [
            f"{channel}={policy}"
            for channel in (
                "NIX_HARDENING_ENABLE",
                "NIX_HARDENING_ENABLE_FOR_BUILD",
                "NIX_HARDENING_ENABLE_FOR_TARGET",
                "NIX_HARDENING_ENABLE_fixture_clang",
                "NIX_HARDENING_ENABLE_fixture_gcc",
            )
        ]
        policy_file = build / "nix-debug-hardening.policy"
        self.assertEqual(policy_file.read_bytes(), "".join(f"{line}\n" for line in assignments).encode())
        digest = hashlib.sha256(policy_file.read_bytes()).hexdigest()
        extra_files = [*configured, self.policy_file(build)]
        return [
            command,
            "-E",
            "env",
            *assignments,
            *[part for path in extra_files for part in ("--modify", EXTRA_FILES + path)],
            "--modify",
            BUSTER + digest,
            "--",
        ]

    def test_debug_composes_once_without_changing_raw_cache_or_ownership(self):
        explicit = [self.cmake, "-E", "env", "MARKER=original launcher"]
        cache = ["owned-sccache-fixture", "--fixture-option"]
        comma = [self.cmake, "-E", "env", "MARKER=literal,comma"]
        for label, launcher, generated_debug, generated_release in (
            ("empty", [], [], []),
            ("explicit", explicit, explicit, explicit),
            ("cache-shaped", cache, cache, cache),
            ("release-only", [f"$<$<CONFIG:Release>:{';'.join(explicit)}>"], [], explicit),
            ("debug-only", [f"$<$<CONFIG:Debug>:{';'.join(explicit)}>"], explicit, []),
            ("literal-comma", comma, comma, comma),
        ):
            for mode, generated in (("Debug", generated_debug), ("Release", generated_release)):
                with self.subTest(launcher=label, mode=mode):
                    build = self.root / f"{label}-{mode}"
                    self.configure(build, launcher, mode=mode)
                    # CMake normalizes its executable's .EXE spelling on Windows;
                    # injected policy uses CMAKE_COMMAND, while raw user argv stays exact.
                    command = (build / "cmake-command.txt").read_text(encoding="utf-8")
                    self.assertEqual(Path(command), Path(self.cmake))
                    expected = [*self.debug_prefix(build, command), *generated] if mode == "Debug" else generated
                    self.assert_launchers(build, mode, launcher, expected)
                    self.configure(build, launcher, mode=mode, initial=False)
                    self.assert_launchers(build, mode, launcher, expected)

    def test_managed_ccache_hashes_compiler_content_in_every_configuration(self):
        ccache = [self.fake_ccache(), "base_dir=/fixture"]
        identity = [*ccache, "compiler_check=content"]
        # Configured files carry over; the configure-time environment is not baked in.
        configured = "/fixture/configured file.h:/fixture/other.h"
        self.environment.update(AOBUS_FIXTURE_CONFIGURED_FILES=configured, CCACHE_EXTRAFILES="/configure/only.h")
        for label, mode, sanitizer in (
            ("debug", "Debug", ""),
            ("release", "Release", ""),
            ("profile", "RelWithDebInfo", ""),
            ("asan", "Debug", "ASAN"),
            ("tsan", "Debug", "TSAN"),
        ):
            with self.subTest(mode=label):
                build = self.root / label
                self.configure(build, ccache, mode=mode, sanitizer=sanitizer)
                command = (build / "cmake-command.txt").read_text(encoding="utf-8")
                expected = (
                    [*self.debug_prefix(build, command, [configured]), *identity] if label == "debug" else identity
                )
                self.assert_launchers(build, mode, ccache, expected)
                # The raw cache list is unchanged, so reconfiguring appends once.
                self.configure(build, ccache, mode=mode, sanitizer=sanitizer, initial=False)
                self.assert_launchers(build, mode, ccache, expected)

    def test_unmanaged_non_linux_or_unreported_ccache_keeps_its_compiler_check(self):
        ccache = [self.fake_ccache()]
        for label, managed, linux, reported in (
            ("unmanaged", False, True, True),
            ("non-linux", True, False, True),
            ("no-report", True, True, False),
        ):
            with self.subTest(case=label):
                if not reported:
                    self.environment.pop(REPORT_VARIABLE)
                build = self.root / label
                self.configure(build, ccache, mode="Release", linux=linux, managed=managed)
                self.assert_launchers(build, "Release", ccache, ccache, managed=managed)
                self.environment[REPORT_VARIABLE] = str(self.report)

    def test_ccache_launcher_setting_receives_the_debug_policy_file(self):
        ccache = self.fake_ccache()
        for label, setting, conditional in (
            ("files", "extra_files_to_hash=/caller/a.h:/caller/b.h", False),
            ("empty", "extra_files_to_hash=", False),
            ("conditional-files", "extra_files_to_hash=/caller/a.h:/caller/b.h", True),
            ("conditional-empty", "extra_files_to_hash=", True),
        ):
            argument = f"$<$<CONFIG:Debug>:{setting}>" if conditional else setting
            raw = [ccache, argument, "namespace=caller"]
            for mode in ("Debug", "Release"):
                with self.subTest(setting=label, mode=mode):
                    build = self.root / f"{label}-{mode}"
                    self.configure(build, raw, mode=mode, managed=False)
                    expected = [ccache, "", raw[2]] if conditional else raw
                    if mode == "Debug":
                        # ccache lets its arguments outrank CCACHE_EXTRAFILES.
                        policy = self.policy_file(build)
                        setting_with_policy = setting + policy if setting.endswith("=") else f"{setting}:{policy}"
                        command = (build / "cmake-command.txt").read_text(encoding="utf-8")
                        expected = [*self.debug_prefix(build, command), ccache, setting_with_policy, raw[2]]
                    self.assert_launchers(build, mode, raw, expected, managed=False)

    def test_conditional_ccache_executable_is_not_queried_before_generation(self):
        ccache = self.fake_ccache()
        self.environment["AOBUS_FIXTURE_CCACHE_ERROR"] = "conditional executable must not be queried"
        raw = [f"$<$<CONFIG:Debug>:{ccache};namespace=caller>"]
        # No static executable means no configure-time snapshot of lower-level extras.
        for mode in ("Debug", "Release"):
            with self.subTest(mode=mode):
                build = self.root / f"conditional-ccache-{mode}"
                self.configure(build, raw, mode=mode, managed=False)
                expected = []
                if mode == "Debug":
                    command = (build / "cmake-command.txt").read_text(encoding="utf-8")
                    expected = [*self.debug_prefix(build, command), ccache, "namespace=caller"]
                self.assert_launchers(build, mode, raw, expected, managed=False)
                self.configure(build, raw, mode=mode, managed=False, initial=False)
                self.assert_launchers(build, mode, raw, expected, managed=False)

    def test_unreadable_ccache_configuration_fails_configure(self):
        self.environment["AOBUS_FIXTURE_CCACHE_ERROR"] = "fixture configuration failure"
        with self.assertRaises(AssertionError) as raised:
            self.configure(self.root / "unreadable", [self.fake_ccache()])
        message = " ".join(str(raised.exception).split())
        self.assertIn("is configured to hash", message)
        self.assertIn("fixture configuration failure", message)

    def test_release_profile_sanitizers_and_non_linux_keep_original_launcher(self):
        launcher = [self.cmake, "-E", "env", "MARKER=unchanged"]
        for label, mode, linux, sanitizer in (
            ("release", "Release", True, ""),
            ("profile", "RelWithDebInfo", True, ""),
            ("asan", "Debug", True, "ASAN"),
            ("tsan", "Debug", True, "TSAN"),
            ("non-linux", "Debug", False, ""),
        ):
            with self.subTest(mode=label):
                build = self.root / label
                self.configure(build, launcher, mode=mode, linux=linux, sanitizer=sanitizer)
                self.assert_launchers(build, mode, launcher, launcher)
                self.configure(build, launcher, mode=mode, linux=linux, sanitizer=sanitizer, initial=False)
                self.assert_launchers(build, mode, launcher, launcher)

    def test_absent_report_leaves_debug_launcher_unchanged(self):
        self.environment.pop(REPORT_VARIABLE)
        build = self.root / "no-report"
        self.configure(build, [])
        self.assert_launchers(build, "Debug", [], [])


@unittest.skipUnless(sys.platform == "linux", "native Nix wrapper policy is Linux-only")
class NixHardeningNativeTest(unittest.TestCase):
    def setUp(self):
        self.cmake = tool("cmake")
        self.ninja = tool("ninja")
        report_path = os.environ.get(REPORT_VARIABLE)
        if not os.environ.get("IN_NIX_SHELL") or not report_path:
            self.skipTest("requires the governed Nix shell and dependency report")
        report = json.loads(Path(report_path).read_text(encoding="utf-8"))
        wrapper = report.get("ccWrapper", {})
        if "gccSuffixSalt" not in wrapper or "suffixSalt" not in wrapper or "tools" not in wrapper:
            self.skipTest("governed report has not yet admitted compiler tools and both wrapper salts")
        self.assertEqual(report["schemaVersion"], 1)
        self.salts = [wrapper["suffixSalt"], wrapper["gccSuffixSalt"]]
        for salt in self.salts:
            self.assertRegex(salt, r"^[a-zA-Z0-9_]+$")
        self.policy = wrapper["hardeningEnable"]
        self.assertIsInstance(self.policy, list)
        for flag in self.policy:
            self.assertRegex(flag, r"^[a-zA-Z0-9_]+$")
        self.assertIn("stackprotector", self.policy)
        self.assertTrue({"fortify", "fortify3"}.intersection(self.policy))
        self.clang = wrapper["tools"]["clang++"]
        temporary = tempfile.TemporaryDirectory(prefix="aobus-nix-native-hardening-")
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        self.source = self.root / "source"
        (self.source / "cmake").mkdir(parents=True)
        # Forward includes to their real owners; do not copy project sources.
        for name in ("SharedWorkspaceCache", "SharedWorkspaceCacheWindows", "NixHardening"):
            (self.source / "cmake" / f"{name}.cmake").write_text(
                f'include("{PROJECT_ROOT.as_posix()}/cmake/{name}.cmake")\n', encoding="utf-8"
            )
        (self.source / "CMakeLists.txt").write_text(
            "cmake_minimum_required(VERSION 3.30)\n"
            "project(NativeHardeningProbe LANGUAGES CXX)\n"
            "set(CMAKE_EXPORT_COMPILE_COMMANDS ON)\n"
            "set(AOBUS_SHARED_WORKSPACES OFF)\n"
            "set(AOBUS_WINDOWS_SHARED_WORKSPACES OFF)\n"
            "set(USE_FAST_LINKER OFF)\n"
            f'include("{PROJECT_ROOT.as_posix()}/cmake/CompilerOptions.cmake")\n'
            "add_library(probe OBJECT probe.cpp)\n"
            'if(CMAKE_BUILD_TYPE STREQUAL "Debug" AND NOT AOBUS_ENABLE_ASAN AND NOT AOBUS_ENABLE_TSAN)\n'
            "  target_compile_definitions(probe PRIVATE AOBUS_FIXTURE_ORDINARY_DEBUG)\n"
            'elseif(NOT CMAKE_BUILD_TYPE STREQUAL "Debug")\n'
            "  target_compile_definitions(probe PRIVATE AOBUS_FIXTURE_OPTIMIZED)\n"
            "endif()\n" + LAUNCHER_SNAPSHOT,
            encoding="utf-8",
        )
        (self.source / "probe.cpp").write_text(
            "#include <cstring>\n"
            "#ifdef AOBUS_FIXTURE_ORDINARY_DEBUG\n"
            "#ifdef __OPTIMIZE__\n#error Ordinary Debug must not optimize\n#endif\n"
            "#if defined(_FORTIFY_SOURCE) && _FORTIFY_SOURCE > 0\n#error Fortify must be disabled at O0\n#endif\n"
            "#endif\n"
            "#ifdef AOBUS_FIXTURE_OPTIMIZED\n"
            "#ifndef __OPTIMIZE__\n#error Optimized configurations must optimize\n#endif\n"
            "#if !defined(_FORTIFY_SOURCE) || _FORTIFY_SOURCE == 0\n"
            "#error Optimized configurations must retain fortify\n#endif\n"
            "#ifndef gsl_CONFIG_CONTRACT_VIOLATION_TERMINATES\n#error Optimized GSL contracts must terminate\n#endif\n"
            "#else\n"
            "#ifndef gsl_CONFIG_CONTRACT_VIOLATION_ASSERTS\n#error Debug GSL assertions must remain enabled\n#endif\n"
            "#endif\n"
            "#ifndef __SSP_STRONG__\n#error Strong stack protection must remain enabled\n#endif\n"
            "static_assert(gsl_CONFIG_CONTRACT_CHECKING_ON == 1);\n"
            "int probe(const char* value) { return static_cast<int>(std::strlen(value)); }\n",
            encoding="utf-8",
        )
        self.channels = list(
            dict.fromkeys(
                [
                    "NIX_HARDENING_ENABLE",
                    "NIX_HARDENING_ENABLE_FOR_BUILD",
                    "NIX_HARDENING_ENABLE_FOR_TARGET",
                    *[f"NIX_HARDENING_ENABLE_{salt}" for salt in self.salts],
                ]
            )
        )
        # Keep the normal shell intact, injecting full policy into every channel.
        self.environment = {**os.environ, **{channel: " ".join(self.policy) for channel in self.channels}}
        self.environment.update(CCACHE_DIR=str(self.root / "ccache"), CCACHE_CONFIGPATH=str(self.root / "ccache.conf"))
        (self.root / "ccache.conf").write_text("", encoding="utf-8")

    def configure(
        self,
        build: Path,
        compiler: str,
        launcher: list[str],
        *,
        mode="Debug",
        sanitizer="",
        initial=True,
        managed=False,
    ):
        argv = [
            self.cmake,
            "-S",
            str(self.source),
            "-B",
            str(build),
            "-G",
            "Ninja",
            f"-DCMAKE_CXX_COMPILER={compiler}",
            f"-DCMAKE_BUILD_TYPE={mode}",
            f"-DAOBUS_ENABLE_ASAN={'ON' if sanitizer == 'ASAN' else 'OFF'}",
            f"-DAOBUS_ENABLE_TSAN={'ON' if sanitizer == 'TSAN' else 'OFF'}",
        ]
        if mode == "RelWithDebInfo":
            argv.append("-DCMAKE_CXX_FLAGS=-fno-omit-frame-pointer")
        if initial:
            for language in ("C", "CXX"):
                argv.extend(
                    [
                        f"-DCMAKE_{language}_COMPILER_LAUNCHER:STRING={';'.join(launcher)}",
                        f"-DAOBUS_MANAGED_{language}_COMPILER_LAUNCHER:BOOL={'ON' if managed else 'OFF'}",
                    ]
                )
        run(argv, cwd=self.source, environment=self.environment)

    def launcher(self, build: Path, mode="Debug") -> list[str]:
        effective = (build / f"launcher-CXX-{mode}.txt").read_text(encoding="utf-8")
        return effective.split(";") if effective else []

    def compile_options(self, build: Path) -> list[str]:
        entries = json.loads((build / "compile_commands.json").read_text(encoding="utf-8"))
        return shlex.split(entries[0]["command"])

    def ccache_counts(self, ccache: str) -> tuple[int, int]:
        """Cache hits and misses, so an inactive cache cannot pass a reuse check."""
        stats = run([ccache, "--print-stats"], cwd=self.root, environment=self.environment)
        values = dict(line.split("\t", 1) for line in stats.splitlines() if "\t" in line)
        return int(values["direct_cache_hit"]) + int(values["preprocessed_cache_hit"]), int(values["cache_miss"])

    def check_debug(self, compiler: str):
        passthrough = self.root / "sccache-fixture"
        passthrough.write_text(
            f"#!{sys.executable}\nimport json, os, sys\n"
            "with open(os.environ['AOBUS_FIXTURE_LAUNCH_LOG'], 'w', encoding='utf-8') as stream:\n"
            "    policy = {key: value for key, value in os.environ.items()\n"
            "              if key.startswith(('NIX_HARDENING_ENABLE', 'CCACHE_EXTRAFILES', 'SCCACHE_C_'))}\n"
            "    json.dump(policy, stream)\n"
            "os.execvp(sys.argv[1], sys.argv[1:])\n",
            encoding="utf-8",
        )
        passthrough.chmod(0o755)
        # Direct Ninja keeps the caller's own cache inputs ahead of the policy.
        caller = self.root / "caller extra.h"
        caller.write_text("caller\n", encoding="utf-8")
        self.environment.update(CCACHE_EXTRAFILES=str(caller), SCCACHE_C_CUSTOM_CACHE_BUSTER="caller-")
        launchers = [
            ("empty", []),
            ("explicit", [self.cmake, "-E", "env", "MARKER=original launcher"]),
            ("sccache-shaped", [str(passthrough)]),
        ]
        if ccache := shutil.which("ccache"):
            launchers.append(("ccache", [ccache]))
        for label, raw in launchers:
            with self.subTest(launcher=label):
                build = self.root / label
                log = self.root / f"{label}-environment.json"
                self.environment["AOBUS_FIXTURE_LAUNCH_LOG"] = str(log)
                managed = label in ("ccache", "sccache-shaped")
                self.configure(build, compiler, raw, managed=managed)
                first = self.launcher(build)
                self.configure(build, compiler, raw, initial=False, managed=managed)
                self.assertEqual(self.launcher(build), first)
                self.assertEqual(first[:3], [self.cmake, "-E", "env"])
                tail = [*raw, "compiler_check=content"] if label == "ccache" else raw
                self.assertEqual(first[first.index("--") + 1 :], tail)
                self.assertEqual(first.count("--"), 1)
                # The governed fixture configuration names no extra files of its own.
                changes = modifications(first)
                policy_file = changes[0].removeprefix(EXTRA_FILES)
                digest = changes[1].removeprefix(BUSTER)
                self.assertEqual(changes, [EXTRA_FILES + policy_file, BUSTER + digest])
                self.assertEqual(hashlib.sha256(Path(policy_file).read_bytes()).hexdigest(), digest)
                plain = first[3 : first.index("--") - 2 * len(changes)]
                self.assertEqual(
                    first[3 + len(plain) : first.index("--")], ["--modify", changes[0], "--modify", changes[1]]
                )
                assignments = dict(part.split("=", 1) for part in plain)
                self.assertEqual(set(assignments), set(self.channels))
                for policy in assignments.values():
                    self.assertNotIn("fortify", policy.split())
                    self.assertNotIn("fortify3", policy.split())
                    self.assertIn("stackprotector", policy.split())
                for language in ("C", "CXX"):
                    self.assertEqual((build / f"raw-{language}.txt").read_text(encoding="utf-8"), ";".join(raw))
                    self.assertEqual(
                        (build / f"managed-{language}.txt").read_text(encoding="utf-8"), "ON" if managed else "OFF"
                    )
                self.assertIn("-O0", self.compile_options(build))
                commands = run([self.ninja, "-t", "commands", "probe"], cwd=build, environment=self.environment)
                self.assertIn("NIX_HARDENING_ENABLE_FOR_TARGET=", commands)
                # Direct Ninja sees the original full policy, not a filtered configure environment.
                run([self.ninja, "-v", "probe"], cwd=build, environment=self.environment)
                if label == "sccache-shaped":
                    effective = json.loads(log.read_text(encoding="utf-8"))
                    keys = {
                        "CCACHE_EXTRAFILES": f"{caller}:{policy_file}",
                        "SCCACHE_C_CUSTOM_CACHE_BUSTER": "caller-" + digest,
                    }
                    for name, value in {**assignments, **keys}.items():
                        self.assertEqual(effective[name], value)

    def test_gcc_direct_ninja_debug_is_o0_without_fortify_and_keeps_strong_protection(self):
        compiler = shlex.split(os.environ.get("CXX", "g++"))
        if len(compiler) != 1:
            self.skipTest("native probe requires CXX to name one governed compiler")
        self.check_debug(tool(compiler[0]))

    def test_clang_direct_ninja_debug_is_o0_without_fortify_and_keeps_strong_protection(self):
        self.check_debug(self.clang)

    def test_managed_ccache_separates_nix_like_compilers_with_equal_size_and_mtime(self):
        ccache = tool("ccache")
        build = self.root / "identity"
        self.configure(build, self.clang, [ccache], mode="Release", managed=True)
        launcher = self.launcher(build, "Release")
        self.assertEqual(launcher, [ccache, "compiler_check=content"])
        source = self.root / "identity.cpp"
        source.write_text(
            'extern const char marker[];\nconst char marker[] = "fixture-compiler-" AOBUS_FIXTURE_COMPILER;\n',
            encoding="utf-8",
        )
        compilers = []
        for value in ("1", "2"):
            compiler = self.root / f"wrapper-{value}" / "c++"
            compiler.parent.mkdir()
            compiler.write_text(
                f'#!/bin/sh\nexec "{self.clang}" "-DAOBUS_FIXTURE_COMPILER=\\"{value}\\"" "$@"\n', encoding="utf-8"
            )
            compiler.chmod(0o755)
            # Store files share mtime 1, and same-template wrappers can share a size.
            os.utime(compiler, (1, 1))
            compilers.append(compiler)
        self.assertEqual(compilers[0].stat().st_size, compilers[1].stat().st_size)
        run([ccache, "-z"], cwd=self.root, environment=self.environment)
        for value in ("1", "2", "1"):
            run(
                [*launcher, str(compilers[int(value) - 1]), "-c", source.name, "-o", "identity.o"],
                cwd=self.root,
                environment=self.environment,
            )
            self.assertTrue(contains(self.root / "identity.o", f"fixture-compiler-{value}"))
        # Only the repeated compiler hits, so the separation above was cached.
        self.assertEqual(self.ccache_counts(ccache), (1, 2))

    def test_debug_cache_key_keeps_caller_extra_files_with_an_unchanged_policy(self):
        ccache = tool("ccache")
        # The wrapper reads an input that only the caller's extra files name.
        wrapper_input = self.root / "wrapper input.txt"
        wrapper = self.root / "wrapper" / "c++"
        wrapper.parent.mkdir()
        semicolon_input = self.root / "configured;input.txt"
        (self.source / "probe.cpp").write_text(
            'extern const char marker[];\nconst char marker[] = "caller-input-" AOBUS_FIXTURE_INPUT;\n',
            encoding="utf-8",
        )
        configuration = self.root / "ccache.conf"
        for label, raw, configured, environment, input_path in (
            ("environment", [ccache], "", {"CCACHE_EXTRAFILES": str(wrapper_input)}, wrapper_input),
            ("configuration", [ccache], f"extra_files_to_hash = {wrapper_input}\n", {}, wrapper_input),
            ("launcher", [ccache, f"extra_files_to_hash={wrapper_input}"], "", {}, wrapper_input),
            ("configuration-semicolon", [ccache], f"extra_files_to_hash = {semicolon_input}\n", {}, semicolon_input),
        ):
            with self.subTest(origin=label):
                read_input = f"$(cat {shlex.quote(str(input_path))})"
                wrapper.write_text(
                    f'#!/bin/sh\nexec "{self.clang}" "-DAOBUS_FIXTURE_INPUT=\\"{read_input}\\"" "$@"\n',
                    encoding="utf-8",
                )
                wrapper.chmod(0o755)
                configuration.write_text(configured, encoding="utf-8")
                self.environment["CCACHE_DIR"] = str(self.root / f"ccache-{label}")
                input_path.write_text("alpha", encoding="utf-8")
                build = self.root / label
                if label == "configuration-semicolon":
                    # Wrongly splitting the filename must cause a stale hit, not cache bypass.
                    (self.root / "configured").write_text("unchanged absolute decoy\n", encoding="utf-8")
                    build.mkdir()
                    (build / "input.txt").write_text("unchanged relative decoy\n", encoding="utf-8")
                self.configure(build, str(wrapper), raw, managed=True)
                self.environment.update(environment)
                run([ccache, "-z"], cwd=self.root, environment=self.environment)
                obj = build / "CMakeFiles" / "probe.dir" / "probe.cpp.o"
                for value in ("alpha", "beta", "alpha"):
                    input_path.write_text(value, encoding="utf-8")
                    obj.unlink(missing_ok=True)
                    run([self.ninja, "probe"], cwd=build, environment=self.environment)
                    self.assertTrue(contains(obj, f"caller-input-{value}"))
                self.assertEqual(self.ccache_counts(ccache), (1, 2))
                for name in environment:
                    self.environment.pop(name)

    def test_debug_cache_key_follows_the_injected_hardening_policy(self):
        ccache = tool("ccache")
        report = json.loads(Path(os.environ[REPORT_VARIABLE]).read_text(encoding="utf-8"))
        fixture_report = self.root / "report.json"
        self.environment[REPORT_VARIABLE] = str(fixture_report)
        (self.source / "probe.cpp").write_text(
            "int probe(const char* value) {\n"
            "  char buffer[64] = {};\n"
            "  int length = 0;\n"
            "  while (value[length] != '\\0' && length < 63) {\n"
            "    buffer[length] = value[length];\n"
            "    ++length;\n"
            "  }\n"
            "  return buffer[0] + length;\n"
            "}\n",
            encoding="utf-8",
        )
        protected = list(self.policy)
        unprotected = [flag for flag in protected if flag != "stackprotector"]
        caller = self.root / "caller extra.h"
        caller.write_text("caller\n", encoding="utf-8")

        def write_report(policy: list[str]):
            report["ccWrapper"]["hardeningEnable"] = policy
            fixture_report.write_text(json.dumps(report), encoding="utf-8")

        # A launcher setting outranks the environment, so it must carry the policy too.
        for label, raw in (
            ("environment", [ccache]),
            ("launcher", [ccache, f"extra_files_to_hash={caller}"]),
            ("conditional-launcher", [ccache, f"$<$<CONFIG:Debug>:extra_files_to_hash={caller}>"]),
            ("conditional-empty", [ccache, "$<$<CONFIG:Debug>:extra_files_to_hash=>"]),
        ):
            with self.subTest(launcher=label):
                build = self.root / f"policy-{label}"
                self.environment["CCACHE_DIR"] = str(self.root / f"ccache-{label}")
                write_report(protected)
                self.configure(build, self.clang, raw, managed=True)
                run([ccache, "-z"], cwd=self.root, environment=self.environment)

                def stack_protected(policy: list[str], build=build) -> bool:
                    # Rewriting the report reconfigures through Ninja; the compiler argv is unchanged.
                    write_report(policy)
                    run([self.ninja, "probe"], cwd=build, environment=self.environment)
                    return b"__stack_chk_fail" in (build / "CMakeFiles" / "probe.dir" / "probe.cpp.o").read_bytes()

                self.assertTrue(stack_protected(protected))
                self.assertFalse(stack_protected(unprotected))
                self.assertTrue(stack_protected(protected))
                # Only the restored policy reuses its earlier object.
                self.assertEqual(self.ccache_counts(ccache), (1, 2))

    def test_conditional_and_literal_comma_launchers_compile_debug_and_release_objects(self):
        explicit = [self.cmake, "-E", "env", "MARKER=conditional launcher"]
        comma = [self.cmake, "-E", "env", "MARKER=literal,comma"]
        for label, raw, generated_debug, generated_release, managed in (
            ("release-only", [f"$<$<CONFIG:Release>:{';'.join(explicit)}>"], [], explicit, False),
            ("debug-only", [f"$<$<CONFIG:Debug>:{';'.join(explicit)}>"], explicit, [], False),
            ("literal-comma", comma, comma, comma, True),
        ):
            for mode, generated in (("Debug", generated_debug), ("Release", generated_release)):
                with self.subTest(launcher=label, mode=mode):
                    build = self.root / f"{label}-{mode}"
                    self.configure(build, self.clang, raw, mode=mode, managed=managed)
                    first = self.launcher(build, mode)
                    self.configure(build, self.clang, raw, mode=mode, managed=managed, initial=False)
                    self.assertEqual(self.launcher(build, mode), first)
                    self.assertNotIn("", first)
                    if mode == "Debug":
                        self.assertEqual(first[:3], [self.cmake, "-E", "env"])
                        self.assertEqual(first[first.index("--") + 1 :], generated)
                    else:
                        self.assertEqual(first, generated)
                    for language in ("C", "CXX"):
                        self.assertEqual((build / f"raw-{language}.txt").read_text(encoding="utf-8"), ";".join(raw))
                        self.assertEqual(
                            (build / f"managed-{language}.txt").read_text(encoding="utf-8"), "ON" if managed else "OFF"
                        )
                    run([self.ninja, "-v", "probe"], cwd=build, environment=self.environment)
                    self.assertTrue((build / "CMakeFiles" / "probe.dir" / "probe.cpp.o").is_file())

    def test_conditional_ccache_launcher_builds_debug_and_disappears_in_release(self):
        ccache = tool("ccache")
        raw = [f"$<$<CONFIG:Debug>:{ccache};namespace=caller>"]
        for mode in ("Debug", "Release"):
            with self.subTest(mode=mode):
                build = self.root / f"conditional-ccache-{mode}"
                self.configure(build, self.clang, raw, mode=mode)
                first = self.launcher(build, mode)
                self.configure(build, self.clang, raw, mode=mode, initial=False)
                self.assertEqual(self.launcher(build, mode), first)
                self.assertNotIn("", first)
                if mode == "Debug":
                    self.assertEqual(first[:3], [self.cmake, "-E", "env"])
                    self.assertEqual(first[first.index("--") + 1 :], [ccache, "namespace=caller"])
                else:
                    self.assertEqual(first, [])
                for language in ("C", "CXX"):
                    self.assertEqual((build / f"raw-{language}.txt").read_text(encoding="utf-8"), ";".join(raw))
                    self.assertEqual((build / f"managed-{language}.txt").read_text(encoding="utf-8"), "OFF")
                run([self.ninja, "-v", "probe"], cwd=build, environment=self.environment)
                self.assertTrue((build / "CMakeFiles" / "probe.dir" / "probe.cpp.o").is_file())

    def test_optimized_and_sanitizer_objects_compile_with_original_launcher_and_policy(self):
        for label, mode, sanitizer, optimization in (
            ("release", "Release", "", "-O3"),
            ("profile", "RelWithDebInfo", "", "-O2"),
            ("asan", "Debug", "ASAN", "-fsanitize=address,undefined"),
            ("tsan", "Debug", "TSAN", "-fsanitize=thread"),
        ):
            for launcher_label, raw in (
                ("empty", []),
                ("explicit", [self.cmake, "-E", "env", "MARKER=unchanged"]),
            ):
                with self.subTest(mode=label, launcher=launcher_label):
                    build = self.root / f"{label}-{launcher_label}"
                    self.configure(build, self.clang, raw, mode=mode, sanitizer=sanitizer)
                    self.configure(build, self.clang, raw, mode=mode, sanitizer=sanitizer, initial=False)
                    self.assertEqual(self.launcher(build, mode), raw)
                    for language in ("C", "CXX"):
                        self.assertEqual((build / f"raw-{language}.txt").read_text(encoding="utf-8"), ";".join(raw))
                    options = self.compile_options(build)
                    self.assertNotIn("-O0", options)
                    self.assertIn(optimization, options)
                    if label != "release":
                        self.assertIn("-fno-omit-frame-pointer", options)
                    commands = run([self.ninja, "-t", "commands", "probe"], cwd=build, environment=self.environment)
                    self.assertNotIn("NIX_HARDENING_ENABLE", commands)
                    run([self.ninja, "-v", "probe"], cwd=build, environment=self.environment)
                    self.assertTrue((build / "CMakeFiles" / "probe.dir" / "probe.cpp.o").is_file())
                    # Compile objects only: no sanitizer runtime or product build gate is claimed.


if __name__ == "__main__":
    unittest.main()
