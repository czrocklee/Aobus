"""Behavior contracts for build-system-independent completion guardrails."""

import json
import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path, PureWindowsPath
from unittest.mock import patch

from ao.core import guardrails
from ao.core.paths import PROJECT_ROOT


class ForbiddenScanTest(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix="aobus shared guards ")
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)

    def put(self, relative, content):
        path = self.root / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(content, encoding="utf-8")
        return path

    def test_raw_scan_sees_comments_and_broad_suffixes_but_explicit_files_have_no_suffix_filter(self):
        self.put("Boundary.mm", "// forbidden\n")
        self.put("Fragment.def", "forbidden\n")
        ignored = self.put("Ignored.txt", "forbidden\n")
        result = guardrails.scan_forbidden([self.root], forbidden="forbidden")
        self.assertEqual({Path(f.path).name for f in result.errors}, {"Boundary.mm", "Fragment.def"})
        self.assertFalse(guardrails.scan_forbidden([ignored], forbidden="forbidden").ok)

    def test_inventory_preserves_native_cmake_glob_case_rules(self):
        self.put("Upper.CPP", "forbidden")
        for platform, expected in (("linux", []), ("win32", ["Upper.CPP"]), ("darwin", ["Upper.CPP"])):
            with self.subTest(platform=platform), patch.object(guardrails.sys, "platform", platform):
                self.assertEqual([path.name for path in guardrails.source_files(self.root)], expected)

    def test_write_transaction_owner_policy_handles_drive_and_share_roots(self):
        examples = (
            (
                "Y:/",
                "Y:/lib/library/WriteTransactionAccess.h",
                "Y:/test/fixture/lib/library/WriteTransactionAccess.h",
            ),
            (
                "//server/share/",
                "//server/share/lib/library/WriteTransaction.cpp",
                "//server/share/test/fixture/lib/library/WriteTransaction.cpp",
            ),
            (
                "Y:/checkout [guards]+",
                "Y:/checkout [guards]+/lib/library/detail/WriteTransactionAccess.h",
                "Y:/checkout [guards]+/test/fixture/lib/library/detail/WriteTransactionAccess.h",
            ),
        )
        for root, owner, fixture in examples:
            with self.subTest(root=root):
                # This policy boundary needs path spelling, not a Windows mount.
                audit = next(
                    audit
                    for audit in guardrails._boundary_audits(PureWindowsPath(root))
                    if audit.name == "ao_library_logical_writer_boundary_check"
                )
                rule = next(rule for rule in audit.rules if rule.name == "write_transaction_access")
                self.assertRegex(owner, rule.exclude)
                self.assertNotRegex(fixture, rule.exclude)
                self.assertNotRegex("Z:/lib/library/WriteTransactionAccess.h", rule.exclude)

    def test_allowed_removal_leaves_later_forbidden_text_visible(self):
        path = self.put("Boundary.cpp", "class Access;\nusing Alias = Access;\n")
        result = guardrails.scan_forbidden([path], forbidden="Access", allowed=r"class Access;")
        self.assertEqual(len(result.errors), 1)
        path.write_text("class Access;\n", encoding="utf-8")
        self.assertTrue(guardrails.scan_forbidden([path], forbidden="Access", allowed=r"class Access;").ok)

    def test_root_overlap_is_not_deduplicated_and_exclusions_match_full_paths(self):
        path = self.put("owner/Boundary.cpp", "forbidden\n")
        result = guardrails.scan_forbidden([self.root, path], forbidden="forbidden")
        self.assertEqual(len(result.errors), 2)
        self.assertTrue(
            guardrails.scan_forbidden([self.root], forbidden="forbidden", exclude=r"/owner/Boundary\.cpp$").ok
        )

    def test_report_mode_warns_without_failing_but_missing_roots_still_fail(self):
        self.put("Boundary.cpp", "forbidden\n")
        result = guardrails.scan_forbidden([self.root], forbidden="forbidden", mode="report")
        self.assertTrue(result.ok)
        self.assertEqual(len(result.warnings), 1)
        with self.assertRaisesRegex(guardrails.AuditInputError, "root does not exist"):
            guardrails.scan_forbidden([self.root / "missing"], mode="report")
        with self.assertRaisesRegex(guardrails.AuditInputError, "MODE must be"):
            guardrails.scan_forbidden([self.root], mode="ignore")

    def test_code_view_carries_comments_without_joining_tokens_or_hiding_terminator_code(self):
        path = self.put("Leaf.h", "/*\nAppRuntime\n*/ Runtime value;\nApp/* gap */Runtime split;\n")
        lines = guardrails.code_lines(path)
        self.assertEqual([code.strip() for _, code in lines], ["", "", "Runtime value;", "App Runtime split;"])
        path.write_text("/*\ncomment\n*/ AppRuntime& runtime;\n", encoding="utf-8")
        self.assertFalse(guardrails.audit_frontend("gtk-leaf", self.root).ok)


