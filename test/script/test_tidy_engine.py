"""Tests for the CMake-free tidy scope, platform and SDK descriptors.

Bazel owns compile commands and header inputs; this module only describes
scope selection, native platform ownership, verified SDK tool selection and
public/private component mapping. No compile database, Ninja or CMake cache
appears here.
"""

import argparse
import contextlib
import io
import unittest
from pathlib import Path
from unittest import mock

from ao.portal import tidy_engine


class ScopeResolutionTest(unittest.TestCase):
    def arguments(self, **overrides):
        arguments = argparse.Namespace(
            files=[], all=False, folder=[], commit=None, check=None, debug=False, output=None, jobs=1, path=None
        )
        for key, value in overrides.items():
            setattr(arguments, key, value)
        return arguments

    def test_explicit_files_are_the_scope(self):
        files, explicit = tidy_engine.resolve_scope(self.arguments(files=["lib/A.cpp", "lib/B.cpp"]), [], "Checking")
        self.assertEqual(files, ["lib/A.cpp", "lib/B.cpp"])
        self.assertTrue(explicit)

    def test_prepared_scope_is_consumed_without_rescanning(self):
        arguments = self.arguments(_resolved_sources=["lib/Prepared.cpp"])
        with mock.patch.object(tidy_engine.git_files, "changed_files", side_effect=AssertionError("rescanned")):
            files, explicit = tidy_engine.resolve_scope(arguments, [], "Checking")
        self.assertEqual(files, ["lib/Prepared.cpp"])
        self.assertFalse(explicit)

    def test_all_uses_the_project_folders(self):
        with (
            mock.patch.object(tidy_engine.git_files, "find_sources", return_value=["lib/A.cpp"]) as find,
            contextlib.redirect_stderr(io.StringIO()),
        ):
            files, explicit = tidy_engine.resolve_scope(self.arguments(all=True), ["lib", "app"], "Checking")
        self.assertEqual(files, ["lib/A.cpp"])
        self.assertFalse(explicit)
        find.assert_called_once_with(["lib", "app"], suffixes=tidy_engine.git_files.CPP_SUFFIXES)

    def test_folder_uses_the_selected_directories(self):
        with (
            mock.patch.object(tidy_engine.git_files, "find_sources", return_value=["app/A.cpp"]) as find,
            contextlib.redirect_stderr(io.StringIO()),
        ):
            files, explicit = tidy_engine.resolve_scope(self.arguments(folder=["app/runtime"]), ["lib"], "Checking")
        self.assertEqual(files, ["app/A.cpp"])
        self.assertFalse(explicit)
        find.assert_called_once_with(["app/runtime"], suffixes=tidy_engine.git_files.CPP_SUFFIXES)

    def test_without_selection_the_changed_files_are_the_scope(self):
        with (
            mock.patch.object(tidy_engine.git_files, "diff_base", return_value="HEAD~1") as base,
            mock.patch.object(tidy_engine.git_files, "changed_files", return_value=["lib/Changed.cpp"]) as changed,
            contextlib.redirect_stderr(io.StringIO()),
        ):
            files, explicit = tidy_engine.resolve_scope(self.arguments(commit="HEAD~1"), [], "Checking")
        self.assertEqual(files, ["lib/Changed.cpp"])
        self.assertFalse(explicit)
        base.assert_called_once_with("HEAD~1")
        changed.assert_called_once_with("HEAD~1", suffixes=tidy_engine.git_files.CPP_SUFFIXES)


class ClangToolTest(unittest.TestCase):
    def test_non_windows_hosts_keep_the_plain_tool_name_without_reading_the_manifest(self):
        for profile in (tidy_engine.build_dir.LINUX_PROFILE, tidy_engine.build_dir.MACOS_PROFILE):
            with (
                self.subTest(profile=profile.name),
                mock.patch.object(tidy_engine.build_dir, "platform_profile", return_value=profile),
                mock.patch.object(
                    tidy_engine, "windows_llvm_sdk_manifest", side_effect=AssertionError("manifest read")
                ),
            ):
                self.assertEqual(tidy_engine.clang_tool(Path("/build"), "clang-tidy"), "clang-tidy")

    def test_windows_clang_tidy_requires_the_verified_bazel_lint_context(self):
        with (
            mock.patch.object(
                tidy_engine.build_dir, "platform_profile", return_value=tidy_engine.build_dir.WINDOWS_PROFILE
            ),
            mock.patch.object(tidy_engine, "windows_llvm_sdk_manifest", side_effect=AssertionError("no SDK probing")),
        ):
            with contextlib.redirect_stderr(io.StringIO()), self.assertRaises(SystemExit):
                tidy_engine.clang_tool(Path("/build"), "clang-tidy")

    def test_windows_format_tool_comes_from_the_ready_sdk_root(self):
        manifest = mock.Mock()
        root = Path("C:/state/llvm/toolchains/llvm-31.2.3-x86_64-windows-msvc")
        with (
            mock.patch.object(
                tidy_engine.build_dir, "platform_profile", return_value=tidy_engine.build_dir.WINDOWS_PROFILE
            ),
            mock.patch.object(tidy_engine, "windows_llvm_sdk_manifest", return_value=manifest),
            mock.patch.object(tidy_engine.windows_llvm_sdk, "ready_root", return_value=(root, True)) as ready,
            mock.patch.object(Path, "is_file", return_value=True),
        ):
            tool = tidy_engine.clang_tool(Path("/build"), "clang-format")
        self.assertEqual(tool, str(root / "bin" / "clang-format.exe"))
        ready.assert_called_once_with(tidy_engine.os.environ, manifest)


