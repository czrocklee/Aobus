"""Tiny Bazel 9.2 JSON-BEP fixtures exercise current-output, fail-closed gating."""

import contextlib
import copy
import dataclasses
import io
import json
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

from ao.core import bazel_lint_reports
from ao.core.bazel_lint_reports import (
    Uncovered,
    gate_analyzer_reports,
    gate_reports,
    lint_aspect_id,
    read_reports,
)
from ao.core.paths import PROJECT_ROOT, absolute_path


class ReportsTestCase(unittest.TestCase):
    """Shared reduced-BEP fixture for the lint and analyzer report consumers."""

    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="ao-lint-bep-")
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.bep = self.root / "current.bep.json"
        self.owner = "@@//lib/sample:sample"
        self.configuration = "bazel-out/k8-dbg/bin"
        # Real full configuration IDs are opaque; no short-id derivation is valid.
        self.config_id = "8a60e16eaec5ea3e4ae9b2bce3f936768e73b9126ea3c0d4d3528b322fcf287c"
        self.index_name = self.configuration + "/lib/sample/sample.aobus-lint.index.json"
        self.identity = {
            "owner": self.owner,
            "configuration": self.configuration,
            "source": "lib/sample/Sample.cpp",
            "main": "lib/sample/Sample.cpp",
            "outputs": [self.configuration + "/lib/sample/_objs/sample/Sample.o"],
            "mode": "STRICT",
            "platform": "linux",
        }
        self.index = {
            "schema": 1,
            "owner": self.owner,
            "configuration": self.configuration,
            "reports": [],
            "sources": ["lib/sample/Sample.cpp"],
            "uncovered": [],
        }
        self.files = []
        self.add_report(self.identity)
        self.events = self.fixture()

    def materialize(self, name, text):
        path = self.root / "execroot/_main" / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text, encoding="utf-8")
        return path

    def file_entry(self, name):
        path = self.root / "execroot/_main" / name
        return {
            "name": name.removeprefix(self.configuration + "/"),
            "pathPrefix": self.configuration.split("/"),
            "uri": path.as_uri(),
        }

    def report_json(self, identity, outcome="clean", status=0, includes=()):
        return json.dumps(
            {"schema": 1, "identity": identity, "outcome": outcome, "tool_exit": status, "includes": list(includes)}
        )

    def add_report(self, identity, outcome="clean", log="", status=0, includes=()):
        index = len(self.index["reports"])
        prefix = self.configuration + f"/lib/sample/sample.aobus-lint/{index}"
        report, log_name = prefix + ".report.json", prefix + ".log"
        self.materialize(report, self.report_json(identity, outcome, status, includes))
        self.materialize(log_name, log)
        self.index["reports"].append({"identity": copy.deepcopy(identity), "report": report, "log": log_name})
        self.files.extend([self.file_entry(report), self.file_entry(log_name)])
        if identity["main"] not in self.index["sources"]:
            self.index["sources"].append(identity["main"])

    def fixture(self):
        # Reduced from 9.2.0 experiment warm BEP. These are native proto JSON
        # spellings (including omitted successful exit code), not an action inventory.
        # The aspect identity is the parameterized spelling Bazel 9.2 writes for the
        # default empty consumer mapping; it is an independent literal, not derived.
        target_id = {
            "targetCompleted": {
                "label": "//lib/sample:sample",
                "configuration": {"id": self.config_id},
                "aspect": '//bazel/lint:lint.bzl%lint_aspect[aobus_lint_consumers="{}"]',
            }
        }
        config_id = {"configuration": {"id": self.config_id}}
        parent_set, leaf_set = {"namedSet": {"id": "0"}}, {"namedSet": {"id": "1"}}
        return [
            {
                "id": {"started": {}},
                "started": {"buildToolVersion": "9.2.0"},
                "children": [config_id, parent_set, leaf_set, target_id, {"buildFinished": {}}],
            },
            {"id": config_id, "configuration": {"mnemonic": "k8-dbg", "makeVariable": {"BINDIR": self.configuration}}},
            {"id": leaf_set, "namedSetOfFiles": {"files": self.files}},
            {
                "id": parent_set,
                "namedSetOfFiles": {"files": [self.file_entry(self.index_name)], "fileSets": [{"id": "1"}]},
            },
            {
                "id": target_id,
                "completed": {"success": True, "outputGroup": [{"name": "aobus_lint", "fileSets": [{"id": "0"}]}]},
            },
            {
                "id": {"buildFinished": {}},
                "finished": {"overallSuccess": True, "exitCode": {"name": "SUCCESS"}},
                "children": [{"buildToolLogs": {}}],
            },
            {"id": {"buildToolLogs": {}}, "lastMessage": True, "buildToolLogs": {}},
        ]

    def save(self):
        self.materialize(self.index_name, json.dumps(self.index))
        self.bep.write_text("".join(json.dumps(e) + "\n" for e in self.events), encoding="utf-8")

    def read(self):
        self.save()
        return read_reports(self.bep, [self.owner])

    def quiet_gate(self, inventories, selected):
        with contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()):
            return gate_reports(inventories, selected)


