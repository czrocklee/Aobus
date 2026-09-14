"""Fixed compiler views retain physical ownership and command lifetime boundaries."""

import argparse
import io
import json
import os
import shutil
import subprocess
import tempfile
import unittest
from contextlib import contextmanager, redirect_stdout
from pathlib import Path
from unittest import mock

from ao.__main__ import make_parser
from ao.command import build, deps
from ao.command import run as run_command
from ao.core import builddir, buildlock, compiler_cache
from ao.core import workspace_cache as cache


class WindowsWorkspaceCacheTest(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        self.source = self.root / "checkout"
        self.source.mkdir()
        (self.source / "cmake").mkdir()
        shutil.copyfile(
            cache.PROJECT_ROOT / "cmake/SharedWorkspaceCacheWindows.cmake",
            self.source / "cmake/SharedWorkspaceCacheWindows.cmake",
        )
        self.build = self.root / "builds" / "windows-debug"
        self.executable = self.root / "ccache.exe"
        self.executable.write_bytes(b"verified fixture")
        self.environment = {
            compiler_cache.SHARED_WORKSPACES_EFFECTIVE: "1",
            compiler_cache.SHARED_WORKSPACES_CCACHE: str(self.executable),
            compiler_cache.SHARED_WORKSPACES_MSBUILD_WRAPPER: str(self.executable),
            "AOBUS_MSBUILD_CL_TOOL_EXE": str(self.executable),
            "CCACHE_NAMESPACE": "personal",
        }
        self.args = argparse.Namespace(command="build", path=str(self.build))
        self.drives = {}
        self.removed = []
        self.mutex_depth = 0
        for name, replacement in (
            ("_windows_is_local_system", lambda: False),
            ("_windows_mapping_mutex", self.mutex),
            ("_windows_drive_target", self.drives.get),
            ("_windows_define_drive", self.define),
            ("_windows_remove_drive", self.remove),
        ):
            patch = mock.patch.object(cache, name, side_effect=replacement)
            patch.start()
            self.addCleanup(patch.stop)

    @contextmanager
    def mutex(self):
        self.mutex_depth += 1
        try:
            yield
        finally:
            self.mutex_depth -= 1

    def define(self, drive, target):
        self.assertEqual(self.mutex_depth, 1)
        self.drives[drive] = target

    def remove(self, drive, target):
        self.assertEqual(self.mutex_depth, 1)
        self.assertEqual(self.drives.pop(drive), target)
        self.removed.append(drive)

    def context(self):
        return cache.command_context(self.args, environ=self.environment, system="Windows", project_root=self.source)

    def prepare(self):
        return cache.prepare(self.build, project_root=self.source, environ=self.environment, system="Windows")

    def test_reserves_pair_without_holding_mutex_during_command_and_restores_environment(self):
        original = dict(self.environment)
        with self.context():
            self.assertEqual(set(self.drives), {"S:", "B:"})
            self.assertEqual(self.mutex_depth, 0)
            self.assertEqual(self.environment["CCACHE_HASHDIR"], "true")
            self.assertEqual(self.environment["CCACHE_BASEDIR"], "")
            self.prepare()
        self.assertEqual(self.environment, original)
        self.assertEqual(self.drives, {})
        self.assertEqual(self.removed, ["B:", "S:"])

    def test_dependency_commands_reacquire_retired_views_before_reading_evidence(self):
        with self.context():
            self.prepare()
            (self.build / "CMakeCache.txt").write_text(
                "CMAKE_HOME_DIRECTORY:INTERNAL=S:/\nAOBUS_MANAGED_WINDOWS_SHARED_WORKSPACES:BOOL=ON\n"
            )
        self.assertEqual(self.drives, {})
        validate = cache.validate_consumer

        def verified(selected):
            self.assertEqual(selected, self.build)
            self.assertEqual(set(self.drives), {"S:", "B:"})
            return {"host": {"platform": "windows"}, "dependencies": {}, "nativeResolution": {"kind": "vcpkg"}}

        for action in ("verify", "report"):
            for selection in ("path", "BUILD_DIR", "default"):
                with self.subTest(action=action, selection=selection):
                    arguments = ["deps", action]
                    if selection == "path":
                        arguments += ["-p", str(self.build)]
                    self.args = make_parser().parse_args(arguments)
                    self.environment.pop("BUILD_DIR", None)
                    if selection == "BUILD_DIR":
                        self.environment["BUILD_DIR"] = str(self.build)
                    with (
                        mock.patch.object(builddir, "windows_build_root", return_value=self.build.parent),
                        self.context(),
                        mock.patch.dict(os.environ, self.environment),
                        mock.patch.object(cache.platform, "system", return_value="Windows"),
                        mock.patch.object(deps.builddir, "build_dir", return_value=self.build),
                        mock.patch.object(
                            cache,
                            "validate_consumer",
                            side_effect=lambda path: validate(path, project_root=self.source),
                        ),
                        mock.patch.object(deps.dependency_policy, "verified_report", side_effect=verified),
                        redirect_stdout(io.StringIO()) as output,
                    ):
                        self.assertEqual(self.args.func(self.args), 0)
                    self.assertIn("windows", output.getvalue())
                    self.assertEqual(self.drives, {})

    def test_default_dependency_tree_is_validated_before_evidence_is_read(self):
        self.build.mkdir(parents=True)
        (self.build / "CMakeCache.txt").write_text("CMAKE_HOME_DIRECTORY:INTERNAL=ordinary\n")
        for action in ("verify", "report"):
            with self.subTest(action=action):
                self.args = make_parser().parse_args(["deps", action])
                with (
                    mock.patch.object(builddir, "windows_build_root", return_value=self.build.parent),
                    self.context(),
                    mock.patch.dict(os.environ, self.environment),
                    mock.patch.object(cache.platform, "system", return_value="Windows"),
                    mock.patch.object(deps.builddir, "build_dir", return_value=self.build),
                    mock.patch.object(deps.dependency_policy, "verified_report") as verify,
                ):
                    with self.assertRaisesRegex(cache.WorkspaceCacheError, "no ownership file"):
                        self.args.func(self.args)
                    verify.assert_not_called()
                self.assertEqual(self.drives, {})

    def test_explicit_clean_replaces_incompatible_tree_under_build_lock(self):
        map_file = self.build / cache.WINDOWS_MAP_FILE_NAME
        sentinel = self.build / "old-tree.txt"
        locked = False
        real_lock = buildlock.build_tree_lock

        @contextmanager
        def lock(selected):
            nonlocal locked
            with real_lock(selected):
                locked = True
                try:
                    yield
                finally:
                    locked = False

        def remove(selected, _profile):
            self.assertTrue(locked)
            self.assertEqual(selected, self.build)
            shutil.rmtree(selected)

        def configure(command, **_kwargs):
            self.assertTrue(locked)
            self.assertFalse(sentinel.exists())
            self.assertTrue(map_file.is_file())
            if "--preset" in command:
                (self.build / "CMakeCache.txt").write_text(
                    "CMAKE_HOME_DIRECTORY:INTERNAL=S:/\nAOBUS_MANAGED_WINDOWS_SHARED_WORKSPACES:BOOL=ON\n"
                )
            return 0

        for selection in ("path", "BUILD_DIR"):
            for previous_owner in (None, {"schemaVersion": 1, "physicalSourceRoot": "foreign"}):
                with self.subTest(selection=selection, previous_owner=previous_owner):
                    self.build.mkdir(parents=True, exist_ok=True)
                    (self.build / "CMakeCache.txt").write_text("CMAKE_HOME_DIRECTORY:INTERNAL=ordinary\n")
                    map_file.unlink(missing_ok=True)
                    if previous_owner:
                        map_file.write_text(json.dumps(previous_owner))
                    sentinel.touch()
                    arguments = ["build", "--clean"]
                    self.environment.pop("BUILD_DIR", None)
                    if selection == "path":
                        arguments += ["-p", str(self.build)]
                    else:
                        self.environment["BUILD_DIR"] = str(self.build)
                    self.args = make_parser().parse_args(arguments)
                    with (
                        self.context(),
                        mock.patch.dict(os.environ, self.environment),
                        mock.patch.object(builddir, "platform_profile", return_value=builddir.WINDOWS_PROFILE),
                        mock.patch.object(build.buildlock, "build_tree_lock", side_effect=lock),
                        mock.patch.object(build, "_remove_build_directory", side_effect=remove),
                        mock.patch.object(
                            build, "_workspace_cache", side_effect=lambda *_args, **_kwargs: self.prepare()
                        ),
                        mock.patch.object(build, "validate_build_tree", return_value="MSVC"),
                        mock.patch.object(build, "run", side_effect=configure),
                        redirect_stdout(io.StringIO()),
                    ):
                        result = build.do_build(self.args, ["ao_core_test"])
                        cache._validate_windows_tree(
                            self.build,
                            self.source,
                            compiler_cache.read_cmake_cache(self.build / "CMakeCache.txt"),
                            self.environment,
                            json.loads(map_file.read_text()),
                        )
                    self.assertEqual(result.build_dir, self.build)
                    self.assertTrue(buildlock.lock_path(self.build).is_file())
                    self.assertFalse(sentinel.exists())
                    self.assertEqual(self.drives, {})

    def test_reuse_and_no_build_keep_rejecting_unowned_trees_even_with_clean(self):
        self.build.mkdir(parents=True)
        old_cache = self.build / "CMakeCache.txt"
        old_cache.write_text("CMAKE_HOME_DIRECTORY:INTERNAL=ordinary\n")
        for command in (
            ["build"],
            ["check"],
            ["run", "tui", "--no-build", "--clean"],
            ["perf", "--no-build", "--clean"],
            ["test", "--core", "--no-build"],
            ["deps", "verify"],
            ["deps", "report"],
        ):
            for selection in ("path", "BUILD_DIR"):
                with self.subTest(command=command, selection=selection):
                    self.environment.pop("BUILD_DIR", None)
                    options = ["-p", str(self.build)] if selection == "path" else []
                    if selection == "BUILD_DIR":
                        self.environment["BUILD_DIR"] = str(self.build)
                    self.args = make_parser().parse_args(command + options)
                    with self.assertRaisesRegex(cache.WorkspaceCacheError, "no ownership file"):
                        with self.context():
                            self.fail("unowned tree must not be reused")
                    self.assertEqual(old_cache.read_text(), "CMAKE_HOME_DIRECTORY:INTERNAL=ordinary\n")
                    self.assertEqual(self.drives, {})

    def test_sanitizer_rejection_precedes_tree_ownership_and_drive_acquisition(self):
        self.build.mkdir(parents=True)
        for existing in (False, True):
            if existing:
                (self.build / "CMakeCache.txt").write_text("CMAKE_HOME_DIRECTORY:INTERNAL=foreign\n")
            for explicit in (False, True):
                for command, no_build in (("build", False), ("test", True)):
                    for sanitizer in ("asan", "tsan"):
                        with self.subTest(existing=existing, explicit=explicit, command=command, sanitizer=sanitizer):
                            self.args = argparse.Namespace(
                                command=command,
                                suite="core",
                                no_build=no_build,
                                path=str(self.build) if explicit else None,
                                **{sanitizer: True},
                            )
                            with self.assertRaisesRegex(
                                cache.WorkspaceCacheError, "sanitizer builds.*AOBUS_SHARED_WORKSPACES=0"
                            ):
                                with self.context():
                                    self.fail("unsupported command must not run")
                            self.assertEqual(self.drives, {})
                            self.assertEqual(self.removed, [])

    def test_consumer_accepts_only_the_configured_owner(self):
        with self.context():
            profile = self.prepare()
            (self.build / "CMakeCache.txt").write_text(
                "CMAKE_HOME_DIRECTORY:INTERNAL=S:/\n"
                "AOBUS_WINDOWS_SHARED_WORKSPACES:BOOL=ON\n"
                "AOBUS_MANAGED_WINDOWS_SHARED_WORKSPACES:BOOL=ON\n"
            )
            with mock.patch.object(cache.platform, "system", return_value="Windows"):
                with mock.patch.dict(os.environ, self.environment, clear=True):
                    cache.validate_consumer(self.build, project_root=self.source)
                    original = profile.map_file.read_text()
                    owner = json.loads(original)
                    owner["physicalSourceRoot"] = "foreign checkout"
                    profile.map_file.write_text(json.dumps(owner))
                    with self.assertRaisesRegex(cache.WorkspaceCacheError, "ownership mismatch"):
                        cache.validate_consumer(self.build, project_root=self.source)
                    profile.map_file.unlink()
                    with self.assertRaisesRegex(cache.WorkspaceCacheError, "no ownership file"):
                        cache.validate_consumer(self.build, project_root=self.source)
                    profile.map_file.write_text(original)
                with mock.patch.dict(os.environ, {}, clear=True):
                    with self.assertRaisesRegex(cache.WorkspaceCacheError, "requires the shared-workspace profile"):
                        cache.validate_consumer(self.build, project_root=self.source)

    def test_occupied_drive_is_never_retargeted_and_partial_reservation_is_retired(self):
        self.drives["B:"] = "foreign"
        with self.assertRaisesRegex(cache.WorkspaceCacheError, "already in use"):
            with self.context():
                self.fail("command must not run")
        self.assertEqual(self.drives, {"B:": "foreign"})
        self.assertEqual(self.removed, ["S:"])

    def test_run_reaps_interrupted_application_before_retiring_views(self):
        args = make_parser().parse_args(["run", "tui", "-n", "-p", str(self.build)])
        child = mock.MagicMock()

        def wait():
            self.assertEqual(set(self.drives), {"S:", "B:"})
            if child.wait.call_count == 1:
                raise KeyboardInterrupt
            return 1

        child.wait.side_effect = wait
        with (
            self.context(),
            mock.patch.dict(os.environ, self.environment),
            mock.patch.object(run_command.build, "validate_build_options", return_value=builddir.LINUX_PROFILE),
            mock.patch.object(run_command.workspace_cache, "validate_consumer"),
            mock.patch.object(run_command.Path, "exists", return_value=True),
            mock.patch.object(run_command.subprocess, "Popen") as launch,
        ):
            launch.return_value.__enter__.return_value = child
            self.assertEqual(run_command.run_command(args), 130)
            child.kill.assert_called_once()
            self.assertEqual(child.wait.call_count, 2)
        self.assertEqual(self.drives, {})

    def test_sibling_build_error_names_the_overlapping_mapping_parent(self):
        self.args.path = str(self.source.parent / "build-debug")
        with self.assertRaisesRegex(cache.WorkspaceCacheError, "B: root.*parent of an explicit build path"):
            with self.context():
                self.fail("overlapping mapping must not be acquired")
        self.assertEqual(self.drives, {})

    def test_keyboard_interrupt_after_dispatch_keeps_views_for_surviving_children(self):
        with self.assertRaises(KeyboardInterrupt):
            with self.context():
                raise KeyboardInterrupt
        self.assertEqual(set(self.drives), {"S:", "B:"})
        self.assertEqual(self.removed, [])
        self.assertNotIn(cache.WINDOWS_SOURCE_VIEW, self.environment)

    def test_interrupted_acquisition_rolls_back_before_any_child_can_exist(self):
        def interrupted(drive, target):
            if drive == "B:":
                raise KeyboardInterrupt
            self.define(drive, target)

        with mock.patch.object(cache, "_windows_define_drive", side_effect=interrupted):
            with self.assertRaises(KeyboardInterrupt):
                with self.context():
                    self.fail("command must not run")
        self.assertEqual(self.drives, {})

    def test_routine_command_failure_cleans_views(self):
        with self.assertRaises(SystemExit):
            with self.context():
                raise SystemExit(2)
        self.assertEqual(self.drives, {})

    def test_foreign_replacement_is_reported_without_removing_it(self):
        with self.assertRaisesRegex(cache.WorkspaceCacheError, "changed during use"):
            with self.context():
                self.drives["S:"] = "foreign"
        self.assertEqual(self.drives, {"S:": "foreign"})

    def test_local_system_and_ambient_cl_options_are_rejected_before_mapping(self):
        with mock.patch.object(cache, "_windows_is_local_system", return_value=True):
            with self.assertRaisesRegex(cache.WorkspaceCacheError, "LocalSystem"):
                with self.context():
                    self.fail("command must not run")
        self.environment["CL"] = "/Zi"
        with self.assertRaisesRegex(cache.WorkspaceCacheError, "CL and _CL_"):
            with self.context():
                self.fail("command must not run")
        self.assertEqual(self.drives, {})

    def test_pure_tooling_does_not_allocate_compiler_views(self):
        self.args.command = "test"
        self.args.suite = "tooling"
        with self.context():
            self.assertEqual(self.drives, {})

    def test_prepared_tree_records_physical_owner_and_fixed_compiler_paths(self):
        with self.context():
            profile = self.prepare()
            self.assertEqual(profile.source_dir, Path("S:/"))
            self.assertEqual(profile.compiler_build_dir, Path("B:/windows-debug"))
            owner = json.loads(profile.map_file.read_text())
            self.assertEqual(owner["physicalSourceRoot"], cache._windows_path_identity(self.source))
            self.assertEqual(owner["physicalBuildDirectory"], cache._windows_path_identity(self.build))
            self.assertEqual(owner["buildView"], "B:/windows-debug")
            self.assertEqual(self.prepare(), profile)

    def test_existing_unowned_tree_is_not_adopted_even_for_no_build(self):
        self.build.mkdir(parents=True)
        (self.build / "CMakeCache.txt").write_text("CMAKE_HOME_DIRECTORY:INTERNAL=S:/\n")
        self.args.no_build = True
        self.args.command = "run"
        with self.assertRaisesRegex(cache.WorkspaceCacheError, "no ownership file"):
            with self.context():
                self.fail("unowned binaries must not run")
        self.assertEqual(self.drives, {})

    def test_owner_mismatch_is_rejected_without_rewriting_record(self):
        with self.context():
            profile = self.prepare()
            owner = json.loads(profile.map_file.read_text())
            owner["physicalSourceRoot"] = "foreign checkout"
            original = json.dumps(owner)
            profile.map_file.write_text(original)
            with self.assertRaisesRegex(cache.WorkspaceCacheError, "ownership mismatch"):
                self.prepare()
            self.assertEqual(profile.map_file.read_text(), original)

    def test_disabled_profile_cannot_reconfigure_a_fixed_view_tree(self):
        self.build.mkdir(parents=True)
        (self.build / "CMakeCache.txt").write_text("AOBUS_WINDOWS_SHARED_WORKSPACES:BOOL=ON\n")
        with self.assertRaisesRegex(cache.WorkspaceCacheError, "requires fixed drive views"):
            cache.prepare(self.build, project_root=self.source, environ={}, system="Windows")

    def test_compiler_identity_maps_only_owned_views_and_rejects_outside_builds(self):
        with self.context():
            physical = cache.canonical_portal_path(
                Path("S:/app/main.cpp"), environ=self.environment, system="Windows", project_root=self.source
            )
            self.assertEqual(physical, self.source / "app/main.cpp")
            compiler = cache.compiler_source_path(
                physical, environ=self.environment, system="Windows", project_root=self.source
            )
            self.assertEqual(compiler, Path("S:/app/main.cpp"))
            with self.assertRaisesRegex(cache.WorkspaceCacheError, "outside the fixed B:"):
                cache.compiler_build_dir(self.root / "other", environ=self.environment, system="Windows")

    def test_debugger_map_resolves_a_temporary_source_drive_to_the_provider(self):
        original = os.path.realpath
        provider = r"\\?\UNC\host\share\MixedCase"

        def resolve(path, **kwargs):
            return provider if Path(path) == self.source else original(path, **kwargs)

        with (
            mock.patch.object(cache.os.path, "realpath", side_effect=resolve),
            mock.patch.object(cache, "absolute_path", side_effect=lambda path: Path(os.path.abspath(path))),
        ):
            with self.context():
                profile = self.prepare()
                record = json.loads(profile.map_file.read_text())
                self.assertEqual(record["debuggerSourceMap"], {"from": "S:/", "to": r"\\host\share\MixedCase"})
                self.assertEqual(cache._windows_raw_target(self.source), r"\??\UNC\host\share\MixedCase")
                self.assertEqual(record["physicalSourceRoot"], r"\\host\share\mixedcase")


class WindowsCacheCompilerAdmissionTest(unittest.TestCase):
    def test_only_native_msvc_for_both_languages_reaches_portal_ownership_admission(self):
        cmake = shutil.which("cmake")
        if not cmake:
            self.skipTest("CMake is not available in this tooling environment")
        module = cache.PROJECT_ROOT / "cmake/SharedWorkspaceCacheWindows.cmake"
        for c_id, cxx_id in (("MSVC", "MSVC"), ("Clang", "MSVC"), ("MSVC", "Clang"), ("Clang", "Clang")):
            with self.subTest(c=c_id, cxx=cxx_id), tempfile.TemporaryDirectory() as temporary:
                script = Path(temporary) / "guard.cmake"
                script.write_text(
                    "set(WIN32 TRUE)\nset(MSVC TRUE)\nset(AOBUS_WINDOWS_SHARED_WORKSPACES ON)\n"
                    f"set(CMAKE_C_COMPILER_ID {c_id})\nset(CMAKE_CXX_COMPILER_ID {cxx_id})\n"
                    f'include("{module.as_posix()}")\n',
                    encoding="utf-8",
                )
                result = subprocess.run([cmake, "-P", str(script)], capture_output=True, text=True, check=False)
                self.assertNotEqual(result.returncode, 0)
                diagnostic = " ".join(result.stderr.split())
                if c_id == cxx_id == "MSVC":
                    self.assertIn("must be configured through ao.bat", diagnostic)
                else:
                    self.assertIn("requires native MSVC for C and C++", diagnostic)
                    self.assertNotIn("must be configured through ao.bat", diagnostic)
