"""Execute WinUI validation with compile-free artifact/probe stand-ins.

Native File API checks own graph isolation. A narrow source guard covers shipping
POST_BUILD copy ownership, which the codemodel does not serialize.
"""

import os
import re
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

from cmake_support import cmake_executable

from ao.core import winuitidy
from ao.core.paths import PROJECT_ROOT

VALIDATION_CMAKE = PROJECT_ROOT / "app" / "windows-winui" / "Validation.cmake"

PRODUCT_TARGET = "aobus-winui"
LIBRARY_TARGET = "aobus-winui-lib"
CHECK_TARGET = "aobus-winui-check"

# The only probes the aggregate executes unattended, in execution order; every
# other probe is a desktop-session GUI binary that must never run from a build.
CONSOLE_PROBES = ("ao_winui_track_item_probe", "ao_winui_localization_probe")

# The probe inventory is the same contract as the lint-context aggregate: every
# project the Python side can query is owned by the check aggregate.
PROBES = tuple(sorted({Path(project).stem for project in winuitidy._AUXILIARY_SOURCE_PROJECTS.values()}))


def deployed_path(target: str, name: str) -> str:
    """Return the repository's exact deployment expression for a build output."""
    return f"$<TARGET_FILE_DIR:{target}>/{name}"


# Per-probe deployment resources as (source target, source file, destination
# file): the library emits the root-relative XBF files at its output root, the
# merged application PRI lives beside the shipping executable, and each probe
# PRI is renamed after its own executable. The expected map is this test's
# oracle for the aggregate's own deployment copies; the console probes execute
# from their own build outputs and take no deployed resources.
PROBE_DEPLOYMENT_RESOURCES = {
    "ao_winui_soul_probe": (
        (LIBRARY_TARGET, "App.xbf", "App.xbf"),
        (PRODUCT_TARGET, "Aobus.pri", "ao_winui_soul_probe.pri"),
    ),
    "ao_winui_track_table_probe": (
        (LIBRARY_TARGET, "App.xbf", "App.xbf"),
        (LIBRARY_TARGET, "MainWindow.xbf", "MainWindow.xbf"),
        (PRODUCT_TARGET, "Aobus.pri", "ao_winui_track_table_probe.pri"),
    ),
    "ao_winui_activity_status_probe": (
        (LIBRARY_TARGET, "App.xbf", "App.xbf"),
        (LIBRARY_TARGET, "MainWindow.xbf", "MainWindow.xbf"),
        (PRODUCT_TARGET, "Aobus.pri", "ao_winui_activity_status_probe.pri"),
    ),
    "ao_winui_automation_probe": (
        (LIBRARY_TARGET, "App.xbf", "App.xbf"),
        (LIBRARY_TARGET, "MainWindow.xbf", "MainWindow.xbf"),
        (PRODUCT_TARGET, "Aobus.pri", "ao_winui_automation_probe.pri"),
    ),
    "ao_winui_credits_probe": (
        (LIBRARY_TARGET, "App.xbf", "App.xbf"),
        (PRODUCT_TARGET, "Aobus.pri", "ao_winui_credits_probe.pri"),
    ),
    "ao_winui_quality_tooltip_probe": (
        (LIBRARY_TARGET, "App.xbf", "App.xbf"),
        (PRODUCT_TARGET, "Aobus.pri", "ao_winui_quality_tooltip_probe.pri"),
    ),
}

# Distinct fixture bytes per resource, so every deployed copy is verified by content.
FIXTURE_RESOURCE_BYTES = {
    (LIBRARY_TARGET, "App.xbf"): b"fixture compiled application XBF",
    (LIBRARY_TARGET, "MainWindow.xbf"): b"fixture compiled shell XBF",
    (PRODUCT_TARGET, "Aobus.pri"): b"fixture merged application PRI",
}


class WinUiShippingDeploymentTest(unittest.TestCase):
    def test_shipping_post_build_owns_only_the_two_product_xbf_copies(self):
        source = (VALIDATION_CMAKE.parent / "CMakeLists.txt").read_text(encoding="utf-8")
        blocks = re.findall(
            r"add_custom_command\(\s*TARGET\s+aobus-winui\s+POST_BUILD\b(.*?)\bVERBATIM\s*\)", source, re.S
        )
        self.assertEqual(len(blocks), 1)
        copies = re.findall(
            r'COMMAND\s+"\$\{CMAKE_COMMAND\}"\s+-E\s+copy_if_different\s+"([^"]+)"\s+"([^"]+)"', blocks[0]
        )
        self.assertEqual(
            copies,
            [
                (deployed_path(LIBRARY_TARGET, name), deployed_path(PRODUCT_TARGET, name))
                for name in ("App.xbf", "MainWindow.xbf")
            ],
        )
        self.assertEqual(len(re.findall(r"\bCOMMAND\b", blocks[0])), len(copies))


