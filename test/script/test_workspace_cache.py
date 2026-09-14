"""Behavior tests for safe compiler-cache reuse across POSIX workspaces."""

import argparse
import contextlib
import io
import json
import os
import re
import shlex
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path
from unittest import mock

from ao.__main__ import main as portal_main
from ao.command import analyze as analyze_command
from ao.command import build as build_command
from ao.command import tidy as tidy_command
from ao.core import buildenv, compiler_cache, paths, tidyengine, workspace_cache


class WorkspaceCacheDisabledTest(unittest.TestCase):
    def test_no_build_sanitizer_commands_load_the_saved_profile_before_dispatch(self):
        def activate(**kwargs):
            os.environ[compiler_cache.SHARED_WORKSPACES_EFFECTIVE] = "1"
            return True

        for host in ("Linux", "Darwin", "Windows"):
            for command in (["test", "--core"], ["run", "cli"]):
                output = io.StringIO()
                with (
                    self.subTest(host=host, command=command),
                    mock.patch.dict(
                        os.environ,
                        {"AOBUS_STATE_ROOT": tempfile.gettempdir(), "AOBUS_CHECKOUT_ID": "profile-admission"},
                        clear=True,
                    ),
                    mock.patch.object(workspace_cache.platform, "system", return_value=host),
                    mock.patch.object(buildenv, "requires_parsed_build_env", return_value=False),
                    mock.patch.object(compiler_cache, "activate_local", side_effect=activate) as activation,
                    contextlib.redirect_stderr(output),
                ):
                    self.assertEqual(portal_main([*command, "--asan", "--no-build"]), 1)
                    activation.assert_called_once()
                    self.assertIn("does not support sanitizer builds", output.getvalue())
                    self.assertNotIn("build directory", output.getvalue())

    def test_sanitizers_are_rejected_before_dispatch_on_every_shared_profile(self):
        for host in ("Linux", "Darwin", "Windows"):
            for enabled in ("0", "1"):
                for sanitizer in ("asan", "tsan"):
                    with self.subTest(host=host, enabled=enabled, sanitizer=sanitizer):
                        args = argparse.Namespace(command="test", suite="core", no_build=True, **{sanitizer: True})
                        context = workspace_cache.command_context(
                            args,
                            system=host,
                            environ={compiler_cache.SHARED_WORKSPACES_EFFECTIVE: enabled},
                        )
                        if enabled == "1":
                            with self.assertRaisesRegex(workspace_cache.WorkspaceCacheError, "sanitizer builds"):
                                with context:
                                    self.fail("unsupported native command must not run")
                        else:
                            with context:
                                pass
            with workspace_cache.command_context(
                argparse.Namespace(command="test", suite="tooling", asan=True),
                system=host,
                environ={compiler_cache.SHARED_WORKSPACES_EFFECTIVE: "1"},
            ):
                pass

    def test_unmanaged_windows_branch_keeps_lexical_paths_without_filesystem_resolution(self):
        source = Path(os.path.abspath("Z:/"))
        build = Path(os.path.abspath("C:/build/Aobus/debug"))

        with (
            mock.patch.object(
                workspace_cache, "absolute_path", side_effect=lambda path: paths.absolute_path(path, os_name="nt")
            ),
            mock.patch.object(Path, "resolve", side_effect=AssertionError("mapped paths must stay lexical")),
            mock.patch.object(compiler_cache, "read_cmake_cache", return_value={}) as cache,
        ):
            profile = workspace_cache.prepare(build, project_root=source, environ={}, system="Windows")
            roots = workspace_cache.validated_source_roots(build, project_root=source)

        self.assertEqual(profile.source_dir, source)
        self.assertFalse(profile.enabled)
        self.assertEqual(profile.cmake_arguments, ())
        self.assertEqual(roots, (source,))
        self.assertEqual(cache.call_args_list, [mock.call(build / "CMakeCache.txt")] * 2)


