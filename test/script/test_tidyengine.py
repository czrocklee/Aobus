"""Tests for the cross-platform clang-tidy execution plumbing."""

import json
import os
import tempfile
import unittest
from pathlib import Path
from unittest import mock

from ao.core import tidyengine


class ClangToolDiscoveryTest(unittest.TestCase):
    def test_python_portal_pin_matches_the_cmake_sdk_pin(self):
        module = (tidyengine.PROJECT_ROOT / "cmake" / "LlvmSdk.cmake").read_text(encoding="utf-8")

        self.assertIn(
            f'set(AOBUS_LLVM_SDK_VERSION "{tidyengine.PINNED_LLVM_VERSION}")',
            module,
        )
        self.assertIn(tidyengine.PINNED_LLVM_SHA256, module)
        for relative in tidyengine.LLVM_SDK_REQUIRED_FILES:
            self.assertIn(f'"{relative}"', module)

    def test_cmake_sdk_cache_defaults_to_local_windows_state(self):
        module = (tidyengine.PROJECT_ROOT / "cmake" / "LlvmSdk.cmake").read_text(encoding="utf-8")

        self.assertIn("AOBUS_LLVM_SDK_CACHE_ROOT", module)
        self.assertIn("$ENV{AOBUS_STATE_ROOT}/cache/llvm", module)
        self.assertIn("$ENV{LOCALAPPDATA}/Aobus/cache/llvm", module)
        self.assertIn("${_aobus_llvm_sdk_cache_root}/toolchains/", module)
        self.assertIn("${_aobus_llvm_sdk_cache_root}/downloads/", module)
        self.assertNotIn("${CMAKE_SOURCE_DIR}/out/toolchains/", module)

    def test_windows_clang_tidy_is_the_aobus_executable(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            build_dir = Path(temp_dir)
            tool = build_dir / "tool" / "lint" / "AobusClangTidy.exe"
            tool.parent.mkdir(parents=True)
            tool.touch()

            self.assertEqual(tidyengine.clang_tool(build_dir, "clang-tidy", os_name="nt"), str(tool))

    def test_linux_tool_uses_the_pinned_environment_path(self):
        self.assertEqual(tidyengine.clang_tool(Path("/tmp/build"), "clang-tidy", os_name="posix"), "clang-tidy")

    def test_windows_sdk_tool_comes_from_the_configured_root(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            build_dir = root / "build"
            sdk = root / "llvm sdk"
            tool = sdk / "bin" / "clang-apply-replacements.exe"
            tool.parent.mkdir(parents=True)
            tool.touch()
            build_dir.mkdir()
            (build_dir / "CMakeCache.txt").write_text(
                f"AOBUS_LLVM_SDK_RESOLVED_ROOT:INTERNAL={sdk}\n",
                encoding="utf-8",
            )

            self.assertEqual(
                tidyengine.clang_tool(build_dir, "clang-apply-replacements", os_name="nt"),
                str(tool),
            )

    def test_windows_sdk_tool_does_not_fall_back_to_path(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            build_dir = Path(temp_dir)
            (build_dir / "CMakeCache.txt").write_text(
                "AOBUS_LLVM_SDK_RESOLVED_ROOT:INTERNAL=C:/missing/llvm\n",
                encoding="utf-8",
            )

            with self.assertRaises(SystemExit):
                tidyengine.clang_tool(build_dir, "clang-format", os_name="nt")

    def test_windows_sdk_version_rejects_a_stale_configure(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            build_dir = Path(temp_dir)
            (build_dir / "CMakeCache.txt").write_text(
                "AOBUS_LLVM_SDK_RESOLVED_VERSION:INTERNAL=21.1.0\n",
                encoding="utf-8",
            )

            with self.assertRaises(SystemExit):
                tidyengine.llvm_sdk_version(build_dir, os_name="nt")

    def test_windows_automatic_sdk_with_invalid_marker_is_reconfigured(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            build_dir = root / "build"
            sdk = root / "sdk"
            build_dir.mkdir()
            for relative in tidyengine.LLVM_SDK_REQUIRED_FILES:
                required = sdk / relative
                required.parent.mkdir(parents=True, exist_ok=True)
                required.touch()
            (sdk / tidyengine.LLVM_SDK_COMPLETION_MARKER).write_text("incomplete\n", encoding="utf-8")
            (build_dir / "CMakeCache.txt").write_text(
                "\n".join(
                    [
                        f"AOBUS_LLVM_SDK_RESOLVED_ROOT:INTERNAL={sdk}",
                        f"AOBUS_LLVM_SDK_RESOLVED_VERSION:INTERNAL={tidyengine.PINNED_LLVM_VERSION}",
                        "AOBUS_LLVM_SDK_ROOT:PATH=",
                    ]
                ),
                encoding="utf-8",
            )

            with mock.patch.object(
                tidyengine.builddir,
                "platform_profile",
                return_value=tidyengine.builddir.WINDOWS_PROFILE,
            ):
                with mock.patch.object(tidyengine, "ensure_compile_db") as ensure:
                    tidyengine.ensure_windows_llvm_sdk(build_dir)

            ensure.assert_called_once_with(
                build_dir,
                ["-DAOBUS_BUILD_LINT_PLUGIN=ON"],
                preset="windows-tidy",
                reconfigure_preset=True,
            )

    def test_windows_automatic_sdk_with_valid_marker_is_reused(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            build_dir = root / "build"
            sdk = root / "sdk"
            build_dir.mkdir()
            for relative in tidyengine.LLVM_SDK_REQUIRED_FILES:
                required = sdk / relative
                required.parent.mkdir(parents=True, exist_ok=True)
                required.touch()
            marker = f"version={tidyengine.PINNED_LLVM_VERSION}\nsha256={tidyengine.PINNED_LLVM_SHA256}\n"
            (sdk / tidyengine.LLVM_SDK_COMPLETION_MARKER).write_text(marker, encoding="utf-8")
            (build_dir / "CMakeCache.txt").write_text(
                "\n".join(
                    [
                        f"AOBUS_LLVM_SDK_RESOLVED_ROOT:INTERNAL={sdk}",
                        f"AOBUS_LLVM_SDK_RESOLVED_VERSION:INTERNAL={tidyengine.PINNED_LLVM_VERSION}",
                        "AOBUS_LLVM_SDK_ROOT:PATH=",
                    ]
                ),
                encoding="utf-8",
            )

            with mock.patch.object(
                tidyengine.builddir,
                "platform_profile",
                return_value=tidyengine.builddir.WINDOWS_PROFILE,
            ):
                with mock.patch.object(tidyengine, "ensure_compile_db") as ensure:
                    tidyengine.ensure_windows_llvm_sdk(build_dir)

            ensure.assert_not_called()

    def test_windows_clang_tidy_supports_a_multi_config_output_directory(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            build_dir = Path(temp_dir)
            tool = build_dir / "tool" / "lint" / "Release" / "AobusClangTidy.exe"
            tool.parent.mkdir(parents=True)
            tool.touch()

            self.assertEqual(tidyengine.clang_tool(build_dir, "clang-tidy", os_name="nt"), str(tool))


class CompileDatabaseProvisioningTest(unittest.TestCase):
    def test_existing_database_is_reconfigured_with_requested_preset(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            build_dir = Path(temp_dir)
            (build_dir / "compile_commands.json").write_text("[]", encoding="utf-8")

            with mock.patch.object(tidyengine, "_run_tail") as run_tail:
                tidyengine.ensure_compile_db(
                    build_dir,
                    ["-DAOBUS_BUILD_LINT_PLUGIN=ON"],
                    preset="windows-tidy",
                    reconfigure_preset=True,
                )

            self.assertEqual(
                run_tail.call_args_list[0].args,
                (
                    [
                        "cmake",
                        "-S",
                        str(tidyengine.PROJECT_ROOT),
                        "--preset",
                        "windows-tidy",
                        "-B",
                        str(build_dir),
                        "-U",
                        "AOBUS_BUILD_*",
                        "-DAOBUS_BUILD_LINT_PLUGIN=ON",
                    ],
                    "configure",
                ),
            )
            self.assertEqual(
                run_tail.call_args_list[1].args[0][3:6],
                ["--target", "aobus_generated_headers", "--parallel"],
            )

    def test_existing_database_is_reused_when_no_preset_refresh_is_requested(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            build_dir = Path(temp_dir)
            (build_dir / "compile_commands.json").write_text("[]", encoding="utf-8")

            with mock.patch.object(tidyengine, "_run_tail") as run_tail:
                tidyengine.ensure_compile_db(build_dir, preset="windows-tidy")

            run_tail.assert_not_called()

    def test_new_database_builds_only_the_generated_header_target(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            build_dir = Path(temp_dir)

            def run_tail(_command, what):
                if what == "configure":
                    (build_dir / "compile_commands.json").write_text("[]", encoding="utf-8")

            with mock.patch.object(tidyengine, "_run_tail", side_effect=run_tail) as run:
                tidyengine.ensure_compile_db(build_dir, preset="windows-tidy")

            self.assertEqual(
                run.call_args_list[1].args,
                (
                    [
                        "cmake",
                        "--build",
                        str(build_dir),
                        "--target",
                        "aobus_generated_headers",
                        "--parallel",
                        str(tidyengine.os.cpu_count() or 1),
                    ],
                    "header generation build",
                ),
            )


class CompileCommandCoverageTest(unittest.TestCase):
    def test_sources_require_exact_commands_and_headers_require_safe_companions(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir) / "repo"
            build_dir = Path(temp_dir) / "build"
            native = root / "lib" / "audio" / "Native.cpp"
            native_header = root / "include" / "ao" / "audio" / "Native.h"
            shared_header = root / "include" / "ao" / "audio" / "Shared.h"
            foreign = root / "lib" / "audio" / "Foreign.cpp"
            foreign_header = root / "include" / "ao" / "audio" / "Foreign.h"
            for path in (native, native_header, shared_header, foreign, foreign_header):
                path.parent.mkdir(parents=True, exist_ok=True)
                path.touch()
            native.write_text("#include <ao/Native.h>\n#include <ao/Shared.h>\n", encoding="utf-8")
            build_dir.mkdir()
            (build_dir / "compile_commands.json").write_text(
                json.dumps([{"directory": str(build_dir), "file": str(native), "command": "clang++ -c Native.cpp"}]),
                encoding="utf-8",
            )

            plan = tidyengine.compile_command_plan(
                build_dir,
                [native, native_header, shared_header, foreign, foreign_header],
                project_root=root,
            )

            self.assertEqual(list(plan.deferred), [shared_header, foreign, foreign_header])
            self.assertEqual(
                [(target.selected, target.translation_unit) for target in plan.targets],
                [(native, native), (native_header, native)],
            )

    def test_header_only_file_not_reachable_from_native_code_is_uncovered(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir) / "repo"
            build_dir = Path(temp_dir) / "build"
            native = root / "lib" / "Native.cpp"
            unused = root / "include" / "ao" / "Unused.h"
            native.parent.mkdir(parents=True)
            unused.parent.mkdir(parents=True)
            native.touch()
            unused.touch()
            build_dir.mkdir()
            (build_dir / "compile_commands.json").write_text(
                json.dumps([{"directory": str(build_dir), "file": str(native), "command": "clang++ -c Native.cpp"}]),
                encoding="utf-8",
            )

            plan = tidyengine.compile_command_plan(build_dir, [unused], project_root=root)

            self.assertEqual(list(plan.deferred), [unused])

    def test_test_header_without_safe_companion_is_deferred(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir) / "repo"
            build_dir = Path(temp_dir) / "build"
            native = root / "test" / "unit" / "utility" / "AtomicFileTest.cpp"
            header = root / "test" / "council" / "TestSupport.h"
            for path in (native, header):
                path.parent.mkdir(parents=True, exist_ok=True)
                path.touch()
            build_dir.mkdir()
            (build_dir / "compile_commands.json").write_text(
                json.dumps([{"directory": str(build_dir), "file": str(native), "command": f"clang++ -c {native}"}]),
                encoding="utf-8",
            )

            plan = tidyengine.compile_command_plan(build_dir, [header], project_root=root)

            self.assertEqual(list(plan.deferred), [header])
            self.assertEqual(list(plan.targets), [])

    def test_explicit_lint_fixture_can_borrow_native_flags(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir) / "repo"
            build_dir = Path(temp_dir) / "build"
            native = root / "tool" / "lint" / "Check.cpp"
            fixture = root / "test" / "integration" / "lint" / "fixture" / "check" / "case.cpp"
            for path in (native, fixture):
                path.parent.mkdir(parents=True, exist_ok=True)
                path.touch()
            build_dir.mkdir()
            (build_dir / "compile_commands.json").write_text(
                json.dumps([{"directory": str(build_dir), "file": str(native), "command": f"clang++ -c {native}"}]),
                encoding="utf-8",
            )

            plan = tidyengine.compile_command_plan(build_dir, [fixture], project_root=root)

            self.assertEqual(list(plan.deferred), [])
            self.assertEqual(
                [(target.selected, target.translation_unit) for target in plan.targets],
                [(fixture.resolve(), native.resolve())],
            )

    def test_conditional_include_does_not_claim_header_coverage(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir) / "repo"
            build_dir = Path(temp_dir) / "build"
            native = root / "app" / "tui" / "AudioBackendBootstrap.cpp"
            header = root / "include" / "ao" / "audio" / "WasapiProvider.h"
            for path in (native, header):
                path.parent.mkdir(parents=True, exist_ok=True)
                path.touch()
            native.write_text(
                "#ifdef _WIN32\n#include <ao/audio/WasapiProvider.h>\n#endif\n",
                encoding="utf-8",
            )
            build_dir.mkdir()
            (build_dir / "compile_commands.json").write_text(
                json.dumps([{"directory": str(build_dir), "file": str(native), "command": "cl /c Native.cpp"}]),
                encoding="utf-8",
            )

            plan = tidyengine.compile_command_plan(build_dir, [header], project_root=root)

            self.assertEqual(list(plan.deferred), [header])
            self.assertEqual(list(plan.targets), [])

    def test_platform_suffix_implementation_covers_header_in_same_component(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir) / "repo"
            build_dir = Path(temp_dir) / "build"
            header = root / "include" / "ao" / "utility" / "AtomicFile.h"
            native = root / "lib" / "utility" / "AtomicFileWindows.cpp"
            for path in (header, native):
                path.parent.mkdir(parents=True, exist_ok=True)
                path.touch()
            build_dir.mkdir()
            (build_dir / "compile_commands.json").write_text(
                json.dumps([{"directory": str(build_dir), "file": str(native), "command": f"cl /c {native}"}]),
                encoding="utf-8",
            )

            plan = tidyengine.compile_command_plan(build_dir, [header], project_root=root)

            self.assertEqual(list(plan.deferred), [])
            self.assertEqual(
                [(target.selected, target.translation_unit) for target in plan.targets],
                [(header, native)],
            )

    def test_same_stem_translation_unit_in_another_component_does_not_cover_header(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir) / "repo"
            build_dir = Path(temp_dir) / "build"
            native = root / "app" / "windows" / "App.cpp"
            foreign = root / "app" / "linux" / "App.cpp"
            foreign_header = root / "app" / "linux" / "App.h"
            for path in (native, foreign, foreign_header):
                path.parent.mkdir(parents=True, exist_ok=True)
                path.touch()
            build_dir.mkdir()
            (build_dir / "compile_commands.json").write_text(
                json.dumps([{"directory": str(build_dir), "file": str(native), "command": "clang++ -c App.cpp"}]),
                encoding="utf-8",
            )

            plan = tidyengine.compile_command_plan(build_dir, [foreign_header], project_root=root)

            self.assertEqual(list(plan.deferred), [foreign_header])

    def test_matching_trailing_directories_across_platform_roots_do_not_cover_header(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir) / "repo"
            build_dir = Path(temp_dir) / "build"
            native = root / "app" / "uimodel" / "layout" / "document" / "LayoutDocument.cpp"
            linux_header = root / "app" / "linux-gtk" / "layout" / "document" / "LayoutDocument.h"
            for path in (native, linux_header):
                path.parent.mkdir(parents=True, exist_ok=True)
                path.touch()
            build_dir.mkdir()
            (build_dir / "compile_commands.json").write_text(
                json.dumps([{"directory": str(build_dir), "file": str(native), "command": "cl /c LayoutDocument.cpp"}]),
                encoding="utf-8",
            )

            plan = tidyengine.compile_command_plan(build_dir, [linux_header], project_root=root)

            self.assertEqual(list(plan.deferred), [linux_header])
            self.assertEqual(list(plan.targets), [])


class FilteredCompileDatabaseTest(unittest.TestCase):
    def test_removes_only_exact_driver_arguments_from_both_database_forms(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            build_dir = root / "build"
            destination = root / "filtered"
            first = root / "First.cpp"
            second = root / "Second.cpp"
            first.touch()
            second.touch()
            build_dir.mkdir()
            (build_dir / "compile_commands.json").write_text(
                json.dumps(
                    [
                        {
                            "directory": str(root),
                            "file": str(first),
                            "arguments": ["cl.exe", "/ZC:PREPROCESSOR", "/c", str(first)],
                        },
                        {
                            "directory": str(root),
                            "file": str(second),
                            "command": f"cl.exe /Zc:preprocessor /Zc:preprocessor- /c {second}",
                        },
                    ]
                ),
                encoding="utf-8",
            )

            database_dir = tidyengine.write_filtered_compile_database(
                build_dir,
                destination,
                ("/Zc:preprocessor",),
            )

            self.assertEqual(database_dir, destination)
            entries = json.loads((destination / "compile_commands.json").read_text(encoding="utf-8"))
            self.assertEqual(entries[0]["arguments"], ["cl.exe", "/c", str(first)])
            self.assertNotIn(" /Zc:preprocessor ", entries[1]["command"])
            self.assertIn("/Zc:preprocessor-", entries[1]["command"])


class HeaderCompileDatabaseTest(unittest.TestCase):
    def test_arguments_command_is_copied_with_header_as_exact_input(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            build_dir = root / "build"
            destination = root / "synthetic"
            source = root / "repo" / "lib" / "audio" / "Player.cpp"
            header = root / "repo" / "include" / "ao" / "audio" / "Player.h"
            source.parent.mkdir(parents=True)
            header.parent.mkdir(parents=True)
            source.touch()
            header.touch()
            build_dir.mkdir()
            (build_dir / "compile_commands.json").write_text(
                json.dumps(
                    [
                        {
                            "directory": str(build_dir),
                            "file": str(source),
                            "arguments": ["clang++", "/TP", "-DFEATURE=1", "-c", str(source)],
                            "output": "Player.obj",
                        }
                    ]
                ),
                encoding="utf-8",
            )
            target = tidyengine.CompileCommandTarget(header, source)

            database_dir = tidyengine.write_header_compile_database(
                build_dir,
                [target],
                destination,
                excluded_arguments=("/TP",),
            )

            self.assertEqual(database_dir, destination)
            entries = json.loads((destination / "compile_commands.json").read_text(encoding="utf-8"))
            self.assertEqual(len(entries), 1)
            self.assertEqual(entries[0]["file"], header.resolve().as_posix())
            self.assertEqual(
                entries[0]["arguments"],
                ["clang++", "-DFEATURE=1", "-c", str(header.resolve())],
            )
            self.assertNotIn("output", entries[0])

    def test_string_command_is_copied_with_quoted_header_as_exact_input(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir) / "repo with spaces"
            build_dir = Path(temp_dir) / "build"
            destination = Path(temp_dir) / "synthetic"
            source = root / "lib" / "audio" / "Player.cpp"
            header = root / "include" / "ao" / "audio" / "Player.h"
            source.parent.mkdir(parents=True)
            header.parent.mkdir(parents=True)
            source.touch()
            header.touch()
            build_dir.mkdir()
            command = f'clang++ /TP -DFEATURE=1 -c "{source}" -o Player.obj'
            (build_dir / "compile_commands.json").write_text(
                json.dumps([{"directory": str(build_dir), "file": str(source), "command": command}]),
                encoding="utf-8",
            )

            tidyengine.write_header_compile_database(
                build_dir,
                [tidyengine.CompileCommandTarget(header, source)],
                destination,
                excluded_arguments=("/TP",),
            )

            entries = json.loads((destination / "compile_commands.json").read_text(encoding="utf-8"))
            rewritten = entries[0]["command"]
            self.assertIn("-DFEATURE=1", rewritten)
            self.assertNotIn("/TP", rewritten)
            self.assertIn(f'"{header.resolve()}"', rewritten)
            self.assertNotIn(str(source), rewritten)
            self.assertEqual(entries[0]["file"], header.resolve().as_posix())

    def test_string_command_accepts_input_relative_to_command_directory(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            build_dir = root / "build"
            source = root / "repo" / "lib" / "Player.cpp"
            header = root / "repo" / "include" / "ao" / "Player.h"
            source.parent.mkdir(parents=True)
            header.parent.mkdir(parents=True)
            source.touch()
            header.touch()
            build_dir.mkdir()
            relative_source = Path(os.path.relpath(source, build_dir))
            (build_dir / "compile_commands.json").write_text(
                json.dumps(
                    [
                        {
                            "directory": str(build_dir),
                            "file": str(source),
                            "command": f'clang++ -c "{relative_source}"',
                        }
                    ]
                ),
                encoding="utf-8",
            )

            tidyengine.write_header_compile_database(
                build_dir,
                [tidyengine.CompileCommandTarget(header, source)],
                root / "synthetic",
            )

            entries = json.loads((root / "synthetic" / "compile_commands.json").read_text(encoding="utf-8"))
            self.assertIn(f'"{header.resolve()}"', entries[0]["command"])

    def test_string_command_accepts_absolute_input_when_source_and_build_are_on_different_volumes(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            build_dir = root / "build"
            source = root / "repo" / "lib" / "Player.cpp"
            header = root / "repo" / "include" / "ao" / "Player.h"
            source.parent.mkdir(parents=True)
            header.parent.mkdir(parents=True)
            source.touch()
            header.touch()
            build_dir.mkdir()
            (build_dir / "compile_commands.json").write_text(
                json.dumps(
                    [
                        {
                            "directory": "C:/local/aobus-build",
                            "file": str(source),
                            "command": f'clang++ -c "{source}"',
                        }
                    ]
                ),
                encoding="utf-8",
            )

            with mock.patch.object(tidyengine.os.path, "relpath", side_effect=ValueError("different drives")):
                tidyengine.write_header_compile_database(
                    build_dir,
                    [tidyengine.CompileCommandTarget(header, source)],
                    root / "synthetic",
                )

            entries = json.loads((root / "synthetic" / "compile_commands.json").read_text(encoding="utf-8"))
            self.assertIn(f'"{header.resolve()}"', entries[0]["command"])
            self.assertNotIn(str(source), entries[0]["command"])

    def test_missing_source_token_fails_closed_for_argument_list(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            build_dir = root / "build"
            source = root / "repo" / "lib" / "audio" / "Player.cpp"
            header = root / "repo" / "include" / "ao" / "audio" / "Player.h"
            source.parent.mkdir(parents=True)
            header.parent.mkdir(parents=True)
            source.touch()
            header.touch()
            build_dir.mkdir()
            (build_dir / "compile_commands.json").write_text(
                json.dumps(
                    [
                        {
                            "directory": str(build_dir),
                            "file": str(source),
                            "arguments": ["clang++", "-c", "Different.cpp"],
                        }
                    ]
                ),
                encoding="utf-8",
            )

            with self.assertRaises(SystemExit):
                tidyengine.write_header_compile_database(
                    build_dir,
                    [tidyengine.CompileCommandTarget(header, source)],
                    root / "synthetic",
                )

    def test_missing_source_token_fails_closed_for_string_command(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            build_dir = root / "build"
            source = root / "repo" / "lib" / "audio" / "Player.cpp"
            header = root / "repo" / "include" / "ao" / "audio" / "Player.h"
            source.parent.mkdir(parents=True)
            header.parent.mkdir(parents=True)
            source.touch()
            header.touch()
            build_dir.mkdir()
            (build_dir / "compile_commands.json").write_text(
                json.dumps(
                    [
                        {
                            "directory": str(build_dir),
                            "file": str(source),
                            "command": "clang++ -c Different.cpp",
                        }
                    ]
                ),
                encoding="utf-8",
            )

            with self.assertRaises(SystemExit):
                tidyengine.write_header_compile_database(
                    build_dir,
                    [tidyengine.CompileCommandTarget(header, source)],
                    root / "synthetic",
                )


class RunnerPortabilityTest(unittest.TestCase):
    def test_windows_absolute_file_name_does_not_leak_into_log_name(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            logs: list[Path] = []

            def runner(_: str, log: Path) -> int:
                logs.append(log)
                log.touch()
                return 0

            result = tidyengine.run_parallel(
                [r"C:\repo\lib\Wasapi.cpp"],
                1,
                Path(temp_dir),
                runner,
            )

            self.assertFalse(result.failed)
            self.assertEqual(logs[0].name, "000000.log")

    def test_compile_command_error_is_detected_in_successful_tool_output(self):
        with tempfile.NamedTemporaryFile(mode="w", encoding="utf-8", delete=False) as stream:
            stream.write("Skipping C:/repo/Missing.cpp. Compile command not found.\n")
            log = Path(stream.name)
        try:
            self.assertTrue(tidyengine.log_has_compile_command_error(log))
        finally:
            log.unlink()


if __name__ == "__main__":
    unittest.main()
