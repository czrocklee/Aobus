"""Tests for deterministic header compile-command selection."""

import json
import os
import subprocess
import tempfile
import unittest
from pathlib import Path
from unittest import mock

from ao.core import tidyengine
from ao.core.paths import absolute_path


class HeaderContextFixture:
    def __init__(self) -> None:
        self._temporary = tempfile.TemporaryDirectory()
        self.temporary_root = Path(self._temporary.name)
        self.root = self.temporary_root / "repo"
        self.build_dir = self.temporary_root / "build"
        self.build_dir.mkdir()

    def close(self) -> None:
        self._temporary.cleanup()

    def file(self, relative: str) -> Path:
        path = self.root / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        path.touch()
        return path

    def write_compile_database(
        self,
        *translation_units: Path,
        database_dir: Path | None = None,
        command_directory: Path | None = None,
        relative_files: bool = False,
    ) -> Path:
        destination = database_dir or self.build_dir
        destination.mkdir(parents=True, exist_ok=True)
        directory = command_directory or destination
        entries = [
            {
                "directory": str(directory),
                "file": os.path.relpath(path, directory) if relative_files else str(path),
                "command": f"clang++ -c {path.name}",
            }
            for path in translation_units
        ]
        (destination / "compile_commands.json").write_text(json.dumps(entries), encoding="utf-8")
        return destination

    @staticmethod
    def dependencies(
        *blocks: tuple[Path, list[Path]],
        base: Path,
        state: str = "VALID",
    ) -> str:
        lines: list[str] = []
        for index, (translation_unit, dependencies) in enumerate(blocks):
            lines.append(f"object-{index}.o: #deps {len(dependencies) + 1}, deps mtime 1 ({state})")
            lines.extend(f"    {os.path.relpath(dependency, base)}" for dependency in [translation_unit, *dependencies])
            lines.append("")
        return "\n".join(lines)


