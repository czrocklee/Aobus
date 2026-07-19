"""Tests for class/file role naming audit."""

import tempfile
import unittest
from pathlib import Path

from ao.core import nameaudit


class NameAuditTest(unittest.TestCase):
    def test_accepts_role_names_in_expected_layers(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            files = {
                "app/include/ao/uimodel/FooViewModel.h": "class FooViewModel final {};",
                "app/runtime/FooService.cpp": "class FooService final {};",
                "app/linux-gtk/FooController.h": "class FooController final {};",
                "app/linux-gtk/layout/component/FooComponent.cpp": "class FooComponent final {};",
                "test/unit/runtime/FakeService.cpp": "class FakeService final {};",
                "test/unit/runtime/SpyObserver.cpp": "class SpyObserver final {};",
            }
            for name, source in files.items():
                path = root / name
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text(source, encoding="utf-8")

            issues = nameaudit.audit_paths([], root)

        self.assertEqual(issues, [])

    def test_reports_generic_files_role_drift_and_production_test_doubles(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            files = {
                "app/FooUtils.h": "struct Foo final {};",
                "app/FooHelpers.h": "struct Foo final {};",
                "lib/audio/BadController.cpp": "class BadController final {};",
                "include/ao/FakeBackend.h": "class FakeBackend final {};",
                "include/ao/SpyProbe.h": "class SpyProbe final {};",
            }
            for name, source in files.items():
                path = root / name
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text(source, encoding="utf-8")

            issues = nameaudit.audit_paths([], root)

        self.assertEqual(
            [(issue.kind, issue.message) for issue in issues],
            [
                (
                    "generic-file",
                    "Helpers files must live in test, tool, or a detail implementation area",
                ),
                (
                    "generic-file",
                    "file names must use a concrete domain concept, not Utils/Util/Utility/Types",
                ),
                ("test-double", "Fake/Mock/Spy/Stub types belong in tests"),
                ("test-double", "Fake/Mock/Spy/Stub types belong in tests"),
                (
                    "role-location",
                    "Controller types must live under one of: app/linux-gtk/, app/tui/, "
                    "test/unit/linux-gtk/, test/unit/tui/",
                ),
            ],
        )

    def test_ignores_lint_fixtures(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            fixture = root / "test" / "integration" / "lint" / "fixture" / "aobus" / "BadUtils.h"
            fixture.parent.mkdir(parents=True)
            fixture.write_text("class BadController final {};", encoding="utf-8")

            issues = nameaudit.audit_paths([], root)

        self.assertEqual(issues, [])

    def test_reports_every_test_access_definition_but_not_forward_declarations(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            files = {
                "include/ao/PlaybackTransport.h": "struct PlaybackTransportTestAccess;",
                "app/runtime/TestAccess.cpp": "class TestAccess final {};",
                "test/unit/RuntimeTestSupport.h": "class RuntimeTestAccess final {};",
            }
            for name, source in files.items():
                path = root / name
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text(source, encoding="utf-8")

            issues = nameaudit.audit_paths([], root)

        self.assertEqual(
            [(issue.path.relative_to(root).as_posix(), issue.kind, issue.message) for issue in issues],
            [
                (
                    "app/runtime/TestAccess.cpp",
                    "test-access",
                    "*TestAccess types are banned; use public behavior or inject collaborators "
                    "through a production composition seam",
                ),
                (
                    "test/unit/RuntimeTestSupport.h",
                    "test-access",
                    "*TestAccess types are banned; use public behavior or inject collaborators "
                    "through a production composition seam",
                ),
            ],
        )


if __name__ == "__main__":
    unittest.main()
