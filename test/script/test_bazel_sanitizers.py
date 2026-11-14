"""Pure admission, retained runtime transport and bounded sanitizer receipts.

Selection parsing, configuration joins, runtime policy, phase recording and BEP
validation stay real. Only native SDK/workspace selection, owned external service
lifecycles, the two independent oracle entry points and the Bazel subprocess are
faked. This establishes orchestration, not native compiler/runtime acceptance.
"""

import argparse
import contextlib
import hashlib
import io
import json
import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

from ao.core import bazel_sanitizers
from ao.core.bazel_native import BazelWorkspaceError
from ao.core.bazel_test_services import stable_service_root
from ao.core.paths import PROJECT_ROOT

CHECKSUM = "a" * 64
APPLICATION = "//test:application"
BACKEND = "//test:backend"
PLATFORM_TEST = "//test:platform"


class SanitizerAdmissionTests(unittest.TestCase):
    def test_admission_uses_only_the_explicit_request_and_platform(self):
        with (
            mock.patch("ao.core.bazel_native.native_platform", side_effect=AssertionError("host lookup")),
            mock.patch("ao.core.bazel_workspace.workspace_path", side_effect=AssertionError("tree lookup")),
            mock.patch("ao.core.bazel_workspace.prepare_workspace", side_effect=AssertionError("SDK lookup")),
            mock.patch("subprocess.run", side_effect=AssertionError("process")),
        ):
            for config in ("debug", "release", "asan", "tsan"):
                with self.subTest(config=config):
                    self.assertIsNone(bazel_sanitizers.validate_request(config, "linux"))
            self.assertIsNone(bazel_sanitizers.validate_request("asan", "windows"))
            self.assertIsNone(bazel_sanitizers.validate_request("tsan", "macos"))
            for config, platform, message in (
                ("tsan", "windows", "Windows"),
                ("coverage", "linux", "unknown"),
                ("asan", "unknown", "unsupported"),
            ):
                with (
                    self.subTest(config=config, platform=platform),
                    self.assertRaisesRegex(BazelWorkspaceError, message),
                ):
                    bazel_sanitizers.validate_request(config, platform)

    def test_profiles_reuse_debug_sdk_mode_without_changing_release(self):
        self.assertEqual(bazel_sanitizers.SANITIZER_CONFIGS, ("asan", "tsan"))
        for config, expected in (("asan", "debug"), ("tsan", "debug"), ("debug", "debug"), ("release", "release")):
            with self.subTest(config=config):
                self.assertEqual(bazel_sanitizers.compilation_config(config), expected)
        with self.assertRaises(BazelWorkspaceError):
            bazel_sanitizers.compilation_config("opt")

    def test_direct_run_rejects_unsupported_or_custom_requests_before_tree_and_sdk(self):
        for config, platform, targets in (
            ("tsan", "windows", []),
            ("unknown", "linux", []),
            ("debug", "linux", []),
            ("asan", "linux", ["//test:custom"]),
            ("asan", "linux", ["--test_filter=example"]),
        ):
            with (
                self.subTest(config=config, platform=platform, targets=targets),
                mock.patch("ao.core.bazel_native.native_platform", return_value=platform),
                mock.patch("ao.core.bazel_workspace.workspace_path") as tree,
                mock.patch("ao.core.bazel_sanitizers.build_tree_lock") as lock,
                mock.patch("ao.core.bazel_workspace.prepare_workspace") as prepare,
                mock.patch("ao.core.bazel_workspace.bazel_executable") as executable,
            ):
                with self.assertRaises(BazelWorkspaceError):
                    bazel_sanitizers.run(argparse.Namespace(config=config, targets=targets))
                tree.assert_not_called()
                lock.assert_not_called()
                prepare.assert_not_called()
                executable.assert_not_called()


class SanitizerOptionGrammarTests(unittest.TestCase):
    def test_parser_accepts_all_runtime_separators_and_preserves_empty_values_and_duplicate_order(self):
        self.assertEqual(
            bazel_sanitizers._parse_options(" \t:verbosity=0,,history_size=7\r\n:empty=:verbosity=1, ", "ASAN_OPTIONS"),
            [("verbosity", "0"), ("history_size", "7"), ("empty", ""), ("verbosity", "1")],
        )
        self.assertEqual(bazel_sanitizers._parse_options(" : ,\t\r\n", "ASAN_OPTIONS"), [])

    def test_parser_keeps_quoted_drive_paths_separators_and_backslashes_literal(self):
        self.assertEqual(
            bazel_sanitizers._parse_options(
                'suppressions="C:\\Program Files\\Aobus, rules:exitcode=0.supp":'
                "label='a \"double\" word\\literal' expression=name=value",
                "UBSAN_OPTIONS",
            ),
            [
                ("suppressions", "C:\\Program Files\\Aobus, rules:exitcode=0.supp"),
                ("label", 'a "double" word\\literal'),
                ("expression", "name=value"),
            ],
        )
        self.assertEqual(
            bazel_sanitizers._parse_options("first='C:\\literal\\':second=\"D:\\literal\\\"", "TSAN_OPTIONS"),
            [("first", "C:\\literal\\"), ("second", "D:\\literal\\")],
        )

    def test_parser_rejects_malformed_input_instead_of_partially_accepting_it(self):
        for text in (
            "=value",
            "verbosity",
            "verbosity=0 missing-equals",
            'suppressions="unterminated',
            "suppressions='unterminated",
            'suppressions="valid"junk',
            "first='mismatched\"",
            "first=good\x00tail",
            "first\x00=good",
            "suppressions=C:\\unquoted\\rules.supp",
        ):
            with self.subTest(text=text), self.assertRaisesRegex(BazelWorkspaceError, "ASAN_OPTIONS"):
                bazel_sanitizers._parse_options(text, "ASAN_OPTIONS")

    def test_renderer_uses_native_quotes_without_shell_escaping_or_changing_plain_values(self):
        for name, value, expected in (
            ("halt_on_error", "1", "halt_on_error=1"),
            ("empty", "", "empty="),
            (
                "external_symbolizer_path",
                "/nix/store/llvm/bin/llvm-symbolizer",
                "external_symbolizer_path=/nix/store/llvm/bin/llvm-symbolizer",
            ),
            (
                "suppressions",
                "C:\\Program Files\\Aobus\\rules.supp",
                'suppressions="C:\\Program Files\\Aobus\\rules.supp"',
            ),
            ("suppressions", "/tmp/odd:dir/rules, extra.supp", 'suppressions="/tmp/odd:dir/rules, extra.supp"'),
            ("label", 'a "double" word', "label='a \"double\" word'"),
            ("label", "'leading", 'label="\'leading"'),
            ("label", '"leading', "label='\"leading'"),
            ("label", "plain\"and'literal", "label=plain\"and'literal"),
            ("label", "line\tbreak\r\n", 'label="line\tbreak\r\n"'),
        ):
            with self.subTest(value=value):
                self.assertEqual(bazel_sanitizers._render_option(name, value), expected)

    def test_renderer_rejects_values_that_cannot_be_represented_without_escaping(self):
        for value in ('both "quote\'s" with space', "nul\x00value"):
            with self.subTest(value=value), self.assertRaises(BazelWorkspaceError):
                bazel_sanitizers._render_option("label", value)


class FrozenRuntimeTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(
            prefix="aobus-sanitizer-runtime-test-", dir="/tmp" if sys.platform == "linux" else None
        )
        self.addCleanup(temporary.cleanup)
        self.directory = Path(temporary.name) / "runtime inputs, owned"
        self.directory.mkdir()
        self.symbolizer = self.directory / "llvm-symbolizer"
        self.symbolizer.write_bytes(b"fixture symbolizer\n")
        selection = mock.patch(
            "ao.core.bazel_sanitizers._symbolizer",
            side_effect=lambda platform, _caller: None if platform == "windows" else self.symbolizer,
        )
        selection.start()
        self.addCleanup(selection.stop)

    def test_linux_asan_preserves_nonowned_options_and_forces_positive_report_status_without_global_writes(self):
        caller = {
            "ASAN_OPTIONS": (
                "exitcode=0:detect_container_overflow=1:halt_on_error=0:allocator_may_return_null=1:exitcode=0:symbolize=0"
            ),
            "UBSAN_OPTIONS": (
                'log_path="C:/reports/ubsan":halt_on_error=0:print_stacktrace=0:exitcode=0:halt_on_error=0:exitcode=0'
            ),
            "LSAN_OPTIONS": "suppressions=/missing/caller-rules:verbosity=1:exitcode=0",
            "ASAN_SYMBOLIZER_PATH": str(self.symbolizer),
        }
        before = dict(os.environ)
        frozen = bazel_sanitizers.freeze_runtime("asan", "linux", caller, self.directory / "tsan-named-tree")
        self.assertEqual(dict(os.environ), before)
        self.assertEqual(
            frozen.environment["ASAN_OPTIONS"],
            "detect_container_overflow=1:allocator_may_return_null=1:symbolize=0:"
            f'external_symbolizer_path="{self.symbolizer}":halt_on_error=1:exitcode=1',
        )
        self.assertEqual(
            frozen.environment["UBSAN_OPTIONS"],
            f'log_path="C:/reports/ubsan":external_symbolizer_path="{self.symbolizer}":'
            "halt_on_error=1:print_stacktrace=1:exitcode=1",
        )
        self.assertEqual(frozen.environment["ASAN_SYMBOLIZER_PATH"], caller["ASAN_SYMBOLIZER_PATH"])
        suppression = Path(next(iter(frozen.suppression_hashes)))
        self.assertEqual(frozen.environment["LSAN_OPTIONS"], f'suppressions="{suppression}":exitcode=23')
        self.assertTrue(suppression.is_relative_to(self.directory))
        self.assertEqual(suppression.read_bytes(), (PROJECT_ROOT / "script/ao/lsan.supp").read_bytes())
        self.assertNotIn("verbosity", frozen.environment["LSAN_OPTIONS"])
        caller["ASAN_OPTIONS"] = "changed-after-freeze"
        self.assertIn("detect_container_overflow=1", frozen.environment["ASAN_OPTIONS"])
        with self.assertRaises(TypeError):
            frozen.environment["ASAN_OPTIONS"] = "not immutable"
        frozen.verify()

    def test_tsan_merges_all_caller_rules_and_retains_exact_content_addressed_bytes(self):
        first = self.directory / "first caller.supp"
        second = self.directory / "second.supp"
        first.write_bytes(b"race:caller_one\n")
        second.write_bytes(b"race:caller_two\n\n")
        caller = {
            "TSAN_OPTIONS": (
                f'exitcode=0,suppressions="{first}" history_size=7\tsuppressions="{second}":'
                "halt_on_error=0:second_deadlock_stack=0:exitcode=0"
            ),
            "ASAN_OPTIONS": "inactive raw options",
            "UBSAN_OPTIONS": 'inactive="unterminated',
            "LSAN_OPTIONS": "also inherited raw",
        }
        frozen = bazel_sanitizers.freeze_runtime("tsan", "linux", caller, self.directory / "attempt-one")
        for name in ("ASAN_OPTIONS", "UBSAN_OPTIONS", "LSAN_OPTIONS"):
            self.assertEqual(frozen.environment[name], caller[name])
        self.assertEqual(len(frozen.suppression_hashes), 1)
        path, digest = next(iter(frozen.suppression_hashes.items()))
        expected = (
            b"race:caller_one\n\nrace:caller_two\n\n"
            + (PROJECT_ROOT / "script/ao/tsan.supp").read_bytes().rstrip()
            + b"\n"
        )
        self.assertEqual(Path(path).read_bytes(), expected)
        self.assertEqual(digest, hashlib.sha256(expected).hexdigest())
        self.assertEqual(frozen.input_hashes[str(first)], hashlib.sha256(b"race:caller_one\n").hexdigest())
        self.assertEqual(
            frozen.environment["TSAN_OPTIONS"],
            f'history_size=7:suppressions="{path}":external_symbolizer_path="{self.symbolizer}":'
            "halt_on_error=1:second_deadlock_stack=1:exitcode=66",
        )
        first.write_bytes(b"race:changed-caller\n")
        frozen.verify()
        other = bazel_sanitizers.freeze_runtime("tsan", "linux", {}, self.directory / "attempt-two")
        self.assertTrue(set(frozen.suppression_hashes).isdisjoint(other.suppression_hashes))
        self.assertEqual(Path(path).read_bytes(), expected)
        Path(path).write_bytes(b"race:tampered\n")
        with self.assertRaisesRegex(BazelWorkspaceError, "suppression changed"):
            frozen.verify()

    def test_unchanged_rules_keep_one_snapshot_path_and_a_tampered_one_is_restored(self):
        store = self.directory / "tree sanitizers"
        first = bazel_sanitizers.freeze_runtime("tsan", "linux", {}, store)
        again = bazel_sanitizers.freeze_runtime("tsan", "linux", {}, store)
        # The hashed test environment repeats, so Bazel may reuse a cached result.
        self.assertEqual(again.environment["TSAN_OPTIONS"], first.environment["TSAN_OPTIONS"])
        rules = self.directory / "caller.supp"
        rules.write_bytes(b"race:caller\n")
        changed = bazel_sanitizers.freeze_runtime("tsan", "linux", {"TSAN_OPTIONS": f'suppressions="{rules}"'}, store)
        self.assertNotEqual(changed.environment["TSAN_OPTIONS"], first.environment["TSAN_OPTIONS"])
        snapshot = Path(next(iter(first.suppression_hashes)))
        snapshot.write_bytes(b"race:tampered\n")
        with self.assertRaisesRegex(BazelWorkspaceError, "suppression changed"):
            first.verify()
        restored = bazel_sanitizers.freeze_runtime("tsan", "linux", {}, store)
        self.assertEqual(restored.environment["TSAN_OPTIONS"], first.environment["TSAN_OPTIONS"])
        restored.verify()

    def test_tsan_empty_and_unreadable_caller_rules_fail_instead_of_being_dropped(self):
        for index, value in enumerate(
            ("suppressions=", 'suppressions=""', "suppressions", f'suppressions="{self.directory / "missing"}"')
        ):
            with self.subTest(value=value), self.assertRaises((BazelWorkspaceError, OSError)):
                bazel_sanitizers.freeze_runtime(
                    "tsan", "linux", {"TSAN_OPTIONS": value}, self.directory / f"attempt-{index}"
                )

    def test_mixed_separator_caller_rules_are_frozen_without_raw_references_or_dropped_options(self):
        rules = {name: self.directory / f"{name} caller.supp" for name in ("asan", "ubsan", "lsan")}
        payloads = {name: f"{name}:caller\n".encode() for name in rules}
        for platform in ("linux", "macos"):
            with self.subTest(platform=platform):
                for name, source in rules.items():
                    source.write_bytes(payloads[name])
                caller = {
                    "ASAN_OPTIONS": f'verbosity=0,suppressions="{rules["asan"]}" detect_leaks=1:verbosity=1',
                    "UBSAN_OPTIONS": f'halt_on_error=0 suppressions="{rules["ubsan"]}",report_error_type=1',
                    "LSAN_OPTIONS": f'verbosity=1 suppressions="{rules["lsan"]}"',
                }
                attempt = self.directory / f"run-{platform}"
                frozen = bazel_sanitizers.freeze_runtime("asan", platform, caller, attempt)
                copies = {}
                for name in ("asan", "ubsan", "lsan") if platform == "macos" else ("asan", "ubsan"):
                    matches = [
                        Path(path) for path in frozen.suppression_hashes if Path(path).read_bytes() == payloads[name]
                    ]
                    self.assertEqual(len(matches), 1)
                    copies[name] = matches[0]
                    self.assertTrue(copies[name].is_relative_to(attempt))
                    self.assertEqual(frozen.input_hashes[str(rules[name])], hashlib.sha256(payloads[name]).hexdigest())
                    self.assertEqual(
                        frozen.suppression_hashes[str(copies[name])], hashlib.sha256(payloads[name]).hexdigest()
                    )
                self.assertEqual(
                    frozen.environment["ASAN_OPTIONS"],
                    f'verbosity=0:suppressions="{copies["asan"]}":detect_leaks=1:verbosity=1:'
                    f'external_symbolizer_path="{self.symbolizer}":halt_on_error=1:exitcode=1',
                )
                self.assertEqual(
                    frozen.environment["UBSAN_OPTIONS"],
                    f'suppressions="{copies["ubsan"]}":report_error_type=1:'
                    f'external_symbolizer_path="{self.symbolizer}":halt_on_error=1:print_stacktrace=1:exitcode=1',
                )
                if platform == "macos":
                    self.assertEqual(frozen.environment["LSAN_OPTIONS"], f'verbosity=1:suppressions="{copies["lsan"]}"')
                else:
                    self.assertNotIn(str(rules["lsan"]), frozen.input_hashes)
                    remaining = [Path(path) for path in frozen.suppression_hashes if Path(path) not in copies.values()]
                    self.assertEqual(len(remaining), 1)
                    self.assertEqual(remaining[0].read_bytes(), (PROJECT_ROOT / "script/ao/lsan.supp").read_bytes())
                    self.assertEqual(frozen.environment["LSAN_OPTIONS"], f'suppressions="{remaining[0]}":exitcode=23')
                for name, copy in copies.items():
                    rules[name].write_bytes(b"changed original source\n")
                    self.assertEqual(copy.read_bytes(), payloads[name])
                frozen.verify()
                copies["asan"].write_bytes(b"tampered\n")
                with self.assertRaisesRegex(BazelWorkspaceError, "suppression changed"):
                    frozen.verify()
        for index, value in enumerate(
            ("suppressions=", 'suppressions=""', "suppressions", f'suppressions="{self.directory / "missing"}"')
        ):
            with self.subTest(value=value), self.assertRaises((BazelWorkspaceError, OSError)):
                bazel_sanitizers.freeze_runtime(
                    "asan", "linux", {"UBSAN_OPTIONS": value}, self.directory / f"invalid-{index}"
                )

    def test_windows_asan_forces_positive_status_but_preserves_unix_options_and_selected_dll_path(self):
        caller = {
            "ASAN_OPTIONS": "halt_on_error=0:exitcode=0:detect_container_overflow=1:exitcode=0",
            "UBSAN_OPTIONS": "caller-ubsan",
            "LSAN_OPTIONS": "caller-lsan",
            "TSAN_OPTIONS": 'inactive="unterminated',
            "PATH": "C:\\selected VS\\bin;C:\\Windows\\System32",
        }
        frozen = bazel_sanitizers.freeze_runtime("asan", "windows", caller, self.directory / "tsan-name")
        for name in ("UBSAN_OPTIONS", "LSAN_OPTIONS", "TSAN_OPTIONS", "PATH"):
            self.assertEqual(frozen.environment[name], caller[name])
        self.assertEqual(frozen.environment["ASAN_OPTIONS"], "detect_container_overflow=1:halt_on_error=1:exitcode=1")
        self.assertIsNone(frozen.symbolizer)
        self.assertEqual(dict(frozen.suppression_hashes), {})
        self.assertEqual(dict(frozen.input_hashes), {})

    def test_windows_profile_freezes_quoted_caller_rules_without_synthetic_drive_files(self):
        source = self.directory / "caller rules, extra.supp"
        source.write_bytes(b"interceptor_via_fun:caller_marker\n")
        caller = {
            "ASAN_OPTIONS": f'log_path="C:\\diagnostics\\reports":suppressions="{source}",verbosity=1',
            "UBSAN_OPTIONS": "inactive raw ubsan",
            "LSAN_OPTIONS": "inactive raw lsan",
        }
        frozen = bazel_sanitizers.freeze_runtime("asan", "windows", caller, self.directory / "windows attempt")
        self.assertEqual(len(frozen.suppression_hashes), 1)
        copy = Path(next(iter(frozen.suppression_hashes)))
        self.assertEqual(
            frozen.environment["ASAN_OPTIONS"],
            f'log_path="C:\\diagnostics\\reports":suppressions="{copy}":verbosity=1:halt_on_error=1:exitcode=1',
        )
        self.assertEqual(copy.read_bytes(), b"interceptor_via_fun:caller_marker\n")
        self.assertEqual(frozen.input_hashes[str(source)], hashlib.sha256(copy.read_bytes()).hexdigest())
        source.write_bytes(b"original changed\n")
        frozen.verify()
        self.assertEqual(copy.read_bytes(), b"interceptor_via_fun:caller_marker\n")
        for name in ("UBSAN_OPTIONS", "LSAN_OPTIONS"):
            self.assertEqual(frozen.environment[name], caller[name])

    def test_malformed_active_options_fail_before_tool_selection_or_runtime_file_creation(self):
        for index, (config, platform, name, text) in enumerate(
            (
                ("asan", "windows", "ASAN_OPTIONS", 'halt_on_error="unterminated'),
                ("asan", "linux", "UBSAN_OPTIONS", "halt_on_error=0,missing-equals"),
                ("asan", "macos", "LSAN_OPTIONS", "suppressions='unterminated"),
                ("tsan", "linux", "TSAN_OPTIONS", 'history_size="7"junk'),
            )
        ):
            attempt = self.directory / f"malformed-{index}"
            with (
                self.subTest(name=name, text=text),
                mock.patch("ao.core.bazel_sanitizers._symbolizer", side_effect=AssertionError("tool selection")),
                self.assertRaisesRegex(BazelWorkspaceError, name),
            ):
                bazel_sanitizers.freeze_runtime(config, platform, {name: text}, attempt)
            self.assertFalse(attempt.exists())

    def test_macos_asan_forces_ubsan_but_does_not_inject_linux_leak_rules(self):
        frozen = bazel_sanitizers.freeze_runtime("asan", "macos", {}, self.directory)
        self.assertEqual(
            frozen.environment["UBSAN_OPTIONS"],
            f'external_symbolizer_path="{self.symbolizer}":halt_on_error=1:print_stacktrace=1:exitcode=1',
        )
        self.assertEqual(frozen.environment["LSAN_OPTIONS"], "")
        self.assertEqual(dict(frozen.suppression_hashes), {})
        self.assertEqual(frozen.input_hashes[str(self.symbolizer)], hashlib.sha256(b"fixture symbolizer\n").hexdigest())


class SymbolizerAdmissionTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(
            prefix="aobus-symbolizer-admission-test-", dir="/tmp" if sys.platform == "linux" else None
        )
        self.addCleanup(temporary.cleanup)
        self.directory = Path(temporary.name) / "symbolizer fixtures"
        self.llvm = self.directory / "selected LLVM"
        self.symbolizer = self.llvm / "bin/llvm-symbolizer"
        self.symbolizer.parent.mkdir(parents=True)
        self.symbolizer.write_bytes(b"selected LLVM symbolizer bytes\n")
        self.symbolizer.chmod(0o755)
        self.metadata = {"tools": {"ar": str(self.llvm / "bin/llvm-ar")}}

    def test_linux_uses_only_the_selected_archive_tools_sibling_and_freezes_its_bytes(self):
        with (
            mock.patch("ao.core.bazel_compiler_probe.nix_cc_wrapper", return_value=self.metadata) as metadata,
            mock.patch("shutil.which", side_effect=AssertionError("ambient tool search")),
            mock.patch("subprocess.run", side_effect=AssertionError("tool execution")),
        ):
            frozen = bazel_sanitizers.freeze_runtime(
                "asan",
                "linux",
                {"PATH": "/ambient/bin", "AOBUS_LLVM_ROOT": "/unrelated/llvm"},
                self.directory / "attempt",
            )
        metadata.assert_called_once_with()
        selected = self.symbolizer.resolve()
        self.assertEqual(frozen.symbolizer, selected)
        self.assertEqual(frozen.environment["ASAN_SYMBOLIZER_PATH"], str(selected))
        self.assertEqual(frozen.environment["UBSAN_SYMBOLIZER_PATH"], str(selected))
        self.assertEqual(
            frozen.environment["ASAN_OPTIONS"], f'external_symbolizer_path="{selected}":halt_on_error=1:exitcode=1'
        )
        self.assertEqual(
            frozen.environment["UBSAN_OPTIONS"],
            f'external_symbolizer_path="{selected}":halt_on_error=1:print_stacktrace=1:exitcode=1',
        )
        self.assertEqual(
            frozen.input_hashes[str(selected)], hashlib.sha256(b"selected LLVM symbolizer bytes\n").hexdigest()
        )
        self.assertEqual(frozen.receipt()["symbolizer"], str(selected))
        frozen.verify()
        self.symbolizer.write_bytes(b"different executable bytes\n")
        with self.assertRaisesRegex(BazelWorkspaceError, "symbolizer changed"):
            frozen.verify()

    def test_macos_uses_selected_llvm_and_normalizes_matching_common_option_without_claiming_forced_frames(self):
        caller = {
            "AOBUS_LLVM_ROOT": str(self.llvm),
            "TSAN_SYMBOLIZER_PATH": str(self.symbolizer),
            "TSAN_OPTIONS": (
                f'external_symbolizer_path="{self.symbolizer}":history_size=4:symbolize=0:halt_on_error=0:exitcode=0'
            ),
        }
        with (
            mock.patch("ao.core.bazel_compiler_probe.nix_cc_wrapper", side_effect=AssertionError("Linux metadata")),
            mock.patch("shutil.which", side_effect=AssertionError("ambient tool search")),
            mock.patch("subprocess.run", side_effect=AssertionError("tool execution")),
        ):
            frozen = bazel_sanitizers.freeze_runtime("tsan", "macos", caller, self.directory / "attempt")
        suppression = next(iter(frozen.suppression_hashes))
        selected = self.symbolizer.resolve()
        self.assertEqual(frozen.environment["TSAN_SYMBOLIZER_PATH"], str(selected))
        self.assertEqual(
            frozen.environment["TSAN_OPTIONS"],
            f'history_size=4:symbolize=0:suppressions="{suppression}":external_symbolizer_path="{selected}":'
            "halt_on_error=1:second_deadlock_stack=1:exitcode=66",
        )
        self.assertIn("caller symbolize=0 retained", frozen.receipt()["symbolization"])

    def test_missing_provider_or_selected_tool_fails_without_ambient_fallback_or_runtime_writes(self):
        for index, metadata in enumerate(
            (
                {},
                {"tools": {"ar": "relative/llvm-ar"}},
                {"tools": {"ar": str(self.directory / "missing/bin/llvm-ar")}},
            )
        ):
            attempt = self.directory / f"attempt-{index}"
            with (
                self.subTest(metadata=metadata),
                mock.patch("ao.core.bazel_compiler_probe.nix_cc_wrapper", return_value=metadata),
                mock.patch("shutil.which", side_effect=AssertionError("ambient tool search")),
                self.assertRaises(BazelWorkspaceError),
            ):
                bazel_sanitizers.freeze_runtime("asan", "linux", {}, attempt)
            self.assertFalse(attempt.exists())
        with self.assertRaisesRegex(BazelWorkspaceError, "AOBUS_LLVM_ROOT"):
            bazel_sanitizers.freeze_runtime("asan", "macos", {}, self.directory / "missing-macos-provider")

    def test_windows_uses_selected_native_dll_path_without_llvm_probes_or_tool_execution(self):
        caller = {"PATH": "C:\\selected VS\\bin", "ASAN_OPTIONS": "halt_on_error=0:exitcode=0"}
        with (
            mock.patch("ao.core.bazel_compiler_probe.nix_cc_wrapper", side_effect=AssertionError("LLVM metadata")),
            mock.patch("shutil.which", side_effect=AssertionError("ambient tool search")),
            mock.patch("subprocess.run", side_effect=AssertionError("tool execution")),
        ):
            frozen = bazel_sanitizers.freeze_runtime("asan", "windows", caller, self.directory / "attempt")
        self.assertIsNone(frozen.symbolizer)
        self.assertEqual(frozen.environment["PATH"], caller["PATH"])
        self.assertEqual(frozen.environment["ASAN_OPTIONS"], "halt_on_error=1:exitcode=1")
        self.assertEqual(dict(frozen.input_hashes), {})

    def test_conflicting_caller_environment_and_common_options_are_rejected_not_silently_replaced(self):
        other = self.directory / "other-llvm-symbolizer"
        other.write_bytes(self.symbolizer.read_bytes())
        for index, (profile, name) in enumerate(
            (
                ("asan", "ASAN_SYMBOLIZER_PATH"),
                ("tsan", "TSAN_SYMBOLIZER_PATH"),
                ("asan", "ASAN_OPTIONS"),
                ("asan", "UBSAN_OPTIONS"),
                ("tsan", "TSAN_OPTIONS"),
            )
        ):
            value = f'external_symbolizer_path="{other}"' if name.endswith("_OPTIONS") else str(other)
            with (
                self.subTest(profile=profile, name=name),
                mock.patch("ao.core.bazel_compiler_probe.nix_cc_wrapper", return_value=self.metadata),
                self.assertRaisesRegex(BazelWorkspaceError, "conflicts with the governed"),
            ):
                bazel_sanitizers.freeze_runtime(profile, "linux", {name: value}, self.directory / f"attempt-{index}")


