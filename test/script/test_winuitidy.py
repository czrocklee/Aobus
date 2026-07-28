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
            project = build_dir / "app" / "windows-winui" / "aobus-winui.vcxproj"
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
            self.assertIn("-getTargetResult:GetCompileCommands", run.call_args.args[0])
            self.assertIn("/p:Configuration=Release", run.call_args.args[0])

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
                build_dir / "app" / "windows-winui" / "aobus-winui.vcxproj",
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
