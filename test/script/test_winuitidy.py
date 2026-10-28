"""Tests for extracting WinUI compile commands from MSBuild."""

import contextlib
import io
import json
import ntpath
import platform
import posixpath
import tempfile
import unittest
from pathlib import Path
from types import SimpleNamespace
from unittest import mock

from phase_support import PhaseRecorder as _PhaseRecorder

from ao.core import winuitidy


def current_fixture_projects(build_dir):
    return sorted((build_dir / "app/windows-winui").glob("*.vcxproj"))


class WinUiCompileCommandsTest(unittest.TestCase):
    def setUp(self) -> None:
        captured_host = platform.system()
        # platform.system() may invoke Windows "ver" through subprocess.run.
        self.enterContext(
            mock.patch.object(winuitidy.workspace_cache, "platform", SimpleNamespace(system=lambda: captured_host))
        )
        self.verify_projects = self.enterContext(mock.patch.object(winuitidy, "verify_auxiliary_source_projects"))
        self.enterContext(
            mock.patch.object(winuitidy, "_configured_winui_projects", side_effect=current_fixture_projects)
        )

    def test_expands_repository_owned_msbuild_groups(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir).resolve()
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

            self.verify_projects.assert_called_once()
            self.assertEqual(self.verify_projects.call_args.args, (build_dir,))
            self.assertEqual(set(self.verify_projects.call_args.kwargs["evaluated_projects"]), {project})
            self.assertEqual(run.call_count, 1)
            self.assertIn("-getItem:ClCompile", run.call_args.args[0])
            self.assertEqual([Path(entry["file"]) for entry in commands], [first, second])
            self.assertTrue(all(str(clang_cl) in str(entry["command"]) for entry in commands))
            self.assertTrue(all("/DWINUI" in str(entry["command"]) for entry in commands))
            self.assertNotIn(str(generated), "\n".join(str(entry) for entry in commands))
            self.assertIn(str(project), run.call_args.args[0])
            self.assertIn("-getTargetResult:GetCompileCommands", run.call_args.args[0])
            self.assertIn("/p:Configuration=Debug", run.call_args.args[0])

    def test_fixed_view_commands_keep_compiler_paths_and_match_physical_owners(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary).resolve()
            source = root / "repo"
            view = root / "fixed-source"
            build = root / "build"
            generator = root / "vs"
            main = source / "app/windows-winui/App.cpp"
            header = main.with_suffix(".h")
            auxiliary = source / "test/helper/WinUiLocalizationProbe.cpp"
            generated = build / "generated/XamlTypeInfo.g.cpp"
            clang = root / "llvm/clang-cl.exe"
            projects = (
                build / "app/windows-winui/aobus-winui-lib.vcxproj",
                build / "app/windows-winui/ao_winui_localization_probe.vcxproj",
            )
            for path in (
                main,
                header,
                auxiliary,
                generated,
                clang,
                *projects,
                generator / "MSBuild/Current/Bin/MSBuild.exe",
            ):
                path.parent.mkdir(parents=True, exist_ok=True)
                path.touch()
            main.write_text('#include "App.h"\n')
            (build / "CMakeCache.txt").write_text(f"CMAKE_GENERATOR_INSTANCE:INTERNAL={generator}\n")
            viewed_main = view / main.relative_to(source)
            viewed_auxiliary = view / auxiliary.relative_to(source)

            def physical(path, **kwargs):
                try:
                    return source / path.relative_to(view)
                except ValueError:
                    return path

            def result(files):
                payload = {
                    "TargetResults": {
                        "GetCompileCommands": {
                            "Result": "Success",
                            "Items": [{"Identity": "/c /DWINUI", "WorkingDirectory": str(build), "Files": files}],
                        }
                    }
                }
                return mock.Mock(returncode=0, stdout=json.dumps(payload))

            with (
                mock.patch.object(winuitidy, "PROJECT_ROOT", source),
                mock.patch.object(winuitidy, "_WINUI_ROOT", main.parent),
                mock.patch("ao.core.workspace_cache.canonical_portal_path", side_effect=physical),
                mock.patch.object(
                    winuitidy.subprocess,
                    "run",
                    side_effect=(
                        result(f"{viewed_main};{generated}"),
                        result(str(viewed_auxiliary)),
                    ),
                ),
            ):
                commands = winuitidy.compile_commands(build, clang, required_translation_units=(main, auxiliary))
                companions = winuitidy.find_header_companions(
                    commands, (header,), project_root=source, winui_root=main.parent
                )
            self.assertEqual([Path(command["file"]) for command in commands], [viewed_main, viewed_auxiliary])
            self.assertIn(str(viewed_main), commands[0]["command"])
            self.assertEqual(Path(commands[0]["directory"]), build)
            self.assertEqual(companions, {header: main})

    def test_indexes_header_companions_from_the_winui_include_graph(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir).resolve() / "repo"
            winui_root = root / "app" / "windows-winui"
            source = winui_root / "layout" / "ShellBuilder.cpp"
            bridge = winui_root / "layout" / "ShellBuilder.h"
            header = winui_root / "include" / "ao" / "winui" / "layout" / "ExactFactoryCapture.h"
            pch = winui_root / "pch.h"
            for path in (source, bridge, header, pch):
                path.parent.mkdir(parents=True, exist_ok=True)
            source.write_text('#include "pch.h"\n#include "ShellBuilder.h"\n', encoding="utf-8")
            bridge.write_text("#include <ao/winui/layout/ExactFactoryCapture.h>\n", encoding="utf-8")
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
            root = Path(temp_dir).resolve() / "repo"
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
            root = Path(temp_dir).resolve()
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

    def test_declared_auxiliary_support_header_uses_its_native_translation_unit(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory).resolve()
            source = root / "test/winui/probe/WinUiSilentAudio.cpp"
            header = source.with_suffix(".h")
            source.parent.mkdir(parents=True)
            source.write_text('#include "WinUiSilentAudio.h"\n', encoding="utf-8")
            header.touch()
            with (
                mock.patch.object(winuitidy, "PROJECT_ROOT", root),
                mock.patch.object(winuitidy, "_WINUI_ROOT", root / "app/windows-winui"),
            ):
                self.assertTrue(winuitidy.requires_winui_compile_context(header))
                companions = winuitidy.find_header_companions(
                    [{"file": str(source), "directory": str(root)}],
                    (header,),
                    project_root=root,
                    winui_root=root / "app/windows-winui",
                )
            self.assertEqual(companions, {header: source})

    def test_extracts_required_auxiliary_sources_from_their_own_projects(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir).resolve()
            source_root = root / "repo"
            build_dir = root / "build"
            generator = root / "vs"
            msbuild = generator / "MSBuild" / "Current" / "Bin" / "MSBuild.exe"
            main_project = build_dir / "app" / "windows-winui" / "aobus-winui-lib.vcxproj"
            probe_project = build_dir / "app" / "windows-winui" / "ao_winui_localization_probe.vcxproj"
            clang_cl = root / "llvm" / "bin" / "clang-cl.exe"
            main_source = source_root / "app" / "windows-winui" / "App.xaml.cpp"
            probe_source = source_root / "test" / "helper" / "WinUiLocalizationProbe.cpp"
            soul_project = build_dir / "app" / "windows-winui" / "ao_winui_soul_probe.vcxproj"
            soul_source = source_root / "test" / "winui" / "probe" / "WinUiSoulProbe.cpp"
            item_project = build_dir / "app" / "windows-winui" / "ao_winui_track_item_probe.vcxproj"
            item_source = source_root / "test" / "helper" / "WinUiTrackItemProbe.cpp"
            table_project = build_dir / "app" / "windows-winui" / "ao_winui_track_table_probe.vcxproj"
            table_source = source_root / "test" / "winui" / "probe" / "WinUiTrackTableProbe.cpp"
            credits_project = build_dir / "app" / "windows-winui" / "ao_winui_credits_probe.vcxproj"
            credits_source = source_root / "test" / "winui" / "probe" / "WinUiCreditsProbe.cpp"
            quality_project = build_dir / "app" / "windows-winui" / "ao_winui_quality_tooltip_probe.vcxproj"
            quality_source = source_root / "test" / "winui" / "probe" / "WinUiQualityToolTipProbe.cpp"
            status_project = build_dir / "app" / "windows-winui" / "ao_winui_activity_status_probe.vcxproj"
            status_source = source_root / "test" / "winui" / "probe" / "WinUiActivityStatusProbe.cpp"
            automation_project = build_dir / "app" / "windows-winui" / "ao_winui_automation_probe.vcxproj"
            automation_source = source_root / "test" / "winui" / "probe" / "WinUiAutomationProbe.cpp"
            for path in (
                msbuild,
                main_project,
                probe_project,
                soul_project,
                clang_cl,
                main_source,
                probe_source,
                soul_source,
                item_project,
                item_source,
                table_project,
                table_source,
                credits_project,
                credits_source,
                quality_project,
                quality_source,
                status_project,
                status_source,
                automation_project,
                automation_source,
            ):
                path.parent.mkdir(parents=True, exist_ok=True)
                path.touch()
            (build_dir / "CMakeCache.txt").write_text(
                f"CMAKE_GENERATOR_INSTANCE:INTERNAL={generator}\n",
                encoding="utf-8",
            )

            def payload(source: Path, identity: str = "/c /DWINUI") -> mock.Mock:
                contents = {
                    "TargetResults": {
                        "GetCompileCommands": {
                            "Result": "Success",
                            "Items": [
                                {
                                    "Identity": identity,
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
                        side_effect=(
                            payload(main_source),
                            payload(status_source, "/c /DACTIVITY_STATUS /Istatus-headers"),
                            payload(automation_source, "/c /DAUTOMATION /Iautomation-headers"),
                            payload(credits_source, "/c /DCREDITS_PROBE /Icredits-headers"),
                            payload(probe_source),
                            payload(quality_source, "/c /DQUALITY_PROBE /Iquality-headers"),
                            payload(soul_source),
                            payload(item_source, "/c /DTRACK_ITEM /Iitem-headers"),
                            payload(table_source, "/c /DTRACK_TABLE /Itable-headers"),
                        ),
                    ) as run:
                        commands = winuitidy.compile_commands(
                            build_dir,
                            clang_cl,
                            required_translation_units=(
                                probe_source,
                                soul_source,
                                item_source,
                                table_source,
                                credits_source,
                                quality_source,
                                status_source,
                                automation_source,
                            ),
                        )
                    requires_context = [
                        winuitidy.requires_winui_compile_context(path)
                        for path in (
                            probe_source,
                            soul_source,
                            item_source,
                            table_source,
                            credits_source,
                            quality_source,
                            status_source,
                            automation_source,
                        )
                    ]

            self.assertEqual(
                [Path(entry["file"]) for entry in commands],
                [
                    main_source,
                    status_source,
                    automation_source,
                    credits_source,
                    probe_source,
                    quality_source,
                    soul_source,
                    item_source,
                    table_source,
                ],
            )
            self.assertEqual(run.call_count, 9)
            self.assertIn(str(main_project), run.call_args_list[0].args[0])
            self.assertIn(str(status_project), run.call_args_list[1].args[0])
            self.assertIn(str(automation_project), run.call_args_list[2].args[0])
            self.assertIn(str(credits_project), run.call_args_list[3].args[0])
            self.assertIn(str(probe_project), run.call_args_list[4].args[0])
            self.assertIn(str(quality_project), run.call_args_list[5].args[0])
            self.assertIn(str(soul_project), run.call_args_list[6].args[0])
            self.assertIn(str(item_project), run.call_args_list[7].args[0])
            self.assertIn(str(table_project), run.call_args_list[8].args[0])
            self.assertIn("/DACTIVITY_STATUS /Istatus-headers", commands[1]["command"])
            self.assertIn("/DAUTOMATION /Iautomation-headers", commands[2]["command"])
            self.assertEqual(
                commands[3]["command"],
                f'"{clang_cl}" /c /DCREDITS_PROBE /Icredits-headers "{credits_source}"',
            )
            self.assertEqual(
                commands[5]["command"],
                f'"{clang_cl}" /c /DQUALITY_PROBE /Iquality-headers "{quality_source}"',
            )
            self.assertIn("/DTRACK_ITEM /Iitem-headers", commands[7]["command"])
            self.assertIn("/DTRACK_TABLE /Itable-headers", commands[8]["command"])
            self.assertEqual(requires_context, [True, True, True, True, True, True, True, True])


class WinUiCompileCommandsPhaseTest(unittest.TestCase):
    def setUp(self) -> None:
        captured_host = platform.system()
        self.enterContext(
            mock.patch.object(winuitidy.workspace_cache, "platform", SimpleNamespace(system=lambda: captured_host))
        )
        self.enterContext(mock.patch.object(winuitidy, "verify_auxiliary_source_projects"))
        self.enterContext(
            mock.patch.object(winuitidy, "_configured_winui_projects", side_effect=current_fixture_projects)
        )

    def _query_fixture(self, temp_dir: str) -> tuple:
        root = Path(temp_dir).resolve()
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

        return root, source_root, build_dir, clang_cl, main_source, probe_source, payload

    def test_case_variant_selection_queries_every_declared_native_project_and_preserves_compiler_paths(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            _, source, build, clang, main, _, payload = self._query_fixture(temp_dir)
            compiler_main = source / "APP/WINDOWS-WINUI/App.xaml.cpp"
            compiler_main.parent.mkdir(parents=True, exist_ok=True)
            compiler_main.touch()
            by_project = {}
            required = []
            for relative, project in winuitidy._AUXILIARY_SOURCE_PROJECTS.items():
                compiler_source = source / relative
                compiler_source.parent.mkdir(parents=True, exist_ok=True)
                compiler_source.touch()
                required.append(source / relative.upper())
                by_project.setdefault(build / project, []).append(compiler_source)
                (build / project).touch()
            unknown = source / "TEST/WINUI/PROBE/Unknown.cpp"
            unknown.parent.mkdir(parents=True, exist_ok=True)
            unknown.touch()
            ordered_projects = sorted(by_project, key=lambda path: str(path).lower())
            results = [payload(compiler_main)]
            for project in ordered_projects:
                results.append(payload(";".join(map(str, [*by_project[project], unknown, main]))))
            with (
                mock.patch.object(winuitidy, "PROJECT_ROOT", source),
                mock.patch.object(winuitidy, "_WINUI_ROOT", main.parent),
                mock.patch.object(winuitidy, "os", SimpleNamespace(path=ntpath)),
                mock.patch.object(
                    winuitidy,
                    "_AUXILIARY_SOURCE_PROJECTS",
                    {ntpath.normcase(k).replace("\\", "/"): v for k, v in winuitidy._AUXILIARY_SOURCE_PROJECTS.items()},
                ),
                mock.patch.object(winuitidy.subprocess, "run", side_effect=results) as run,
            ):
                commands = winuitidy.compile_commands(build, clang, required_translation_units=tuple(required))
            expected = [compiler_main, *(path for project in ordered_projects for path in by_project[project])]
            self.assertEqual([Path(command["file"]) for command in commands], expected)
            self.assertEqual([Path(call.args[0][1]) for call in run.call_args_list[1:]], ordered_projects)
            for command, path in zip(commands, expected, strict=True):
                self.assertTrue(command["command"].endswith(f'"{path}"'))

    def test_posix_case_variant_and_unknown_auxiliary_requests_fail_without_querying_extra_projects(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            _, source, build, clang, main, _probe, payload = self._query_fixture(temp_dir)
            for path_module, required in (
                (posixpath, source / "TEST/HELPER/WINUILOCALIZATIONPROBE.CPP"),
                (ntpath, source / "test/helper/Unknown.cpp"),
                (ntpath, source.parent / "alias/test/helper/WinUiLocalizationProbe.cpp"),
            ):
                with (
                    self.subTest(required=required, platform=path_module.__name__),
                    mock.patch.object(winuitidy, "PROJECT_ROOT", source),
                    mock.patch.object(winuitidy, "_WINUI_ROOT", main.parent),
                    mock.patch.object(winuitidy, "os", SimpleNamespace(path=path_module)),
                    mock.patch.object(winuitidy.subprocess, "run", return_value=payload(main)) as run,
                    contextlib.redirect_stderr(io.StringIO()),
                    self.assertRaises(SystemExit),
                ):
                    winuitidy.compile_commands(build, clang, required_translation_units=(required,))
                self.assertEqual(run.call_count, 2)
                self.assertIn("-getItem:ClCompile", run.call_args_list[1].args[0])
                self.assertNotIn("-getTargetResult:GetCompileCommands", run.call_args_list[1].args[0])

    def test_context_admission_uses_platform_case_rules_without_admitting_unknowns_or_aliases(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            source = Path(temp_dir).resolve() / "repo"
            with (
                mock.patch.object(winuitidy, "PROJECT_ROOT", source),
                mock.patch.object(winuitidy, "_WINUI_ROOT", source / "app/windows-winui"),
            ):
                for path_module, variant_admitted in ((ntpath, True), (posixpath, False)):
                    with (
                        mock.patch.object(winuitidy, "os", SimpleNamespace(path=path_module)),
                        mock.patch.object(
                            winuitidy,
                            "_AUXILIARY_SOURCE_PROJECTS",
                            {
                                path_module.normcase(k).replace("\\", "/"): v
                                for k, v in winuitidy._AUXILIARY_SOURCE_PROJECTS.items()
                            },
                        ),
                    ):
                        for relative in winuitidy._AUXILIARY_SOURCE_PROJECTS:
                            for spelling in (relative, str(Path(relative).with_suffix(".h"))):
                                self.assertTrue(winuitidy.requires_winui_compile_context(source / spelling))
                                self.assertEqual(
                                    winuitidy.requires_winui_compile_context(source / spelling.upper()),
                                    variant_admitted,
                                )
                        self.assertEqual(
                            winuitidy.requires_winui_compile_context(source / "APP/WINDOWS-WINUI/Actual.cpp"),
                            variant_admitted,
                        )
                        self.assertEqual(
                            winuitidy.requires_winui_compile_context(
                                source.parent / "REPO/APP/WINDOWS-WINUI/Actual.cpp"
                            ),
                            variant_admitted,
                        )
                        for rejected in (
                            source / "test/helper/Unknown.cpp",
                            source / "te\u017ft/helper/WinUiLocalizationProbe.cpp",
                            source / "app/windows-winui-other/Unknown.cpp",
                            source.parent / "alias/test/helper/WinUiLocalizationProbe.cpp",
                            source.parent / "alias/app/windows-winui/App.cpp",
                        ):
                            self.assertFalse(winuitidy.requires_winui_compile_context(rejected))

    def test_context_admission_normalizes_authoritative_roots_and_candidate_once(self):
        root = winuitidy.PROJECT_ROOT
        real = winuitidy.workspace_cache.canonical_portal_path
        candidates = (
            root / "app/windows-winui/App.cpp",
            root / "src/Ordinary.cpp",
            root / "test/helper/Unknown.cpp",
            root / "test/helper/WinUiLocalizationProbe.cpp",
            root / "test/helper/WinUiLocalizationProbe.h",
            root.parent / "foreign/test/helper/WinUiLocalizationProbe.cpp",
        )
        for candidate in candidates:
            with (
                self.subTest(candidate=candidate),
                mock.patch.object(winuitidy.workspace_cache, "canonical_portal_path", wraps=real) as canonical,
            ):
                winuitidy.requires_winui_compile_context(candidate)
            self.assertEqual(
                [call.args[0] for call in canonical.call_args_list], [root, winuitidy._WINUI_ROOT, candidate]
            )

    def test_msbuild_query_is_one_aggregated_record(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            (
                _root,
                source_root,
                build_dir,
                clang_cl,
                main_source,
                probe_source,
                payload,
            ) = self._query_fixture(temp_dir)
            recorder = _PhaseRecorder()

            with (
                mock.patch.object(winuitidy, "PROJECT_ROOT", source_root),
                mock.patch.object(winuitidy, "_WINUI_ROOT", source_root / "app" / "windows-winui"),
                mock.patch.object(winuitidy, "phases", recorder),
                mock.patch.object(
                    winuitidy.subprocess,
                    "run",
                    side_effect=(payload(main_source), payload(probe_source)),
                ),
            ):
                commands = winuitidy.compile_commands(
                    build_dir,
                    clang_cl,
                    required_translation_units=(probe_source,),
                )

            self.assertEqual([Path(entry["file"]) for entry in commands], [main_source, probe_source])
            self.assertEqual(
                recorder.records,
                [{"name": "compile-commands", "tree": "winui", "jobs": 1, "count": 2, "status": 0}],
            )

    def test_msbuild_query_records_the_actual_failure_status(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            (
                _root,
                source_root,
                build_dir,
                clang_cl,
                _main_source,
                _probe_source,
                _payload,
            ) = self._query_fixture(temp_dir)
            recorder = _PhaseRecorder()
            failure = mock.Mock(returncode=3, stdout="MSBuild failed")

            with (
                mock.patch.object(winuitidy, "PROJECT_ROOT", source_root),
                mock.patch.object(winuitidy, "_WINUI_ROOT", source_root / "app" / "windows-winui"),
                mock.patch.object(winuitidy, "phases", recorder),
                mock.patch.object(winuitidy.subprocess, "run", return_value=failure),
            ):
                with self.assertRaises(SystemExit):
                    winuitidy.compile_commands(build_dir, clang_cl)

            self.assertEqual(
                recorder.records,
                [{"name": "compile-commands", "tree": "winui", "jobs": 1, "count": 2, "status": 3}],
            )