@unittest.skipIf(os.name == "nt", "POSIX symbolic-link profile")
class WorkspaceCacheOwnershipTest(unittest.TestCase):
    def setUp(self) -> None:
        self.temporary = tempfile.TemporaryDirectory()
        self.root = Path(self.temporary.name)
        self.source = self.root / "workspace" / "Aobus"
        self.build = self.root / "builds" / "debug"
        self.ccache = self.root / "ccache"
        self.ccache.write_bytes(b"verified")
        self._make_source(self.source)
        self.environment = {
            compiler_cache.SHARED_WORKSPACES_EFFECTIVE: "1",
            compiler_cache.SHARED_WORKSPACES_CCACHE: str(self.ccache),
            compiler_cache.SHARED_WORKSPACES_NAMESPACE_PREFIX: "personal",
        }

    def tearDown(self) -> None:
        self.temporary.cleanup()

    @staticmethod
    def _make_source(source: Path) -> None:
        module = workspace_cache.PROJECT_ROOT / "cmake" / "SharedWorkspaceCache.cmake"
        (source / "cmake").mkdir(parents=True)
        shutil.copy2(module, source / "cmake" / module.name)

    def prepare(self, source: Path | None = None) -> workspace_cache.WorkspaceCache:
        return workspace_cache.prepare(
            self.build,
            project_root=source or self.source,
            environ=self.environment,
            system="Linux",
        )

    def test_prepares_immutable_alias_namespace_and_debugger_mapping(self):
        profile = self.prepare()
        alias = self.build / "source"
        self.assertTrue(alias.is_symlink())
        self.assertEqual(alias.resolve(), self.source.resolve())
        self.assertEqual(profile.source_dir, alias)
        mapping = json.loads(profile.map_file.read_text(encoding="utf-8"))
        self.assertEqual(mapping["originalSourceRoot"], str(self.source.resolve()))
        self.assertEqual(mapping["debuggerSourceMap"], {"from": "/aobus/build", "to": str(self.build.resolve())})
        namespace = next(
            argument.split("=", 1)[1]
            for argument in profile.cmake_arguments
            if argument.startswith("-DAOBUS_SHARED_WORKSPACE_NAMESPACE:")
        )
        self.assertTrue(namespace.startswith("personal:aobus-shared-"))

        second = self.prepare()
        self.assertEqual(second, profile)
        self.assertEqual(
            workspace_cache.validated_source_roots(self.build, project_root=self.source),
            (self.source.resolve(),),
        )

    def test_rejects_wrong_or_nonlink_alias_without_replacing_it(self):
        for kind in ("wrong-link", "directory"):
            with self.subTest(kind=kind):
                build = self.root / kind
                build.mkdir()
                alias = build / "source"
                if kind == "wrong-link":
                    other = self.root / "other"
                    other.mkdir(exist_ok=True)
                    alias.symlink_to(other, target_is_directory=True)
                else:
                    alias.mkdir()
                with self.assertRaises(workspace_cache.WorkspaceCacheError):
                    workspace_cache.prepare(
                        build,
                        project_root=self.source,
                        environ=self.environment,
                        system="Linux",
                    )
                self.assertEqual(alias.is_symlink(), kind == "wrong-link")

    def test_missing_alias_cannot_adopt_another_checkout_from_persisted_ownership(self):
        self.prepare()
        (self.build / "source").unlink()
        replacement = self.root / "different" / "RS2"
        self._make_source(replacement)
        with self.assertRaisesRegex(workspace_cache.WorkspaceCacheError, "originalSourceRoot"):
            self.prepare(replacement)
        self.assertFalse((self.build / "source").exists())

        restored = self.prepare()
        self.assertEqual(restored.source_dir.resolve(), self.source.resolve())

    def test_existing_cmake_source_ownership_is_validated_before_alias_creation(self):
        self.build.mkdir(parents=True)
        foreign = self.root / "foreign"
        foreign.mkdir()
        (self.build / "CMakeCache.txt").write_text(f"CMAKE_HOME_DIRECTORY:INTERNAL={foreign}\n", encoding="utf-8")
        with self.assertRaisesRegex(workspace_cache.WorkspaceCacheError, "belongs to CMake source"):
            self.prepare()
        self.assertFalse((self.build / "source").exists())

    def test_configured_alias_without_a_map_file_is_rejected(self):
        self.build.mkdir(parents=True)
        alias = self.build / "source"
        alias.symlink_to(self.source, target_is_directory=True)
        (self.build / "CMakeCache.txt").write_text(
            f"CMAKE_HOME_DIRECTORY:INTERNAL={alias}\n",
            encoding="utf-8",
        )

        with self.assertRaisesRegex(workspace_cache.WorkspaceCacheError, "no workspace-cache ownership"):
            workspace_cache.validated_source_roots(self.build, project_root=self.source)
        with self.assertRaisesRegex(workspace_cache.WorkspaceCacheError, "no workspace-cache ownership"):
            workspace_cache.prepare(
                self.build,
                project_root=self.source,
                environ={compiler_cache.SHARED_WORKSPACES_EFFECTIVE: "0"},
                system="Linux",
            )

    def test_disabled_profile_clears_only_managed_cache_fields(self):
        self.build.mkdir(parents=True)
        (self.build / "CMakeCache.txt").write_text(
            "AOBUS_SHARED_WORKSPACES:BOOL=ON\n"
            "AOBUS_MANAGED_SHARED_WORKSPACES:BOOL=ON\n"
            "AOBUS_SHARED_WORKSPACE_NAMESPACE:STRING=old\n",
            encoding="utf-8",
        )
        profile = workspace_cache.prepare(
            self.build,
            project_root=self.source,
            environ={compiler_cache.SHARED_WORKSPACES_EFFECTIVE: "0"},
            system="Linux",
        )
        self.assertFalse(profile.enabled)
        self.assertEqual(
            profile.cmake_arguments,
            tuple(argument for key in workspace_cache._PROFILE_CACHE_KEYS for argument in ("-U", key)),
        )

    def test_symlinked_build_path_returns_the_physical_compiler_directory(self):
        self.build.parent.mkdir(parents=True)
        alias = self.root / "build-alias"
        alias.symlink_to(self.build.parent, target_is_directory=True)
        profile = workspace_cache.prepare(
            alias / self.build.name, project_root=self.source, environ=self.environment, system="Linux"
        )
        self.assertEqual(profile.compiler_build_dir, self.build.resolve())
        owner = json.loads(profile.map_file.read_text())
        self.assertEqual(str(profile.compiler_build_dir), owner["buildDirectory"])

    def test_unsupported_mode_names_the_process_off_switch(self):
        with self.assertRaisesRegex(workspace_cache.WorkspaceCacheError, "AOBUS_SHARED_WORKSPACES=0"):
            workspace_cache.prepare(
                self.build,
                project_root=self.source,
                environ=self.environment,
                system="Linux",
                unsupported_reason="coverage builds",
            )


