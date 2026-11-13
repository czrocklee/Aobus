"""Bazel-owned selected-file locations and native compiler-view admission.

The retired CMake selected-file VFS overlay is not an input to these actions.
Declared consumer argv and virtual-header origins now own that responsibility.
"""

import json
import re
import unittest
from pathlib import PurePosixPath, PureWindowsPath

from ao.core import bazel_lint_action as action
from ao.core.bazel_lint_reports import Identity


class TidyPathLineFilterTest(unittest.TestCase):
    def names(self, spellings, *, windows):
        payload = action._line_filter(spellings, windows=windows)
        entries = json.loads(payload)
        self.assertTrue(all(entry["lines"] == [[1, 2_147_483_647]] for entry in entries))
        self.assertEqual(json.loads(json.dumps(entries, ensure_ascii=False)), entries)
        return [entry["name"] for entry in entries]

    def test_windows_drive_path_preserves_native_and_forward_slashes_through_json(self):
        spelling = r"C:\Music Projects\音楽\include\ao\Player.h"
        self.assertEqual(self.names([spelling], windows=True), [spelling, "C:/Music Projects/音楽/include/ao/Player.h"])

    def test_unc_path_preserves_both_exact_full_names(self):
        spelling = r"\\music-server\Shared Music\音楽\include\ao\Player.h"
        self.assertEqual(
            self.names([spelling], windows=True), [spelling, "//music-server/Shared Music/音楽/include/ao/Player.h"]
        )

    def test_owned_and_physical_root_spellings_are_deduplicated_in_first_seen_order(self):
        owned = r"S:\include\ao\Player.h"
        physical = r"C:\Music Projects\音楽\include\ao\Player.h"
        self.assertEqual(
            self.names([owned, physical, owned, physical], windows=True),
            [owned, "S:/include/ao/Player.h", physical, "C:/Music Projects/音楽/include/ao/Player.h"],
        )

    def test_posix_path_keeps_one_unchanged_full_name(self):
        spelling = "/home/music projects/音楽/include/ao/Player.h"
        self.assertEqual(self.names([spelling, spelling], windows=False), [spelling])

    def test_header_filter_preserves_target_root_identity_without_posix_separator_aliases(self):
        for root, windows in (
            (PurePosixPath(r"/home/music\drafts/音楽"), False),
            (PureWindowsPath("C:/Music Projects/音楽"), True),
            (PureWindowsPath("//music-server/Shared Music/音楽"), True),
        ):
            with self.subTest(root=root):
                identity = Identity(
                    "@@//lib:sample",
                    "bazel-out/native/bin",
                    "lib/value.cpp",
                    "include/ao/Player.h",
                    ("bazel-out/native/bin/lib/value.o",),
                    "STRICT",
                    "windows" if windows else "linux",
                )
                expression = action._header_filter(identity, {"header_filter": None}, (root,), {})
                self.assertIsNotNone(re.search(expression, str(root / "include/ao/Player.h")))
                self.assertIsNone(re.search(expression, str(root / "other/Player.h")))
                if not windows:
                    self.assertIsNone(re.search(expression, str(root / "include/ao/Player.h").replace("\\", "/")))
                    self.assertIsNone(re.search(expression, str(root / "INCLUDE/ao/Player.h")))
                    self.assertEqual(
                        action._report_path(str(root / "include/ao/Player.h"), (root,), {}, windows=False),
                        "include/ao/Player.h",
                    )
                    foreign = str(root / "include/ao/Player.h").replace("\\", "/")
                    self.assertEqual(action._report_path(foreign, (root,), {}, windows=False), foreign)

    def test_analyzer_mixed_virtual_directory_does_not_admit_generated_or_unowned_siblings(self):
        identity = Identity(
            "@@//lib:sample",
            "bazel-out/native/bin",
            "lib/value.cpp",
            "lib/value.cpp",
            ("bazel-out/native/bin/lib/value.o",),
            "ANALYZER",
            "windows",
        )
        directory = "bazel-out/native/bin/lib/_virtual_includes/mixed/"
        origins = {
            directory + "Player.h": "include/ao/Player.h",
            directory + "Generated.h": None,
            directory + "Other.h": "tool/other/A.h",
        }
        expression = action._header_filter(identity, {"header_filter": None}, (), origins)
        self.assertIsNotNone(re.search(expression, directory + "Player.h"))
        self.assertIsNone(re.search(expression, directory + "Generated.h"))
        self.assertIsNone(re.search(expression, directory + "Other.h"))

    def test_windows_header_main_filters_fold_ascii_but_keep_unicode_and_full_root_identity(self):
        identity = Identity(
            "@@//lib:sample",
            "bazel-out/native/bin",
            "lib/value.cpp",
            "include/ao/Player.h",
            ("bazel-out/native/bin/lib/value.o",),
            "STRICT",
            "windows",
        )
        for root, alias, foreign in (
            (
                PureWindowsPath("C:/Music Projects/josé Straße"),
                "c:/mUSIC pROJECTS/josé sTRAßE",
                "D:/Music Projects/josé Straße",
            ),
            (
                PureWindowsPath("//music-server/Shared Music/josé Straße"),
                "//MUSIC-SERVER/sHARED mUSIC/josé sTRAßE",
                "//other-server/Shared Music/josé Straße",
            ),
        ):
            with self.subTest(root=root):
                expression = action._header_filter(identity, {"header_filter": None}, (root,), {})
                self.assertNotIn("(?", expression)  # LLVM consumes POSIX ERE.
                for name in (
                    "INCLUDE/AO/PLAYER.H",
                    alias + "/INCLUDE/AO/pLAYER.H",
                    (alias + "/INCLUDE/AO/pLAYER.H").replace("/", "\\"),
                ):
                    self.assertIsNotNone(re.search(expression, name), name)
                for name in (
                    foreign + "/include/ao/Player.h",
                    alias + "-foreign/include/ao/Player.h",
                    alias.replace("é", "É") + "/include/ao/Player.h",
                    alias.replace("ß", "ss") + "/include/ao/Player.h",
                    alias + "/other/Player.h",
                    alias + "/include/ao/Player.hpp",
                ):
                    self.assertIsNone(re.search(expression, name), name)

    def test_windows_scope_filters_fold_ascii_for_owned_folders_and_virtual_headers(self):
        virtual = "bazel-out/native/bin/lib/_virtual_includes/headers/ao/Player.h"
        generated = "bazel-out/native/bin/lib/_virtual_includes/headers/ao/Generated.h"
        for mode, accepted, rejected in (
            ("STRICT", "APP/runtime/A.h", "TEST/unit/A.h"),
            ("RELAXED", "TEST/unit/A.h", "APP/runtime/A.h"),
            ("ANALYZER", "TEST/unit/A.h", "TOOL/other/A.h"),
        ):
            with self.subTest(mode=mode):
                identity = Identity(
                    "@@//lib:sample",
                    "bazel-out/native/bin",
                    "lib/value.cpp",
                    "lib/value.cpp",
                    ("bazel-out/native/bin/lib/value.o",),
                    mode,
                    "windows",
                )
                expression = action._header_filter(
                    identity, {"header_filter": None}, (), {virtual: "include/ao/Player.h", generated: None}
                )
                for name in (accepted, "INCLUDE/AO/PLAYER.H", virtual.upper().replace("/", "\\")):
                    self.assertIsNotNone(re.search(expression, name), name)
                for name in (rejected, generated.upper(), "EXTERNAL/include/ao/Player.h", "other/Player.h"):
                    self.assertIsNone(re.search(expression, name), name)

    def test_same_basename_in_another_directory_is_not_selected(self):
        names = self.names(["include/ao/Player.h"], windows=True)
        self.assertEqual(names, ["include/ao/Player.h", r"include\ao\Player.h"])
        self.assertNotIn("Player.h", names)
        self.assertNotIn("app/windows-winui/Player.h", names)


