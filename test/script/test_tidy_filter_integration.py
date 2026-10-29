"""Real LLVM controls for portal diagnostic delivery and selected-file isolation."""

import argparse
import contextlib
import io
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

from ao.command import tidy
from ao.core import builddir, tidyengine


def native_filter_tools(*, os_name=None):
    """Use the portal's native LLVM pair; missing CI coverage must fail."""

    def unavailable(message):
        if os.environ.get("CI") == "true":
            raise AssertionError(message)
        raise unittest.SkipTest(message)

    expected = None
    resource = None
    if (os.name if os_name is None else os_name) == "nt":
        build = builddir.windows_lint_build_dir(builddir.build_dir("debug"))
        if not (build / "CMakeCache.txt").is_file():
            unavailable("prepare native filter controls with ao.bat setup llvm-sdk")
        sdk = tidyengine.llvm_sdk_root(build)
        expected = tidyengine.llvm_sdk_version(build)
        compiler = str(sdk / "bin" / "clang-cl.exe")
        checker = str(sdk / "bin" / "clang-tidy.exe")
        resource = tidyengine.clang_resource_dir(build)
    elif sys.platform == "darwin" and (root := os.environ.get("AOBUS_LLVM_ROOT")):
        # The macOS portal selects Homebrew llvm@22, not an SDK patch pin.
        compiler = str(Path(root) / "bin" / "clang++")
        checker = str(Path(root) / "bin" / "clang-tidy")
    else:
        compiler = shutil.which("clang++")
        checker = shutil.which("clang-tidy")
        if compiler is None or checker is None:
            unavailable(
                "native Clang tools are unavailable; use the portal's pinned Nix or Homebrew llvm@22 environment"
            )
    versions = []
    for executable in (compiler, checker):
        if not Path(executable).is_file():
            raise AssertionError(f"native filter-control tool is missing: {executable}")
        try:
            result = subprocess.run(
                [executable, "--version"],
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
                text=True,
                encoding="utf-8",
                errors="replace",
                timeout=30,
            )
        except (OSError, subprocess.TimeoutExpired) as error:
            raise AssertionError(f"cannot probe native filter-control tool: {executable}") from error
        version = re.search(r"\b(?:clang|LLVM) version\s+(\d+\.\d+\.\d+)(?![\w.+-])", result.stdout)
        if result.returncode != 0 or version is None:
            raise AssertionError(f"native filter-control version probe failed: {executable}\n{result.stdout}")
        versions.append(version.group(1))
    major = tidyengine.windows_llvm_sdk_manifest()[0].split(".", 1)[0]
    if versions[0] != versions[1] or versions[0].split(".", 1)[0] != major:
        raise AssertionError(f"native filter controls require matching LLVM {major} versions, got {versions}")
    if expected is not None and versions[0] != expected:
        raise AssertionError(f"native filter-control tools do not match the configured SDK {expected}: {versions}")
    return compiler, tidy.TidyToolchain(checker, None, resource), versions[0]


class TidyFilterIntegrationTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.compiler, cls.toolchain, cls.llvm_version = native_filter_tools()

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

    def run_portal(
        self,
        selected,
        translation_unit,
        *,
        winui=False,
        broken_filter=False,
        compile_arguments=(),
        tidy_arguments=(),
        exact_filter=False,
    ):
        entry = self.compile_entry(translation_unit)
        entry["arguments"].extend(compile_arguments)
        (self.build / "compile_commands.json").write_text(json.dumps([entry]), encoding="utf-8")
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
            no_build=True,
            tidy_arg=[*tidy_arguments],
            header_filter=None if exact_filter else ".*",
        )
        original_sources = {path: path.read_bytes() for path in self.source.rglob("*") if path.is_file()}
        planner = tidyengine.compile_command_plan

        def plan_context(*arguments, **keywords):
            return planner(*arguments, **keywords, project_root=self.source)

        native_call = tidy.subprocess.call
        self.native_statuses = []
        self.native_commands = []

        def run_native(*arguments, **keywords):
            command = arguments[0] if arguments else keywords.get("args")
            self.native_commands.append(command)
            status = native_call(*arguments, **keywords)
            self.native_statuses.append(status)
            return status

        output = io.StringIO()
        with contextlib.ExitStack() as stack:
            # Observe the real child exit; never substitute diagnostic output.
            stack.enter_context(mock.patch.object(tidy.subprocess, "call", side_effect=run_native))
            # Supply a tiny native context, not fake checker output or filter behavior.
            stack.enter_context(mock.patch.object(tidy, "PROJECT_ROOT", self.source))
            stack.enter_context(mock.patch.object(tidy, "WINUI_ROOT", self.source / "app" / "windows-winui"))
            stack.enter_context(mock.patch.object(tidy.winuitidy, "PROJECT_ROOT", self.source))
            stack.enter_context(mock.patch.object(tidy.winuitidy, "_WINUI_ROOT", self.source / "app/windows-winui"))
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
        for command in self.native_commands:
            tokens = command if isinstance(command, (list, tuple)) else [command]
            exported = [token for token in tokens if "export-fixes" in str(token)]
            self.assertEqual(exported, [])
        return status, output.getvalue()

    def assert_selected_diagnostic(self, status, output, selected, excluded):
        self.assertEqual(status, 1, output)
        self.assertIn("[modernize-use-nullptr]", output)
        self.assertNotIn("[clang-diagnostic-error]", output)
        self.assertNotRegex(output, r"\berror:")
        normalized = output.replace("\\", "/")

        def assert_selected_file(name, evidence):
            path = Path(name)
            if not path.is_absolute():
                path = self.source / path
            self.assertTrue(path.is_file(), evidence)
            # LLVM can canonicalize a symlinked native temp directory (notably
            # /var to /private/var on macOS). Compare physical identity, never
            # a basename or suffix, for the diagnostic location.
            self.assertTrue(path.samefile(selected), evidence)

        diagnostics = re.findall(r"^(.+?):\d+:\d+: warning:.*\[modernize-use-nullptr\]$", normalized, re.MULTILINE)
        self.assertTrue(diagnostics, output)
        for path in diagnostics:
            assert_selected_file(path, output)
        for path in excluded:
            self.assertNotIn(f"{path.relative_to(self.source).as_posix()}:", normalized)

    def assert_selected_compiler_error(self, status, output, selected):
        self.assertNotEqual(status, 0, output)
        self.assertTrue(self.native_statuses, "no native checker process ran")
        self.assertTrue(all(code != 0 for code in self.native_statuses), output)
        diagnostics = re.findall(
            r"^(\S.*?):(\d+):(\d+): (?:fatal )?error: [^\r\n]*\[clang-diagnostic-error\]$",
            output,
            re.MULTILINE,
        )
        self.assertTrue(diagnostics, output)
        for name, line, column in diagnostics:
            path = Path(name)
            if not path.is_absolute():
                path = self.source / path
            self.assertTrue(path.is_file(), output)
            self.assertTrue(path.samefile(selected), output)
            self.assertEqual(int(line), 1, output)
            self.assertGreater(int(column), 0, output)

    def header_context(self):
        excluded = self.write("lib/other/value.h", "#pragma once\ninline int* excluded_pointer() { return 0; }\n")
        selected = self.write(
            "lib/selected/value.h",
            '#pragma once\n#include "../other/value.h"\ninline int* selected_pointer() { return 0; }\n',
        )
        consumer = self.write("lib/selected/value.cpp", '#include "value.h"\nint* consumer_pointer() { return 0; }\n')
        return selected, excluded, consumer

    def test_selected_identity_controls_reject_an_unselected_same_basename(self):
        selected, excluded, _ = self.header_context()

        def warning(path):
            return f"{path}:3:1: warning: use nullptr [modernize-use-nullptr]\n"

        # Only a diagnostic whose location is physically the selected file can
        # satisfy the control; a same-basename header cannot stand in for it.
        with self.assertRaises(AssertionError):
            self.assert_selected_diagnostic(1, warning(excluded), selected, ())
        self.assert_selected_diagnostic(1, warning(selected.resolve()), selected, ())

    def test_selected_header_diagnostic_survives_but_other_files_do_not(self):
        selected, excluded, consumer = self.header_context()
        result = self.run_portal(selected, consumer)
        self.assert_selected_diagnostic(*result, selected, (excluded, consumer))

    def test_wrong_filter_cannot_satisfy_the_expected_diagnostic_control(self):
        selected, excluded, consumer = self.header_context()
        result = self.run_portal(selected, consumer, broken_filter=True)
        # A zero-warning exit alone cannot distinguish a filtered clean pass
        # from a portal that silently skipped the native checker.
        self.assertTrue(self.native_commands, "no native checker process ran")
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

    def mixed_fragment_context(self, spelling):
        selected = self.write(
            "app/include/ao/i18n/deep/MessageInventory.def", "inline int* selected_pointer() { return 0; }\n"
        )
        excluded = self.write(
            "app/include/other/deep/MessageInventory.def", "inline int* excluded_pointer() { return 0; }\n"
        )
        wrapper = self.write(
            "app/include/ao/i18n/Wrapper.h",
            '#include "deep/MessageInventory.def"\ninline int* wrapper_pointer() { return 0; }\n',
        )
        consumer = self.write(
            "app/i18n/deep/MessageInventory.cpp",
            f"#include <{spelling}>\n#include <other/deep/MessageInventory.def>\n"
            "int* consumer_pointer() { return 0; }\n",
        )
        return selected, excluded, wrapper, consumer

    @unittest.skipUnless(os.name == "nt", "Windows LLVM path identity")
    def test_selected_fragment_survives_native_forward_and_nested_mixed_includes(self):
        for spelling, mixed_prefix in (
            ("ao/i18n/deep/MessageInventory.def", False),
            (r"ao\i18n\deep\MessageInventory.def", False),
            (r"ao\i18n/Wrapper.h", True),
        ):
            with self.subTest(spelling=spelling, mixed_prefix=mixed_prefix):
                selected, excluded, wrapper, consumer = self.mixed_fragment_context(spelling)
                directory = self.source / "app" / "include"
                prefix = str(directory)
                if mixed_prefix:
                    # Multiple transitions before a mixed include and a nested
                    # quoted include: not a single native/POSIX separator cut.
                    parts = prefix.split("\\")
                    prefix = parts[0] + "".join(
                        ("/" if index % 2 else "\\") + part for index, part in enumerate(parts[1:], 1)
                    )
                self.assertTrue(Path(prefix).samefile(directory))
                result = self.run_portal(selected, consumer, compile_arguments=["-I" + prefix], exact_filter=True)
                self.assert_selected_diagnostic(*result, selected, (excluded, wrapper, consumer))

    @unittest.skipUnless(os.name == "nt", "Windows LLVM path identity")
    def test_selected_fragment_does_not_shadow_caller_vfs_contents(self):
        selected, _excluded, _wrapper, consumer = self.mixed_fragment_context("ao/i18n/deep/MessageInventory.def")
        replacement = self.write("virtual.def", "inline int* virtual_pointer() { return nullptr; }\n")
        overlay = self.root / "caller.json"
        overlay.write_text(
            json.dumps(
                {
                    "version": 0,
                    "roots": [{"type": "file", "name": selected.as_posix(), "external-contents": str(replacement)}],
                }
            ),
            encoding="utf-8",
        )
        caller_bytes = overlay.read_bytes()
        for compile_options, tool_options in (
            (["-ivfsoverlay", str(overlay)], []),
            (["/clang:-ivfsoverlay", "/clang:" + str(overlay)], []),
            ([], ["--vfsoverlay=" + str(overlay)]),
            ([], ["--extra-arg=-ivfsoverlay=" + str(overlay)]),
        ):
            with self.subTest(compile_options=compile_options, tool_options=tool_options):
                status, output = self.run_portal(
                    selected,
                    consumer,
                    compile_arguments=compile_options,
                    tidy_arguments=tool_options,
                    exact_filter=True,
                )
                self.assertEqual(status, 1, output)
                self.assertIn("cannot compose caller VFS", output)
                # The preflight rejects the composed overlay before any native run.
                self.assertEqual(self.native_commands, [], "the VFS preflight rejection must not run a native checker")
                self.assertNotIn("[modernize-use-nullptr]", output)
                self.assertEqual(overlay.read_bytes(), caller_bytes)

    @unittest.skipUnless(os.name == "nt", "Windows LLVM path identity")
    def test_clean_fragment_stays_clean_when_consumer_and_other_fragment_warn(self):
        selected, _excluded, _wrapper, consumer = self.mixed_fragment_context("ao/i18n/deep/MessageInventory.def")
        selected.write_text("inline int* clean_pointer() { return nullptr; }\n", encoding="utf-8")
        status, output = self.run_portal(
            selected,
            consumer,
            compile_arguments=["-I" + str(self.source / "app" / "include")],
            exact_filter=True,
        )
        # The consumer and the other fragment still warn, so a clean status
        # must be a real filtered native pass, not a skipped run.
        self.assertTrue(self.native_commands, "no native checker process ran")
        self.assertEqual(status, 0, output)
        self.assertNotIn("[modernize-use-nullptr]", output)
        self.assertNotIn("error:", output)

    @unittest.skipUnless(os.name == "nt", "Windows LLVM path identity")
    def test_compiler_error_in_mixed_path_fragment_remains_visible(self):
        selected, _excluded, _wrapper, consumer = self.mixed_fragment_context("ao/i18n/deep/MessageInventory.def")
        selected.write_text("inline int* invalid_pointer(;\n", encoding="utf-8")
        status, output = self.run_portal(
            selected,
            consumer,
            compile_arguments=["-I" + str(self.source / "app" / "include")],
            exact_filter=True,
        )
        self.assert_selected_compiler_error(status, output, selected)

    def test_genuinely_clean_selected_header_passes(self):
        selected = self.write("lib/selected/value.h", "#pragma once\ninline int* clean_pointer() { return nullptr; }\n")
        consumer = self.write("lib/selected/value.cpp", '#include "value.h"\n')
        status, output = self.run_portal(selected, consumer)
        # A genuinely clean pass must come from a real native run, not a skipped one.
        self.assertTrue(self.native_commands, "no native checker process ran")
        self.assertEqual(status, 0, output)
        self.assertNotIn("[modernize-use-nullptr]", output)
        self.assertNotIn("[clang-diagnostic-error]", output)

    def test_compiler_error_in_selected_header_does_not_look_clean(self):
        selected = self.write("lib/selected/value.h", "inline int* invalid_pointer(;\n")
        consumer = self.write("lib/selected/value.cpp", '#include "value.h"\n')
        status, output = self.run_portal(selected, consumer)
        self.assert_selected_compiler_error(status, output, selected)


