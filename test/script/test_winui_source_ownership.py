"""Reconcile Debug WinUI admission with the current CMake solution graph."""

import contextlib
import io
import json
import ntpath
import platform
import posixpath
import tempfile
import threading
import unittest
import xml.etree.ElementTree as ET
from pathlib import Path
from types import SimpleNamespace
from unittest import mock

from ao.core import winuitidy


class WinUiSourceOwnershipTest(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        root = Path(temporary.name).resolve()
        self.source = root / "source"
        self.build = root / "build"
        self.mapping = {
            winuitidy.os.path.normcase("test/winui/probe/Example.cpp").replace(
                "\\", "/"
            ): "app/windows-winui/example.vcxproj"
        }
        self.enterContext(mock.patch.object(winuitidy, "PROJECT_ROOT", self.source))
        self.enterContext(mock.patch.object(winuitidy, "_AUXILIARY_SOURCE_PROJECTS", self.mapping))
        host = platform.system()
        self.enterContext(
            mock.patch.object(winuitidy.workspace_cache, "platform", SimpleNamespace(system=lambda: host))
        )
        self.items = {}
        self.graph = ET.Element("Solution")
        self.project("example", ["test/winui/probe/Example.cpp"])
        self.generator = root / "vs"
        msbuild = self.generator / "MSBuild/Current/Bin/MSBuild.exe"
        msbuild.parent.mkdir(parents=True)
        msbuild.touch()
        (self.build / "CMakeCache.txt").write_text(
            f"CMAKE_PROJECT_NAME:STATIC=Ownership\nCMAKE_GENERATOR_INSTANCE:INTERNAL={self.generator}\n"
            "CMAKE_GENERATOR:INTERNAL=Visual Studio 18 2026\n",
            encoding="utf-8",
        )
        self.run = self.enterContext(mock.patch.object(winuitidy.subprocess, "run", side_effect=self.query))

    def project(self, name, sources, *, current=True):
        path = self.build / f"app/windows-winui/{name}.vcxproj"
        path.parent.mkdir(parents=True, exist_ok=True)
        namespace = "http://schemas.microsoft.com/developer/msbuild/2003"
        project = ET.Element(f"{{{namespace}}}Project")
        group = ET.SubElement(project, f"{{{namespace}}}ItemGroup")
        for source in sources:
            ET.SubElement(group, f"{{{namespace}}}ClCompile", Include=str(self.source / source))
        ET.ElementTree(project).write(path, encoding="utf-8")
        self.items[path] = [{"Identity": str(self.source / source)} for source in sources]
        if current:
            ET.SubElement(self.graph, "Project", Path=f"app/windows-winui/{name}.vcxproj")
        ET.ElementTree(self.graph).write(self.build / "Ownership.slnx", encoding="utf-8")
        return path

    def query(self, command, **kwargs):
        self.assertIn("-getItem:ClCompile", command)
        self.assertIn("/p:Configuration=Debug", command)
        self.assertIn("/p:Platform=x64", command)
        self.assertNotIn("-getTargetResult:GetCompileCommands", command)
        return mock.Mock(returncode=0, stdout=json.dumps({"Items": {"ClCompile": self.items[Path(command[1])]}}))

    def snapshot(self, build=None):
        build = self.build if build is None else build
        evaluated = {}
        for project in winuitidy._configured_winui_projects(build):
            result = winuitidy._query_project(winuitidy._msbuild_path(build), project, commands=False)
            if result.returncode:
                raise winuitidy.die(f"query failed (exit {result.returncode}): {result.stdout}")
            evaluated[project] = winuitidy._parse_msbuild_json(result.stdout)
        return evaluated

    def verify(self, build=None):
        build = self.build if build is None else build
        errors = io.StringIO()
        with contextlib.redirect_stderr(errors):
            winuitidy.verify_auxiliary_source_projects(build, evaluated_projects=self.snapshot(build))
        return errors.getvalue()

    def assert_rejected(self, message):
        with contextlib.redirect_stderr(io.StringIO()) as errors, self.assertRaises(SystemExit):
            winuitidy.verify_auxiliary_source_projects(self.build, evaluated_projects=self.snapshot())
        self.assertIn(message, errors.getvalue())

    def generator_version(self, value):
        (self.build / "CMakeCache.txt").write_text(
            f"CMAKE_PROJECT_NAME:STATIC=Ownership\nCMAKE_GENERATOR_INSTANCE:INTERNAL={self.generator}\n"
            + (f"CMAKE_GENERATOR:INTERNAL={value}\n" if value is not None else ""),
            encoding="utf-8",
        )

    def test_selected_project_is_a_real_owner_even_when_another_probe_shares_the_source(self):
        self.project("other", ["test/winui/probe/Example.cpp", "app/windows-winui/Production.cpp"])
        self.assertEqual(self.verify(), "")
        self.assertEqual(self.run.call_count, 2)

    def test_new_unmapped_current_test_source_fails_closed(self):
        self.project("new_probe", ["test/helper/NewProbe.cpp"])
        self.assert_rejected(winuitidy.os.path.normcase("test/helper/NewProbe.cpp").replace("\\", "/"))

    def test_obsolete_project_outside_current_graph_is_ignored_without_deletion(self):
        obsolete = self.project("removed", ["test/helper/Removed.cpp"], current=False)
        self.assertEqual(self.verify(), "")
        self.assertTrue(obsolete.is_file())
        self.assertEqual(self.run.call_count, 1)

    def test_unmapped_project_in_nested_solution_folder_is_still_inventoried(self):
        path = self.project("new_probe", ["test/helper/NewProbe.cpp"], current=False)
        folder = ET.SubElement(self.graph, "Folder", Name="/Probes/")
        ET.SubElement(folder, "Project", Path=str(path.relative_to(self.build)))
        ET.ElementTree(self.graph).write(self.build / "Ownership.slnx", encoding="utf-8")
        self.assert_rejected("new_probe.vcxproj")

    def test_legacy_solution_uses_only_listed_projects_not_recursive_files(self):
        self.generator_version("Visual Studio 17 2022")
        (self.build / "Ownership.slnx").unlink()
        (self.build / "Ownership.sln").write_text(
            "Microsoft Visual Studio Solution File, Format Version 12.00\n"
            'Project("{TYPE}") = "example", "app\\windows-winui\\example.vcxproj", "{ID}"\nEndProject\n',
            encoding="utf-8",
        )
        obsolete = self.project("removed", ["test/helper/Removed.cpp"], current=False)
        (self.build / "Ownership.slnx").unlink()
        self.assertEqual(self.verify(), "")
        self.assertTrue(obsolete.exists())

    def test_selected_debug_excluded_owner_fails_even_with_an_active_alternative(self):
        project = self.build / "app/windows-winui/example.vcxproj"
        self.items[project][0]["ExcludedFromBuild"] = "true"
        self.project("alternative", ["test/winui/probe/Example.cpp"])
        self.assert_rejected("selected='app/windows-winui/example.vcxproj'")

    def test_debug_excluded_unmapped_items_do_not_require_admission(self):
        project = self.project("release_only", ["test/helper/ReleaseOnly.cpp"])
        self.items[project][0]["ExcludedFromBuild"] = "TRUE"
        self.assertEqual(self.verify(), "")

    def test_false_exclusion_still_requires_admission(self):
        project = self.project("new_probe", ["test/helper/NewProbe.cpp"])
        self.items[project][0]["ExcludedFromBuild"] = "false"
        self.assert_rejected("new_probe.vcxproj")

    def test_stale_source_or_wrong_selected_project_fails_closed(self):
        for mapping in (
            {"test/winui/probe/Removed.cpp": "app/windows-winui/example.vcxproj"},
            {"test/winui/probe/Example.cpp": "app/windows-winui/wrong.vcxproj"},
        ):
            normalized = {
                winuitidy.os.path.normcase(source).replace("\\", "/"): project for source, project in mapping.items()
            }
            with self.subTest(mapping=mapping), mock.patch.object(winuitidy, "_AUXILIARY_SOURCE_PROJECTS", normalized):
                self.assert_rejected("differs from configured Debug|x64 ownership")

    def test_missing_or_malformed_current_graph_fails_with_regeneration_hint(self):
        solution = self.build / "Ownership.slnx"
        for content in (None, "<invalid", "<Solution />"):
            with self.subTest(content=content):
                if content is None:
                    solution.unlink()
                else:
                    solution.write_text(content, encoding="utf-8")
                self.assert_rejected("regenerate")
        self.assertEqual(self.run.call_count, 0)

    def test_generator_selects_current_format_despite_a_retained_alternate(self):
        (self.build / "Ownership.sln").write_text("retained old solution", encoding="utf-8")
        self.assertEqual(self.verify(), "")
        self.assertEqual(self.run.call_count, 1)

    def test_missing_current_format_never_falls_back_to_retained_alternate(self):
        (self.build / "Ownership.slnx").rename(self.build / "Ownership.sln")
        self.assert_rejected("Ownership.slnx")
        self.assertEqual(self.run.call_count, 0)

    def test_missing_unknown_and_non_vs_generators_fail_readably(self):
        for generator in (None, "", "Ninja", "Visual Studio 19 2029", "Visual Studio 18 invalid"):
            with self.subTest(generator=generator):
                self.generator_version(generator)
                self.assert_rejected("CMAKE_GENERATOR")
        self.assertEqual(self.run.call_count, 0)

    def test_legacy_generators_accept_kitware_bom_and_leading_linebreaks(self):
        solution = self.build / "Ownership.sln"
        for generator in (
            "Visual Studio 14 2015",
            "Visual Studio 15 2017",
            "Visual Studio 16 2019",
            "Visual Studio 17 2022",
        ):
            self.generator_version(generator)
            for prefix in ("", "\ufeff", "\ufeff\n", "\ufeff\r\n", "\ufeff\n\r\n"):
                with self.subTest(generator=generator, prefix=repr(prefix)):
                    solution.write_text(
                        prefix + "Microsoft Visual Studio Solution File, Format Version 12.00\n"
                        'Project("{TYPE}") = "example", "app\\windows-winui\\example.vcxproj", "{ID}"\nEndProject\n',
                        encoding="utf-8",
                    )
                    self.assertEqual(self.verify(), "")
        solution.write_text("not a solution\nMicrosoft Visual Studio Solution File,", encoding="utf-8")
        self.assert_rejected("solution header")
        solution.unlink()
        self.assert_rejected("Ownership.sln")

    def test_relative_and_dotdot_build_roots_match_absolute_ownership(self):
        with contextlib.chdir(self.build.parent):
            self.assertEqual(self.verify(Path("build")), "")
            self.assertEqual(self.verify(self.build / "../build"), "")

    def test_snapshot_validator_performs_no_queries_or_solution_reads(self):
        snapshot = {project: {"Items": {"ClCompile": items}} for project, items in self.items.items()}
        with (
            mock.patch.object(winuitidy, "_configured_winui_projects", side_effect=AssertionError("solution read")),
            mock.patch.object(winuitidy.subprocess, "run", side_effect=AssertionError("query")),
        ):
            winuitidy.verify_auxiliary_source_projects(self.build, evaluated_projects=snapshot)
            with self.assertRaises(TypeError):
                winuitidy.verify_auxiliary_source_projects(self.build)

    def test_evaluated_imported_source_absent_from_raw_project_still_requires_admission(self):
        project = self.build / "app/windows-winui/example.vcxproj"
        self.items[project].append({"Identity": str(self.source / "test/helper/Imported.cpp")})
        self.assert_rejected(winuitidy.os.path.normcase("Imported.cpp"))

    def test_selected_queries_reuse_evaluated_items_and_leave_other_projects_inventory_only(self):
        main = self.project("aobus-winui-lib", ["app/windows-winui/Main.cpp"])
        other = self.project("other", ["test/winui/probe/Example.cpp"])
        selected = self.build / "app/windows-winui/example.vcxproj"
        source = self.source / "test/winui/probe/Example.cpp"
        for path in (source, self.source / "app/windows-winui/Main.cpp"):
            path.parent.mkdir(parents=True, exist_ok=True)
            path.touch()
        clang = self.source.parent / "clang-cl.exe"
        clang.touch()

        def query(command, **kwargs):
            project = Path(command[1])
            payload = {"Items": {"ClCompile": self.items[project]}}
            if "-getTargetResult:GetCompileCommands" in command:
                payload["TargetResults"] = {
                    "GetCompileCommands": {
                        "Result": "Success",
                        "Items": [
                            {
                                "Identity": "/c /DPROJECT_FLAGS",
                                "WorkingDirectory": str(self.source),
                                "Files": self.items[project][0]["Identity"],
                            }
                        ],
                    }
                }
            return mock.Mock(returncode=0, stdout=json.dumps(payload))

        with (
            mock.patch.object(winuitidy, "_WINUI_ROOT", self.source / "app/windows-winui"),
            mock.patch.object(winuitidy.subprocess, "run", side_effect=query) as run,
        ):
            commands = winuitidy.compile_commands(self.build, clang, required_translation_units=(source,))
        self.assertEqual(
            [Path(command["file"]) for command in commands], [self.source / "app/windows-winui/Main.cpp", source]
        )
        self.assertEqual([Path(call.args[0][1]) for call in run.call_args_list], [main, selected, other])
        self.assertTrue(all("-getItem:ClCompile" in call.args[0] for call in run.call_args_list))
        self.assertEqual(sum("-getTargetResult:GetCompileCommands" in call.args[0] for call in run.call_args_list), 2)

    def compile_fixture(self):
        self.project("aobus-winui-lib", ["app/windows-winui/Main.cpp"])
        source = self.source / "test/winui/probe/Example.cpp"
        for path in (source, self.source / "app/windows-winui/Main.cpp"):
            path.parent.mkdir(parents=True, exist_ok=True)
            path.touch()
        clang = self.source.parent / "clang-cl.exe"
        clang.touch()
        self.enterContext(mock.patch.object(winuitidy, "_WINUI_ROOT", self.source / "app/windows-winui"))
        return source, clang

    def compile_payload(self, project, *, commands):
        payload = {"Items": {"ClCompile": self.items[project]}}
        if commands:
            payload["TargetResults"] = {
                "GetCompileCommands": {
                    "Result": "Success",
                    "Items": [
                        {
                            "Identity": "/c /DFIXTURE",
                            "WorkingDirectory": str(self.source),
                            "Files": self.items[project][0]["Identity"],
                        }
                    ],
                }
            }
        return mock.Mock(returncode=0, stdout=json.dumps(payload))

    def test_extraction_normalizes_relative_dotdot_and_metadata_only_build_roots(self):
        source, clang = self.compile_fixture()
        real_canonical = winuitidy.workspace_cache.canonical_portal_path
        environment = {
            "AOBUS_SHARED_WORKSPACES_EFFECTIVE": "1",
            "AOBUS_WINDOWS_BUILD_VIEW": "B:/",
            "AOBUS_WINDOWS_BUILD_PHYSICAL_ROOT": str(self.build.parent),
        }

        def canonical(path, **kwargs):
            return real_canonical(path, environ=environment, system="Windows", **kwargs)

        with (
            contextlib.chdir(self.build.parent),
            mock.patch.object(winuitidy.workspace_cache, "canonical_portal_path", side_effect=canonical),
            mock.patch.object(
                winuitidy,
                "_query_project",
                side_effect=lambda msbuild, project, **kw: self.compile_payload(project, **kw),
            ),
        ):
            expected = winuitidy.compile_commands(self.build, clang, required_translation_units=(source,))
            for build in (Path("build"), self.build / "../build", Path("B:/build")):
                with self.subTest(build=build):
                    self.assertEqual(
                        winuitidy.compile_commands(build, clang, required_translation_units=(source,)), expected
                    )

    def test_parallel_queries_overlap_with_caller_width_preserving_order(self):
        source, clang = self.compile_fixture()
        for number in range(6):
            self.project(f"inventory_{number}", [])
        expected = None
        for jobs in (1, 2, 4, 20):
            width = min(jobs, 8)
            barrier = threading.Barrier(width, timeout=10)
            lock = threading.Lock()
            active = peak = 0
            completed = []

            def query(msbuild, project, *, commands, lock=lock, barrier=barrier, completed=completed):
                nonlocal active, peak
                with lock:
                    active += 1
                    peak = max(peak, active)
                barrier.wait()
                result = self.compile_payload(project, commands=commands)
                with lock:
                    active -= 1
                    completed.append(project)
                return result

            with self.subTest(jobs=jobs), mock.patch.object(winuitidy, "_query_project", side_effect=query):
                actual = winuitidy.compile_commands(self.build, clang, required_translation_units=(source,), jobs=jobs)
            self.assertEqual(peak, width)
            self.assertEqual(active, 0)
            self.assertEqual(len(completed), 8)
            self.assertEqual(set(completed), set(self.items))
            if expected is None:
                expected = actual
            self.assertEqual(actual, expected)

    def test_parallel_native_failures_are_ordered_and_all_workers_join_before_phase_exit(self):
        source, clang = self.compile_fixture()
        other = self.project("other", [])
        last = self.project("zzz", [])
        main = self.build / "app/windows-winui/aobus-winui-lib.vcxproj"
        selected = self.build / "app/windows-winui/example.vcxproj"
        barrier = threading.Barrier(4, timeout=10)
        finished_later = threading.Event()
        completed = set()
        lock = threading.Lock()

        def query(msbuild, project, *, commands):
            barrier.wait()
            if project == main:
                self.assertTrue(finished_later.wait(10))
            with lock:
                completed.add(project)
            if project == selected:
                raise OSError("cannot launch selected")
            if project == last:
                finished_later.set()
                return self.compile_payload(project, commands=commands)
            return mock.Mock(returncode=7 if project == main else 3, stdout=f"failure {project.name}")

        @contextlib.contextmanager
        def measure(*args, **kwargs):
            try:
                yield SimpleNamespace(status=0)
            finally:
                self.assertEqual(completed, set(self.items))

        with (
            mock.patch.object(winuitidy, "_query_project", side_effect=query),
            mock.patch.object(winuitidy.phases, "measure", side_effect=measure),
            contextlib.redirect_stderr(io.StringIO()) as errors,
            self.assertRaises(SystemExit),
        ):
            winuitidy.compile_commands(self.build, clang, required_translation_units=(source,), jobs=4)
        diagnostic = errors.getvalue()
        self.assertLess(diagnostic.index(str(main)), diagnostic.index(str(selected)))
        self.assertLess(diagnostic.index(str(selected)), diagnostic.index(str(other)))
        for message in ("exit 7", "cannot launch selected", "exit 3"):
            self.assertIn(message, diagnostic)

    def test_worker_exceptions_are_all_reported_in_inventory_order(self):
        source, clang = self.compile_fixture()

        def query(msbuild, project, *, commands):
            raise RuntimeError(f"worker exception in {project.name}")

        with (
            mock.patch.object(winuitidy, "_query_project", side_effect=query) as run,
            contextlib.redirect_stderr(io.StringIO()) as errors,
            self.assertRaises(SystemExit),
        ):
            winuitidy.compile_commands(self.build, clang, required_translation_units=(source,), jobs=2)
        self.assertEqual(run.call_count, 2)
        diagnostic = errors.getvalue()
        self.assertIn("worker exception in aobus-winui-lib.vcxproj", diagnostic)
        self.assertIn("worker exception in example.vcxproj", diagnostic)
        self.assertLess(diagnostic.index("aobus-winui-lib.vcxproj"), diagnostic.index("example.vcxproj"))

    def test_parallel_inventory_still_rejects_unselected_unmapped_sources(self):
        source, clang = self.compile_fixture()
        self.project("unselected", ["test/helper/Unmapped.cpp"])
        with (
            mock.patch.object(
                winuitidy,
                "_query_project",
                side_effect=lambda msbuild, project, **kw: self.compile_payload(project, **kw),
            ) as query,
            contextlib.redirect_stderr(io.StringIO()) as errors,
            self.assertRaises(SystemExit),
        ):
            winuitidy.compile_commands(self.build, clang, required_translation_units=(source,), jobs=4)
        self.assertEqual(query.call_count, 3)
        self.assertIn("unmapped.cpp", errors.getvalue().lower())

    def test_missing_current_project_fails_before_query(self):
        (self.build / "app/windows-winui/example.vcxproj").unlink()
        self.assert_rejected("example.vcxproj")
        self.assertEqual(self.run.call_count, 0)

    def test_missing_cmake_project_name_fails_readably(self):
        (self.build / "CMakeCache.txt").write_text("", encoding="utf-8")
        self.assert_rejected("CMAKE_PROJECT_NAME")

    def test_cache_lookup_keeps_the_last_duplicate_and_raw_value(self):
        (self.build / "CMakeCache.txt").write_text(
            "CMAKE_PROJECT_NAME:STATIC=First\n"
            "CMAKE_GENERATOR:INTERNAL=Visual Studio 18 2026=x64\n"
            "CMAKE_PROJECT_NAME:STATIC=Ownership\n",
            encoding="utf-8",
        )

        self.assertEqual(winuitidy._cmake_cache_value(self.build, "CMAKE_PROJECT_NAME"), "Ownership")
        self.assertEqual(winuitidy._cmake_cache_value(self.build, "CMAKE_GENERATOR"), "Visual Studio 18 2026=x64")
        self.assertIsNone(winuitidy._cmake_cache_value(self.build, "ABSENT"))

    def test_cache_read_error_dies_with_a_cache_diagnostic(self):
        (self.build / "CMakeCache.txt").unlink()

        with contextlib.redirect_stderr(io.StringIO()) as errors, self.assertRaises(SystemExit):
            winuitidy._cmake_cache_value(self.build, "CMAKE_PROJECT_NAME")

        self.assertIn("cannot read WinUI CMake cache", errors.getvalue())

    def test_malformed_evaluated_items_and_query_failure_fail_closed(self):
        for result, message in (
            (mock.Mock(returncode=3, stdout="native failure"), "exit 3"),
            (mock.Mock(returncode=0, stdout="not json"), "JSON"),
            (mock.Mock(returncode=0, stdout='{"Items": {}}'), "ClCompile"),
            (mock.Mock(returncode=0, stdout='{"Items": {"ClCompile": [null]}}'), "ClCompile"),
            (mock.Mock(returncode=0, stdout='{"Items": {"ClCompile": [{}]}}'), "Identity"),
        ):
            with self.subTest(message=message), mock.patch.object(winuitidy.subprocess, "run", return_value=result):
                self.assert_rejected(message)

    def test_case_variants_match_only_on_windows_including_project_names(self):
        project = self.build / "app/windows-winui/example.vcxproj"
        self.items[project] = [{"Identity": str(self.source / "TEST/WINUI/PROBE/EXAMPLE.CPP")}]
        for path_module, accepted in ((ntpath, True), (posixpath, False)):
            mapping = {
                path_module.normcase(source).replace("\\", "/"): "APP/WINDOWS-WINUI/EXAMPLE.VCXPROJ"
                for source in self.mapping
            }
            with (
                self.subTest(platform=path_module.__name__),
                mock.patch.object(winuitidy, "os", SimpleNamespace(path=path_module)),
                mock.patch.object(winuitidy, "_AUXILIARY_SOURCE_PROJECTS", mapping),
            ):
                if accepted:
                    self.assertEqual(self.verify(), "")
                else:
                    self.assert_rejected("differs from configured Debug|x64 ownership")

    def test_windows_lower_does_not_turn_long_s_into_an_ascii_test_directory(self):
        self.project("other", ["te\u017ft/helper/Foreign.cpp"])
        mapping = {ntpath.normcase(source).replace("\\", "/"): project for source, project in self.mapping.items()}
        with (
            mock.patch.object(winuitidy, "os", SimpleNamespace(path=ntpath)),
            mock.patch.object(winuitidy, "_AUXILIARY_SOURCE_PROJECTS", mapping),
        ):
            self.assertEqual(self.verify(), "")

    def test_owned_compiler_views_are_mapped_before_source_comparison(self):
        viewed = self.source.parent / "view"
        project = self.build / "app/windows-winui/example.vcxproj"
        self.items[project] = [{"Identity": str(viewed / "test/winui/probe/Example.cpp")}]

        def canonical(path, **kwargs):
            path = path.absolute()
            try:
                return self.source / path.relative_to(viewed)
            except ValueError:
                return path

        with mock.patch.object(winuitidy.workspace_cache, "canonical_portal_path", side_effect=canonical):
            self.assertEqual(self.verify(), "")
        self.assert_rejected("differs from configured Debug|x64 ownership")


if __name__ == "__main__":
    unittest.main()