@unittest.skipIf(os.name == "nt", "the Linux TSan SDK policy only runs on POSIX hosts with /nix/store paths")
class TsanInterfaceRootTests(unittest.TestCase):
    """The compile-contract allowlist admits only the selected SDK interface root."""

    def setUp(self):
        temporary = tempfile.TemporaryDirectory(
            prefix="aobus-tsan-interface-test-", dir="/tmp" if sys.platform == "linux" else None
        )
        self.addCleanup(temporary.cleanup)
        self.directory = Path(temporary.name)
        # A synthetic store path mapped onto the fixture: no real SDK is read.
        self.selected = Path("/nix/store") / (self.directory.name + "-compiler-rt-dev") / "include"
        self.actual = self.directory / "sdk/include"
        (self.actual / "sanitizer").mkdir(parents=True)
        for name in ("tsan_interface.h", "common_interface_defs.h"):
            (self.actual / "sanitizer" / name).write_text("// Synthetic interface\n", encoding="utf-8")

    def roots(self, metadata, config="tsan", platform="linux"):
        is_dir, is_file = Path.is_dir, Path.is_file

        def local(path):
            if path == self.selected or path.is_relative_to(self.selected):
                return self.actual / path.relative_to(self.selected)
            if path.is_relative_to("/nix/store"):
                raise AssertionError("unexpected SDK path: " + str(path))
            return path

        with (
            mock.patch("ao.core.bazel_compiler_probe.nix_cc_wrapper", return_value=metadata),
            mock.patch.object(Path, "is_dir", lambda path: is_dir(local(path))),
            mock.patch.object(Path, "is_file", lambda path: is_file(local(path))),
        ):
            return bazel_sanitizers.compiler_include_roots(config, platform)

    def test_linux_tsan_admits_the_raw_selected_root_with_its_interface_headers(self):
        self.assertEqual(self.roots({"sanitizerIncludeDirectory": str(self.selected)}), (str(self.selected),))