class FilterToolPreflightTest(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix="aobus-filter-preflight-")
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        (self.root / "bin").mkdir()
        self.compiler = self.root / "bin" / "clang++"
        self.checker = self.root / "bin" / "clang-tidy"
        self.compiler.touch()
        self.checker.touch()
        self.build = self.root / "build"
        self.build.mkdir()
        (self.build / "CMakeCache.txt").touch()
        self.sdk = self.root / "sdk"
        (self.sdk / "bin").mkdir(parents=True)
        for name in ("clang-cl.exe", "clang-tidy.exe"):
            (self.sdk / "bin" / name).touch()
        patchers = (
            mock.patch.dict(os.environ, {"CI": "false"}),
            mock.patch.object(sys, "platform", "linux"),
            mock.patch.object(builddir, "build_dir", return_value=self.build),
            mock.patch.object(builddir, "windows_lint_build_dir", return_value=self.build),
            mock.patch.object(tidyengine, "llvm_sdk_root", return_value=self.sdk),
            mock.patch.object(tidyengine, "llvm_sdk_version", return_value="22.1.8"),
            mock.patch.object(tidyengine, "clang_resource_dir", return_value=self.sdk / "lib" / "clang" / "22"),
        )
        for patcher in patchers:
            patcher.start()
            self.addCleanup(patcher.stop)
        lookup = mock.patch.object(
            shutil,
            "which",
            side_effect={
                "clang++": str(self.compiler),
                "clang-tidy": str(self.checker),
            }.get,
        )
        self.lookup = lookup.start()
        self.addCleanup(lookup.stop)

    def versions(self, compiler="22.1.8", checker="22.1.8", *, code=0):
        return mock.patch.object(
            subprocess,
            "run",
            side_effect=[
                subprocess.CompletedProcess([], code, f"clang version {compiler}\n"),
                subprocess.CompletedProcess([], code, f"LLVM version {checker}\n"),
            ],
        )

    def test_ci_missing_one_or_both_posix_tools_is_a_failure(self):
        for compiler, checker in ((None, None), (str(self.compiler), None), (None, str(self.checker))):
            with (
                self.subTest(compiler=compiler, checker=checker),
                mock.patch.dict(os.environ, {"CI": "true"}),
                mock.patch.object(shutil, "which", side_effect=[compiler, checker]),
                self.versions() as probe,
            ):
                with self.assertRaisesRegex(AssertionError, "unavailable"):
                    native_filter_tools(os_name="posix")
                probe.assert_not_called()

    def test_local_missing_one_or_both_posix_tools_is_optional(self):
        for compiler, checker in ((None, None), (str(self.compiler), None), (None, str(self.checker))):
            with (
                self.subTest(compiler=compiler, checker=checker),
                mock.patch.object(shutil, "which", side_effect=[compiler, checker]),
            ):
                with self.assertRaisesRegex(unittest.SkipTest, "portal"):
                    native_filter_tools(os_name="posix")

    def test_missing_windows_sdk_is_optional_only_outside_ci(self):
        (self.build / "CMakeCache.txt").unlink()
        for ci, error in (("true", AssertionError), ("false", unittest.SkipTest)):
            with self.subTest(ci=ci), mock.patch.dict(os.environ, {"CI": ci}):
                with self.assertRaisesRegex(error, "setup llvm-sdk"):
                    native_filter_tools(os_name="nt")

    def test_configured_missing_tool_never_becomes_an_optional_skip(self):
        self.checker.unlink()
        for ci in ("true", "false"):
            with self.subTest(ci=ci), mock.patch.dict(os.environ, {"CI": ci}), self.versions():
                with self.assertRaisesRegex(AssertionError, "tool is missing"):
                    native_filter_tools(os_name="posix")

    def test_configured_windows_missing_one_or_both_tools_fails(self):
        compiler = self.sdk / "bin" / "clang-cl.exe"
        checker = self.sdk / "bin" / "clang-tidy.exe"
        for missing in ((compiler,), (checker,), (compiler, checker)):
            for path in missing:
                path.unlink()
            for ci in ("true", "false"):
                with self.subTest(missing=missing, ci=ci), mock.patch.dict(os.environ, {"CI": ci}), self.versions():
                    with self.assertRaisesRegex(AssertionError, "tool is missing"):
                        native_filter_tools(os_name="nt")
            for path in missing:
                path.touch()

    def test_matching_portal_pair_is_accepted(self):
        with self.versions():
            compiler, toolchain, version = native_filter_tools(os_name="posix")
        self.assertEqual((compiler, toolchain.clang_tidy, version), (str(self.compiler), str(self.checker), "22.1.8"))

    def test_mismatched_and_incompatible_versions_never_skip(self):
        for versions in (("22.1.8", "22.1.7"), ("21.1.8", "21.1.8"), ("23.1.8", "23.1.8")):
            for ci in ("true", "false"):
                with (
                    self.subTest(versions=versions, ci=ci),
                    mock.patch.dict(os.environ, {"CI": ci}),
                    self.versions(*versions),
                ):
                    with self.assertRaisesRegex(AssertionError, "matching LLVM"):
                        native_filter_tools(os_name="posix")

    def test_failed_or_unrecognized_version_probes_never_skip(self):
        for ci in ("true", "false"):
            with mock.patch.dict(os.environ, {"CI": ci}):
                for arguments in ({"compiler": "unrecognized"}, {"code": 1}):
                    with self.subTest(ci=ci, arguments=arguments), self.versions(**arguments):
                        with self.assertRaisesRegex(AssertionError, "version probe failed"):
                            native_filter_tools(os_name="posix")
                for error in (OSError("unavailable"), subprocess.TimeoutExpired("clang++", 30)):
                    with self.subTest(ci=ci, error=error), mock.patch.object(subprocess, "run", side_effect=error):
                        with self.assertRaisesRegex(AssertionError, "cannot probe"):
                            native_filter_tools(os_name="posix")

    def test_windows_pair_must_match_its_configured_sdk_version(self):
        with self.versions("22.1.9", "22.1.9"):
            with self.assertRaisesRegex(AssertionError, "configured SDK 22.1.8"):
                native_filter_tools(os_name="nt")

    def test_mac_portal_root_wins_without_imposing_a_windows_patch_pin(self):
        with (
            mock.patch.object(sys, "platform", "darwin"),
            mock.patch.dict(os.environ, {"AOBUS_LLVM_ROOT": str(self.root)}),
            self.versions("22.2.0", "22.2.0"),
        ):
            compiler, toolchain, version = native_filter_tools(os_name="posix")
        self.lookup.assert_not_called()
        self.assertEqual((compiler, toolchain.clang_tidy, version), (str(self.compiler), str(self.checker), "22.2.0"))
        with (
            mock.patch.object(sys, "platform", "darwin"),
            mock.patch.dict(os.environ, {"AOBUS_LLVM_ROOT": str(self.root / "missing")}),
        ):
            with self.assertRaisesRegex(AssertionError, "tool is missing"):
                native_filter_tools(os_name="posix")
        self.lookup.assert_not_called()