class WinUiValidationModuleTest(unittest.TestCase):
    """The actual Validation.cmake, executed by an isolated LANGUAGES NONE CMake fixture."""

    @classmethod
    def setUpClass(cls) -> None:
        cls.cmake = cmake_executable()

    def setUp(self) -> None:
        temporary = tempfile.TemporaryDirectory(prefix="aobus winui check ")
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name).resolve()
        self.source = self.root / "source"
        self.build = self.root / "build"
        self.product_directory = self.root / "out" / "product"
        self.library_directory = self.root / "out" / "library"
        # The log is the test's own observation file, not a fake build output;
        # every "out/" artifact below is created only by a utility producer.
        self.console_log = self.root / "console.log"

    def probe_directory(self, probe: str) -> Path:
        return self.root / "out" / probe

    def probe_executable(self, probe: str) -> Path:
        # The aggregate invokes the probe file directly, so the stand-in uses the
        # platform's script launcher form instead of a compiled binary.
        return self.probe_directory(probe) / f"{probe}{'.cmd' if os.name == 'nt' else ''}"

    def owner_directory(self, owner: str) -> Path:
        return self.library_directory if owner == LIBRARY_TARGET else self.product_directory

    def write_stub(self, probe: str, exit_code: int) -> None:
        """Require completed deployment before logging execution and exiting."""
        expected_resources = {
            str(self.probe_directory(target) / destination): FIXTURE_RESOURCE_BYTES[owner, name]
            for target, copies in PROBE_DEPLOYMENT_RESOURCES.items()
            for owner, name, destination in copies
        }
        script = (
            "import sys\nfrom pathlib import Path\n"
            f"for path, payload in {expected_resources!r}.items():\n"
            "    assert Path(path).read_bytes() == payload\n"
            f"with open({str(self.console_log)!r}, 'a', encoding='utf-8') as stream:\n"
            f"    stream.write({probe!r} + '\\n')\n"
            f"sys.exit({exit_code})\n"
        )
        launcher = self.source / f"stub-{probe}{'.cmd' if os.name == 'nt' else ''}"
        if os.name == "nt":
            script_path = self.source / f"stub-{probe}.py"
            script_path.write_text(script, encoding="utf-8")
            launcher.write_text(f'@"{sys.executable}" "{script_path}" %*\n', encoding="utf-8")
        else:
            launcher.write_text(f"#!{sys.executable}\n{script}", encoding="utf-8")
            launcher.chmod(0o755)

    def append_stand_in(
        self, lines: list[str], target: str, declaration: str, location: Path, commands: list[str]
    ) -> None:
        """Declare an imported stand-in whose utility producer fabricates its build outputs."""
        producer = f"fixture-produce-{target}"
        lines.append(declaration)
        lines.append(f'set_target_properties({target} PROPERTIES IMPORTED_LOCATION "{location.as_posix()}")')
        lines.append(f"add_custom_target({producer}")
        lines.append(f'  COMMAND "${{CMAKE_COMMAND}}" -E make_directory "{location.parent.as_posix()}"')
        lines.extend(commands)
        lines += ["  VERBATIM)", f"add_dependencies({target} {producer})", ""]

    def run_aggregate(self, *, failing_probe: str | None = None) -> subprocess.CompletedProcess[str]:
        """Build only the check aggregate of the actual Validation.cmake in the fixture project."""
        self.source.mkdir(parents=True)
        for (_owner, name), payload in FIXTURE_RESOURCE_BYTES.items():
            (self.source / name).write_bytes(payload)
        lines = [
            "cmake_minimum_required(VERSION 3.30)",
            "project(AobusWinUiCheckFixture LANGUAGES NONE)",
        ]
        # Imported stand-ins anchor the aggregate's target expressions. Each one
        # carries its own utility producer, and CMake follows imported-target
        # dependencies transitively: building only the aggregate prepares
        # exactly the artifacts its own DEPENDS declares, nothing more.
        resource_copies = {
            owner: [
                f'  COMMAND "${{CMAKE_COMMAND}}" -E copy "{(self.source / name).as_posix()}"'
                f' "{(self.owner_directory(owner) / name).as_posix()}"'
                for copy_owner, name in FIXTURE_RESOURCE_BYTES
                if copy_owner == owner
            ]
            for owner in (LIBRARY_TARGET, PRODUCT_TARGET)
        }
        self.append_stand_in(
            lines,
            PRODUCT_TARGET,
            "add_executable(aobus-winui IMPORTED)",
            self.product_directory / "Aobus.exe",
            [f'  COMMAND "${{CMAKE_COMMAND}}" -E touch "{(self.product_directory / "Aobus.exe").as_posix()}"']
            + resource_copies[PRODUCT_TARGET],
        )
        self.append_stand_in(
            lines,
            LIBRARY_TARGET,
            # The aggregate only consumes the library's output directory, and
            # CMake follows DEPENDS entries on imported executables (including
            # their producers); an imported library stand-in would be ignored.
            "add_executable(aobus-winui-lib IMPORTED)",
            self.library_directory / "aobus-winui-lib.lib",
            [f'  COMMAND "${{CMAKE_COMMAND}}" -E touch "{(self.library_directory / "aobus-winui-lib.lib").as_posix()}"']
            + resource_copies[LIBRARY_TARGET],
        )
        # The real product builds the library first; model that edge so the
        # aggregate's DEPENDS on the product alone prepares the library's XBF
        # outputs, exactly as in the shipping graph. Fixture-only: it does not
        # claim proof of the real shipping graph's probe isolation.
        lines.append(f"add_dependencies({PRODUCT_TARGET} {LIBRARY_TARGET})")
        for probe in PROBES:
            self.write_stub(probe, 3 if probe == failing_probe else 0)
            launcher = self.source / f"stub-{probe}{'.cmd' if os.name == 'nt' else ''}"
            destination = self.probe_executable(probe)
            self.append_stand_in(
                lines,
                probe,
                f"add_executable({probe} IMPORTED)",
                destination,
                [f'  COMMAND "${{CMAKE_COMMAND}}" -E copy "{launcher.as_posix()}" "{destination.as_posix()}"'],
            )
        # A plain include of the actual module: a missing module or an
        # unresolved target is a configure error that fails this test, never an
        # optional skip.
        lines.append(f'include("{VALIDATION_CMAKE.as_posix()}")')
        (self.source / "CMakeLists.txt").write_text("\n".join(lines), encoding="utf-8")
        configure = subprocess.run(
            [self.cmake, "-S", str(self.source), "-B", str(self.build)],
            capture_output=True,
            text=True,
            check=False,
        )
        self.assertEqual(configure.returncode, 0, configure.stdout + configure.stderr)
        # --config Debug selects the configuration for multi-config generators
        # such as Visual Studio; single-config generators ignore it.
        return subprocess.run(
            [self.cmake, "--build", str(self.build), "--target", CHECK_TARGET, "--config", "Debug"],
            capture_output=True,
            text=True,
            check=False,
        )

    def test_building_the_aggregate_prepares_every_dependency_and_deploys_exact_probe_resources(self):
        build = self.run_aggregate()
        self.assertEqual(build.returncode, 0, build.stdout + build.stderr)
        # Every probe stub logs its execution: only the two console probes run,
        # and they run in aggregate order; no GUI probe executes from a build.
        self.assertEqual(self.console_log.read_text(encoding="utf-8").splitlines(), list(CONSOLE_PROBES))
        # Building only the aggregate prepared every direct dependency's artifact
        # through the aggregate's own DEPENDS: the product, the library, and all
        # eight probes.
        for artifact in (
            self.product_directory / "Aobus.exe",
            self.library_directory / "aobus-winui-lib.lib",
            *(self.probe_executable(probe) for probe in PROBES),
        ):
            self.assertTrue(artifact.is_file(), f"the aggregate did not prepare {artifact}")
        # Each probe directory holds exactly its stub and its expected resources,
        # byte-identical to the producing artifact.
        for probe in PROBES:
            directory = self.probe_directory(probe)
            expected = {self.probe_executable(probe).name}
            expected.update(destination for _, _, destination in PROBE_DEPLOYMENT_RESOURCES.get(probe, ()))
            self.assertEqual({path.name for path in directory.iterdir()}, expected)
            for source, source_file, destination_file in PROBE_DEPLOYMENT_RESOURCES.get(probe, ()):
                deployed = directory / destination_file
                self.assertEqual(deployed.read_bytes(), FIXTURE_RESOURCE_BYTES[(source, source_file)])
        # The shipping outputs keep only their own artifacts; the aggregate never copies into them.
        self.assertEqual({path.name for path in self.product_directory.iterdir()}, {"Aobus.exe", "Aobus.pri"})
        self.assertEqual(
            {path.name for path in self.library_directory.iterdir()},
            {"aobus-winui-lib.lib", "App.xbf", "MainWindow.xbf"},
        )

    def test_a_failing_first_console_probe_fails_the_build_without_running_the_second(self):
        build = self.run_aggregate(failing_probe=CONSOLE_PROBES[0])
        self.assertNotEqual(build.returncode, 0)
        # Fail-fast: the first console probe's failure stops the aggregate
        # before the second one ever executes.
        self.assertEqual(self.console_log.read_text(encoding="utf-8").splitlines(), list(CONSOLE_PROBES[:1]))

    def test_a_failing_last_console_probe_fails_the_aggregate_build(self):
        build = self.run_aggregate(failing_probe=CONSOLE_PROBES[-1])
        self.assertNotEqual(build.returncode, 0)
        # Both console probes were reached in order; the second one failed.
        self.assertEqual(self.console_log.read_text(encoding="utf-8").splitlines(), list(CONSOLE_PROBES))


if __name__ == "__main__":
    unittest.main()