class CurrentReportsTest(ReportsTestCase):
    def test_cached_equivalent_bep_without_execution_events_reads_and_gates_clean(self):
        self.save()
        with patch("subprocess.run", side_effect=AssertionError("reader must never launch tools")):
            first = read_reports(self.bep, [self.owner])
            second = read_reports(self.bep, ["//lib/sample:sample"])
            self.assertEqual(first, second)
            self.assertEqual(first[0].bep_configuration, self.config_id)
            self.assertEqual(first[0].configuration, self.configuration)
            self.assertEqual(self.quiet_gate(first, {self.identity["main"]}), 0)
        self.assertFalse(any("action" in event for event in self.events))

    def test_supported_inline_local_files_do_not_require_named_sets(self):
        self.events[0]["children"] = [child for child in self.events[0]["children"] if "namedSet" not in child]
        self.events = [event for event in self.events if "namedSetOfFiles" not in event]
        complete = next(event["completed"] for event in self.events if "completed" in event)
        complete["outputGroup"] = [
            {"name": "aobus_lint", "inlineFiles": [self.file_entry(self.index_name), *self.files]}
        ]
        self.assertEqual(self.quiet_gate(self.read(), {self.identity["main"]}), 0)

    def test_findings_are_gated_on_each_read_even_without_analyzer_execution(self):
        for outcome, status, log in (
            ("warning", 0, "lib/sample/Sample.cpp:3:4: warning: prefix [aobus-example]\n"),
            ("compiler_error", 1, "lib/sample/Sample.cpp:3:4: error: unknown type [clang-diagnostic-error]\n"),
        ):
            with self.subTest(outcome=outcome):
                report = self.root / "execroot/_main" / self.index["reports"][0]["report"]
                report.write_text(
                    self.report_json(self.identity, outcome, status),
                    encoding="utf-8",
                )
                (self.root / "execroot/_main" / self.index["reports"][0]["log"]).write_text(log, encoding="utf-8")
                inventories = self.read()
                self.assertEqual(self.quiet_gate(inventories, {self.identity["main"]}), 1)
                self.assertEqual(self.quiet_gate(self.read(), {self.identity["main"]}), 1)

    def test_selection_gates_complete_context_logs_including_header_notes_and_dedup(self):
        log = (
            "include/ao/sample/Sample.h:2:3: warning: included diagnostic [aobus-example]\n"
            "include/ao/sample/Sample.h:1:1: note: declaration\n"
        )
        for entry in self.index["reports"]:
            self.materialize(entry["log"], log)
            self.materialize(
                entry["report"],
                self.report_json(entry["identity"], "warning", 0),
            )
        variant = {**self.identity, "outputs": [self.configuration + "/lib/sample/_objs/sample/Sample.pic.o"]}
        self.add_report(variant, "warning", log)
        other = {
            **self.identity,
            "source": "lib/sample/Other.cpp",
            "main": "lib/sample/Other.cpp",
            "outputs": [self.configuration + "/lib/sample/_objs/sample/Other.o"],
        }
        self.add_report(other, "warning", "lib/sample/Other.cpp:1:1: warning: unselected [aobus-example]\n")
        output = self.root / "diagnostics.txt"
        inventories = self.read()
        self.assertEqual(gate_reports(inventories, {self.identity["main"]}, output), 1)
        text = output.read_text(encoding="utf-8")
        self.assertEqual(text.count("included diagnostic"), 1)
        self.assertIn("note: declaration", text)
        self.assertNotIn("unselected", text)
        self.assertEqual(self.quiet_gate(inventories, {"include/ao/sample/Sample.h"}), 2)

    def test_compatible_unpaired_header_is_uncovered_not_a_green_partial_pass(self):
        header = "include/ao/sample/Unpaired.h"
        self.index["sources"].append(header)
        self.index["uncovered"].append({"path": header, "reason": "no unambiguous same-owner source"})
        inventories = self.read()
        self.assertEqual(self.quiet_gate(inventories, {header, self.identity["main"]}), 2)
        self.assertEqual(self.quiet_gate(inventories, {self.identity["main"]}), 0)
        self.assertEqual(inventories[0].uncovered[0].reason, "no unambiguous same-owner source")

    def test_another_owners_report_covers_a_header_this_owner_cannot_pair(self):
        header = "include/ao/sample/Sample.h"
        self.add_report(
            {**self.identity, "main": header}, "warning", f"{header}:1:1: warning: paired [aobus-example]\n"
        )
        paired = self.read()[0]
        reason = "no unambiguous same-component native header companion"
        # A header-only dependency lists the same interface in a consumer owner.
        consumer = dataclasses.replace(
            paired, owner="@@//lib/sample:consumer", reports=(), uncovered=(Uncovered(header, reason),)
        )
        output = self.root / "diagnostics.txt"
        self.assertEqual(gate_reports((paired, consumer), {header}, output), 1)
        text = output.read_text(encoding="utf-8")
        self.assertIn("paired", text)
        self.assertNotIn(reason, text)
        unpaired = dataclasses.replace(paired, reports=(), uncovered=(Uncovered(header, reason),))
        self.assertEqual(gate_reports((unpaired, consumer), {header}, output), 2)
        self.assertEqual(output.read_text(encoding="utf-8").count(reason), 1)

    def test_reports_carry_the_checkout_includes_their_analysis_recorded(self):
        other = {
            **self.identity,
            "source": "lib/sample/Other.cpp",
            "main": "lib/sample/Other.cpp",
            "outputs": [self.configuration + "/lib/sample/_objs/sample/Other.o"],
        }
        self.add_report(other, includes=["app/include/ao/i18n/MessageInventory.def", "lib/sample/Other.h"])
        reports = {report.identity.main: report for report in self.read()[0].reports}
        self.assertEqual(reports["lib/sample/Sample.cpp"].includes, ())
        self.assertEqual(
            reports["lib/sample/Other.cpp"].includes,
            ("app/include/ao/i18n/MessageInventory.def", "lib/sample/Other.h"),
        )

    def test_caller_reason_replaces_index_reasons_for_a_path_no_report_covers(self):
        header = "include/ao/sample/Unpaired.h"
        fragment = "include/ao/sample/Table.def"
        self.index["sources"].append(header)
        self.index["uncovered"].append({"path": header, "reason": "no companion"})
        output = self.root / "diagnostics.txt"
        reasons = {
            header: "no analyzed translation unit includes it",
            fragment: "no analyzed translation unit includes it",
        }
        self.assertEqual(gate_reports(self.read(), {header, fragment}, output, reasons=reasons), 2)
        self.assertEqual(
            output.read_text(encoding="utf-8").splitlines(),
            [f"{fragment}: {reasons[fragment]}", f"{header}: {reasons[header]}"],
        )
        self.assertEqual(gate_reports(self.read(), {self.identity["main"]}, output, reasons=reasons), 0)

    def test_report_paths_display_through_the_checkout_or_their_execution_root(self):
        log = (
            "lib/sample/Sample.cpp:1:2: warning: source [aobus-example]\n"
            "bazel-out/k8-dbg/bin/lib/sample/Generated.h:3:4: warning: generated [aobus-example]\n"
            "external/dependency/Header.h:5:6: warning: external [aobus-example]\n"
        )
        entry = self.index["reports"][0]
        self.materialize(entry["log"], log)
        self.materialize(entry["report"], self.report_json(self.identity, "warning", 0))
        output = self.root / "diagnostics.txt"
        self.assertEqual(gate_reports(self.read(), {self.identity["main"]}, output), 1)
        execroot = self.root / "execroot/_main"
        self.assertEqual(
            [line.partition(": warning:")[0] for line in output.read_text(encoding="utf-8").splitlines()],
            [
                f"{absolute_path(PROJECT_ROOT / 'lib/sample/Sample.cpp')}:1:2",
                f"{absolute_path(execroot / 'bazel-out/k8-dbg/bin/lib/sample/Generated.h')}:3:4",
                f"{absolute_path(execroot / 'external/dependency/Header.h')}:5:6",
            ],
        )

    def test_reports_from_different_execution_roots_cannot_be_gated_together(self):
        inventory = self.read()[0]
        elsewhere = dataclasses.replace(inventory, owner="@@//lib/sample:other", execroot=self.root / "other")
        self.assertEqual(self.quiet_gate((inventory, elsewhere), {self.identity["main"]}), 2)

    def test_header_as_main_is_a_distinct_report_not_its_companion_translation_unit(self):
        header = "include/ao/sample/Sample.h"
        self.add_report(
            {**self.identity, "main": header}, "warning", f"{header}:1:1: warning: header main [aobus-example]\n"
        )
        inventories = self.read()
        self.assertEqual(self.quiet_gate(inventories, {self.identity["main"]}), 0)
        self.assertEqual(self.quiet_gate(inventories, {header}), 1)

    def test_lint_aspect_id_spells_the_fixed_base_and_literal_consumer_payload(self):
        self.assertEqual(lint_aspect_id(), '//bazel/lint:lint.bzl%lint_aspect[aobus_lint_consumers="{}"]')
        consumers = '{"@@//lib/sample:sample":[["lib/sample/Sample.cpp","include/ao/sample/Sample.h"]]}'
        # The escaped mapping spelling matches the native 9.2 parameterized-aspect
        # BEP identity, including the backslash-escaped quotes inside the value.
        self.assertEqual(
            lint_aspect_id(consumers),
            '//bazel/lint:lint.bzl%lint_aspect[aobus_lint_consumers="{'
            '\\"@@//lib/sample:sample\\":[[\\"lib/sample/Sample.cpp\\",\\"include/ao/sample/Sample.h\\"]]}"]',
        )
        # The payload keeps its actual spelling; it is not re-encoded or validated.
        self.assertEqual(
            lint_aspect_id('{"a": 1}'),
            '//bazel/lint:lint.bzl%lint_aspect[aobus_lint_consumers="{\\"a\\": 1}"]',
        )
        for bad in (None, 7, ["{}"]):
            with self.subTest(consumers=bad), self.assertRaises(ValueError):
                lint_aspect_id(bad)

    def test_default_and_explicit_empty_consumers_bind_the_same_aspect_completions(self):
        self.assertEqual(lint_aspect_id(), lint_aspect_id("{}"))
        self.save()
        self.assertEqual(read_reports(self.bep, [self.owner]), read_reports(self.bep, [self.owner], consumers="{}"))

    def test_explicit_consumer_mapping_binds_only_its_exact_aspect_identity(self):
        consumers = '{"@@//lib/sample:sample":[["lib/sample/Sample.cpp","include/ao/sample/Sample.h"]]}'
        self.events[4]["id"]["targetCompleted"]["aspect"] = (
            '//bazel/lint:lint.bzl%lint_aspect[aobus_lint_consumers="{'
            '\\"@@//lib/sample:sample\\":[[\\"lib/sample/Sample.cpp\\",\\"include/ao/sample/Sample.h\\"]]}"]'
        )
        self.save()
        self.assertEqual(read_reports(self.bep, [self.owner], consumers=consumers)[0].owner, self.owner)
        # The default empty parameterization cannot stand in for the mapping.
        with self.assertRaises(ValueError):
            read_reports(self.bep, [self.owner])
        # The mapping parameterization cannot stand in for the default BEP either.
        self.events[4]["id"]["targetCompleted"]["aspect"] = (
            '//bazel/lint:lint.bzl%lint_aspect[aobus_lint_consumers="{}"]'
        )
        self.save()
        with self.assertRaises(ValueError):
            read_reports(self.bep, [self.owner], consumers=consumers)

    def test_foreign_aspect_completions_for_other_owners_do_not_disturb_binding(self):
        self.events.insert(
            5,
            {
                "id": {
                    "targetCompleted": {
                        "label": "//lib/sample:foreign",
                        "configuration": {"id": "another-full-configuration-id"},
                        "aspect": '//bazel/lint:lint.bzl%origin_aspect[aobus_lint_consumers="{}"]',
                    }
                },
                "completed": {"success": True},
            },
        )
        inventories = self.read()
        self.assertEqual(len(inventories), 1)
        self.assertEqual(inventories[0].owner, self.owner)

    def test_materialization_and_exec_path_identity_are_required_not_suffix_glob_matches(self):
        original = copy.deepcopy(self.events)
        for fault in ("remote", "missing-file", "wrong-local-prefix", "traversal", "wrong-exec-name"):
            with self.subTest(fault=fault):
                self.events = copy.deepcopy(original)
                output = self.events[2]["namedSetOfFiles"]["files"][0]
                if fault == "remote":
                    output["uri"] = "bytestream://cache/instance/blob"
                elif fault == "missing-file":
                    output["uri"] = (self.root / "missing" / self.index["reports"][0]["report"]).as_uri()
                elif fault == "wrong-local-prefix":
                    path = self.materialize("bazel-out/other/bin/lib/sample/sample.aobus-lint/0.report.json", "{}")
                    output["uri"] = path.as_uri()
                elif fault == "traversal":
                    output["name"] = "../" + output["name"]
                else:
                    output["name"] = "other/0.report.json"
                with self.assertRaises((ValueError, OSError)):
                    self.read()

    def test_report_inventory_requires_coverage_and_owner_scoped_paths(self):
        original = copy.deepcopy(self.index)
        for fault in ("unaccounted", "foreign-report", "foreign-log"):
            with self.subTest(fault=fault):
                self.index = copy.deepcopy(original)
                if fault == "unaccounted":
                    self.index["sources"].append("include/ao/sample/Unpaired.h")
                else:
                    key = fault.removeprefix("foreign-")
                    self.index["reports"][0][key] = self.configuration + "/lib/other/stale." + key
                with self.assertRaises(ValueError):
                    self.read()

    def test_infrastructure_log_or_missing_log_cannot_be_gated_clean(self):
        entry = self.index["reports"][0]
        for log in (
            "Skipping lib/sample/Sample.cpp.\n",
            "Could not auto-detect compilation database\n",
            "Stack dump:\n",
            "Error: no checks enabled.\n",
        ):
            with self.subTest(log=log):
                self.materialize(entry["log"], log)
                with self.assertRaises(ValueError):
                    self.read()
        self.materialize(entry["log"], "")
        inventories = self.read()
        (self.root / "execroot/_main" / entry["log"]).unlink()
        self.assertEqual(self.quiet_gate(inventories, {self.identity["main"]}), 2)