class _ManagedFixture:
    """A small graph witness; never a duplicate of the production test inventory."""

    def __init__(self, case, directory, *, profile="asan", platform="linux"):
        # Match the portal's canonical tree identity, including macOS's
        # /var -> /private/var temporary-directory alias.
        directory = directory.resolve()
        self.case = case
        self.tree = directory / "tree"
        self.base = directory / "base"
        self.base.mkdir()
        self.symbolizer = directory / "fixture symbolizer"
        self.symbolizer.write_bytes(b"fixture symbolizer\n")
        self.profile = profile
        self.platform = platform
        self.root = f"//:{profile}_tests"
        (self.base / "aobus.bazelrc").write_text(
            f"build --config={profile}\ncommon --repo_env=AOBUS_FIXTURE_SDK=selected-debug\n", encoding="utf-8"
        )
        self.args = argparse.Namespace(
            config=profile, path=str(self.tree), targets=[], jobs=16, vcpkg_prefix=None, remote_cache="grpc://cache"
        )
        self.invocations = []
        self.events = []
        self.rc_bytes = None
        self.prepared = None
        self.bep_mutation = None
        self.command_error = None
        self.service_error = None
        self.cleanup_error = None
        self.oracle_status = 0
        self.tamper = None
        self.all_incompatible = False
        self.omit_configured_label = False

    @property
    def native_tests(self):
        return [BACKEND]

    @property
    def selected(self):
        return [APPLICATION, *self.native_tests, PLATFORM_TEST]

    @property
    def compatible(self):
        return [APPLICATION, *self.native_tests]

    @contextlib.contextmanager
    def display(self, count, *, ipc_root):
        self.case.assertEqual((count, ipc_root), (1, stable_service_root(self.tree, "gtk-bus")))
        self.events.append("display-start")
        if self.service_error is not None:
            raise self.service_error
        try:
            yield [{"DISPLAY": ":73", "AOBUS_OWNED_GTK_BUS": "unix:path=/owned/bus"}]
        finally:
            self.events.append("display-stop")

    @contextlib.contextmanager
    def audio(self, directory, environment, *, runtime_root):
        self.case.assertEqual(environment["DISPLAY"], ":73")
        self.case.assertEqual(runtime_root, stable_service_root(self.tree, "pipewire"))
        self.events.append("audio-start")
        try:
            yield environment | {"XDG_RUNTIME_DIR": "/owned/audio", "AOBUS_REQUIRE_PIPEWIRE": "1"}
        finally:
            self.events.append("audio-stop")
            if self.cleanup_error is not None:
                raise self.cleanup_error

    def prepare(self, tree, config, *, prefix, graph_options):
        self.case.assertEqual((tree, config, prefix, graph_options), (self.tree, self.profile, None, ()))
        (self.base / "request.json").write_text(
            json.dumps({"schema": 1, "host": self.platform, "source": str(PROJECT_ROOT), "prefix": prefix}),
            encoding="utf-8",
        )
        return self.base

    def observe_rc(self, prepared):
        content = (prepared / "aobus.bazelrc").read_bytes()
        if self.rc_bytes is None:
            self.rc_bytes = content
            self.prepared = prepared
        self.case.assertEqual(content, self.rc_bytes)
        self.case.assertEqual(prepared, self.prepared)
        lines = content.decode("utf-8").splitlines()
        self.case.assertIn("common --lockfile_mode=error", lines)
        fresh = getattr(self.args, "fresh", False)
        self.case.assertIn("build --nocache_test_results" if fresh else "build --cache_test_results=auto", lines)
        self.case.assertIn("build --test_strategy=standalone", lines)
        self.case.assertIn("build --strategy=TestRunner=local", lines)
        self.case.assertIn(b"AOBUS_FIXTURE_SDK=selected-debug", content)
        return content

    def compile_oracle(self, executable, tree, prepared, config, *, jobs, isolate_rc, compiler_include_roots):
        self.case.assertEqual(
            (executable, tree, config, jobs, isolate_rc), (Path("/fixture/bazel"), self.tree, "debug", 16, True)
        )
        expected_roots = (
            ("/selected/compiler-rt/include",) if self.profile == "tsan" and self.platform == "linux" else ()
        )
        self.case.assertEqual(compiler_include_roots, expected_roots)
        self.observe_rc(prepared)
        self.events.append("compile-oracle")
        return self.oracle_status

    def subprocess(self, argv, *, cwd, env, stdout, stderr, check):
        self.case.assertEqual(cwd, PROJECT_ROOT)
        self.case.assertFalse(check)
        self.case.assertEqual(argv[0], str(Path("/fixture/bazel")))
        for flag in ("--nosystem_rc", "--nohome_rc", "--noworkspace_rc", f"--bazelrc={PROJECT_ROOT / '.bazelrc'}"):
            self.case.assertIn(flag, argv)
        rcs = [Path(value.removeprefix("--bazelrc=")) for value in argv if value.startswith("--bazelrc=")]
        self.case.assertEqual(len(rcs), 2)
        self.observe_rc(rcs[1].parent)
        action_index = next(index for index, value in enumerate(argv) if value in {"query", "cquery", "build", "test"})
        action = argv[action_index]
        arguments = argv[action_index + 1 :]
        self.invocations.append((action, arguments))
        self.events.append(action)
        if action != "query":
            self.case.assertIn("--jobs=16", arguments)
        expression = 'set("//test:application" "//test:backend" "//test:platform")'
        if action == "cquery" and "labels(srcs, //:check_native_tests)" in arguments:
            self.case.assertIn("--output=starlark", arguments)
            stdout.write("".join(f"{label}\n" for label in self.native_tests).encode())
        elif action == "query" and f"tests({self.root})" in arguments:
            self.case.assertIn("--output=label", arguments)
            stdout.write(b"//test:application\n//test:platform\n")
        elif action == "cquery" and expression in arguments and "--output=jsonproto" in arguments:
            labels = self.selected[:-1] if self.omit_configured_label else self.selected
            stdout.write(
                json.dumps(
                    {
                        "results": [
                            {
                                "target": {"type": "RULE", "rule": {"name": label, "ruleClass": "cc_test"}},
                                "configurationId": 1,
                            }
                            for label in labels
                        ],
                        "configurations": [{"id": 1, "checksum": CHECKSUM}],
                    }
                ).encode()
            )
        elif action == "cquery" and expression in arguments and "--output=starlark" in arguments:
            self.case.assertIn(
                '--starlark:expr=str(target.label) + "|" + str("IncompatiblePlatformProvider" in providers(target))',
                arguments,
            )
            stdout.write(
                "".join(
                    f"{label}|{self.all_incompatible or label == PLATFORM_TEST}\n" for label in self.selected
                ).encode()
            )
        elif action in {"build", "test"}:
            self.case.assertIn(self.root, arguments)
            for label in self.native_tests:
                self.case.assertIn(label, arguments)
            self.case.assertNotIn("//:check_products", arguments)
            self.case.assertIn("--remote_cache=grpc://cache", arguments)
            path = Path(
                next(
                    value.removeprefix("--build_event_json_file=")
                    for value in arguments
                    if value.startswith("--build_event_json_file=")
                )
            )
            events = [{"lastMessage": True}]
            if action == "test":
                events = [
                    {
                        "id": {"testSummary": {"label": label, "configuration": {"id": CHECKSUM}}},
                        "testSummary": {"overallStatus": "PASSED", "totalRunCount": 1, "totalNumCached": 0},
                    }
                    for label in self.compatible
                ] + events
                if self.bep_mutation:
                    self.bep_mutation(events)
            path.write_text("".join(json.dumps(event) + "\n" for event in events), encoding="utf-8")
        else:
            self.case.fail(f"unexpected Bazel invocation: {argv}")
        if self.tamper is not None:
            self.tamper(rcs[1], self)
        if self.command_error is not None and action == "test":
            if isinstance(self.command_error, BaseException):
                raise self.command_error
            return subprocess.CompletedProcess(argv, self.command_error)
        return subprocess.CompletedProcess(argv, 0)

    def run(self):
        self.stdout = io.StringIO()
        with (
            mock.patch("ao.core.bazel_native.native_platform", return_value=self.platform),
            mock.patch("ao.core.bazel_native.require_supported_source"),
            mock.patch("ao.core.bazel_workspace.bazel_executable", return_value=Path("/fixture/bazel")),
            mock.patch("ao.core.bazel_workspace.prepare_workspace", side_effect=self.prepare),
            mock.patch("ao.core.bazel_workspace._linux_server_javabase", return_value=Path("/fixture/jdk")),
            mock.patch(
                "ao.core.bazel_sanitizers._symbolizer",
                side_effect=lambda platform, _caller: None if platform == "windows" else self.symbolizer,
            ),
            mock.patch("ao.core.bazel_contract.verify", side_effect=self.compile_oracle),
            mock.patch(
                "ao.core.bazel_sanitizers._selected_tsan_interface_root",
                return_value="/selected/compiler-rt/include",
            ),
            mock.patch("ao.command.test.virtual_gtk_displays", side_effect=self.display),
            mock.patch("ao.core.bazel_test_services.private_pipewire", side_effect=self.audio),
            mock.patch("ao.core.bazel_check.subprocess.run", side_effect=self.subprocess),
            contextlib.redirect_stdout(self.stdout),
            contextlib.redirect_stderr(io.StringIO()),
        ):
            return bazel_sanitizers.run(self.args)

    def receipts(self):
        return sorted(self.tree.glob(f"sanitizers/{self.profile}/run-*/receipt.json"))

    def receipt(self):
        paths = self.receipts()
        self.case.assertEqual(len(paths), 1)
        return json.loads(paths[0].read_text(encoding="utf-8"))


class ManagedSanitizerTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(
            prefix="aobus-managed-sanitizer-test-", dir="/tmp" if sys.platform == "linux" else None
        )
        self.addCleanup(temporary.cleanup)
        self.directory = Path(temporary.name)

    def test_linux_default_uses_graph_owned_scope_and_one_frozen_runtime_for_every_phase(self):
        fixture = _ManagedFixture(self, self.directory)
        with mock.patch.dict(
            os.environ, {"ASAN_OPTIONS": "detect_container_overflow=1", "UBSAN_OPTIONS": "halt_on_error=0"}
        ):
            before = dict(os.environ)
            self.assertEqual(fixture.run(), 0)
            self.assertEqual(dict(os.environ), before)
        receipt = fixture.receipt()
        self.assertEqual((receipt["status"], receipt["exit"]), ("passed", 0))
        self.assertEqual(receipt["roots"], ["//:asan_tests", BACKEND])
        self.assertEqual(
            receipt["selected_tests"],
            [{"label": APPLICATION, "configuration": CHECKSUM}, {"label": BACKEND, "configuration": CHECKSUM}],
        )
        self.assertEqual(receipt["incompatible_tests"], [{"label": PLATFORM_TEST, "configuration": CHECKSUM}])
        self.assertEqual(len(receipt["test_results"]), 2)
        self.assertIn("normal whole completion ownership separately required", receipt["scope"]["claim"])
        self.assertNotIn("ownership", receipt)
        self.assertEqual(
            receipt["runtime"]["environment"]["ASAN_OPTIONS"],
            f'detect_container_overflow=1:external_symbolizer_path="{fixture.symbolizer}":halt_on_error=1:exitcode=1',
        )
        self.assertEqual(
            receipt["runtime"]["environment"]["UBSAN_OPTIONS"],
            f'external_symbolizer_path="{fixture.symbolizer}":halt_on_error=1:print_stacktrace=1:exitcode=1',
        )
        self.assertEqual(receipt["runtime"]["symbolizer"], str(fixture.symbolizer))
        self.assertIn(str(fixture.symbolizer), receipt["runtime"]["source_files"])
        self.assertIn(b"DISPLAY=:73", fixture.rc_bytes)
        self.assertIn(b"AOBUS_REQUIRE_PIPEWIRE=1", fixture.rc_bytes)
        self.assertIn(b"--test_env=UBSAN_OPTIONS=external_symbolizer_path=", fixture.rc_bytes)
        self.assertIn(b"halt_on_error=1:print_stacktrace=1:exitcode=1", fixture.rc_bytes)
        self.assertEqual(fixture.events[:3], ["display-start", "audio-start", "cquery"])
        self.assertLess(fixture.events.index("compile-oracle"), fixture.events.index("build"))
        self.assertEqual(fixture.events[-2:], ["audio-stop", "display-stop"])
        self.assertTrue(all(phase["status"] == "passed" for phase in receipt["phases"]))

    def test_tsan_and_windows_follow_configured_native_selection_without_python_backfill(self):
        for profile, platform in (("tsan", "linux"), ("asan", "windows")):
            with self.subTest(profile=profile, platform=platform):
                directory = self.directory / platform
                directory.mkdir()
                fixture = _ManagedFixture(self, directory, profile=profile, platform=platform)
                self.assertEqual(fixture.run(), 0)
                receipt = fixture.receipt()
                self.assertEqual(receipt["roots"], [f"//:{profile}_tests", *fixture.native_tests])
                self.assertEqual(receipt["compilation_config"], "debug")
                if platform == "windows":
                    self.assertNotIn("display-start", fixture.events)
                    self.assertEqual(
                        sorted(result["label"] for result in receipt["test_results"]), sorted(fixture.compatible)
                    )
                else:
                    self.assertTrue(receipt["runtime"]["suppression_files"])

    def test_malformed_caller_options_leave_terminal_failure_without_configured_work(self):
        for index, value in enumerate(('halt_on_error="unterminated', "verbosity=0,missing-equals")):
            with self.subTest(value=value):
                directory = self.directory / f"bad-options-{index}"
                directory.mkdir()
                fixture = _ManagedFixture(self, directory)
                with mock.patch.dict(os.environ, {"ASAN_OPTIONS": value}):
                    self.assertEqual(fixture.run(), 1)
                self.assertEqual(fixture.receipt()["status"], "error")
                self.assertEqual(fixture.invocations, [])
                self.assertNotIn("compile-oracle", fixture.events)

    def test_cached_results_are_admitted_unless_the_caller_requires_fresh_runs(self):
        for fresh in (False, True):
            with self.subTest(fresh=fresh):
                directory = self.directory / f"fresh-{fresh}"
                directory.mkdir()
                fixture = _ManagedFixture(self, directory)
                fixture.args.fresh = fresh
                fixture.bep_mutation = lambda events: events[0]["testSummary"].update(totalNumCached=1)
                self.assertEqual(fixture.run(), 1 if fresh else 0)
                receipt = fixture.receipt()
                self.assertEqual(receipt["test_cache_policy"], "fresh" if fresh else "auto")
                if fresh:
                    self.assertNotIn("test_results", receipt)
                else:
                    self.assertEqual([result["cached"] for result in receipt["test_results"]].count(1), 1)
                    self.assertIn(", 1 cached runs.", fixture.stdout.getvalue())

    def test_native_failures_and_interruptions_keep_actual_status_and_terminal_receipts(self):
        for name, failure, expected, state in (
            ("exit", 23, 23, "failed"),
            ("interrupt", KeyboardInterrupt(), 130, "interrupted"),
            ("helper-exit", SystemExit(0), 1, "failed"),
        ):
            with self.subTest(name=name):
                directory = self.directory / name
                directory.mkdir()
                fixture = _ManagedFixture(self, directory)
                fixture.command_error = failure
                self.assertEqual(fixture.run(), expected)
                receipt = fixture.receipt()
                self.assertEqual((receipt["status"], receipt["exit"]), (state, expected))
                self.assertNotIn("test_results", receipt)
                self.assertEqual(fixture.events[-2:], ["audio-stop", "display-stop"])
                if isinstance(failure, BaseException):
                    self.assertIn("not established", receipt["writer_state"])

    def test_service_startup_and_cleanup_failures_cannot_leave_a_passed_attempt(self):
        for name in ("startup", "cleanup"):
            with self.subTest(name=name):
                directory = self.directory / name
                directory.mkdir()
                fixture = _ManagedFixture(self, directory)
                if name == "startup":
                    fixture.service_error = SystemExit(7)
                else:
                    fixture.cleanup_error = BazelWorkspaceError("owned audio exit uncertain")
                self.assertEqual(fixture.run(), 7 if name == "startup" else 1)
                self.assertNotEqual(fixture.receipt()["status"], "passed")
                if name == "startup":
                    self.assertEqual(fixture.invocations, [])

    def test_compile_contract_failure_gates_build_and_test_without_weakening_its_request(self):
        fixture = _ManagedFixture(self, self.directory)
        fixture.oracle_status = 19
        self.assertEqual(fixture.run(), 19)
        self.assertEqual(fixture.receipt()["status"], "failed")
        self.assertNotIn("build", fixture.events)
        self.assertNotIn("test", fixture.events)

    def test_empty_or_drifting_configured_selection_fails_before_oracles_and_tests(self):
        for attribute in ("all_incompatible", "omit_configured_label"):
            with self.subTest(attribute=attribute):
                directory = self.directory / attribute
                directory.mkdir()
                fixture = _ManagedFixture(self, directory)
                setattr(fixture, attribute, True)
                self.assertEqual(fixture.run(), 1)
                self.assertNotIn("compile-oracle", fixture.events)
                self.assertNotIn("test", fixture.events)

    def test_second_attempt_does_not_reuse_first_passing_bep_or_receipt(self):
        fixture = _ManagedFixture(self, self.directory)
        self.assertEqual(fixture.run(), 0)
        first = fixture.receipts()[0]
        original = first.read_bytes()
        fixture.rc_bytes = None
        fixture.prepared = None
        fixture.bep_mutation = lambda events: events.pop(0)
        self.assertEqual(fixture.run(), 1)
        self.assertEqual(first.read_bytes(), original)
        self.assertEqual(len(fixture.receipts()), 2)
        statuses = {json.loads(path.read_text(encoding="utf-8"))["status"] for path in fixture.receipts()}
        self.assertEqual(statuses, {"passed", "error"})


if __name__ == "__main__":
    unittest.main()