@unittest.skipIf(os.name == "nt", "POSIX compiler-cache integration")
class WorkspaceCacheCompilerIntegrationTest(unittest.TestCase):
    @staticmethod
    def _tool(name: str) -> str:
        tool = shutil.which(name)
        if tool is None:
            raise unittest.SkipTest(f"{name} is unavailable")
        return str(Path(tool).resolve())

    @staticmethod
    def _write_source(source: Path) -> None:
        source.mkdir(parents=True)
        (source / "cmake").mkdir()
        shutil.copy2(
            workspace_cache.PROJECT_ROOT / "cmake" / "SharedWorkspaceCache.cmake",
            source / "cmake" / "SharedWorkspaceCache.cmake",
        )
        (source / "include").mkdir()
        (source / "include" / "value.h").write_text(
            "#pragma once\n"
            "inline int source_value() { return 7; }\n"
            "inline const char* header_file() { return __FILE__; }\n",
            encoding="utf-8",
        )
        (source / "include" / "c_value.h").write_text(
            "#pragma once\n#define C_VALUE 3\nstatic inline const char* c_header_file(void) { return __FILE__; }\n",
            encoding="utf-8",
        )
        (source / "generated.h.in").write_text(
            "#pragma once\n"
            "inline int generated_value() { return 5; }\n"
            "inline const char* generated_file() { return __FILE__; }\n",
            encoding="utf-8",
        )
        (source / "main.cpp").write_text(
            "#include <generated.h>\n"
            "#include <value.h>\n"
            "#include <iostream>\n"
            "#include <source_location>\n"
            'extern "C" const char* c_translation_file(void);\n'
            'extern "C" const char* c_header_file_name(void);\n'
            'extern "C" int c_value(void);\n'
            "int main() {\n"
            "  std::cout << __FILE__ << '\\n' << header_file() << '\\n'\n"
            "            << std::source_location::current().file_name() << '\\n'\n"
            "            << generated_file() << '\\n'\n"
            "            << c_translation_file() << '\\n' << c_header_file_name() << '\\n'\n"
            "            << source_value() + generated_value() + c_value() << '\\n';\n"
            "}\n",
            encoding="utf-8",
        )
        (source / "c_probe.c.in").write_text(
            "#include <c_value.h>\n"
            "const char* c_translation_file(void) { return __FILE__; }\n"
            "const char* c_header_file_name(void) { return c_header_file(); }\n"
            "int c_value(void) { return C_VALUE; }\n",
            encoding="utf-8",
        )
        (source / "CMakeLists.txt").write_text(
            "cmake_minimum_required(VERSION 3.30)\n"
            "project(WorkspaceCacheProbe LANGUAGES C CXX)\n"
            "set(CMAKE_EXPORT_COMPILE_COMMANDS ON)\n"
            "set(CMAKE_CXX_SCAN_FOR_MODULES OFF)\n"
            'set(AOBUS_MANAGED_C_COMPILER_LAUNCHER OFF CACHE BOOL "")\n'
            'set(AOBUS_MANAGED_CXX_COMPILER_LAUNCHER OFF CACHE BOOL "")\n'
            "include(cmake/SharedWorkspaceCache.cmake)\n"
            'file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/generated")\n'
            'configure_file(generated.h.in "${CMAKE_BINARY_DIR}/generated/generated.h" @ONLY)\n'
            'configure_file(c_probe.c.in "${CMAKE_BINARY_DIR}/generated/c_probe.c" @ONLY)\n'
            'add_executable(probe main.cpp "${CMAKE_BINARY_DIR}/generated/c_probe.c")\n'
            "target_compile_features(probe PRIVATE cxx_std_20)\n"
            "target_compile_options(probe PRIVATE -g)\n"
            'target_include_directories(probe PRIVATE "${CMAKE_SOURCE_DIR}/include" '
            '"${CMAKE_BINARY_DIR}/generated")\n',
            encoding="utf-8",
        )

    @staticmethod
    def _run(argv: list[str], *, cwd: Path, environment: dict[str, str]) -> str:
        result = subprocess.run(
            argv,
            cwd=cwd,
            env=environment,
            check=False,
            capture_output=True,
            text=True,
            timeout=90,
        )
        if result.returncode != 0:
            raise AssertionError(f"command failed ({result.returncode}): {argv}\n{result.stdout}\n{result.stderr}")
        return result.stdout + result.stderr

    def test_symlinked_build_parent_configures_and_builds_with_consistent_identity(self):
        cmake = self._tool("cmake")
        ccache = self._tool("ccache")
        c_compiler = self._tool("gcc")
        cxx_compiler = self._tool("g++")
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            source = root / "checkout"
            self._write_source(source)
            physical_parent = root / "physical"
            physical_parent.mkdir()
            alias_parent = root / "alias"
            alias_parent.symlink_to(physical_parent, target_is_directory=True)
            selected = alias_parent / "debug"
            environment = {
                **os.environ,
                "CCACHE_DIR": str(root / "cache"),
                compiler_cache.SHARED_WORKSPACES_EFFECTIVE: "1",
                compiler_cache.SHARED_WORKSPACES_CCACHE: ccache,
            }
            profile = workspace_cache.prepare(selected, project_root=source, environ=environment, system="Linux")
            compiler_build = profile.compiler_build_dir or selected
            configure = [
                cmake,
                "-S",
                str(profile.source_dir),
                "-B",
                str(compiler_build),
                "-G",
                "Ninja",
                f"-DCMAKE_C_COMPILER={c_compiler}",
                f"-DCMAKE_CXX_COMPILER={cxx_compiler}",
                f"-DCMAKE_C_COMPILER_LAUNCHER={ccache}",
                f"-DCMAKE_CXX_COMPILER_LAUNCHER={ccache}",
                "-DAOBUS_MANAGED_C_COMPILER_LAUNCHER=ON",
                "-DAOBUS_MANAGED_CXX_COMPILER_LAUNCHER=ON",
                *profile.cmake_arguments,
            ]
            self._run(configure, cwd=source, environment=environment)
            self._run([cmake, "--build", str(compiler_build)], cwd=source, environment=environment)
            result = self._run([str(selected / "probe")], cwd=source, environment=environment)
            self.assertTrue(result.endswith("15\n"))
            self.assertIn("source/main.cpp\n", result)
            self.assertNotIn(str(root), result)
            self.assertIn(
                "no work to do",
                self._run([cmake, "--build", str(compiler_build)], cwd=source, environment=environment).lower(),
            )

    def test_clang_tidy_filters_accept_owned_alias_diagnostics(self):
        clang_tidy = self._tool("clang-tidy")
        clang = self._tool("clang++")
        ccache = self._tool("ccache")
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            source = root / "checkout"
            build = root / "build"
            include = source / "include"
            include.mkdir(parents=True)
            (source / "cmake").mkdir()
            shutil.copy2(
                workspace_cache.PROJECT_ROOT / "cmake" / "SharedWorkspaceCache.cmake",
                source / "cmake" / "SharedWorkspaceCache.cmake",
            )
            header = include / "value.h"
            header.write_text("inline int* pointer() { return 0; }\n", encoding="utf-8")
            translation_unit = source / "main.cpp"
            translation_unit.write_text(
                '#include "value.h"\nint main() { return pointer() != nullptr; }\n',
                encoding="utf-8",
            )
            environment = {
                compiler_cache.SHARED_WORKSPACES_EFFECTIVE: "1",
                compiler_cache.SHARED_WORKSPACES_CCACHE: ccache,
            }
            profile = workspace_cache.prepare(
                build,
                project_root=source,
                environ=environment,
                system="Linux",
            )
            (build / "CMakeCache.txt").write_text(
                f"CMAKE_HOME_DIRECTORY:INTERNAL={source}\n"
                "AOBUS_SHARED_WORKSPACES:BOOL=ON\n"
                "AOBUS_MANAGED_SHARED_WORKSPACES:BOOL=ON\n"
                f"AOBUS_SHARED_WORKSPACE_SOURCE_ALIAS:STRING={profile.source_dir}\n",
                encoding="utf-8",
            )
            alias_header = profile.source_dir / "include" / "value.h"
            compile_command = [
                clang,
                "-I",
                str(profile.source_dir / "include"),
                "-c",
                str(profile.source_dir / "main.cpp"),
                "-o",
                "main.o",
            ]
            (build / "compile_commands.json").write_text(
                json.dumps(
                    [
                        {
                            "directory": str(build),
                            "file": str(profile.source_dir / "main.cpp"),
                            "arguments": compile_command,
                        }
                    ]
                ),
                encoding="utf-8",
            )
            source_roots = workspace_cache.validated_source_roots(build, project_root=source)
            with mock.patch.object(tidy_command, "PROJECT_ROOT", source):
                header_filter = tidy_command.project_header_filter(("include",), source_roots=source_roots)
                exact_filter = tidy_command.exact_header_filter([header], source_roots=source_roots)
                line_filter = tidy_command.path_line_filter([header], source_roots=source_roots)
            analyzer_filter = analyze_command.project_header_filter(source_roots=source_roots)

            self.assertIsNotNone(re.fullmatch(exact_filter, alias_header.as_posix()))
            self.assertEqual(
                [entry["name"] for entry in json.loads(line_filter)],
                [header.as_posix(), alias_header.as_posix()],
            )
            for name, default_filter in (("tidy", header_filter), ("analyzer", analyzer_filter)):
                with self.subTest(command=name):
                    result = subprocess.run(
                        [
                            clang_tidy,
                            "-checks=-*,modernize-use-nullptr",
                            f"-header-filter={default_filter}",
                            f"-line-filter={line_filter}",
                            "-p",
                            str(build),
                            str(translation_unit),
                        ],
                        cwd=build,
                        check=False,
                        capture_output=True,
                        text=True,
                        timeout=60,
                    )
                    output = result.stdout + result.stderr
                    self.assertEqual(result.returncode, 0, output)
                    self.assertIn(f"{alias_header}:1:", output)
                    self.assertIn("modernize-use-nullptr", output)

    def test_gcc_and_clang_share_cache_without_leaking_workspace_identity(self):
        cmake = self._tool("cmake")
        ninja = self._tool("ninja")
        ccache = self._tool("ccache")
        compilers = (("gcc", "g++"), ("clang", "clang++"))
        for c_name, cxx_name in compilers:
            with self.subTest(compiler=cxx_name), tempfile.TemporaryDirectory() as temporary:
                root = Path(temporary)
                c_compiler = self._tool(c_name)
                cxx_compiler = self._tool(cxx_name)
                source_a = root / "workspaces" / "Aobus"
                source_b = root / "unrelated" / "deeper" / "RS2"
                build_a = root / "ram" / "Aobus" / "debug"
                build_b = root / "other-builds" / "RS2"
                self._write_source(source_a)
                self._write_source(source_b)
                config = root / "ccache.conf"
                config.write_text("", encoding="utf-8")
                environment = {
                    key: value for key, value in os.environ.items() if not key.startswith(("CCACHE_", "SCCACHE_"))
                }
                environment.update(
                    CCACHE_DIR=str(root / "cache"),
                    CCACHE_CONFIGPATH=str(config),
                    CCACHE_DEBUG="1",
                )
                profiles: list[workspace_cache.WorkspaceCache] = []
                objects: list[dict[str, bytes]] = []

                for label, source, build in (
                    ("a", source_a, build_a),
                    ("b", source_b, build_b),
                ):
                    log = root / f"ccache-{label}.log"
                    invocation_environment = {
                        **environment,
                        "CCACHE_LOGFILE": str(log),
                        compiler_cache.SHARED_WORKSPACES_EFFECTIVE: "1",
                        compiler_cache.SHARED_WORKSPACES_CCACHE: ccache,
                        compiler_cache.SHARED_WORKSPACES_NAMESPACE_PREFIX: "personal",
                    }
                    profile = workspace_cache.prepare(
                        build,
                        project_root=source,
                        environ=invocation_environment,
                        system="Linux",
                    )
                    profiles.append(profile)
                    launcher_environment = {
                        "CMAKE_C_COMPILER_LAUNCHER": ccache,
                        "CMAKE_CXX_COMPILER_LAUNCHER": ccache,
                        "AOBUS_MANAGED_C_COMPILER_LAUNCHER": "1",
                        "AOBUS_MANAGED_CXX_COMPILER_LAUNCHER": "1",
                    }
                    configure = [
                        cmake,
                        "-S",
                        str(profile.source_dir),
                        "-B",
                        str(build),
                        "-G",
                        "Ninja",
                        f"-DCMAKE_C_COMPILER={c_compiler}",
                        f"-DCMAKE_CXX_COMPILER={cxx_compiler}",
                        *compiler_cache.cmake_launcher_arguments(launcher_environment, build_dir=build),
                        *profile.cmake_arguments,
                    ]
                    self._run(configure, cwd=build, environment=invocation_environment)
                    log.write_text("", encoding="utf-8")
                    if label == "b":
                        source_a.rename(root / "hidden-Aobus")
                    self._run([cmake, "--build", str(build)], cwd=build, environment=invocation_environment)
                    output = self._run([str(build / "probe")], cwd=build, environment=invocation_environment)
                    self.assertEqual(
                        output,
                        "source/main.cpp\nsource/include/value.h\nsource/main.cpp\n"
                        "generated/generated.h\ngenerated/c_probe.c\nsource/include/c_value.h\n15\n",
                    )
                    object_dir = build / "CMakeFiles" / "probe.dir"
                    objects.append(
                        {
                            "C": (object_dir / "generated" / "c_probe.c.o").read_bytes(),
                            "CXX": (object_dir / "main.cpp.o").read_bytes(),
                        }
                    )
                    if label == "b":
                        self.assertGreaterEqual(log.read_text(encoding="utf-8").count("Result: direct_cache_hit"), 2)
                        no_change = self._run(
                            [cmake, "--build", str(build)], cwd=build, environment=invocation_environment
                        )
                        self.assertIn("no work to do", no_change.lower())

                self.assertEqual(objects[0], objects[1])
                first_map = json.loads(profiles[0].map_file.read_text(encoding="utf-8"))
                second_map = json.loads(profiles[1].map_file.read_text(encoding="utf-8"))
                self.assertEqual(first_map["namespace"], second_map["namespace"])

                cache_values = compiler_cache.read_cmake_cache(build_b / "CMakeCache.txt")
                self.assertEqual(cache_values["CMAKE_C_COMPILER_LAUNCHER"], ccache)
                self.assertEqual(cache_values["CMAKE_CXX_COMPILER_LAUNCHER"], ccache)
                command_entries = json.loads((build_b / "compile_commands.json").read_text(encoding="utf-8"))
                command_entry = next(entry for entry in command_entries if entry["file"].endswith("main.cpp"))
                command = shlex.split(command_entry["command"])
                self.assertLess(
                    command.index(f"-fdebug-prefix-map={build_b.resolve()}=/aobus/build"),
                    command.index(f"-ffile-prefix-map={build_b.resolve()}/="),
                )
                ninja_commands = self._run([ninja, "-t", "commands", "probe"], cwd=build_b, environment=environment)
                for source_name in ("main.cpp", "c_probe.c"):
                    compile_line = next(
                        line for line in ninja_commands.splitlines() if source_name in line and ccache in line
                    )
                    launcher_command = shlex.split(compile_line)
                    self.assertLess(
                        launcher_command.index(f"-fdebug-prefix-map={build_b.resolve()}=/aobus/build"),
                        launcher_command.index(f"-ffile-prefix-map={build_b.resolve()}/="),
                    )
                    for cache_argument in (
                        f"base_dir={build_b.resolve()}",
                        f"namespace={second_map['namespace']}",
                        "hash_dir=true",
                        "sloppiness=",
                    ):
                        self.assertIn(cache_argument, launcher_command)

                header = source_b / "include" / "value.h"
                header.write_text(header.read_text(encoding="utf-8").replace("return 7", "return 8"), encoding="utf-8")
                c_header = source_b / "include" / "c_value.h"
                c_header.write_text(
                    c_header.read_text(encoding="utf-8").replace("C_VALUE 3", "C_VALUE 4"),
                    encoding="utf-8",
                )
                b_log = root / "ccache-b.log"
                b_log.write_text("", encoding="utf-8")
                invocation_environment["CCACHE_LOGFILE"] = str(b_log)
                self._run([cmake, "--build", str(build_b)], cwd=build_b, environment=invocation_environment)
                self.assertGreaterEqual(b_log.read_text(encoding="utf-8").count("Result: cache_miss"), 2)

                object_dir = build_b / "CMakeFiles" / "probe.dir"
                for entry, compiler, object_name in (
                    (command_entry, cxx_compiler, "main.cpp.o"),
                    (
                        next(entry for entry in command_entries if entry["file"].endswith("c_probe.c")),
                        c_compiler,
                        "generated/c_probe.c.o",
                    ),
                ):
                    object_path = object_dir / object_name
                    cached_object = object_path.read_bytes()
                    direct_command = shlex.split(entry["command"])
                    compiler_index = direct_command.index(compiler)
                    self._run(
                        direct_command[compiler_index:],
                        cwd=Path(entry["directory"]),
                        environment=environment,
                    )
                    self.assertEqual(object_path.read_bytes(), cached_object)

    def test_cmake_rejects_additional_user_prefix_maps(self):
        cmake = self._tool("cmake")
        self._tool("ninja")
        ccache = self._tool("ccache")
        c_compiler = self._tool("gcc")
        cxx_compiler = self._tool("g++")
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            source = root / "workspace"
            build = root / "build"
            self._write_source(source)
            environment = {
                **os.environ,
                compiler_cache.SHARED_WORKSPACES_EFFECTIVE: "1",
                compiler_cache.SHARED_WORKSPACES_CCACHE: ccache,
                compiler_cache.SHARED_WORKSPACES_NAMESPACE_PREFIX: "",
            }
            profile = workspace_cache.prepare(
                build,
                project_root=source,
                environ=environment,
                system="Linux",
            )
            launcher_environment = {
                "CMAKE_C_COMPILER_LAUNCHER": ccache,
                "CMAKE_CXX_COMPILER_LAUNCHER": ccache,
                "AOBUS_MANAGED_C_COMPILER_LAUNCHER": "1",
                "AOBUS_MANAGED_CXX_COMPILER_LAUNCHER": "1",
            }
            result = subprocess.run(
                [
                    cmake,
                    "-S",
                    str(profile.source_dir),
                    "-B",
                    str(build),
                    "-G",
                    "Ninja",
                    f"-DCMAKE_C_COMPILER={c_compiler}",
                    f"-DCMAKE_CXX_COMPILER={cxx_compiler}",
                    "-DCMAKE_CXX_FLAGS=-ffile-prefix-map=/outside=/logical",
                    *compiler_cache.cmake_launcher_arguments(launcher_environment, build_dir=build),
                    *profile.cmake_arguments,
                ],
                cwd=build,
                env=environment,
                check=False,
                capture_output=True,
                text=True,
                timeout=90,
            )
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("owns all C/C++ prefix maps", result.stdout + result.stderr)

    def test_raw_ninja_rejects_changed_module_until_portal_refreshes_namespace(self):
        cmake = self._tool("cmake")
        ninja = self._tool("ninja")
        ccache = self._tool("ccache")
        c_compiler = self._tool("gcc")
        cxx_compiler = self._tool("g++")
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            source = root / "workspace"
            build = root / "build"
            self._write_source(source)
            config = root / "ccache.conf"
            config.write_text("", encoding="utf-8")
            environment = {
                **os.environ,
                "CCACHE_DIR": str(root / "cache"),
                "CCACHE_CONFIGPATH": str(config),
                compiler_cache.SHARED_WORKSPACES_EFFECTIVE: "1",
                compiler_cache.SHARED_WORKSPACES_CCACHE: ccache,
                compiler_cache.SHARED_WORKSPACES_NAMESPACE_PREFIX: "personal",
            }
            launcher_environment = {
                "CMAKE_C_COMPILER_LAUNCHER": ccache,
                "CMAKE_CXX_COMPILER_LAUNCHER": ccache,
                "AOBUS_MANAGED_C_COMPILER_LAUNCHER": "1",
                "AOBUS_MANAGED_CXX_COMPILER_LAUNCHER": "1",
            }
            first = workspace_cache.prepare(
                build,
                project_root=source,
                environ=environment,
                system="Linux",
            )
            configure = [
                cmake,
                "-S",
                str(first.source_dir),
                "-B",
                str(build),
                "-G",
                "Ninja",
                f"-DCMAKE_C_COMPILER={c_compiler}",
                f"-DCMAKE_CXX_COMPILER={cxx_compiler}",
                *compiler_cache.cmake_launcher_arguments(launcher_environment, build_dir=build),
                *first.cmake_arguments,
            ]
            self._run(configure, cwd=build, environment=environment)
            self._run([cmake, "--build", str(build)], cwd=build, environment=environment)
            first_map = json.loads(first.map_file.read_text(encoding="utf-8"))

            module = source / "cmake" / "SharedWorkspaceCache.cmake"
            module.write_text(
                module.read_text(encoding="utf-8") + "\n# Changed policy fixture.\n",
                encoding="utf-8",
            )
            stale = subprocess.run(
                [cmake, "--build", str(build)],
                cwd=build,
                env=environment,
                check=False,
                capture_output=True,
                text=True,
                timeout=90,
            )
            self.assertNotEqual(stale.returncode, 0)
            self.assertIn("policy changed after this tree was configured", stale.stdout + stale.stderr)

            refreshed = workspace_cache.prepare(
                build,
                project_root=source,
                environ=environment,
                system="Linux",
            )
            refreshed_map = json.loads(refreshed.map_file.read_text(encoding="utf-8"))
            self.assertNotEqual(first_map["namespace"], refreshed_map["namespace"])
            self._run(
                [
                    cmake,
                    "-S",
                    str(refreshed.source_dir),
                    "-B",
                    str(build),
                    *compiler_cache.cmake_launcher_arguments(launcher_environment, build_dir=build),
                    *refreshed.cmake_arguments,
                ],
                cwd=build,
                environment=environment,
            )
            commands = self._run([ninja, "-t", "commands", "probe"], cwd=build, environment=environment)
            self.assertIn(f"namespace={refreshed_map['namespace']}", commands)
            self.assertNotIn(f"namespace={first_map['namespace']}", commands)