class PlatformIncompatibilityTest(unittest.TestCase):
    ROOT = Path("/repo")

    def incompatible(self, profile, path):
        with mock.patch.object(tidy_engine.build_dir, "platform_profile", return_value=profile):
            return tidy_engine._is_platform_incompatible(path, self.ROOT)

    def test_winui_sources_are_incompatible_with_the_linux_native_graph(self):
        for relative in (
            "test/winui/ThemeTest.cpp",
            "test/winui/layout/LayoutDialectTest.cpp",
            "app/windows-winui/MainWindow.xaml.cpp",
            "app/windows/Platform.cpp",
        ):
            with self.subTest(relative=relative):
                self.assertTrue(self.incompatible(tidy_engine.build_dir.LINUX_PROFILE, self.ROOT / relative))

    def test_linux_sources_stay_compatible_with_the_linux_native_graph(self):
        for relative in (
            "app/linux-gtk/MainWindow.cpp",
            "app/systemmedia/linux/MprisBusSession.cpp",
            "test/linux-gtk/GtkTest.cpp",
            "test/systemmedia/linux/TuiMprisProcessTest.cpp",
            "lib/utility/Path.cpp",
        ):
            with self.subTest(relative=relative):
                self.assertFalse(self.incompatible(tidy_engine.build_dir.LINUX_PROFILE, self.ROOT / relative))

    def test_posix_and_linux_sources_are_incompatible_with_the_windows_native_graph(self):
        for relative in (
            "app/linux-gtk/MainWindow.cpp",
            "app/systemmedia/linux/MprisBusSession.cpp",
            "lib/audio/backend/PipewireBackend.cpp",
            "lib/utility/AtomicFilePosix.cpp",
        ):
            with self.subTest(relative=relative):
                self.assertTrue(self.incompatible(tidy_engine.build_dir.WINDOWS_PROFILE, self.ROOT / relative))

    def test_windows_sources_stay_compatible_with_the_windows_native_graph(self):
        for relative in (
            "app/windows-winui/MainWindow.xaml.cpp",
            "test/winui/ThemeTest.cpp",
            "lib/utility/AtomicFileWindows.cpp",
            "lib/audio/backend/WasapiProvider.cpp",
        ):
            with self.subTest(relative=relative):
                self.assertFalse(self.incompatible(tidy_engine.build_dir.WINDOWS_PROFILE, self.ROOT / relative))

    def test_macos_ownership_is_exclusive_to_the_macos_native_graph(self):
        for relative in (
            "app/macos-appkit/AppKitAdapter.mm",
            "test/macos/CoreAudioOutputTest.cpp",
            "test/macos-appkit/AppKitSmokeMain.mm",
            "lib/audio/backend/CoreAudioProvider.cpp",
        ):
            with self.subTest(relative=relative):
                self.assertFalse(self.incompatible(tidy_engine.build_dir.MACOS_PROFILE, self.ROOT / relative))
                self.assertTrue(self.incompatible(tidy_engine.build_dir.LINUX_PROFILE, self.ROOT / relative))
                self.assertTrue(self.incompatible(tidy_engine.build_dir.WINDOWS_PROFILE, self.ROOT / relative))

    def test_foreign_frontends_are_incompatible_with_the_macos_native_graph(self):
        for relative in ("app/linux-gtk/MainWindow.cpp", "app/windows-winui/MainWindow.xaml.cpp"):
            with self.subTest(relative=relative):
                self.assertTrue(self.incompatible(tidy_engine.build_dir.MACOS_PROFILE, self.ROOT / relative))

    def test_shared_posix_sources_stay_compatible_with_the_macos_native_graph(self):
        for relative in ("lib/utility/AtomicFilePosix.cpp", "lib/utility/FileAllocationPosix.cpp"):
            with self.subTest(relative=relative):
                self.assertFalse(self.incompatible(tidy_engine.build_dir.MACOS_PROFILE, self.ROOT / relative))
                self.assertTrue(self.incompatible(tidy_engine.build_dir.WINDOWS_PROFILE, self.ROOT / relative))

    def test_paths_outside_the_root_are_not_classified(self):
        self.assertFalse(self.incompatible(tidy_engine.build_dir.LINUX_PROFILE, Path("/elsewhere/ThemeTest.cpp")))


class ComponentMappingTest(unittest.TestCase):
    def test_public_include_roots_map_to_their_implementation_components(self):
        cases = (
            (("include", "ao", "utility", "Path.h"), ("lib", "utility", "Path.h")),
            (("include", "ao", "library", "MusicLibrary.h"), ("lib", "library", "MusicLibrary.h")),
            (("app", "include", "ao", "rt", "Signal.h"), ("app", "runtime", "Signal.h")),
            (("app", "include", "ao", "uimodel", "Field.h"), ("app", "uimodel", "Field.h")),
            (("app", "linux-gtk", "include", "ao", "gtk", "Leaf.h"), ("app", "linux-gtk", "Leaf.h")),
        )
        for parent, expected in cases:
            with self.subTest(parent=parent):
                self.assertEqual(tidy_engine.component_parts(parent), expected)

    def test_implementation_roots_map_to_themselves_and_others_are_rejected(self):
        for parent in (("lib", "utility"), ("app", "runtime"), ("test", "core"), ("tool", "lint")):
            with self.subTest(parent=parent):
                self.assertEqual(tidy_engine.component_parts(parent), parent)
        for parent in ((), ("script", "ao"), ("doc", "development"), ("include", "thirdparty", "x.h")):
            with self.subTest(parent=parent):
                self.assertIsNone(tidy_engine.component_parts(parent))


if __name__ == "__main__":
    unittest.main()
