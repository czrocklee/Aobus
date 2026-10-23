"""Real LLVM controls for portal diagnostic delivery and selected-file isolation."""

import argparse
import contextlib
import io
import json
import os
import re
import shutil
import tempfile
import unittest
from pathlib import Path
from unittest import mock

from ao.command import tidy
from ao.core import builddir, tidyengine


class TidyFilterIntegrationTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        if os.name == "nt":
            build = builddir.tidy_dir()
            if not (build / "CMakeCache.txt").is_file():
                message = "prepare native filter controls with ao.bat format --check tool/lint/AobusClangTidyMain.cpp"
                if os.environ.get("CI") == "true":
                    raise AssertionError(message)
                raise unittest.SkipTest(message)
            sdk = tidyengine.llvm_sdk_root(build)
            tidyengine.llvm_sdk_version(build)
            cls.compiler = str(sdk / "bin" / "clang-cl.exe")
            cls.toolchain = tidy.TidyToolchain(
                str(sdk / "bin" / "clang-tidy.exe"), None, tidyengine.clang_resource_dir(build)
            )
        else:
            compiler = shutil.which("clang++")
            checker = shutil.which("clang-tidy")
            if compiler is None or checker is None:
                raise unittest.SkipTest("native Clang tools are unavailable")
            cls.compiler = compiler
            cls.toolchain = tidy.TidyToolchain(checker, None, None)
        for executable in (cls.compiler, cls.toolchain.clang_tidy):
            if not Path(executable).is_file():
                raise AssertionError(f"native filter-control tool is missing: {executable}")

    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="aobus-tidy-filter-")
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.source = self.root / "checkout space Ω"
        self.build = self.root / "build"
        self.source.mkdir()
        self.build.mkdir()

    def write(self, relative, text):
        path = self.source / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text, encoding="utf-8")
        return path

    def compile_entry(self, translation_unit):
        if os.name == "nt":
            arguments = [self.compiler, "/nologo", "/std:c++latest", "/c", str(translation_unit)]
        else:
            arguments = [self.compiler, "-std=c++26", "-c", str(translation_unit)]
        return {"directory": str(self.build), "file": str(translation_unit), "arguments": arguments}

    def run_portal(self, selected, translation_unit, *, winui=False, broken_filter=False):
        entry = self.compile_entry(translation_unit)
        (self.build / "compile_commands.json").write_text(json.dumps([entry]), encoding="utf-8")
        export = self.root / "fixes.yaml"
        export.unlink(missing_ok=True)
        args = argparse.Namespace(
            files=[str(selected)],
            all=False,
            folder=[],
            commit=None,
            check="modernize-use-nullptr",
            debug=True,
            output=None,
            jobs=1,
            path=str(self.build),
            fix=False,
            no_build=True,
            tidy_arg=[f"-export-fixes={export}"],
            header_filter=".*",
        )
        original_sources = {path: path.read_bytes() for path in self.source.rglob("*") if path.is_file()}
        planner = tidyengine.compile_command_plan

        def plan_context(*arguments, **keywords):
            return planner(*arguments, **keywords, project_root=self.source)

        output = io.StringIO()
        with contextlib.ExitStack() as stack:
            # Supply a tiny native context, not fake checker output or filter behavior.
            stack.enter_context(mock.patch.object(tidy, "PROJECT_ROOT", self.source))
            stack.enter_context(mock.patch.object(tidy, "WINUI_ROOT", self.source / "app" / "windows-winui"))
            stack.enter_context(mock.patch.object(tidy, "prepare_toolchain", return_value=self.toolchain))
            stack.enter_context(mock.patch.object(tidyengine, "compile_command_plan", side_effect=plan_context))
            stack.enter_context(
                mock.patch.object(tidy.workspace_cache, "validated_source_roots", return_value=(self.source,))
            )
            if winui:
                stack.enter_context(mock.patch.object(tidy, "prepare_winui_compile_commands", return_value=[entry]))
            if broken_filter:
                wrong = json.dumps([{"name": str(self.source / "not-selected.h"), "lines": [[1, 2147483647]]}])
                stack.enter_context(mock.patch.object(tidy, "path_line_filter", return_value=wrong))
            stack.enter_context(contextlib.redirect_stdout(output))
            stack.enter_context(contextlib.redirect_stderr(output))
            status = tidy.run_command(args)
        self.assertEqual({path: path.read_bytes() for path in original_sources}, original_sources)
        return status, output.getvalue(), export.read_text(encoding="utf-8") if export.is_file() else ""

    def assert_selected_diagnostic(self, status, output, export, selected, excluded):
        self.assertEqual(status, 1, output)
        self.assertIn("[modernize-use-nullptr]", output)
        self.assertNotIn("[clang-diagnostic-error]", output)
        self.assertNotRegex(output, r"\berror:")
        expected = str(selected).replace("\\", "/")
        normalized = output.replace("\\", "/")
        # Portal output is repository-relative; accept exactly the selected location.
        relative = selected.relative_to(self.source).as_posix()
        diagnostics = re.findall(r"^(.+?):\d+:\d+: warning:.*\[modernize-use-nullptr\]$", normalized, re.MULTILINE)
        self.assertTrue(diagnostics, output)
        self.assertTrue(all(path in (expected, relative) for path in diagnostics), output)
        self.assertRegex(export, r"ReplacementText:\s+nullptr")
        file_paths = re.findall(r"^\s*(?:-\s+)?FilePath:\s*(.+)$", export, re.MULTILINE)
        self.assertTrue(file_paths, export)
        for value in file_paths:
            value = json.loads(value) if value.startswith('"') else value.strip("'").replace("''", "'")
            self.assertEqual(value.replace("\\", "/"), expected, export)
        for path in excluded:
            self.assertNotIn(f"{path.relative_to(self.source).as_posix()}:", normalized)

    def header_context(self):
        excluded = self.write("lib/other/value.h", "#pragma once\ninline int* excluded_pointer() { return 0; }\n")
        selected = self.write(
            "lib/selected/value.h",
            '#pragma once\n#include "../other/value.h"\ninline int* selected_pointer() { return 0; }\n',
        )
        consumer = self.write("lib/selected/value.cpp", '#include "value.h"\nint* consumer_pointer() { return 0; }\n')
        return selected, excluded, consumer

    def test_selected_header_diagnostic_and_fix_survive_but_other_files_do_not(self):
        selected, excluded, consumer = self.header_context()
        result = self.run_portal(selected, consumer)
        self.assert_selected_diagnostic(*result, selected, (excluded, consumer))

    def test_wrong_filter_cannot_satisfy_the_expected_diagnostic_control(self):
        selected, excluded, consumer = self.header_context()
        result = self.run_portal(selected, consumer, broken_filter=True)
        self.assertEqual(result[0], 0, result[1])
        with self.assertRaises(AssertionError):
            self.assert_selected_diagnostic(*result, selected, (excluded, consumer))

    def test_selected_winui_source_excludes_same_basename_header(self):
        excluded = self.write("other/Probe.h", "#pragma once\ninline int* excluded_pointer() { return 0; }\n")
        selected = self.write(
            "app/windows-winui/Probe.cpp",
            '#include "../../other/Probe.h"\nint* selected_pointer() { return 0; }\n',
        )
        result = self.run_portal(selected, selected, winui=True)
        self.assert_selected_diagnostic(*result, selected, (excluded,))

    def test_selected_fragment_excludes_its_consumer_main(self):
        selected = self.write("lib/selected/value.def", "inline int* fragment_pointer() { return 0; }\n")
        consumer = self.write("lib/selected/value.cpp", '#include "value.def"\nint* consumer_pointer() { return 0; }\n')
        result = self.run_portal(selected, consumer)
        self.assert_selected_diagnostic(*result, selected, (consumer,))

    def test_genuinely_clean_selected_header_passes(self):
        selected = self.write("lib/selected/value.h", "#pragma once\ninline int* clean_pointer() { return nullptr; }\n")
        consumer = self.write("lib/selected/value.cpp", '#include "value.h"\n')
        status, output, _export = self.run_portal(selected, consumer)
        self.assertEqual(status, 0, output)
        self.assertNotIn("[modernize-use-nullptr]", output)
        self.assertNotIn("[clang-diagnostic-error]", output)

    def test_compiler_error_in_selected_header_does_not_look_clean(self):
        selected = self.write("lib/selected/value.h", "inline int* invalid_pointer(;\n")
        consumer = self.write("lib/selected/value.cpp", '#include "value.h"\n')
        status, output, _export = self.run_portal(selected, consumer)
        self.assertNotEqual(status, 0, output)
        self.assertIn("error:", output)