class HeaderCompileCommandSelectionTest(unittest.TestCase):
    def setUp(self) -> None:
        self.fixture = HeaderContextFixture()

    def tearDown(self) -> None:
        self.fixture.close()

    def test_same_stem_companion_precedes_dependency_graph(self):
        header = self.fixture.file("include/ao/utility/Value.h")
        companion = self.fixture.file("lib/utility/Value.cpp")
        other_consumer = self.fixture.file("app/cli/PrintValue.cpp")
        (self.fixture.build_dir / "build.ninja").touch()
        self.fixture.write_compile_database(other_consumer, companion)

        with mock.patch.object(tidyengine.subprocess, "run") as run:
            plan = tidyengine.compile_command_plan(
                self.fixture.build_dir,
                [header],
                project_root=self.fixture.root,
            )

        run.assert_not_called()
        self.assertEqual([(target.selected, target.translation_unit) for target in plan.targets], [(header, companion)])

    def test_def_include_fragment_uses_consuming_translation_unit(self):
        fragment = self.fixture.file("app/include/ao/i18n/MessageInventory.def")
        consumer = self.fixture.file("app/i18n/MessageCatalog.cpp")
        (self.fixture.build_dir / "build.ninja").touch()
        self.fixture.write_compile_database(consumer)
        output = self.fixture.dependencies((consumer, [fragment]), base=self.fixture.build_dir)

        with mock.patch.object(
            tidyengine.subprocess,
            "run",
            return_value=subprocess.CompletedProcess(["ninja", "-t", "deps"], 0, output),
        ):
            plan = tidyengine.compile_command_plan(
                self.fixture.build_dir,
                [fragment],
                project_root=self.fixture.root,
            )

        self.assertFalse(plan.targets[0].is_header)
        self.assertTrue(plan.targets[0].is_include_fragment)
        self.assertEqual(
            [(target.selected, target.translation_unit) for target in plan.targets],
            [(fragment, consumer)],
        )

    def test_header_uses_real_consumer_from_ninja_dependency_graph(self):
        header = self.fixture.file("include/ao/utility/HeaderOnly.h")
        consumer = self.fixture.file("lib/utility/Consumer.cpp")
        (self.fixture.build_dir / "build.ninja").touch()
        self.fixture.write_compile_database(consumer)
        output = self.fixture.dependencies((consumer, [header]), base=self.fixture.build_dir)

        with (
            mock.patch.object(
                tidyengine.subprocess,
                "run",
                return_value=subprocess.CompletedProcess(["ninja", "-t", "deps"], 0, output),
            ) as run,
            mock.patch.object(
                tidyengine.buildlock,
                "build_tree_lock",
                wraps=tidyengine.buildlock.build_tree_lock,
            ) as lock,
        ):
            plan = tidyengine.compile_command_plan(
                self.fixture.build_dir,
                [header],
                project_root=self.fixture.root,
            )

        run.assert_called_once_with(
            ["ninja", "-t", "deps"],
            cwd=absolute_path(self.fixture.build_dir),
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            encoding="utf-8",
            errors="replace",
        )
        lock.assert_called_once_with(absolute_path(self.fixture.build_dir))
        self.assertEqual([(target.selected, target.translation_unit) for target in plan.targets], [(header, consumer)])

    def test_fallback_selects_lexicographically_first_consumer_and_reads_graph_once(self):
        first_header = self.fixture.file("include/ao/utility/First.h")
        second_header = self.fixture.file("include/ao/utility/Second.h")
        first_consumer = self.fixture.file("app/cli/AConsumer.cpp")
        second_consumer = self.fixture.file("lib/utility/ZConsumer.cpp")
        (self.fixture.build_dir / "build.ninja").touch()
        self.fixture.write_compile_database(
            second_consumer,
            first_consumer,
            relative_files=True,
        )
        output = self.fixture.dependencies(
            (second_consumer, [first_header, second_header]),
            (first_consumer, [first_header]),
            base=self.fixture.build_dir,
        )

        with mock.patch.object(
            tidyengine.subprocess,
            "run",
            return_value=subprocess.CompletedProcess(["ninja", "-t", "deps"], 0, output),
        ) as run:
            plan = tidyengine.compile_command_plan(
                self.fixture.build_dir,
                [first_header, second_header],
                project_root=self.fixture.root,
            )

        run.assert_called_once()
        self.assertEqual(
            [(target.selected, target.translation_unit) for target in plan.targets],
            [(first_header, first_consumer), (second_header, second_consumer)],
        )

    def test_merged_database_uses_original_ninja_tree(self):
        native_build_dir = self.fixture.temporary_root / "native-build"
        merged_database_dir = self.fixture.temporary_root / "merged"
        native_build_dir.mkdir()
        (native_build_dir / "build.ninja").touch()
        header = self.fixture.file("include/ao/utility/HeaderOnly.h")
        consumer = self.fixture.file("lib/utility/Consumer.cpp")
        self.fixture.write_compile_database(
            consumer,
            database_dir=merged_database_dir,
            command_directory=native_build_dir,
        )
        output = self.fixture.dependencies((consumer, [header]), base=native_build_dir)

        with mock.patch.object(
            tidyengine.subprocess,
            "run",
            return_value=subprocess.CompletedProcess(["ninja", "-t", "deps"], 0, output),
        ) as run:
            plan = tidyengine.compile_command_plan(
                merged_database_dir,
                [header],
                project_root=self.fixture.root,
            )

        self.assertEqual(run.call_args.kwargs["cwd"], absolute_path(native_build_dir))
        self.assertEqual([(target.selected, target.translation_unit) for target in plan.targets], [(header, consumer)])

    def test_additional_tree_proves_consumption_but_primary_database_supplies_command(self):
        dependency_build_dir = self.fixture.temporary_root / "debug"
        dependency_build_dir.mkdir()
        (self.fixture.build_dir / "build.ninja").touch()
        (dependency_build_dir / "build.ninja").touch()
        header = self.fixture.file("include/ao/utility/HeaderOnly.h")
        consumer = self.fixture.file("lib/utility/Consumer.cpp")
        primary_output = self.fixture.build_dir / "object" / "Consumer.cpp.o"
        dependency_output = dependency_build_dir / "object" / "Consumer.cpp.o"
        (self.fixture.build_dir / "compile_commands.json").write_text(
            json.dumps(
                [
                    {
                        "directory": str(self.fixture.build_dir),
                        "file": str(consumer),
                        "output": str(primary_output),
                        "command": f'clang++ -DPRIMARY_TIDY_DB=1 -c "{consumer}"',
                    }
                ]
            ),
            encoding="utf-8",
        )
        (dependency_build_dir / "compile_commands.json").write_text(
            json.dumps(
                [
                    {
                        "directory": str(dependency_build_dir),
                        "file": str(consumer),
                        "output": str(dependency_output),
                        "command": f'clang++ -DNORMAL_DEBUG_DB=1 -c "{consumer}"',
                    }
                ]
            ),
            encoding="utf-8",
        )
        dependency_output_text = (
            f"{os.path.relpath(dependency_output, dependency_build_dir)}: "
            "#deps 1, deps mtime 1 (VALID)\n"
            f"    {os.path.relpath(header, dependency_build_dir)}\n"
        )

        def ninja_deps(_command, *, cwd, **_kwargs):
            output = dependency_output_text if cwd == absolute_path(dependency_build_dir) else ""
            return subprocess.CompletedProcess(["ninja", "-t", "deps"], 0, output)

        with mock.patch.object(tidyengine.subprocess, "run", side_effect=ninja_deps):
            plan = tidyengine.compile_command_plan(
                self.fixture.build_dir,
                [header],
                project_root=self.fixture.root,
                additional_dependency_build_dirs=(dependency_build_dir,),
            )

        destination = self.fixture.temporary_root / "header-db"
        tidyengine.write_header_compile_database(
            self.fixture.build_dir,
            list(plan.targets),
            destination,
        )
        synthetic = json.loads((destination / "compile_commands.json").read_text(encoding="utf-8"))
        self.assertEqual([(target.selected, target.translation_unit) for target in plan.targets], [(header, consumer)])
        self.assertIn("-DPRIMARY_TIDY_DB=1", synthetic[0]["command"])
        self.assertNotIn("-DNORMAL_DEBUG_DB=1", synthetic[0]["command"])

    def test_windows_shaped_output_maps_without_source_dependency_on_any_host(self):
        dependency_build_dir = self.fixture.temporary_root / "windows-debug"
        dependency_build_dir.mkdir()
        (self.fixture.build_dir / "build.ninja").touch()
        (dependency_build_dir / "build.ninja").touch()
        header = self.fixture.file("include/ao/utility/HeaderOnly.h")
        consumer = self.fixture.file("lib/utility/Consumer.cpp")
        primary_output = self.fixture.build_dir / "objects" / "consumer.cpp.obj"
        dependency_output = dependency_build_dir / "objects" / "consumer.cpp.obj"
        for build_dir, output in (
            (self.fixture.build_dir, primary_output),
            (dependency_build_dir, dependency_output),
        ):
            (build_dir / "compile_commands.json").write_text(
                json.dumps(
                    [
                        {
                            "directory": str(build_dir),
                            "file": str(consumer),
                            "output": str(output),
                            "command": f'clang-cl /c "{consumer}"',
                        }
                    ]
                ),
                encoding="utf-8",
            )

        output_spelling = os.path.relpath(dependency_output, dependency_build_dir).upper().replace("/", "\\")
        header_spelling = os.path.relpath(header, dependency_build_dir).upper().replace("/", "\\")
        dependency_output_text = f"{output_spelling}: #deps 1, deps mtime 1 (VALID)\n    {header_spelling}\n"

        def ninja_deps(_command, *, cwd, **_kwargs):
            output = dependency_output_text if cwd == absolute_path(dependency_build_dir) else ""
            return subprocess.CompletedProcess(["ninja", "-t", "deps"], 0, output)

        with mock.patch.object(tidyengine.subprocess, "run", side_effect=ninja_deps):
            plan = tidyengine.compile_command_plan(
                self.fixture.build_dir,
                [header],
                project_root=self.fixture.root,
                additional_dependency_build_dirs=(dependency_build_dir,),
            )

        self.assertEqual([(target.selected, target.translation_unit) for target in plan.targets], [(header, consumer)])
        self.assertEqual(list(plan.deferred), [])

    def test_missing_dependency_data_has_an_explicit_deferral_reason(self):
        header = self.fixture.file("include/ao/utility/HeaderOnly.h")
        consumer = self.fixture.file("lib/utility/Consumer.cpp")
        self.fixture.write_compile_database(consumer)

        plan = tidyengine.compile_command_plan(
            self.fixture.build_dir,
            [header],
            project_root=self.fixture.root,
        )

        self.assertEqual(
            list(plan.deferral_details),
            [
                tidyengine.CompileCommandDeferral(
                    header,
                    "Ninja dependency data is unavailable for the compiled translation units",
                )
            ],
        )

    def test_complete_graph_reports_header_without_a_consumer(self):
        included = self.fixture.file("include/ao/utility/Included.h")
        unused = self.fixture.file("include/ao/utility/Unused.h")
        consumer = self.fixture.file("lib/utility/Consumer.cpp")
        (self.fixture.build_dir / "build.ninja").touch()
        self.fixture.write_compile_database(consumer)
        output = self.fixture.dependencies((consumer, [included]), base=self.fixture.build_dir)

        with mock.patch.object(
            tidyengine.subprocess,
            "run",
            return_value=subprocess.CompletedProcess(["ninja", "-t", "deps"], 0, output),
        ):
            plan = tidyengine.compile_command_plan(
                self.fixture.build_dir,
                [unused],
                project_root=self.fixture.root,
            )

        self.assertEqual(
            list(plan.deferral_details),
            [tidyengine.CompileCommandDeferral(unused, "no compiled translation unit consumes the header")],
        )

    def test_stale_dependency_record_does_not_claim_header_coverage(self):
        header = self.fixture.file("include/ao/utility/HeaderOnly.h")
        consumer = self.fixture.file("lib/utility/Consumer.cpp")
        (self.fixture.build_dir / "build.ninja").touch()
        self.fixture.write_compile_database(consumer)
        output = self.fixture.dependencies(
            (consumer, [header]),
            base=self.fixture.build_dir,
            state="STALE",
        )

        with mock.patch.object(
            tidyengine.subprocess,
            "run",
            return_value=subprocess.CompletedProcess(["ninja", "-t", "deps"], 0, output),
        ):
            plan = tidyengine.compile_command_plan(
                self.fixture.build_dir,
                [header],
                project_root=self.fixture.root,
            )

        self.assertEqual(list(plan.targets), [])
        self.assertEqual(
            plan.deferral_details[0].reason,
            "Ninja dependency data is unavailable for the compiled translation units",
        )


if __name__ == "__main__":
    unittest.main()
