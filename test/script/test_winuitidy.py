"""Tests for extracting WinUI compile commands from MSBuild."""

import json
import tempfile
import unittest
from pathlib import Path
from unittest import mock

from ao.core import winuitidy


class WinUiCompileCommandsTest(unittest.TestCase):
    def test_expands_repository_owned_msbuild_groups(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            source_root = root / "repo"
            build_dir = root / "build"
            generator = root / "vs"
            msbuild = generator / "MSBuild" / "Current" / "Bin" / "MSBuild.exe"
            project = build_dir / "app" / "windows-winui" / "aobus-winui-lib.vcxproj"
            clang_cl = root / "llvm" / "bin" / "clang-cl.exe"
            first = source_root / "app" / "windows-winui" / "First.cpp"
            second = source_root / "app" / "windows-winui" / "detail" / "Second.cpp"
            generated = build_dir / "generated" / "XamlTypeInfo.g.cpp"
            for path in (msbuild, project, clang_cl, first, second, generated):
                path.parent.mkdir(parents=True, exist_ok=True)
                path.touch()
            (build_dir / "CMakeCache.txt").write_text(
                f"CMAKE_GENERATOR_INSTANCE:INTERNAL={generator}\n",
                encoding="utf-8",
            )
            payload = {
                "TargetResults": {
                    "GetCompileCommands": {
                        "Result": "Success",
                        "Items": [
                            {
                                "Identity": "/c /DWINUI",
                                "WorkingDirectory": str(source_root),
                                "Files": f"{first};{second};{generated}",
                            }
                        ],
                    }
                }
            }
            result = mock.Mock(returncode=0, stdout=f"MSBuild preamble\n{json.dumps(payload)}")

            with mock.patch.object(winuitidy, "PROJECT_ROOT", source_root):
                with mock.patch.object(winuitidy, "_WINUI_ROOT", source_root / "app" / "windows-winui"):
                    with mock.patch.object(winuitidy.subprocess, "run", return_value=result) as run:
                        commands = winuitidy.compile_commands(
                            build_dir,
                            clang_cl,
                            required_translation_units=(second,),
                        )

            self.assertEqual([Path(entry["file"]) for entry in commands], [first, second])
            self.assertTrue(all(str(clang_cl) in str(entry["command"]) for entry in commands))
            self.assertTrue(all("/DWINUI" in str(entry["command"]) for entry in commands))
            self.assertNotIn(str(generated), "\n".join(str(entry) for entry in commands))
            self.assertIn(str(project), run.call_args.args[0])
            self.assertIn("-getTargetResult:GetCompileCommands", run.call_args.args[0])
            self.assertIn("/p:Configuration=Release", run.call_args.args[0])

    def test_indexes_header_companions_from_the_winui_include_graph(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir) / "repo"
            winui_root = root / "app" / "windows-winui"
            source = winui_root / "layout" / "ShellBuilder.cpp"
            bridge = winui_root / "layout" / "ShellBuilder.h"
            header = winui_root / "layout" / "runtime" / "ShellLibraryAccess.h"
            pch = winui_root / "pch.h"
            for path in (source, bridge, header, pch):
                path.parent.mkdir(parents=True, exist_ok=True)
            source.write_text('#include "pch.h"\n#include "ShellBuilder.h"\n', encoding="utf-8")
            bridge.write_text('#include "layout/runtime/ShellLibraryAccess.h"\n', encoding="utf-8")
            header.write_text("#pragma once\n", encoding="utf-8")
            pch.write_text("#pragma once\n", encoding="utf-8")

            companions = winuitidy.find_header_companions(
                [{"directory": str(root), "file": str(source), "command": "clang-cl /c ShellBuilder.cpp"}],
                (header, pch),
                project_root=root,
                winui_root=winui_root,
            )

            self.assertEqual(companions, {header: source, pch: source})

    def test_unreachable_header_is_left_for_normal_coverage_handling(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir) / "repo"
            winui_root = root / "app" / "windows-winui"
            source = winui_root / "App.xaml.cpp"
            header = winui_root / "layout" / "runtime" / "Unused.h"
            source.parent.mkdir(parents=True, exist_ok=True)
            header.parent.mkdir(parents=True, exist_ok=True)
            source.write_text('#include "pch.h"\n', encoding="utf-8")
            header.write_text("#pragma once\n", encoding="utf-8")

            companions = winuitidy.find_header_companions(
                [{"file": str(source)}],
                (header,),
                project_root=root,
                winui_root=winui_root,
            )

            self.assertEqual(companions, {})

    def test_missing_required_translation_unit_fails_closed(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            source_root = root / "repo"
            build_dir = root / "build"
            generator = root / "vs"
            source = source_root / "app" / "windows-winui" / "Present.cpp"
            missing = source_root / "app" / "windows-winui" / "Missing.cpp"
            paths = (
                generator / "MSBuild" / "Current" / "Bin" / "MSBuild.exe",
                build_dir / "app" / "windows-winui" / "aobus-winui-lib.vcxproj",
                root / "llvm" / "bin" / "clang-cl.exe",
                source,
            )
            for path in paths:
                path.parent.mkdir(parents=True, exist_ok=True)
                path.touch()
            (build_dir / "CMakeCache.txt").write_text(
                f"CMAKE_GENERATOR_INSTANCE:INTERNAL={generator}\n",
                encoding="utf-8",
            )
            payload = {
                "TargetResults": {
                    "GetCompileCommands": {
                        "Result": "Success",
                        "Items": [
                            {
                                "Identity": "/c",
                                "WorkingDirectory": str(source_root),
                                "Files": str(source),
                            }
                        ],
                    }
                }
            }

            with mock.patch.object(winuitidy, "PROJECT_ROOT", source_root):
                with mock.patch.object(winuitidy, "_WINUI_ROOT", source_root / "app" / "windows-winui"):
                    with mock.patch.object(
                        winuitidy.subprocess,
                        "run",
                        return_value=mock.Mock(returncode=0, stdout=json.dumps(payload)),
                    ):
                        with self.assertRaises(SystemExit):
                            winuitidy.compile_commands(
                                build_dir,
                                paths[2],
                                required_translation_units=(missing,),
                            )

    def test_extracts_required_auxiliary_source_from_its_own_project(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            source_root = root / "repo"
            build_dir = root / "build"
            generator = root / "vs"
            msbuild = generator / "MSBuild" / "Current" / "Bin" / "MSBuild.exe"
            main_project = build_dir / "app" / "windows-winui" / "aobus-winui-lib.vcxproj"
            probe_project = build_dir / "app" / "windows-winui" / "ao_winui_localization_probe.vcxproj"
            clang_cl = root / "llvm" / "bin" / "clang-cl.exe"
            main_source = source_root / "app" / "windows-winui" / "App.xaml.cpp"
            probe_source = source_root / "test" / "helper" / "WinUiLocalizationProbe.cpp"
            for path in (msbuild, main_project, probe_project, clang_cl, main_source, probe_source):
                path.parent.mkdir(parents=True, exist_ok=True)
                path.touch()
            (build_dir / "CMakeCache.txt").write_text(
                f"CMAKE_GENERATOR_INSTANCE:INTERNAL={generator}\n",
                encoding="utf-8",
            )

            def payload(source: Path) -> mock.Mock:
                contents = {
                    "TargetResults": {
                        "GetCompileCommands": {
                            "Result": "Success",
                            "Items": [
                                {
                                    "Identity": "/c /DWINUI",
                                    "WorkingDirectory": str(source_root),
                                    "Files": str(source),
                                }
                            ],
                        }
                    }
                }
                return mock.Mock(returncode=0, stdout=json.dumps(contents))

            with mock.patch.object(winuitidy, "PROJECT_ROOT", source_root):
                with mock.patch.object(winuitidy, "_WINUI_ROOT", source_root / "app" / "windows-winui"):
                    with mock.patch.object(
                        winuitidy.subprocess,
                        "run",
                        side_effect=(payload(main_source), payload(probe_source)),
                    ) as run:
                        commands = winuitidy.compile_commands(
                            build_dir,
                            clang_cl,
                            required_translation_units=(probe_source,),
                        )
                    requires_context = winuitidy.requires_winui_compile_context(probe_source)

            self.assertEqual([Path(entry["file"]) for entry in commands], [main_source, probe_source])
            self.assertEqual(run.call_count, 2)
            self.assertIn(str(main_project), run.call_args_list[0].args[0])
            self.assertIn(str(probe_project), run.call_args_list[1].args[0])
            self.assertTrue(requires_context)
