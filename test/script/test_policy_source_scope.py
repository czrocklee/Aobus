"""Tests for the real-Clang source-policy scope smoke stage."""

import contextlib
import io
import json
import os
import re
import subprocess
import tempfile
import threading
import unittest
from pathlib import Path
from unittest import mock

from ao.core import builddir, linttest, phases

# The smoke fixtures use only a literal '#include "..."' grammar, so a simple
# regex is enough here; this is deliberately not a general include parser.
INCLUDE_TOKEN_RE = re.compile(r'^\s*#\s*include\s*"([^"]+)"', re.MULTILINE)
DEFINE_RE = re.compile(r"^\s*#\s*define\s+([A-Za-z_][A-Za-z0-9_]*)", re.MULTILINE)
LINE_MARKER_RE = re.compile(r'^\s*#\s*(\d+)\s+"([^"]*)"(.*)$')

MACRO_CASES = ("macro-spelling-exempt-definition", "macro-spelling-production-definition")


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
                    root, root, plugin=root / "tool/lint/libAobusLintPlugin.so"
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
                        root, root, plugin=root / "tool/lint/libAobusLintPlugin.so"
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
                        root, root, plugin=root / "tool/lint/libAobusLintPlugin.so", jobs=4
                    )
                    self.assertFalse(success)
                    self.assertIn(f"[{dropped}]", log.read_text(encoding="utf-8"))

    def test_metadata_faults_and_dump_round_trips_use_the_real_host_boundary(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            with (
                mock.patch.object(linttest.tidyengine, "clang_tool", return_value="pinned-clang-tidy"),
                mock.patch.object(linttest.subprocess, "run", side_effect=self.native_result) as run,
            ):
                success, _log = linttest._run_source_policy_smoke(
                    root, root, plugin=root / "tool/lint/libAobusLintPlugin.so"
                )
            self.assertTrue(success)
            payloads = []
            dumps = []
            round_trips = []
            for call in run.call_args_list:
                command = call.args[0]
                config = next(arg.removeprefix("-config=") for arg in command if arg.startswith("-config="))
                if '"CheckOptions"' not in config:
                    continue
                options = json.loads(config)["CheckOptions"]
                if "--dump-config" in command:
                    dumps.append(options)
                elif "AobusSourceRoots" not in options and any(".AobusSourceRoots" in key for key in options):
                    round_trips.append(options)
                else:
                    payloads.append(options.get("AobusSourceRoots"))
            self.assertIn("{", payloads)
            self.assertIn("{}", payloads)
            self.assertIn("[]", payloads)
            self.assertIn('[null,17,false,{},""]', payloads)
            self.assertTrue(any(value and value.startswith('["') and not value.endswith("]") for value in payloads))
            self.assertEqual(len(dumps), 2)
            self.assertEqual(len(round_trips), 2)
            self.assertIn({}, dumps)
            self.assertTrue(any(set(options.values()) == {"[]"} for options in round_trips))

    # ---------- case inventory and derived counters ----------

    def test_case_inventory_tracks_renamed_and_new_controls_with_derived_counters(self):
        for windows in (False, True):
            with self.subTest(windows=windows):
                root = Path("source-policy")
                cases = linttest._source_policy_cases(root, windows=windows)
                names = [name for name, *_ in cases]
                self.assertEqual(len(names), len(set(names)))
                for name in (
                    "production",
                    "same-file-system",
                    "header-file-switch",
                    "header-file-switch-unicode",
                    "macro-spelling-exempt-definition",
                    "macro-spelling-production-definition",
                    "multi-tu-exempt-first",
                    "multi-tu-production-first",
                    "ordinary-test",
                    "fixture",
                    "nested-test-component",
                    "test-ancestor-production",
                    "test-ancestor-test",
                    "test-ancestor-fixture",
                    "fixture-ancestor-test",
                    "fixture-near-miss",
                    "root-near-miss",
                    "longest-root",
                    "root-encoding",
                    "encoded-root-test",
                    "unknown-root",
                    "case-varied-root",
                    "case-varied-test",
                ):
                    self.assertIn(name, names)
                self.assertNotIn("multi-tu", names)
                self.assertNotIn("multi-tu-reversed", names)
                path, roots = next(
                    (path, roots) for name, path, roots, _expected in cases if name == "header-file-switch-unicode"
                )
                self.assertIn("josé 测试", path.parts)
                self.assertIn("josé 测试", roots[0].parts)
                self.assertTrue(path.is_relative_to(roots[0]))
                metadata = linttest._source_policy_metadata_cases(root)
                dumps = sum(dump for _name, _options, _expected, dump, _warning in metadata)
                profiles = 2 if windows else 1
                self.assertEqual(
                    linttest._source_policy_case_count(windows=windows),
                    (len(cases) + len(metadata) + dumps) * profiles,
                )
                self.assertEqual(
                    linttest._source_policy_task_count(windows=windows),
                    (len(cases) + len(metadata)) * profiles,
                )

    def test_ascii_case_variant_varies_only_ascii_letters(self):
        variant = linttest._source_policy_ascii_case_variant
        self.assertEqual(variant(Path("plain/checkout")), Path("PLAIN/CHECKOUT"))
        # Only ASCII letters vary: é and the Chinese characters must not gain an
        # invented uppercase form (É) or any other Unicode mutation.
        self.assertEqual(variant(Path("josé 测试/checkout")), Path("JOSé 测试/CHECKOUT"))
        self.assertNotIn("É", str(variant(Path("josé 测试/checkout"))))
        self.assertEqual(variant(variant(Path("josé 测试/checkout"))), Path("josé 测试/checkout"))
        self.assertEqual(variant(Path("Éric/checkout")), Path("ÉRIC/CHECKOUT"))

    def test_forward_profile_option_roots_vary_only_ascii_letters_under_non_ascii_parents(self):
        for parent_name in ("ascii", "josé", "测试", "native", "forward"):
            with self.subTest(parent=parent_name), tempfile.TemporaryDirectory() as temporary:
                run_dir = Path(temporary) / parent_name / "run"
                run_dir.mkdir(parents=True)
                tools = linttest.WindowsFixtureTools("static-host", ())
                with mock.patch.object(linttest.subprocess, "run", side_effect=self.native_result) as run:
                    success, _log = linttest._run_source_policy_smoke(run_dir, run_dir, plugin=None, tools=tools)
                self.assertTrue(success)
                checked = 0
                for call in run.call_args_list:
                    command = call.args[0]
                    units = self._command_units(command)
                    if not units or self._profile_of(command) != "forward":
                        continue
                    config = next(
                        argument.removeprefix("-config=") for argument in command if argument.startswith("-config=")
                    )
                    if "key: 'AobusSourceRoots', value: " not in config:
                        continue  # Raw metadata and dump round trips bypass config_for.
                    case = self._command_case(command)
                    profile_dir = next(parent for parent in units[0].parents if parent.name in ("native", "forward"))
                    declared = {
                        name: roots
                        for name, _path, roots, _expected in linttest._source_policy_cases(profile_dir, windows=True)
                    }
                    encoded = config.split("key: 'AobusSourceRoots', value: ", 1)[1]
                    roots_json, _end = json.JSONDecoder().raw_decode(encoded)
                    self.assertEqual(
                        json.loads(roots_json),
                        [linttest._source_policy_ascii_case_variant(root).as_posix() for root in declared[case]],
                    )
                    checked += 1
                self.assertEqual(checked, len(linttest._source_policy_cases(Path("source-policy"), windows=True)))

    # ---------- per-case diagnostic flags ----------

    def test_diagnostic_flags_are_scoped_to_one_control_per_case(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            with (
                mock.patch.object(linttest.tidyengine, "clang_tool", return_value="pinned-clang-tidy"),
                mock.patch.object(linttest.subprocess, "run", side_effect=self.native_result) as run,
            ):
                success, _log = linttest._run_source_policy_smoke(root, root, plugin=root / "verified.so")
            self.assertTrue(success)
            self.assertEqual(run.call_count, linttest._source_policy_case_count(windows=False))
            for call in run.call_args_list:
                command = call.args[0]
                self._assert_case_flags(command, self._command_case(command))

    # ---------- lexical include contract ----------

    def test_context_includes_stay_lexically_relative_under_adversarial_parents(self):
        # A mocked host cannot claim a real compile under these parents. This
        # test asserts the generated include token itself: relative, free of
        # absolute parents and JSON escaping, and resolving to the declared
        # context header once backslashes are replaced by slashes.
        parents = ["ascii", "josé", "测试", 'quoted"name']
        if os.name == "nt":  # a quote is not a valid Windows filename character
            parents = [name for name in parents if '"' not in name]
        for parent_name in parents:
            with self.subTest(parent=parent_name), tempfile.TemporaryDirectory() as temporary:
                base = Path(temporary) / parent_name
                run_dir = base / "run"
                run_dir.mkdir(parents=True)
                with (
                    mock.patch.object(linttest.tidyengine, "clang_tool", return_value="pinned-clang-tidy"),
                    mock.patch.object(linttest.subprocess, "run", side_effect=self.native_result) as run,
                ):
                    success, _log = linttest._run_source_policy_smoke(run_dir, run_dir, plugin=run_dir / "verified.so")
                self.assertTrue(success)
                names = [name for name, *_ in linttest._source_policy_cases(Path("source-policy"))]
                include_cases = [name for name in names if self._declared_context_header(name) is not None]
                checked = 0
                for call in run.call_args_list:
                    command = call.args[0]
                    case = self._command_case(command)
                    if case not in include_cases:
                        continue
                    checked += 1
                    main = self._command_units(command)[-1]
                    source = main.read_text(encoding="utf-8")
                    if case in ("header-file-switch", "header-file-switch-unicode"):
                        self.assertTrue(source.startswith('#include "'))
                    token = self._first_include_token(source)
                    self.assertFalse(token.startswith("/"))
                    self.assertNotIn(str(base), token)
                    self.assertNotIn("\\u", token)  # ensure_ascii escaping must not reach the token
                    self.assertNotIn('\\"', token)  # ditto for quote escaping
                    header = self._assert_context_include(command, case, "native", windows=False)
                    header_source = header.read_text(encoding="utf-8")
                    self.assertNotIn("POSITIVE", header_source)
                    if case in ("header-file-switch", "header-file-switch-unicode"):
                        self.assertEqual(header_source.count("// NEGATIVE:"), 3)
                self.assertEqual(checked, len(include_cases))

    # ---------- macro spelling controls ----------

    def test_macro_spelling_controls_pin_primary_locations_to_main_use_lines(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            with (
                mock.patch.object(linttest.tidyengine, "clang_tool", return_value="pinned-clang-tidy"),
                mock.patch.object(linttest.subprocess, "run", side_effect=self.native_result) as run,
            ):
                success, _log = linttest._run_source_policy_smoke(root, root, plugin=root / "verified.so")
            self.assertTrue(success)
            seen = set()
            for call in run.call_args_list:
                command = call.args[0]
                case = self._command_case(command)
                if case not in MACRO_CASES:
                    continue
                seen.add(case)
                sources = self._recorded_sources(command)
                main = self._command_units(command)[-1]
                main_source = sources[main]
                headers = [path for path in sources if path.suffix == ".h"]
                self.assertEqual(len(headers), 1)
                header_source = sources[headers[0]]
                if case == "macro-spelling-exempt-definition":
                    self.assertIn("/lib/", main.as_posix())
                    self.assertIn("/test/", headers[0].as_posix())
                else:
                    self.assertIn("/test/", main.as_posix())
                    self.assertIn("/lib/", headers[0].as_posix())
                # Three non-contract macro definitions whose bodies spell the
                # raw violation tokens in the header.
                defines = DEFINE_RE.findall(header_source)
                self.assertEqual(len(defines), 3)
                for name in defines:
                    self.assertFalse(name.startswith("AO_"), "control macros must stay outside the contract namespace")
                for token in ("std::abort()", "throw 1", "std::filesystem::path"):
                    self.assertIn(token, header_source)
                for name in defines:
                    self.assertRegex(main_source, rf"\b{re.escape(name)}\b")
                self.assertNotIn("#define", main_source)
                # All three aliases are required exactly once per manifest.
                positive_lines = [line for line in main_source.splitlines() if "// POSITIVE:" in line]
                negative_lines = [line for line in main_source.splitlines() if "// NEGATIVE:" in line]
                self.assertEqual(len(positive_lines), 3)
                self.assertEqual(len(negative_lines), 3)
                for alias in linttest._SOURCE_POLICY_CHECKS:
                    self.assertEqual(sum(alias in line for line in positive_lines), 1)
                    self.assertEqual(sum(alias in line for line in negative_lines), 1)
                # Primary warnings sit on MAIN macro-use lines, never at the
                # #define lines in the header.
                self.assertNotIn("POSITIVE", header_source)

                def uses_macro(line, defines=defines):
                    return any(re.search(rf"\b{re.escape(name)}\b", line) for name in defines)

                if case == "macro-spelling-production-definition":
                    self.assertEqual([uses_macro(line) for line in positive_lines], [True, True, True])
                    self.assertEqual([uses_macro(line) for line in negative_lines], [False, False, False])
                else:
                    self.assertEqual([uses_macro(line) for line in positive_lines], [False, False, False])
                    self.assertEqual([uses_macro(line) for line in negative_lines], [True, True, True])
            self.assertEqual(set(seen), set(MACRO_CASES))

    # ---------- GNU line markers ----------

    def test_line_marker_numbers_track_physical_lines_and_host_spelling(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            with (
                mock.patch.object(linttest.tidyengine, "clang_tool", return_value="pinned-clang-tidy"),
                mock.patch.object(linttest.subprocess, "run", side_effect=self.native_result) as run,
            ):
                success, _log = linttest._run_source_policy_smoke(root, root, plugin=root / "verified.so")
            self.assertTrue(success)
            checked = 0
            for call in run.call_args_list:
                command = call.args[0]
                if self._command_case(command) != "same-file-system":
                    continue
                checked += 1
                main = self._command_units(command)[-1]
                source = main.read_text(encoding="utf-8")
                self.assertEqual(source.count("// POSITIVE:"), 6)
                self.assertEqual(source.count("// NEGATIVE:"), 3)
                markers = []
                for physical, line in enumerate(source.splitlines(), 1):
                    match = LINE_MARKER_RE.match(line)
                    if match is not None:
                        markers.append((physical, match))
                self.assertEqual(len(markers), 2)
                for physical, match in markers:
                    # The presumed line is derived from the assembled physical
                    # buffer (source.count("\n") + 2), not a duplicated constant.
                    self.assertEqual(int(match.group(1)), physical + 1)
                    # Line-marker filenames are json-encoded string literals in
                    # the host's native spelling.
                    self.assertEqual(f'"{match.group(2)}"', json.dumps(str(main)))
                self.assertEqual([match.group(3).strip() for _physical, match in markers], ["3", ""])
            self.assertEqual(checked, 1)

    # ---------- multi-translation-unit orders ----------

    def test_multi_tu_orders_match_their_descriptive_case_files(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            with (
                mock.patch.object(linttest.tidyengine, "clang_tool", return_value="pinned-clang-tidy"),
                mock.patch.object(linttest.subprocess, "run", side_effect=self.native_result) as run,
            ):
                success, _log = linttest._run_source_policy_smoke(root, root, plugin=root / "verified.so")
            self.assertTrue(success)
            orders = {}
            for call in run.call_args_list:
                command = call.args[0]
                case = self._command_case(command)
                if case not in ("multi-tu-exempt-first", "multi-tu-production-first"):
                    continue
                units = self._command_units(command)
                self.assertEqual(len(units), 2)
                self.assertTrue(all(case in unit.stem for unit in units))
                orders[case] = units
            self.assertEqual(set(orders), {"multi-tu-exempt-first", "multi-tu-production-first"})
            exempt_first = orders["multi-tu-exempt-first"]
            production_first = orders["multi-tu-production-first"]
            # The literal file components describe the argv order: the exempt
            # test TU and the production lib TU each lead in their own case.
            self.assertIn("/test/", exempt_first[0].as_posix())
            self.assertIn("/lib/", exempt_first[-1].as_posix())
            self.assertEqual(exempt_first[0].read_text(encoding="utf-8").count("// NEGATIVE:"), 3)
            self.assertEqual(exempt_first[-1].read_text(encoding="utf-8").count("// POSITIVE:"), 3)
            self.assertIn("/lib/", production_first[0].as_posix())
            self.assertIn("/test/", production_first[-1].as_posix())
            self.assertEqual(production_first[0].read_text(encoding="utf-8").count("// POSITIVE:"), 3)
            self.assertEqual(production_first[-1].read_text(encoding="utf-8").count("// NEGATIVE:"), 3)

    # ---------- POSIX case sensitivity ----------

    def test_posix_case_mismatches_keep_policy_without_changing_windows_semantics(self):
        for windows in (False, True):
            cases = linttest._source_policy_cases(Path("source-policy"), windows=windows)
            case_rules = {name: expected for name, _path, _roots, expected in cases}
            self.assertEqual(case_rules["case-varied-root"], not windows)
            self.assertEqual(case_rules["case-varied-test"], not windows)

    # ---------- Windows dual-profile smoke ----------

    def test_windows_smoke_uses_both_path_profiles_and_the_prepared_static_host(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            tools = linttest.WindowsFixtureTools("native-static-host", ("--driver-mode=cl", "/EHsc"))
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

    def test_parallel_smoke_bounds_children_and_freezes_every_task_input_per_invocation(self):
        for windows in (False, True):
            with self.subTest(windows=windows), tempfile.TemporaryDirectory() as temporary:
                root = Path(temporary)
                lock = threading.Lock()
                release = threading.Event()
                active = 0
                peak = 0
                prepared_bytes = {}
                call_inputs = {}

                def native(
                    command,
                    lock=lock,
                    release=release,
                    prepared_bytes=prepared_bytes,
                    call_inputs=call_inputs,
                    root=root,
                    **kwargs,
                ):
                    nonlocal active, peak
                    # Every argv translation unit and its included context
                    # header, snapshotted per invocation -- not just the first
                    # .cpp argument.
                    files = self._recorded_sources(command)
                    before = {path: path.read_bytes() for path in files}
                    key = tuple(sorted(before))
                    with lock:
                        if not prepared_bytes:
                            prepared_bytes.update(
                                (path, path.read_bytes()) for path in root.rglob("*") if path.suffix in (".cpp", ".h")
                            )
                        active += 1
                        peak = max(peak, active)
                        call_inputs.setdefault(key, before)
                        if active == 4:
                            release.set()
                    try:
                        self.assertTrue(release.wait(timeout=5), "four source-policy workers did not start")
                        self.assertEqual({path: path.read_bytes() for path in before}, before)
                        result = self.native_result(command, **kwargs)
                        self.assertEqual({path: path.read_bytes() for path in before}, before)
                        return result
                    finally:
                        with lock:
                            active -= 1

                tools = linttest.WindowsFixtureTools("static-host", ()) if windows else None
                with (
                    mock.patch.dict(os.environ, {phases.LOG_DIR_ENV: str(root / "phases")}),
                    mock.patch.object(linttest.tidyengine, "clang_tool", return_value="pinned-clang-tidy"),
                    mock.patch.object(linttest.subprocess, "run", side_effect=native) as run,
                    phases.measure("command", command="test"),
                    phases.measure("lint-source-policy", tree="tidy", jobs=4) as phase,
                ):
                    success, log = linttest._run_source_policy_smoke(
                        root, root, plugin=None if windows else root / "verified.so", tools=tools, jobs=4
                    )
                    phase.status = 0 if success else 1
                self.assertTrue(success, log.read_text(encoding="utf-8"))
                self.assertEqual(peak, 4)
                self.assertEqual(active, 0)
                self.assertTrue(prepared_bytes)
                self.assertEqual({path: path.read_bytes() for path in prepared_bytes}, prepared_bytes)
                self.assertEqual(run.call_count, linttest._source_policy_case_count(windows=windows))
                # One distinct frozen input set per prepared task; a task's
                # dump and semantic invocations share the same inputs.
                self.assertEqual(len(call_inputs), linttest._source_policy_task_count(windows=windows))

                # Filesystem identity verifies coverage even when APFS retains
                # an earlier directory case or a native path has an alias. The
                # separate policy assertions still compare literal spellings.
                def file_identities(paths):
                    identities = set()
                    for path in paths:
                        metadata = path.stat()
                        identities.add((metadata.st_dev, metadata.st_ino))
                    return identities

                self.assertEqual(
                    file_identities(path for key in call_inputs for path in key),
                    file_identities(prepared_bytes),
                )
                for key, snapshot in call_inputs.items():
                    self.assertTrue(key)
                    self.assertEqual({path: path.read_bytes() for path in key}, snapshot)
                names = [name for name, *_ in linttest._source_policy_cases(Path("source-policy"), windows=windows)]
                two_file_cases = sum(
                    name.startswith(("header-file-switch", "macro-spelling-", "multi-tu-")) for name in names
                )
                multi_input_keys = [
                    key
                    for key in call_inputs
                    if any(path.stem.startswith(("header-file-switch", "macro-spelling-", "multi-tu-")) for path in key)
                ]
                self.assertEqual(len(multi_input_keys), two_file_cases * (2 if windows else 1))
                self.assertTrue(all(len(key) == 2 for key in multi_input_keys))
                labels = [name for name, *_ in linttest._source_policy_cases(root, windows=windows)]
                positions = [log.read_text(encoding="utf-8").index(f"=== {name}/native ===") for name in labels]
                self.assertEqual(positions, sorted(positions))

    # ---------- lint gate integration ----------

    def test_source_policy_failure_fails_the_lint_gate_and_preserves_its_workspace(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            (root / "compile_commands.json").write_text("[]", encoding="utf-8")
            plugin = root / "tool/lint/libAobusLintPlugin.so"
            plugin.parent.mkdir(parents=True)
            plugin.touch()
            source = root / "Fixture.cpp"
            source.write_text("int value;\n", encoding="utf-8")
            log = root / "failure.log"
            log.write_text("source policy control failed\n", encoding="utf-8")
            with (
                mock.patch.object(linttest.builddir, "platform_profile", return_value=builddir.LINUX_PROFILE),
                mock.patch.object(
                    linttest, "discover_fixtures", return_value=[linttest.Fixture(source, "aobus-example")]
                ),
                mock.patch.object(linttest, "_run_diagnostic", return_value=(True, log)),
                mock.patch.object(linttest, "_run_source_policy_smoke", return_value=(False, log)),
                mock.patch.object(linttest, "_run_compiler_error_smoke", return_value=(True, log)),
                mock.patch.object(linttest.shutil, "rmtree") as cleanup,
                contextlib.redirect_stdout(io.StringIO()),
            ):
                self.assertEqual(linttest.run(root, jobs=1), 1)
            cleanup.assert_not_called()


if __name__ == "__main__":
    unittest.main()
