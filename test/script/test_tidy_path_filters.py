"""Exact clang-tidy line-filter path spellings across host platforms."""

import json
import subprocess
import tempfile
import unittest
from pathlib import Path, PurePosixPath, PureWindowsPath
from unittest import mock

from ao.command import tidy


class TidyPathLineFilterTest(unittest.TestCase):
    def test_windows_drive_path_preserves_native_and_forward_slashes_through_json(self):
        selected = Path("include/ao/Player.h")
        spelling = PureWindowsPath(r"C:\Music Projects\音楽\include\ao\Player.h")

        with mock.patch.object(tidy, "_source_spellings", return_value=[spelling]):
            payload = tidy.path_line_filter([selected])

        self.assertEqual(
            json.loads(payload),
            [
                {"name": r"C:\Music Projects\音楽\include\ao\Player.h", "lines": [[1, 2_147_483_647]]},
                {"name": "C:/Music Projects/音楽/include/ao/Player.h", "lines": [[1, 2_147_483_647]]},
            ],
        )
        self.assertEqual(json.loads(json.dumps(json.loads(payload), ensure_ascii=False)), json.loads(payload))

    def test_unc_path_preserves_both_exact_full_names(self):
        selected = Path("include/ao/Player.h")
        spelling = PureWindowsPath(r"\\music-server\Shared Music\音楽\include\ao\Player.h")

        with mock.patch.object(tidy, "_source_spellings", return_value=[spelling]):
            entries = json.loads(tidy.path_line_filter([selected]))

        self.assertEqual(
            entries,
            [
                {
                    "name": r"\\music-server\Shared Music\音楽\include\ao\Player.h",
                    "lines": [[1, 2_147_483_647]],
                },
                {"name": "//music-server/Shared Music/音楽/include/ao/Player.h", "lines": [[1, 2_147_483_647]]},
            ],
        )

    def test_owned_and_physical_root_spellings_are_deduplicated_in_first_seen_order(self):
        selected = Path("include/ao/Player.h")
        other_selection = Path("app/include/ao/Player.h")
        roots = (PureWindowsPath("S:/"), PureWindowsPath("C:/Music Projects/音楽"))
        owned = PureWindowsPath(r"S:\include\ao\Player.h")
        physical = PureWindowsPath(r"C:\Music Projects\音楽\include\ao\Player.h")

        with mock.patch.object(
            tidy,
            "_source_spellings",
            side_effect=[[owned, physical, owned], [physical, owned], [owned, physical]],
        ) as spellings:
            entries = json.loads(tidy.path_line_filter([selected, other_selection, selected], source_roots=roots))

        self.assertEqual(
            spellings.call_args_list,
            [mock.call(selected, roots), mock.call(other_selection, roots), mock.call(selected, roots)],
        )
        self.assertEqual(
            entries,
            [
                {"name": r"S:\include\ao\Player.h", "lines": [[1, 2_147_483_647]]},
                {"name": "S:/include/ao/Player.h", "lines": [[1, 2_147_483_647]]},
                {"name": r"C:\Music Projects\音楽\include\ao\Player.h", "lines": [[1, 2_147_483_647]]},
                {"name": "C:/Music Projects/音楽/include/ao/Player.h", "lines": [[1, 2_147_483_647]]},
            ],
        )

    def test_posix_path_keeps_one_unchanged_full_name(self):
        selected = Path("include/ao/Player.h")
        spelling = PurePosixPath("/home/music projects/音楽/include/ao/Player.h")

        with mock.patch.object(tidy, "_source_spellings", return_value=[spelling, spelling]):
            entries = json.loads(tidy.path_line_filter([selected, selected]))

        self.assertEqual(
            entries,
            [{"name": "/home/music projects/音楽/include/ao/Player.h", "lines": [[1, 2_147_483_647]]}],
        )

    def test_posix_literal_backslash_is_not_rewritten_as_a_separator(self):
        selected = Path("include/ao/Player.h")
        spelling = PurePosixPath(r"/home/music/音楽/include/ao/Player\Draft.h")

        with mock.patch.object(tidy, "_source_spellings", return_value=[spelling]):
            entries = json.loads(tidy.path_line_filter([selected]))

        self.assertEqual(
            entries,
            [{"name": r"/home/music/音楽/include/ao/Player\Draft.h", "lines": [[1, 2_147_483_647]]}],
        )

    def test_same_basename_in_another_directory_is_not_selected(self):
        selected = Path("include/ao/Player.h")
        spelling = PureWindowsPath(r"S:\include\ao\Player.h")
        unrelated = PureWindowsPath(r"S:\app\windows-winui\Player.h")

        with mock.patch.object(tidy, "_source_spellings", return_value=[spelling]):
            entries = json.loads(tidy.path_line_filter([selected]))

        self.assertEqual(
            entries,
            [
                {"name": r"S:\include\ao\Player.h", "lines": [[1, 2_147_483_647]]},
                {"name": "S:/include/ao/Player.h", "lines": [[1, 2_147_483_647]]},
            ],
        )
        names = [entry["name"] for entry in entries]
        self.assertNotIn(unrelated.name, names)
        self.assertNotIn(str(unrelated), names)
        self.assertNotIn(unrelated.as_posix(), names)