class AnalyzerProfileTest(ReportsTestCase):
    """The explicit analyzer profile binds only its own aspect and mode set."""

    ANALYZER_ASPECT = '//bazel/lint:lint.bzl%analyzer_aspect[aobus_lint_consumers="{}"]'
    LINT_ASPECT = '//bazel/lint:lint.bzl%lint_aspect[aobus_lint_consumers="{}"]'

    def test_analyzer_profile_spells_its_own_fixed_aspect_identity(self):
        self.assertEqual(lint_aspect_id(profile="analyzer"), self.ANALYZER_ASPECT)
        self.assertEqual(lint_aspect_id(), self.LINT_ASPECT)
        for bad in ("generic", "", "LINT"):
            with self.subTest(profile=bad), self.assertRaises(ValueError):
                lint_aspect_id(profile=bad)

    def test_analyzer_profile_binds_only_its_exact_aspect_and_mode_set(self):
        for report in self.index["reports"]:
            report["identity"]["mode"] = "ANALYZER"
            self.materialize(report["report"], self.report_json(report["identity"], "clean"))
        self.materialize(self.index_name, json.dumps(self.index))
        self.events[4]["id"]["targetCompleted"]["aspect"] = self.ANALYZER_ASPECT
        self.save()
        inventories = read_reports(self.bep, [self.owner], profile="analyzer")
        self.assertEqual(inventories[0].reports[0].identity.mode, "ANALYZER")
        # The ordinary lint profile cannot accept the analyzer completion.
        with self.assertRaises(ValueError):
            read_reports(self.bep, [self.owner])
        # The analyzer profile cannot accept the ordinary lint aspect either.
        self.events[4]["id"]["targetCompleted"]["aspect"] = self.LINT_ASPECT
        self.save()
        with self.assertRaises(ValueError):
            read_reports(self.bep, [self.owner], profile="analyzer")

    def test_each_profile_rejects_the_foreign_mode_set(self):
        self.events[4]["id"]["targetCompleted"]["aspect"] = self.ANALYZER_ASPECT
        self.save()
        with self.assertRaisesRegex(ValueError, "foreign to the requested analysis kind"):
            read_reports(self.bep, [self.owner], profile="analyzer")
        for report in self.index["reports"]:
            report["identity"]["mode"] = "ANALYZER"
            self.materialize(report["report"], self.report_json(report["identity"], "clean"))
        self.materialize(self.index_name, json.dumps(self.index))
        self.events[4]["id"]["targetCompleted"]["aspect"] = self.LINT_ASPECT
        self.save()
        with self.assertRaisesRegex(ValueError, "foreign to the requested analysis kind"):
            read_reports(self.bep, [self.owner])


