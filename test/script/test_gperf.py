"""Tests for gperf header generation."""

import json
import os
import re
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

from ao.core.paths import PROJECT_ROOT


class GperfCMakeTest(unittest.TestCase):
    def run_checked(self, command: list[str]) -> subprocess.CompletedProcess[str]:
        result = subprocess.run(
            command,
            capture_output=True,
            text=True,
            encoding="utf-8",
            errors="replace",
            check=False,
        )
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        return result

    def write_fixture(self, source: Path, gperf: str) -> Path:
        nested = source / "components" / "nested generator"
        cmake_dir = source / "cmake"
        nested.mkdir(parents=True)
        cmake_dir.mkdir()
        shutil.copyfile(PROJECT_ROOT / "cmake/Gperf.cmake", cmake_dir / "Gperf.cmake")
        shutil.copyfile(PROJECT_ROOT / "cmake/RunGperf.cmake", cmake_dir / "RunGperf.cmake")

        (source / "CMakeLists.txt").write_text(
            "cmake_minimum_required(VERSION 3.25)\n"
            "project(GperfFixture LANGUAGES CXX)\n"
            f"set(GPERF_EXECUTABLE [==[{Path(gperf).as_posix()}]==])\n"
            "include(cmake/Gperf.cmake)\n"
            'add_subdirectory("components/nested generator")\n',
            encoding="utf-8",
        )
        (nested / "CMakeLists.txt").write_text(
            "add_gperf_header(words_header\n"
            '  "${CMAKE_CURRENT_SOURCE_DIR}/words input.gperf"\n'
            '  "fixture/Words.h"\n'
            ")\n"
            'add_executable(gperf_probe main.cpp "${words_header}")\n'
            'target_include_directories(gperf_probe PRIVATE "${CMAKE_BINARY_DIR}")\n'
            "set_target_properties(gperf_probe PROPERTIES\n"
            '  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"\n'
            ")\n",
            encoding="utf-8",
        )
        (nested / "main.cpp").write_text(
            '#include "generated/fixture/Words.h"\n'
            "#include <cstring>\n"
            "#include <iostream>\n"
            "\n"
            "int main()\n"
            "{\n"
            '  auto const* entry = fixture::Table::lookup("alpha", std::strlen("alpha"));\n'
            "  if (entry == nullptr)\n"
            "    return 1;\n"
            "  std::cout << entry->value << '\\n';\n"
            "}\n",
            encoding="utf-8",
        )
        gperf_input = nested / "words input.gperf"
        gperf_input.write_text(
            "%language=C++\n"
            "%readonly-tables\n"
            "%compare-strncmp\n"
            "%compare-lengths\n"
            "%struct-type\n"
            "%{\n"
            "#include <cstring>\n"
            "namespace fixture {\n"
            "%}\n"
            "%define class-name Table\n"
            "%define lookup-function-name lookup\n"
            "struct Entry\n"
            "{\n"
            "  char const* name;\n"
            "  int value;\n"
            "};\n"
            "%%\n"
            "alpha, 1\n"
            "%%\n"
            "} // namespace fixture\n",
            encoding="utf-8",
        )
        return gperf_input

    def configure_and_build(self, cmake: str, compiler: str, source: Path, build: Path) -> bytes:
        self.run_checked(
            [
                cmake,
                "-S",
                str(source),
                "-B",
                str(build),
                "-G",
                "Ninja",
                "-DCMAKE_EXPORT_COMPILE_COMMANDS=ON",
                f"-DCMAKE_CXX_COMPILER={Path(compiler).as_posix()}",
            ]
        )
        self.run_checked([cmake, "--build", str(build), "--target", "gperf_probe"])
        return (build / "generated/fixture/Words.h").read_bytes()

    def run_probe(self, build: Path) -> str:
        executable = build / "bin" / ("gperf_probe.exe" if os.name == "nt" else "gperf_probe")
        return self.run_checked([str(executable)]).stdout.strip()

    def require_build_tools(self) -> tuple[str, str, str]:
        cmake = shutil.which("cmake")
        ninja = shutil.which("ninja")
        gperf = shutil.which("gperf")
        compiler = next((shutil.which(name) for name in ("c++", "clang++", "g++", "cl") if shutil.which(name)), None)
        if cmake is None:
            self.skipTest("cmake is not on PATH")
        if ninja is None:
            self.skipTest("Ninja is not on PATH")
        if gperf is None:
            self.skipTest("gperf is not on PATH")
        if compiler is None:
            self.skipTest("a C++ compiler is not on PATH")

        return cmake, gperf, compiler

    def test_ninja_uses_relocatable_gperf_provenance_and_tracks_input_changes(self):
        cmake, gperf, compiler = self.require_build_tools()

        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            first_source = root / "first workspace" / "source tree"
            first_build = root / "first workspace" / "build tree"
            second_source = root / "second workspace" / "source tree"
            second_build = root / "second workspace" / "build tree"
            first_input = self.write_fixture(first_source, gperf)
            self.write_fixture(second_source, gperf)

            first_header = self.configure_and_build(cmake, compiler, first_source, first_build)
            second_header = self.configure_and_build(cmake, compiler, second_source, second_build)

            self.assertEqual(first_header, second_header)
            self.assertEqual(self.run_probe(first_build), "1")

            commands = json.loads((first_build / "compile_commands.json").read_text(encoding="utf-8"))
            compile_entry = next(entry for entry in commands if Path(entry["file"]).name == "main.cpp")
            compiler_working_directory = Path(compile_entry["directory"])
            self.assertEqual(compiler_working_directory.resolve(), first_build.resolve())

            line_paths = re.findall(rb'^#line \d+ "([^"]+)"', first_header, flags=re.MULTILINE)
            self.assertTrue(line_paths)
            for encoded_path in line_paths:
                line_path = Path(encoded_path.decode("utf-8"))
                self.assertFalse(line_path.is_absolute())
                self.assertIn(" ", str(line_path))
                self.assertEqual((compiler_working_directory / line_path).resolve(), first_input.resolve())

            generated_header = first_build / "generated/fixture/Words.h"
            os.utime(generated_header, ns=(1_000_000_000, 1_000_000_000))
            updated_input = first_input.read_text(encoding="utf-8").replace("alpha, 1", "alpha, 2")
            first_input.write_text(updated_input, encoding="utf-8")

            self.run_checked([cmake, "--build", str(first_build), "--target", "gperf_probe"])

            self.assertNotEqual(generated_header.read_bytes(), first_header)
            self.assertEqual(self.run_probe(first_build), "2")

    @unittest.skipIf(os.name == "nt", "directory symlinks require Windows privileges")
    def test_ninja_resolves_provenance_from_the_physical_build_directory(self):
        cmake, gperf, compiler = self.require_build_tools()

        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir).resolve()
            physical_parent = root / "physical" / "nested"
            physical_parent.mkdir(parents=True)
            alias = root / "alias"
            alias.symlink_to(physical_parent, target_is_directory=True)

            for source_layout in ("external", "common-alias", "physical-alias", "lexical-alias"):
                with self.subTest(source_layout=source_layout):
                    source_parent = alias if source_layout == "common-alias" else root
                    source = source_parent / f"workspace-{source_layout}"
                    gperf_input = self.write_fixture(source, gperf)
                    build = alias / f"build-{source_layout}"
                    build.mkdir()
                    shared_source = source_layout in {"physical-alias", "lexical-alias"}
                    if shared_source:
                        alias_parent = build.resolve() if source_layout == "physical-alias" else build
                        source_alias = alias_parent / "source"
                        source_alias.symlink_to(source, target_is_directory=True)
                        source = source_alias

                    header = self.configure_and_build(cmake, compiler, source, build)

                    self.assertEqual(self.run_probe(build), "1")
                    line_paths = re.findall(rb'^#line \d+ "([^"]+)"', header, flags=re.MULTILINE)
                    self.assertTrue(line_paths)
                    for encoded_path in line_paths:
                        line_path = Path(encoded_path.decode("utf-8"))
                        self.assertFalse(line_path.is_absolute())
                        self.assertEqual((build.resolve() / line_path).resolve(), gperf_input.resolve())
                        if shared_source:
                            self.assertEqual(line_path.parts[0], "source")
                        elif source_layout == "common-alias":
                            self.assertEqual(
                                line_path.as_posix(),
                                "../workspace-common-alias/components/nested generator/words input.gperf",
                            )


if __name__ == "__main__":
    unittest.main()