class SelectedFileOverlayTest(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix="aobus-selected-vfs-")
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        self.selected = self.root / "value.def"
        self.selected.write_bytes(b"inline int* pointer() { return 0; }\n")
        self.consumer = self.root / "value.cpp"
        self.consumer.write_bytes(b'#include "value.def"\n')
        self.overlay = self.root / "selected.json"
        environment = mock.patch.dict(tidy.os.environ, {"CL": "", "_CL_": ""})
        environment.start()
        self.addCleanup(environment.stop)

    def database(self, arguments, *, command=False):
        entry = {"directory": str(self.root), "file": str(self.consumer)}
        entry.update({"command": subprocess.list2cmdline(arguments)} if command else {"arguments": arguments})
        (self.root / "compile_commands.json").write_text(json.dumps([entry]), encoding="utf-8")

    def test_overlay_names_only_validated_aliases_of_the_same_physical_file(self):
        alias = self.root / "alias.def"
        alias.hardlink_to(self.selected)
        original = self.selected.read_bytes()
        with mock.patch.object(tidy, "_source_spellings", return_value=[self.selected, alias, self.selected]):
            result = tidy.write_selected_file_overlay(self.overlay, self.selected, source_roots=(self.root,))
        self.assertEqual(result, self.overlay)
        self.assertEqual(
            json.loads(self.overlay.read_text(encoding="utf-8")),
            {
                "version": 0,
                "case-sensitive": False,
                "use-external-names": True,
                "roots": [
                    {"type": "file", "name": path.as_posix(), "external-contents": str(self.selected)}
                    for path in (self.selected, alias)
                ],
            },
        )
        self.assertTrue(alias.samefile(self.selected))
        self.assertEqual(self.selected.read_bytes(), original)

    def test_windows_drive_and_unc_overlay_serialization_keeps_full_external_names(self):
        # Serialization only: filesystem identity is tested with real hardlinks
        # above; this does not claim access to a native UNC scratch share.
        for spelling in (
            PureWindowsPath(r"C:\Music Projects\音楽\include\value.def"),
            PureWindowsPath(r"\\music-server\Shared Music\音楽\include\value.def"),
        ):
            with (
                self.subTest(spelling=spelling),
                mock.patch.object(tidy, "_source_spellings", return_value=[spelling]),
                mock.patch.object(PureWindowsPath, "is_file", return_value=True, create=True),
                mock.patch.object(PureWindowsPath, "samefile", return_value=True, create=True),
            ):
                tidy.write_selected_file_overlay(self.overlay, self.selected, source_roots=(self.root,))
                root = json.loads(self.overlay.read_text(encoding="utf-8"))["roots"]
                self.assertEqual(
                    root, [{"type": "file", "name": spelling.as_posix(), "external-contents": str(spelling)}]
                )

    def test_missing_relative_and_different_file_aliases_are_not_adopted(self):
        unrelated = self.root / "unrelated.def"
        unrelated.write_bytes(self.selected.read_bytes())
        for alias in (self.root / "missing.def", Path("relative.def"), unrelated):
            with self.subTest(alias=alias), mock.patch.object(tidy, "_source_spellings", return_value=[alias]):
                with self.assertRaisesRegex(ValueError, "does not identify"):
                    tidy.write_selected_file_overlay(self.overlay, self.selected, source_roots=(self.root,))
            self.assertFalse(self.overlay.exists())

    def test_caller_vfs_flags_are_rejected_without_reading_or_shadowing_their_contents(self):
        variants = (
            ["--vfsoverlay=caller.json"],
            ["--vfsoverlay", "caller.json"],
            ["-vfsoverlay=caller.json"],
            ["-vfsoverlay", "caller.json"],
            ["-ivfsoverlay", "caller.json"],
            ["-ivfsoverlay=caller.json"],
            ["-ivfsoverlaycaller.json"],
            ["/vfsoverlay", "caller.json"],
            ["/vfsoverlay=caller.json"],
            ["/ivfsoverlay=caller.json"],
            ["/clang:-ivfsoverlay", "/clang:caller.json"],
            ["/clang:/vfsoverlay", "/clang:caller.json"],
            ["--extra-arg=-ivfsoverlay=caller.json"],
            ["--extra-arg=/vfsoverlay=caller.json"],
            ["-extra-arg-before=/clang:-ivfsoverlaycaller.json"],
        )
        for options in variants:
            for command in (False, True):
                with self.subTest(options=options, command=command):
                    self.database(["clang-cl", *options, str(self.consumer)], command=command)
                    with self.assertRaisesRegex(ValueError, "caller VFS"):
                        tidy._validate_selected_vfs_inputs(self.root, {self.consumer}, [])
            self.database(["clang-cl", str(self.consumer)])
            with self.subTest(tool_arguments=options), self.assertRaisesRegex(ValueError, "caller VFS"):
                tidy._validate_selected_vfs_inputs(self.root, {self.consumer}, options)

    def test_response_and_config_files_cannot_hide_caller_overlays(self):
        # clang-tidy and the clang driver accept the single-dash -config
        # spellings too; the governed invocation's -config stays mandatory.
        for options in (
            ["@compiler.rsp"],
            ["--config=compiler.cfg"],
            ["-config=compiler.cfg"],
            ["--config", "compiler.cfg"],
            ["-config", "compiler.cfg"],
            ["--extra-arg=@compiler.rsp"],
        ):
            with self.subTest(options=options):
                self.database(["clang-cl", str(self.consumer)])
                with self.assertRaisesRegex(ValueError, "response/config"):
                    tidy._validate_selected_vfs_inputs(self.root, {self.consumer}, options)
        self.database(["clang-cl", str(self.consumer)])
        with mock.patch.dict(tidy.os.environ, {"CL": "-ivfsoverlay caller.json"}):
            with self.assertRaisesRegex(ValueError, "caller VFS"):
                tidy._validate_selected_vfs_inputs(self.root, {self.consumer}, [])

    def test_unrelated_flags_and_unselected_compile_contexts_remain_usable(self):
        self.database(["clang-cl", "/Iinclude-vfsoverlay", '/DLEGEND="-ivfsoverlay"', str(self.consumer)])
        path = self.root / "compile_commands.json"
        entries = json.loads(path.read_text(encoding="utf-8"))
        entries.append(
            {
                "directory": str(self.root),
                "file": str(self.root / "other.cpp"),
                "arguments": ["clang-cl", "-ivfsoverlay", "other.json"],
            }
        )
        path.write_text(json.dumps(entries), encoding="utf-8")
        tidy._validate_selected_vfs_inputs(self.root, {self.consumer}, ["--extra-arg=-DNAME=vfsoverlay"])


if __name__ == "__main__":
    unittest.main()