class FilterCompilerErrorAssertionTest(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix="aobus-filter-error-identity-")
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        self.selected = self.root / "selected.h"
        self.consumer = self.root / "consumer.cpp"
        self.excluded = self.root / "excluded.h"
        for path in (self.selected, self.consumer, self.excluded):
            path.touch()
        self.control = TidyFilterIntegrationTest("test_compiler_error_in_selected_header_does_not_look_clean")
        self.control.source = self.root
        self.control.native_statuses = [1]

    def test_consumer_unselected_source_echo_and_wrong_line_cannot_satisfy_the_control(self):
        selected = f"{self.selected}:1:29: error: expected expression [clang-diagnostic-error]"
        for output in (
            selected.replace(str(self.selected), str(self.consumer)),
            selected.replace(str(self.selected), str(self.excluded)),
            selected.replace(":1:29:", ":2:29:"),
            selected.replace(" [clang-diagnostic-error]", ""),
            "    1 | " + selected,
            "error: arbitrary setup failure",
        ):
            with self.subTest(output=output), self.assertRaises(AssertionError):
                self.control.assert_selected_compiler_error(1, output, self.selected)

    def test_selected_location_alone_cannot_replace_nonzero_portal_and_native_exits(self):
        output = f"{self.selected}:1:29: error: expected expression [clang-diagnostic-error]"
        self.control.assert_selected_compiler_error(1, output, self.selected)
        for status, native in ((0, [1]), (1, [0]), (1, [])):
            self.control.native_statuses = native
            with self.subTest(status=status, native=native), self.assertRaises(AssertionError):
                self.control.assert_selected_compiler_error(status, output, self.selected)