class WorkspaceCachePortalSeamTest(unittest.TestCase):
    def test_windows_profile_requires_portal_views_before_filesystem_changes(self):
        with tempfile.TemporaryDirectory() as temporary:
            build_dir = Path(temporary) / "build"
            with self.assertRaisesRegex(workspace_cache.WorkspaceCacheError, "source view was not prepared"):
                workspace_cache.prepare(
                    build_dir,
                    environ={compiler_cache.SHARED_WORKSPACES_EFFECTIVE: "1"},
                    system="Windows",
                )
            self.assertFalse(build_dir.exists())

    def test_normal_reconfigure_uses_alias_and_combines_owned_arguments(self):
        build_dir = Path("/tmp/aobus-workspace-cache-seam")
        profile = workspace_cache.WorkspaceCache(
            build_dir / "source",
            ("-DAOBUS_SHARED_WORKSPACES:BOOL=ON",),
            True,
            build_dir / workspace_cache.MAP_FILE_NAME,
        )
        with (
            mock.patch.object(build_command.workspace_cache, "prepare", return_value=profile),
            mock.patch.object(
                build_command.compiler_cache,
                "cmake_launcher_arguments",
                return_value=["-DCMAKE_CXX_COMPILER_LAUNCHER=/cache/ccache"],
            ),
            mock.patch.object(build_command, "run", return_value=0) as run,
        ):
            build_command.sync_compiler_cache(build_dir)

        run.assert_called_once_with(
            [
                "cmake",
                "-S",
                str(build_dir / "source"),
                "-B",
                str(build_dir),
                "-DCMAKE_CXX_COMPILER_LAUNCHER=/cache/ccache",
                "-DAOBUS_SHARED_WORKSPACES:BOOL=ON",
            ],
            cwd=build_command.PROJECT_ROOT,
        )

    def test_tidy_compile_database_configures_through_alias(self):
        with tempfile.TemporaryDirectory() as temporary:
            build_dir = Path(temporary) / "tidy"
            source_alias = build_dir / "source"
            profile = workspace_cache.WorkspaceCache(
                source_alias,
                ("-DAOBUS_SHARED_WORKSPACES:BOOL=ON",),
                True,
                build_dir / workspace_cache.MAP_FILE_NAME,
            )

            def run_tail(argv: list[str], action: str) -> None:
                if action == "configure":
                    build_dir.mkdir(parents=True, exist_ok=True)
                    (build_dir / "compile_commands.json").write_text("[]\n", encoding="utf-8")

            with (
                mock.patch.object(tidyengine.workspace_cache, "prepare", return_value=profile),
                mock.patch.object(
                    tidyengine.compiler_cache,
                    "cmake_launcher_arguments",
                    return_value=["-DCMAKE_CXX_COMPILER_LAUNCHER=/cache/ccache"],
                ),
                mock.patch.object(tidyengine, "_run_tail", side_effect=run_tail) as run,
            ):
                tidyengine.ensure_compile_db(build_dir, preset="linux-debug")

            configure = run.call_args_list[0].args[0]
            self.assertEqual(configure[configure.index("-S") + 1], str(source_alias))
            self.assertIn("-DCMAKE_CXX_COMPILER_LAUNCHER=/cache/ccache", configure)
            self.assertIn("-DAOBUS_SHARED_WORKSPACES:BOOL=ON", configure)


if __name__ == "__main__":
    unittest.main()