class AnalyzerGateTest(unittest.TestCase):
    """The report-only analyzer gate: findings gated, tool failures nonzero."""

    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="ao-analyzer-gate-")
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.owner = "@@//lib/sample:sample"
        self.configuration = "bazel-out/k8-dbg/bin"
        self.execroot = self.root / "execroot"
        self.sequence = 0

    def report(self, main, *, outcome="clean", log="", source=None):
        identity = bazel_lint_reports.Identity(
            self.owner,
            self.configuration,
            source or main,
            main,
            (self.configuration + "/lib/sample/_objs/sample/Sample.o",),
            "ANALYZER",
            "linux",
        )
        log_path = self.root / f"analyzer-{self.sequence}.log"
        self.sequence += 1
        log_path.write_text(log, encoding="utf-8")
        return bazel_lint_reports.Report(
            identity, outcome, 1 if outcome == "compiler_error" else 0, self.root / "report.json", log_path
        )

    def inventory(self, *reports, uncovered=()):
        sources = tuple(
            dict.fromkeys(
                [path for report in reports for path in (report.identity.source, report.identity.main)]
                + [item.path for item in uncovered]
            )
        )
        return bazel_lint_reports.OwnerInventory(
            self.owner, self.configuration, "config-id", tuple(reports), sources, tuple(uncovered), self.execroot
        )

    def gate(self, inventories, selected, **kwargs):
        with contextlib.redirect_stdout(io.StringIO()) as out, contextlib.redirect_stderr(io.StringIO()) as err:
            status = gate_analyzer_reports(inventories, set(selected), **kwargs)
        return status, out.getvalue(), err.getvalue()

    def test_clean_reports_claim_no_diagnostics(self):
        inventory = self.inventory(self.report("lib/sample/Sample.cpp"))
        status, out, err = self.gate((inventory,), {"lib/sample/Sample.cpp"})
        self.assertEqual(status, 0)
        self.assertIn("No analyzer diagnostics found.", out)
        self.assertEqual(err, "")

    def test_findings_are_report_only_until_the_gate_is_requested(self):
        log = "lib/sample/Sample.cpp:1:1: warning: leak [clang-analyzer-cplusplus.NewDeleteLeaks]\n"
        inventory = self.inventory(self.report("lib/sample/Sample.cpp", outcome="warning", log=log))
        status, out, _ = self.gate((inventory,), {"lib/sample/Sample.cpp"})
        self.assertEqual(status, 0)
        self.assertIn("Analyzer diagnostics: 1", out)
        self.assertIn("leak", out)
        status, _, _ = self.gate((inventory,), {"lib/sample/Sample.cpp"}, fail_on_diagnostics=True)
        self.assertEqual(status, 1)

    def test_compiler_errors_are_tool_failures_even_report_only(self):
        log = "lib/sample/Sample.cpp:2:3: error: unknown type [clang-diagnostic-error]\n"
        inventory = self.inventory(self.report("lib/sample/Sample.cpp", outcome="compiler_error", log=log))
        for gate in (False, True):
            with self.subTest(fail_on_diagnostics=gate):
                status, _, err = self.gate((inventory,), {"lib/sample/Sample.cpp"}, fail_on_diagnostics=gate)
                self.assertEqual(status, 1)
                self.assertIn("Analyzer tool failure output:", err)
                self.assertIn("unknown type", err)

    def test_uncovered_and_missing_selected_paths_fail_like_tool_failures(self):
        header = "include/ao/sample/Unpaired.h"
        inventory = self.inventory(self.report("lib/sample/Sample.cpp"), uncovered=(Uncovered(header, "no companion"),))
        status, _, err = self.gate((inventory,), {header, "lib/sample/Missing.cpp"})
        self.assertEqual(status, 1)
        self.assertIn("Uncovered analyzer source: lib/sample/Missing.cpp", err)
        self.assertIn(f"{header}: no companion", err)

    def test_external_diagnostics_need_the_explicit_option(self):
        log = (
            "/external/dependency/Header.h:1:1: warning: external [clang-analyzer-core.DivideZero]\n"
            "lib/sample/Sample.cpp:1:1: warning: internal [clang-analyzer-core.DivideZero]\n"
        )
        inventory = self.inventory(self.report("lib/sample/Sample.cpp", outcome="warning", log=log))
        status, out, _ = self.gate((inventory,), {"lib/sample/Sample.cpp"}, include_external=False)
        self.assertEqual(status, 0)
        self.assertIn("internal", out)
        self.assertNotIn("external", out)
        status, out, _ = self.gate((inventory,), {"lib/sample/Sample.cpp"}, include_external=True)
        self.assertIn("external", out)

    def test_output_file_receives_the_diagnostics_and_the_written_notice(self):
        log = "lib/sample/Sample.cpp:1:1: warning: leak [clang-analyzer-cplusplus.NewDeleteLeaks]\n"
        inventory = self.inventory(self.report("lib/sample/Sample.cpp", outcome="warning", log=log))
        output = self.root / "analyzer.txt"
        with contextlib.redirect_stdout(io.StringIO()) as out:
            status = gate_analyzer_reports((inventory,), {"lib/sample/Sample.cpp"}, output, fail_on_diagnostics=True)
        self.assertEqual(status, 1)
        self.assertIn(f"Output written to {output}", out.getvalue())
        self.assertIn("leak", output.read_text(encoding="utf-8"))

    def test_deduplicated_findings_count_once_across_reports(self):
        log = "include/ao/sample/Sample.h:2:3: warning: shared [clang-analyzer-core.NullDereference]\n"
        inventory = self.inventory(
            self.report("lib/sample/Sample.cpp", outcome="warning", log=log),
            self.report("lib/sample/Other.cpp", outcome="warning", log=log),
        )
        status, out, _ = self.gate(
            (inventory,), {"lib/sample/Sample.cpp", "lib/sample/Other.cpp"}, fail_on_diagnostics=True
        )
        self.assertEqual(status, 1)
        self.assertIn("Analyzer diagnostics: 1", out)
        self.assertEqual(out.count("shared"), 1)


if __name__ == "__main__":
    unittest.main()
