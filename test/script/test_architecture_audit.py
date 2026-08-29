"""Tests for the declarative application architecture audit."""

import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

from ao.core.paths import PROJECT_ROOT


class ArchitectureAuditTest(unittest.TestCase):
    def run_leaf_guardrail(self, script: str, files: dict[str, str]) -> subprocess.CompletedProcess[str]:
        cmake = shutil.which("cmake")
        if cmake is None:
            self.skipTest("cmake is not on PATH")

        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            for relative, source in files.items():
                path = root / relative
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text(source, encoding="utf-8")

            return subprocess.run(
                [
                    cmake,
                    f"-DROOT={root.as_posix()}",
                    "-P",
                    str(PROJECT_ROOT / "cmake" / script),
                ],
                capture_output=True,
                text=True,
                encoding="utf-8",
                errors="replace",
                check=False,
            )

    def test_rule_examples_are_adjudicated_outside_configure(self):
        cmake = shutil.which("cmake")
        if cmake is None:
            self.skipTest("cmake is not on PATH")

        result = subprocess.run(
            [
                cmake,
                f"-DAOBUS_SOURCE_DIR={PROJECT_ROOT}",
                "-DAOBUS_ARCHITECTURE_AUDIT_SELF_TEST=ON",
                "-P",
                str(PROJECT_ROOT / "app" / "cmake" / "ArchitectureAudit.cmake"),
            ],
            capture_output=True,
            text=True,
            encoding="utf-8",
            errors="replace",
            check=False,
        )

        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("Application architecture rule self-test passed", result.stdout)

    def test_audit_reports_all_violations_from_a_governed_source_tree(self):
        cmake = shutil.which("cmake")
        if cmake is None:
            self.skipTest("cmake is not on PATH")

        with tempfile.TemporaryDirectory() as temp_dir:
            source_root = Path(temp_dir)
            for relative_dir in (
                "app/include/ao/desktop",
                "app/include/ao/uimodel",
                "app/uimodel",
                "app/desktop",
                "app/runtime",
                "app/linux-gtk",
                "app/windows-winui",
                "app/tui",
                "app/cli",
                "include/ao/yaml",
                "lib",
                "test",
            ):
                (source_root / relative_dir).mkdir(parents=True, exist_ok=True)

            for relative_file in (
                "app/include/ao/rt/TrackField.h",
                "app/include/ao/rt/TrackPresentation.h",
                "app/include/ao/rt/completion/CompletionItem.h",
                "include/ao/audio/BackendProvider.h",
            ):
                path = source_root / relative_file
                path.parent.mkdir(parents=True, exist_ok=True)
                path.touch()

            violation = source_root / "app/tui/Violation.cpp"
            violation.write_text("#include <ao/rt/CoreRuntime.h>\n", encoding="utf-8")
            managed_state_violation = source_root / "app/tui/ManagedState.def"
            managed_state_violation.write_text("#include <ao/yaml/Reflect.h>\n", encoding="utf-8")
            suffix_violation = source_root / "app/tui/Unsupported.cc"
            suffix_violation.write_text("// unsupported suffix\n", encoding="utf-8")

            result = subprocess.run(
                [
                    cmake,
                    f"-DAOBUS_SOURCE_DIR={source_root.as_posix()}",
                    "-P",
                    str(PROJECT_ROOT / "app" / "cmake" / "ArchitectureAudit.cmake"),
                ],
                capture_output=True,
                text=True,
                encoding="utf-8",
                errors="replace",
                check=False,
            )

        output = result.stdout + result.stderr
        self.assertNotEqual(result.returncode, 0, output)
        self.assertIn("Application architecture audit found 3 violation", output)
        self.assertIn("frontend_core: app/tui/Violation.cpp", output)
        self.assertIn("managed_state_mechanism: app/tui/ManagedState.def", output)
        self.assertIn("unsupported_cpp_suffix: app/tui/Unsupported.cc", output)

    def test_gtk_leaf_guardrail_keeps_runtime_unpacking_at_registrations(self):
        rejected = self.run_leaf_guardrail(
            "AssertGtkLeafCapabilities.cmake",
            {"layout/component/track/Leaf.cpp": "void build(ao::rt::AppRuntime& runtime);\n"},
        )
        output = rejected.stdout + rejected.stderr
        self.assertNotEqual(rejected.returncode, 0, output)
        self.assertIn("layout/component/track/Leaf.cpp", output)
        self.assertIn("high-authority dependency", output)

        allowed = self.run_leaf_guardrail(
            "AssertGtkLeafCapabilities.cmake",
            {
                "layout/component/track/TrackRegistrations.cpp": "void registerAll(ao::rt::AppRuntime& runtime);\n",
                "layout/component/track/Leaf.cpp": "void build(ao::rt::PlaybackService& playback);\n",
            },
        )
        self.assertEqual(allowed.returncode, 0, allowed.stdout + allowed.stderr)

    def test_winui_leaf_guardrail_scans_adapters_beyond_component_directories(self):
        rejected = self.run_leaf_guardrail(
            "AssertWinUiLeafCapabilities.cmake",
            {"playback/TransportButton.cpp": "void build(ao::winui::LibrarySession& session);\n"},
        )
        output = rejected.stdout + rejected.stderr
        self.assertNotEqual(rejected.returncode, 0, output)
        self.assertIn("playback/TransportButton.cpp", output)
        self.assertIn("high-authority dependency", output)

        allowed = self.run_leaf_guardrail(
            "AssertWinUiLeafCapabilities.cmake",
            {
                "layout/ShellBuilder.cpp": "void build(ao::rt::AppRuntime& runtime);\n",
                "playback/TransportButton.cpp": "void build(ao::rt::PlaybackService& playback);\n",
            },
        )
        self.assertEqual(allowed.returncode, 0, allowed.stdout + allowed.stderr)


if __name__ == "__main__":
    unittest.main()
