"""Tests for the real-Clang source-policy scope smoke stage."""

import contextlib
import io
import json
import os
import re
import subprocess
import tempfile
import unittest
from pathlib import Path
from unittest import mock

from ao.core import linttest

# The smoke fixtures use only a literal '#include "..."' grammar, so a simple
# regex is enough here; this is deliberately not a general include parser.
INCLUDE_TOKEN_RE = re.compile(r'^\s*#\s*include\s*"([^"]+)"', re.MULTILINE)
LINE_MARKER_RE = re.compile(r'^\s*#\s*(\d+)\s+"([^"]*)"(.*)$')


def posix_tools(plugin):
    return linttest.FixtureTools("pinned-clang-tidy", (), windows=False, plugin=plugin)


class SourcePolicyScopeSmokeTest(unittest.TestCase):
    # ---------- shared command and input inspection ----------

    def _command_units(self, command):
        """Return the translation units of a recorded command in argv order."""
        separator = command.index("--")
        return [Path(argument) for argument in command[:separator] if argument.endswith(".cpp")]

    def _recorded_sources(self, command):
        """Snapshot every argv translation unit and its included context header.

        Relative include tokens are resolved against the including file after
        replacing backslash separators, which also covers the Windows native
        profile's single-backslash header spelling.
        """
        sources: dict[Path, str] = {}
        pending = list(self._command_units(command))
        while pending:
            path = pending.pop()
            if path in sources:
                continue
            source = path.read_text(encoding="utf-8")
            sources[path] = source
            for match in INCLUDE_TOKEN_RE.finditer(source):
                header = (path.parent / match.group(1).replace("\\", "/")).resolve()
                if header.suffix == ".h" and header.is_file():
                    pending.append(header)
        return sources

    def _command_case(self, command):
        """Identify the source-policy or metadata case a recorded command serves."""
        stems = [unit.stem for unit in self._command_units(command)]
        names = [name for name, *_ in linttest._source_policy_cases(Path("source-policy"))]
        names += [name for name, *_ in linttest._source_policy_metadata_cases(Path("source-policy"))]
        matches = {name for name in names if any(stem == name or stem.startswith(f"{name}-") for stem in stems)}
        self.assertTrue(matches, f"unrecognized source-policy fixture for stems {stems}")
        return max(matches, key=len)

    def _profile_of(self, command):
        """Return the path-spelling profile a recorded command was prepared for."""
        return next(
            parent.name for parent in self._command_units(command)[0].parents if parent.name in ("native", "forward")
        )

    def _declared_context_header(self, case):
        """Return the declared context-header directory and file name for a case."""
        if case in ("header-file-switch", "header-file-switch-unicode"):
            return "test", f"{case}-ordinary.h"
        if case == "macro-spelling-exempt-definition":
            return "test", f"{case}-macros.h"
        if case == "macro-spelling-production-definition":
            return "lib", f"{case}-macros.h"
        return None

    def _first_include_token(self, source):
        match = INCLUDE_TOKEN_RE.search(source)
        self.assertIsNotNone(match, "fixture context include not found")
        return match.group(1)

    def _assert_context_include(self, command, case, profile, *, windows):
        """Assert the generated include token is relative and resolves to its header.

        The native Windows profile spells the relative header with single
        backslash separators; every other profile (including the POSIX native
        one) uses forward slashes.
        """
        main = self._command_units(command)[-1]
        token = self._first_include_token(main.read_text(encoding="utf-8"))
        directory, header_name = self._declared_context_header(case)
        separator = "\\" if windows and profile == "native" else "/"
        self.assertEqual(token, f"..{separator}{directory}{separator}{header_name}")
        header = (main.parent / token.replace("\\", "/")).resolve()
        self.assertTrue(header.is_file())
        self.assertEqual(header, (main.parent.parent / directory / header_name).resolve())
        return header

    def _assert_case_flags(self, command, case):
        """Diagnostic flags are scoped to the one control that needs them."""
        if case == "same-file-system":
            self.assertIn("--system-headers", command)
            self.assertNotIn("--header-filter=.*", command)
        elif case in ("header-file-switch", "header-file-switch-unicode"):
            self.assertIn("--header-filter=.*", command)
            self.assertNotIn("--system-headers", command)
        else:
            self.assertNotIn("--system-headers", command)
            self.assertNotIn("--header-filter=.*", command)

    # ---------- mocked native host ----------

    def native_result(self, command, **_kwargs):
        if "--dump-config" in command:
            config = json.loads(next(arg.removeprefix("-config=") for arg in command if arg.startswith("-config=")))
            roots = config["CheckOptions"].get("AobusSourceRoots", "[]")
            config["CheckOptions"] = {f"{alias}.AobusSourceRoots": roots for alias in linttest._SOURCE_POLICY_CHECKS}
            return subprocess.CompletedProcess(command, 0, stdout=json.dumps(config))
        output = ""
        config_text = next(arg.removeprefix("-config=") for arg in command if arg.startswith("-config="))
        if '"CheckOptions"' in config_text:
            options = json.loads(config_text)["CheckOptions"]
            value = options.get("AobusSourceRoots", "[]")
            try:
                roots = json.loads(value)
                invalid = not isinstance(roots, list) or any(not isinstance(root, str) or not root for root in roots)
            except json.JSONDecodeError:
                invalid = True
            if invalid:
                output += "warning: invalid AobusSourceRoots [clang-tidy-config]\n"
        for path, source in self._recorded_sources(command).items():
            for line_number, line in enumerate(source.splitlines(), 1):
                if "// POSITIVE:" in line:
                    check = line.split("// POSITIVE:", 1)[1]
                    output += f"{path}:{line_number}:1: warning: expected [{check}]\n"
        return subprocess.CompletedProcess(command, 0, stdout=output)

    # ---------- passing smoke and explicit roots ----------

    def test_scope_smoke_passes_only_with_all_required_diagnostics_and_explicit_roots(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            with (
                mock.patch.object(linttest.tidyengine, "clang_tool", return_value="pinned-clang-tidy"),
                mock.patch.object(linttest.subprocess, "run", side_effect=self.native_result) as run,
            ):
                success, _log = linttest._run_source_policy_smoke(
                    root,
                    root,
                    plugin=root / "tool/lint/libAobusLintPlugin.so",
                    tools=posix_tools(root / "tool/lint/libAobusLintPlugin.so"),
                )
            self.assertTrue(success)
            self.assertEqual(run.call_count, linttest._source_policy_case_count(windows=False))
            all_roots = []
            for call in run.call_args_list:
                command = call.args[0]
                self.assertEqual(command[0], "pinned-clang-tidy")
                self.assertIn(f"-load={root / 'tool/lint/libAobusLintPlugin.so'}", command)
                config = next(
                    argument.removeprefix("-config=") for argument in command if argument.startswith("-config=")
                )
                if "key: 'AobusSourceRoots', value: " not in config:
                    continue  # Raw metadata and dump round trips intentionally bypass config_for.
                encoded = config.split("key: 'AobusSourceRoots', value: ", 1)[1]
                roots_json, _end = json.JSONDecoder().raw_decode(encoded)
                all_roots.append(json.loads(roots_json))
            self.assertTrue(all(all_roots))
            self.assertTrue(any(len(roots) == 2 for roots in all_roots))
            self.assertTrue(any("space;and checkout" in root for roots in all_roots for root in roots))

    def test_missing_or_unexpected_warnings_and_compiler_errors_fail_the_smoke(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            for mode in (
                "missing",
                "unexpected",
                "compiler-error",
                "crash",
                "dump-missing-option",
                "dump-error",
                "configuration-warning-missing",
                "configuration-warning-unexpected",
            ):

                def result(command, mode=mode, **_kwargs):
                    if "--dump-config" in command:
                        if mode == "dump-missing-option":
                            return subprocess.CompletedProcess(command, 0, stdout="CheckOptions: {}\n")
                        if mode == "dump-error":
                            return subprocess.CompletedProcess(command, 1, stdout="error: cannot dump config\n")
                        return self.native_result(command)
                    if mode.startswith("dump-"):
                        return self.native_result(command)
                    if mode.startswith("configuration-warning-"):
                        completed = self.native_result(command)
                        if mode.endswith("missing"):
                            completed.stdout = "\n".join(
                                line for line in completed.stdout.splitlines() if "[clang-tidy-config]" not in line
                            )
                        else:
                            completed.stdout += "warning: invalid AobusSourceRoots [clang-tidy-config]\n"
                        return completed
                    path = command[command.index("--") - 1]
                    if mode == "missing":
                        return subprocess.CompletedProcess(command, 0, stdout="")
                    if mode == "unexpected":
                        completed = self.native_result(command)
                        text = Path(path).read_text(encoding="utf-8")
                        if "// NEGATIVE:" in text:
                            line_number = next(
                                number for number, line in enumerate(text.splitlines(), 1) if "// NEGATIVE:" in line
                            )
                            completed.stdout += (
                                f"{path}:{line_number}:1: warning: unexpected [aobus-readability-forbid-raw-throw]\n"
                            )
                        return completed
                    if mode == "compiler-error":
                        return subprocess.CompletedProcess(
                            command, 1, stdout=f"{path}:1:1: error: invalid fixture [clang-diagnostic-error]\n"
                        )
                    return subprocess.CompletedProcess(command, -6, stdout="")

                with (
                    self.subTest(mode=mode),
                    mock.patch.object(linttest.tidyengine, "clang_tool", return_value="pinned-clang-tidy"),
                    mock.patch.object(linttest.subprocess, "run", side_effect=result),
                ):
                    success, log = linttest._run_source_policy_smoke(
                        root,
                        root,
                        plugin=root / "tool/lint/libAobusLintPlugin.so",
                        tools=posix_tools(root / "tool/lint/libAobusLintPlugin.so"),
                    )
                    self.assertFalse(success)
                    self.assertIn("ERROR:", log.read_text(encoding="utf-8"))

    def test_losing_any_one_selected_check_fails_the_smoke(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            for dropped in linttest._SOURCE_POLICY_CHECKS:

                def partial(command, dropped=dropped, **_kwargs):
                    completed = self.native_result(command)
                    if "--dump-config" not in command:
                        completed.stdout = "\n".join(
                            line for line in completed.stdout.splitlines() if f"[{dropped}]" not in line
                        )
                    return completed

                with (
                    self.subTest(dropped=dropped),
                    mock.patch.object(linttest.tidyengine, "clang_tool", return_value="pinned-clang-tidy"),
                    mock.patch.object(linttest.subprocess, "run", side_effect=partial),
                ):
                    success, log = linttest._run_source_policy_smoke(
                        root,
                        root,
                        plugin=root / "tool/lint/libAobusLintPlugin.so",
                        jobs=4,
                        tools=posix_tools(root / "tool/lint/libAobusLintPlugin.so"),
                    )
                    self.assertFalse(success)
                    self.assertIn(f"[{dropped}]", log.read_text(encoding="utf-8"))

    # ---------- case inventory and derived counters ----------

    # ---------- per-case diagnostic flags ----------

    # ---------- lexical include contract ----------

    # ---------- macro spelling controls ----------

    # ---------- GNU line markers ----------

    # ---------- multi-translation-unit orders ----------

    # ---------- POSIX case sensitivity ----------

    # ---------- Windows dual-profile smoke ----------

    def test_windows_smoke_uses_both_path_profiles_and_the_prepared_static_host(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            tools = linttest.FixtureTools("native-static-host", ("--driver-mode=cl", "/EHsc"), windows=True)
            with mock.patch.object(linttest.subprocess, "run", side_effect=self.native_result) as run:
                success, _log = linttest._run_source_policy_smoke(root, root, plugin=None, tools=tools)
            self.assertTrue(success)
            self.assertEqual(run.call_count, linttest._source_policy_case_count(windows=True))
            expected_per_profile = linttest._source_policy_case_count(windows=True) // 2
            profiles = {"native": 0, "forward": 0}
            for call in run.call_args_list:
                command = call.args[0]
                self.assertEqual(command[0], tools.tidy)
                self.assertIn("/clang:-fdiagnostics-format=clang", command)
                self.assertIn("/clang:-std=c++26", command)
                self.assertFalse(any(argument.startswith("-load=") for argument in command))
                units = self._command_units(command)
                case = self._command_case(command)
                self._assert_case_flags(command, case)
                profile = self._profile_of(command)
                profiles[profile] += 1
                if profile == "forward":
                    for argument in command[: command.index("--")]:
                        if argument.endswith(".cpp"):
                            self.assertNotIn("\\", argument)
                elif os.name == "nt":
                    # The native profile spells argv with the host's own separators.
                    self.assertIn("\\", str(units[0]))
                if self._declared_context_header(case) is not None:
                    self._assert_context_include(command, case, profile, windows=True)
                if case == "same-file-system":
                    source = units[-1].read_text(encoding="utf-8")
                    for line in source.splitlines():
                        match = LINE_MARKER_RE.match(line)
                        if match is None:
                            continue
                        expected = units[-1].as_posix() if profile == "forward" else str(units[-1])
                        self.assertEqual(f'"{match.group(2)}"', json.dumps(expected))
            self.assertEqual(profiles, {"native": expected_per_profile, "forward": expected_per_profile})

    # ---------- parallel bounds and per-invocation input freezing ----------

    # ---------- lint gate integration ----------

    def test_source_policy_failure_fails_the_lint_gate_and_preserves_its_workspace(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            plugin = root / "tool/lint/libAobusLintPlugin.so"
            plugin.parent.mkdir(parents=True)
            plugin.touch()
            source = root / "Fixture.cpp"
            source.write_text("int value;\n", encoding="utf-8")
            log = root / "failure.log"
            log.write_text("source policy control failed\n", encoding="utf-8")
            with (
                mock.patch.object(linttest.subprocess, "run", side_effect=AssertionError("unexpected native work")),
                mock.patch.object(
                    linttest, "discover_fixtures", return_value=[linttest.Fixture(source, "aobus-example")]
                ),
                mock.patch.object(linttest, "_run_diagnostic", return_value=(True, log)),
                mock.patch.object(linttest, "_run_source_policy_smoke", return_value=(False, log)) as smoke,
                mock.patch.object(linttest, "_run_compiler_error_smoke", return_value=(True, log)),
                mock.patch.object(linttest.shutil, "rmtree") as cleanup,
                contextlib.redirect_stdout(io.StringIO()),
            ):
                self.assertEqual(linttest.run(root, tools=posix_tools(plugin), jobs=1), 1)
            smoke.assert_called_once()
            self.assertEqual(smoke.call_args.kwargs["tools"], posix_tools(plugin))
            cleanup.assert_not_called()


if __name__ == "__main__":
    unittest.main()