class CompletionAuditTest(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix="aobus completion [guards] ")
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name) / "source"
        self.output = Path(temporary.name) / "reports"
        # Empty governed domains are valid, but every required domain must exist.
        for relative in (
            "app/include/ao/desktop",
            "app/include/ao/i18n",
            "app/include/ao/rt",
            "app/include/ao/systemmedia",
            "app/include/ao/uimodel",
            "app/uimodel",
            "app/desktop",
            "app/i18n",
            "app/runtime",
            "app/linux-gtk",
            "app/systemmedia",
            "app/macos-appkit",
            "app/windows-winui",
            "app/tui",
            "app/cli",
            "include/ao/yaml",
            "include/ao/library",
            "include/ao/media",
            "lib/media",
            "lib/library",
            "test/uimodel",
            "tool",
        ):
            (self.root / relative).mkdir(parents=True, exist_ok=True)
        for relative in (
            "app/include/ao/rt/TrackField.h",
            "app/include/ao/rt/TrackPresentation.h",
            "app/include/ao/rt/completion/CompletionItem.h",
            "include/ao/audio/BackendProvider.h",
        ):
            self.put(relative, "")

    def put(self, relative, content):
        path = self.root / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(content, encoding="utf-8")
        return path

    def assert_audit(self, audit, path, rejected, allowed):
        source = self.put(path, rejected)
        result = guardrails.run_audits(self.root, [audit])
        self.assertEqual({finding.path for finding in result.errors}, {path}, result)
        source.write_text(allowed, encoding="utf-8")
        self.assertTrue(guardrails.run_audits(self.root, [audit]).ok)
        source.unlink()

    def test_contract_and_termination_spelling_boundaries(self):
        examples = (
            (
                "ao_no_c_assert_check",
                ("assert(value)", " assert (value)", "assert\n(value)"),
                "static_assert(value); AO_INVARIANT(value); myassert(value);",
            ),
            (
                "ao_no_raw_gsl_contract_check",
                ("gsl_Expects(value)", " gsl_Ensures (value)", "gsl_Assert\n(value)"),
                "AO_EXPECTS(value); AO_ENSURES(value); AO_INVARIANT(value); mygsl_Assert(value);",
            ),
            (
                "ao_no_unconditional_terminal_check",
                (
                    "AO_EXPECTS(false)",
                    'AO_ENSURES ( false, "message")',
                    "AO_INVARIANT\n(\nfalse)",
                    "std::unreachable()",
                ),
                'AO_EXPECTS(falseValue); AO_ENSURES(value); AO_INVARIANT(condition); AO_FATAL("message");',
            ),
            (
                "ao_no_removed_exception_surface_check",
                ("#include <ao/Exception.h>", "ExceptionFormat<int> error", "throwException(value)"),
                "std::exception; std::current_exception(); throwOperationCancelled(); throwExceptionResult;",
            ),
            (
                "ao_no_raw_fatal_primitive_check",
                ("std::terminate()", "std::abort ()", "std::quick_exit(1)", "std::_Exit(1)", "::abort()", "_Exit(1)"),
                'AO_FATAL("context"); std::exit(0); worker.abort(); worker->terminate(); application::abort();',
            ),
        )
        # Separate files ensure each forbidden spelling must independently fail;
        # one early match cannot conceal a broken alternative later in the file.
        for audit, rejected, allowed in examples:
            for index, spelling in enumerate(rejected):
                with self.subTest(audit=audit, spelling=spelling):
                    self.assert_audit(audit, f"app/Violation{index}.cpp", spelling, allowed)
        self.put("lib/utility/Fatal.cpp", "std::abort();")
        self.assertTrue(guardrails.run_audits(self.root, ["ao_no_raw_fatal_primitive_check"]).ok)
        self.assert_audit("ao_no_raw_fatal_primitive_check", "app/Fatal.cpp", "std::abort();", "std::exit(0);")

    def test_library_logical_ports_allow_only_declarations_and_implementation_owners(self):
        audit = "ao_library_logical_writer_boundary_check"
        for rejected, allowed in (
            (
                '#include "lib/library/PhysicalStoreAccess.h"',
                "class PhysicalStoreAccess; friend class detail::PhysicalStoreAccess;",
            ),
            ("struct ao::library::detail::PhysicalStoreAccess\n{", "PhysicalStoreAccessibility value;"),
            ("detail::PhysicalStoreAccess::writer(store, transaction)", "class PhysicalStoreAccess;"),
            ('#include "TrackWrite.h"', "TrackWriteResult value;"),
            ("createPreparedTrackRecord(writer, hot, cold)", "xcreatePreparedTrackRecord(writer, hot, cold)"),
            ("updatePreparedHotTrackRecord (writer, id, hot)", "createPreparedTrackRecordResult value;"),
            ("options.optInjectedCommitFailure", "xoptInjectedCommitFailure"),
            ("recordOpenValidationTrackRow()", "OpenValidationMetricsPolicy policy;"),
            (
                "using Access = ao::library::detail::WriteTransactionAccess;",
                "friend class ::ao::library::detail::WriteTransactionAccess; class WriteTransactionAccess;",
            ),
        ):
            with self.subTest(rejected=rejected):
                self.assert_audit(audit, "app/Violation.cpp", rejected, allowed)
        for path, text in (
            ("lib/library/PhysicalStoreAccess.h", "struct PhysicalStoreAccess {"),
            ("lib/library/TrackWriter.cpp", "createPreparedTrackRecord(writer, hot, cold);"),
            ("include/ao/library/WriteTransaction.h", "optInjectedCommitFailure"),
            ("lib/library/MusicLibrary.cpp", "OpenValidationMetrics metrics;"),
            ("lib/library/detail/LibraryError.cpp", "using Access = WriteTransactionAccess;"),
        ):
            self.put(path, text)
        self.assertTrue(guardrails.run_audits(self.root, [audit]).ok)
        for path in (
            "test/fixture/lib/library/Escape.cpp",
            "lib/library/nested/Deep.cpp",
            "app/runtime/library/Escape.cpp",
        ):
            with self.subTest(path=path):
                self.assert_audit(
                    audit,
                    path,
                    "using Access = WriteTransactionAccess;",
                    "friend class detail::WriteTransactionAccess;",
                )

    def test_test_seams_stay_out_of_production_but_are_available_to_tests(self):
        for audit, identifier, owner in (
            ("ao_lmdb_read_fault_injection_boundary_check", "ReadFaultInjection", "lib/lmdb/detail/ThrowError.cpp"),
            (
                "ao_lmdb_database_open_admission_probe_boundary_check",
                "DatabaseOpenAdmissionProbe",
                "lib/lmdb/Transaction.cpp",
            ),
            ("ao_lmdb_unvalidated_database_boundary_check", "UnvalidatedDatabase", "lib/lmdb/Database.cpp"),
            (
                "ao_gtk_field_grid_operation_probe_boundary_check",
                "TrackFieldGridOperationProbe",
                "app/linux-gtk/layout/component/track/TrackFieldGridComponent.cpp",
            ),
        ):
            with self.subTest(audit=audit):
                for spelling in (f'#include "private/{identifier}.h"', f"auto seam = {identifier}{{}};"):
                    self.assert_audit(audit, "app/Violation.cpp", spelling, f"{identifier}Policy policy;")
                self.put(owner, f"{identifier} seam;")
                self.put("test/SeamTest.cpp", f"{identifier} seam;")
                self.assertTrue(guardrails.run_audits(self.root, [audit]).ok)
                (self.root / owner).unlink()

    def test_dependency_direction_covers_public_and_private_media_sources(self):
        for audit, paths, rejected, allowed in (
            (
                "ao_media_boundary_check",
                ("include/ao/media/Boundary.h", "lib/media/Boundary.cpp"),
                "#include <ao/library/MusicLibrary.h>",
                "#include <ao/utility/Path.h>",
            ),
            (
                "ao_library_media_file_boundary_check",
                ("include/ao/library/Boundary.h", "lib/library/Boundary.cpp"),
                '# include "ao/media/file/File.h"',
                "#include <ao/media/opus/Header.h>",
            ),
        ):
            for path in paths:
                with self.subTest(audit=audit, path=path):
                    self.assert_audit(audit, path, rejected, allowed)

    def test_application_policy_retains_signal_names_and_playback_only_command_access(self):
        audit = "ao_application_architecture_audit"
        for spelling in ("rt::Signal", "ao::rt::Subscription", "::ao::rt::Signal"):
            self.assert_audit(
                audit,
                "app/runtime/Signal.cpp",
                spelling,
                "rt::SignalRouter; rt::SubscriptionScope; Chart::Signal; other::rt::Signal;",
            )
        for spelling in (
            "runtime.library().commands()",
            "_library.commands()",
            "libraryPtr->commands()",
            "lib.commands()",
            "playbackLibrary.commands()",
        ):
            self.assert_audit(
                audit,
                "app/tui/Commands.cpp",
                spelling,
                "runtime.playback().commands(); _playback.commands(); playbackPtr->commands();",
            )
        self.put("app/runtime/library/LibraryWriteLane.cpp", "library.writeTransaction();")
        self.assertTrue(guardrails.run_audits(self.root, [audit]).ok)
        self.assert_audit(
            audit, "app/runtime/library/Lookalike.cpp", "library.writeTransaction();", "library.readTransaction();"
        )

    def test_disabled_gtk_probe_does_not_disable_cross_frontend_architecture_policy(self):
        path = self.put("app/linux-gtk/Probe.cpp", "TrackFieldGridOperationProbe probe;")
        enabled = guardrails.run(self.root, self.output, "linux")
        self.assertEqual(enabled.exit_code, 1)
        report = json.loads(enabled.report_path.read_text())
        self.assertEqual(report["failed"], ["ao_gtk_field_grid_operation_probe_boundary_check"])
        disabled = guardrails.run(self.root, self.output, "linux", gtk=False)
        self.assertEqual(disabled.exit_code, 0)
        report = json.loads(disabled.report_path.read_text())
        self.assertEqual(report["failed"], [])  # No stale failure receipt.
        self.assertFalse(next(a for a in report["inventory"] if a["condition"] == "gtk")["enabled"])
        path.write_text("AppRuntime& runtime;\n", encoding="utf-8")
        result = guardrails.run(self.root, self.output, "windows")
        self.assertEqual(result.exit_code, 1)
        self.assertEqual({f.audit for f in result.errors}, {"ao_application_architecture_audit"})
        self.assertIn("gtk-leaf", {f.rule for f in result.errors})

    def test_receipt_records_all_blockers_and_missing_inputs_fail_closed(self):
        self.put("app/Assert.cpp", "assert(value);")
        self.put("lib/media/Dependency.cpp", "#include <ao/library/MusicLibrary.h>")
        self.put("app/include/ao/rt/Signal.h", "")
        result = guardrails.run(self.root, self.output, "macos")
        report = json.loads(result.report_path.read_text())
        self.assertEqual(
            set(report["failed"]),
            {"ao_no_c_assert_check", "ao_media_boundary_check", "ao_application_architecture_audit"},
        )
        self.assertEqual(report["advisory"], [])
        self.assertEqual(report["warnings"], [])
        self.assertIn("removed_surface", {finding["rule"] for finding in report["errors"]})
        (self.root / "tool").rmdir()
        result = guardrails.run(self.root, self.output, "linux")
        self.assertNotEqual(result.exit_code, 0)
        self.assertIn("audit_input", {finding.rule for finding in result.errors})
        with self.assertRaisesRegex(guardrails.AuditInputError, "unknown or disabled"):
            guardrails.run_audits(self.root, ["ao_not_a_real_check"])

    def test_cli_runs_without_cmake_and_replaces_its_machine_readable_receipt(self):
        result = subprocess.run(
            [
                sys.executable,
                str(PROJECT_ROOT / "script/ao/guardrails.py"),
                "--source-root",
                str(self.root),
                "--output-dir",
                str(self.output),
                "--platform",
                "linux",
            ],
            env={**os.environ, "PATH": ""},
            capture_output=True,
            text=True,
            check=False,
        )
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(json.loads((self.output / "guardrails.json").read_text())["errors"], [])


if __name__ == "__main__":
    unittest.main()
