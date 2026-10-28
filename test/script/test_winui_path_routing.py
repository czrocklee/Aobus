"""Keep frontend header selection narrower than WinUI auxiliary context admission."""

import argparse
import contextlib
import ntpath
import posixpath
import tempfile
import unittest
from pathlib import Path
from types import SimpleNamespace
from unittest import mock

from ao.command import tidy
from ao.core import compiler_cache, winuitidy, workspace_cache


class WinUiPathRoutingTest(unittest.TestCase):
    def test_frontend_and_source_roots_normalize_dotdot_and_owned_view_metadata(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary).resolve()
            frontend = root / "app/windows-winui"
            environment = {
                compiler_cache.SHARED_WORKSPACES_EFFECTIVE: "1",
                workspace_cache.WINDOWS_SOURCE_VIEW: "Q:/",
            }

            def canonical(path, **kwargs):
                return workspace_cache.canonical_portal_path(path, environ=environment, system="Windows", **kwargs)

            with (
                mock.patch.object(winuitidy, "PROJECT_ROOT", root / "../" / root.name),
                mock.patch.object(winuitidy, "_WINUI_ROOT", Path("Q:/app/windows-winui")),
                mock.patch.object(winuitidy, "workspace_cache", SimpleNamespace(canonical_portal_path=canonical)),
            ):
                self.assertTrue(winuitidy.is_winui_path(frontend / "App.h"))
                self.assertTrue(winuitidy.requires_winui_compile_context(frontend / "App.cpp"))
                self.assertTrue(
                    winuitidy.requires_winui_compile_context(root / "test/helper/WinUiLocalizationProbe.cpp")
                )
                self.assertTrue(winuitidy.is_winui_path(frontend / "App.h", winui_root=frontend / "../windows-winui"))

    def test_prepare_forwards_jobs_and_holds_lock_until_query_returns(self):
        events = []

        @contextlib.contextmanager
        def held(path):
            events.append("lock")
            yield
            events.append("unlock")

        def query(*args, **kwargs):
            self.assertEqual(events, ["lock"])
            self.assertEqual(kwargs["jobs"], 3)
            events.append("query-complete")
            return []

        args = argparse.Namespace(path="primary", no_build=True, jobs=3)
        toolchain = tidy.TidyToolchain("AobusClangTidy.exe", None, Path("llvm/lib/clang/22"))
        with (
            mock.patch.object(tidy.buildlock, "build_tree_lock", side_effect=held),
            mock.patch.object(tidy.tidyengine, "clang_tool", return_value="clang-cl.exe"),
            mock.patch.object(winuitidy, "compile_commands", side_effect=query),
        ):
            tidy.prepare_winui_compile_commands(args, Path("primary"), toolchain, [tidy.WINUI_ROOT / "App.cpp"])
        self.assertEqual(events, ["lock", "query-complete", "unlock"])

    def test_owned_view_frontend_headers_share_the_boundary_without_admitting_auxiliary_headers(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary).resolve()
            environment = {
                compiler_cache.SHARED_WORKSPACES_EFFECTIVE: "1",
                workspace_cache.WINDOWS_SOURCE_VIEW: "Q:/",
            }

            def canonical(path, **kwargs):
                return workspace_cache.canonical_portal_path(path, environ=environment, system="Windows", **kwargs)

            with (
                mock.patch.object(winuitidy, "PROJECT_ROOT", root),
                mock.patch.object(winuitidy, "_WINUI_ROOT", root / "app/windows-winui"),
                mock.patch.object(tidy, "WINUI_ROOT", root / "app/windows-winui"),
                mock.patch.object(winuitidy, "workspace_cache", SimpleNamespace(canonical_portal_path=canonical)),
            ):
                for paths, uppercase in ((ntpath, True), (posixpath, False)):
                    # Start with literal fixture spelling, not production keys
                    # already normalized for the host that imported the module.
                    mapping = {
                        paths.normcase("test/helper/WinUiLocalizationProbe.cpp").replace(
                            "\\", "/"
                        ): "app/windows-winui/ao_winui_localization_probe.vcxproj"
                    }
                    with (
                        self.subTest(platform=paths.__name__),
                        mock.patch.object(winuitidy, "os", SimpleNamespace(path=paths)),
                        mock.patch.object(winuitidy, "_AUXILIARY_SOURCE_PROJECTS", mapping),
                    ):
                        for spelling, frontend, context in (
                            ("Q:/app/windows-winui/App.h", True, True),
                            ("Q:/APP/WINDOWS-WINUI/App.h", uppercase, uppercase),
                            ("Q:/test/helper/WinUiLocalizationProbe.h", False, True),
                            ("R:/app/windows-winui/App.h", False, False),
                            ("Q:/app/windows-winui-other/App.h", False, False),
                            ("Q:/test/helper/Unknown.h", False, False),
                        ):
                            path = Path(spelling)
                            self.assertEqual(tidy.is_winui_path(path), frontend, spelling)
                            self.assertEqual(winuitidy.is_winui_path(path), frontend, spelling)
                            self.assertEqual(tidy.requires_winui_compile_context(path), context, spelling)
                # Ending a view's metadata lifetime must not leave cached alias admission.
                environment.clear()
                self.assertFalse(tidy.is_winui_path(Path("Q:/app/windows-winui/App.h")))
                self.assertFalse(tidy.requires_winui_compile_context(Path("Q:/app/windows-winui/App.h")))


if __name__ == "__main__":
    unittest.main()