class SelectedFileContextTest(unittest.TestCase):
    def setUp(self):
        self.identity = Identity(
            "@@//lib:sample",
            "bazel-out/native/bin",
            "lib/value.cpp",
            "lib/value.def",
            ("bazel-out/native/bin/lib/value.o",),
            "STRICT",
            "linux",
        )

    def test_declared_virtual_aliases_map_to_the_selected_checkout_file_without_an_overlay(self):
        virtual = "bazel-out/native/bin/lib/_virtual_includes/sample/value.def"
        origins = action._virtual_headers({"virtual_headers": {virtual: "lib/value.def"}})
        self.assertEqual(action._report_path(virtual, (), origins, windows=False), "lib/value.def")
        self.assertEqual(action._report_path("lib/value.def", (), origins, windows=False), "lib/value.def")
        self.assertEqual(action._report_path("lib/unrelated.def", (), origins, windows=False), "lib/unrelated.def")

    def test_windows_drive_and_unc_report_paths_keep_full_root_identity(self):
        for root in (PureWindowsPath("C:/Music Projects/音楽"), PureWindowsPath("//server/Shared Music/音楽")):
            with self.subTest(root=root):
                self.assertEqual(
                    action._report_path(str(root / "include/value.def"), (root,), {}, windows=True),
                    "include/value.def",
                )
                other = str(root / "other/value.def")
                self.assertEqual(action._report_path(other, (), {}, windows=True), other)

    def test_windows_virtual_diagnostics_and_summaries_share_exact_ascii_root_mapping(self):
        virtual = "bazel-out/native/bin/lib/_virtual_includes/sample/josé/Value.def"
        origins = {virtual: "lib/josé/Value.def"}
        for root, alias, foreign in (
            (
                PureWindowsPath("C:/Music Projects/josé Straße"),
                "c:/mUSIC pROJECTS/josé sTRAßE",
                "D:/Music Projects/josé Straße",
            ),
            (
                PureWindowsPath("//server/Shared Music/josé Straße"),
                "//SERVER/sHARED mUSIC/josé sTRAßE",
                "//server/Other Share/josé Straße",
            ),
        ):
            with self.subTest(root=root):
                # Mixed separators, case-varied virtual directory and basename.
                path = alias.replace("/", "\\") + "/BAZEL-OUT/native/BIN/LIB/_VIRTUAL_INCLUDES/SAMPLE/josé/vALUE.DEF"
                text = f"Error while processing {path}.\n{path}:2:3: error: bad [clang-diagnostic-error]\n"
                self.assertEqual(
                    action._report_output(text, (root,), origins, windows=True),
                    "Error while processing lib/josé/Value.def.\n"
                    "lib/josé/Value.def:2:3: error: bad [clang-diagnostic-error]\n",
                )
                for unrelated in (
                    foreign + "/" + virtual,
                    alias + "-foreign/" + virtual,
                    alias.replace("é", "É") + "/" + virtual,
                    alias.replace("ß", "ss") + "/" + virtual,
                ):
                    self.assertEqual(action._report_path(unrelated, (root,), origins, windows=True), unrelated)
                self.assertEqual(
                    action._report_path(virtual.replace("é", "É"), (root,), origins, windows=True),
                    virtual.replace("é", "É"),
                )
                self.assertEqual(
                    action._report_path("other/Value.def", (root,), origins, windows=True), "other/Value.def"
                )

    def test_fragment_warning_selection_folds_ascii_only_without_losing_errors(self):
        text = (
            "LIB/josé/vALUE.DEF:1:1: warning: selected [aobus-example]\n"
            "lib/josÉ/Value.def:2:1: warning: another file [aobus-example]\n"
            "lib/josÉ/Value.def:3:1: note: continuation of another file\n"
            "lib/josÉ/Value.def:4:1: error: still required [clang-diagnostic-error]\n"
        )
        self.assertEqual(
            action._fragment_output(text, "lib/josé/Value.def", windows=True),
            "LIB/josé/vALUE.DEF:1:1: warning: selected [aobus-example]\n"
            "lib/josÉ/Value.def:4:1: error: still required [clang-diagnostic-error]\n",
        )
        self.assertEqual(
            action._fragment_output(text, "lib/josé/Value.def", windows=False),
            "lib/josÉ/Value.def:4:1: error: still required [clang-diagnostic-error]\n",
        )


if __name__ == "__main__":
    unittest.main()
