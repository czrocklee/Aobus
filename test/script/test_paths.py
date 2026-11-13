"""Tests for filesystem-safe path normalization."""

import re
import unittest
from pathlib import Path
from unittest import mock

from ao.core import bazel_lint_action, paths


class AbsolutePathTest(unittest.TestCase):
    def test_windows_uses_lexical_abspath_when_filesystem_resolution_fails(self):
        candidate = Path("Y:/repo/script/ao/core/paths.py")
        lexical = r"Y:\repo\script\ao\core\paths.py"

        with mock.patch.object(paths.os.path, "abspath", return_value=lexical) as abspath:
            with mock.patch.object(Path, "resolve", side_effect=OSError(1005, "unrecognized volume")) as resolve:
                result = paths.absolute_path(candidate, os_name="nt")

        self.assertEqual(result, Path(lexical))
        abspath.assert_called_once_with(candidate)
        resolve.assert_not_called()

    def test_non_windows_preserves_filesystem_resolution(self):
        candidate = Path("repo/script/ao/core/paths.py")
        resolved = Path("/canonical/repo/script/ao/core/paths.py")

        with mock.patch.object(Path, "resolve", return_value=resolved) as resolve:
            result = paths.absolute_path(candidate, os_name="posix")

        self.assertEqual(result, resolved)
        resolve.assert_called_once_with()


class DriveRootFilterTest(unittest.TestCase):
    def test_project_filters_accept_both_separators_at_a_windows_drive_root(self):
        for mode, folder in (("STRICT", "lib"), ("RELAXED", "test"), ("ANALYZER", "lib")):
            expression = bazel_lint_action._header_filter(
                bazel_lint_action.Identity(
                    "@@//lib:lib",
                    "bazel-out/k8-dbg/bin",
                    "lib/Foo.cpp",
                    "lib/Foo.cpp",
                    ("bazel-out/k8-dbg/bin/lib/_objs/Foo.o",),
                    mode,
                    "windows",
                ),
                {"header_filter": None},
                (Path("Y:/"),),
                {},
            )
            for path in (f"Y:/{folder}/Foo.cpp", f"Y:\\{folder}\\Foo.cpp"):
                with self.subTest(mode=mode, path=path):
                    self.assertIsNotNone(re.search(expression, path))
            self.assertIsNone(re.search(expression, "Y:/outside/Foo.cpp"))


if __name__ == "__main__":
    unittest.main()
