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

from ao.core.bazel_lint_reports import Uncovered, gate_reports, read_reports
from ao.core.paths import PROJECT_ROOT, absolute_path


class CurrentReportsTest(unittest.TestCase):
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
        target_id = {
            "targetCompleted": {
                "label": "//lib/sample:sample",
                "configuration": {"id": self.config_id},
                "aspect": "//bazel/lint:lint.bzl%lint_aspect",
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

    def test_partial_unsuccessful_aborted_or_duplicate_bep_is_rejected(self):
        original = copy.deepcopy(self.events)
        for fault in (
            "last",
            "truncated",
            "exit",
            "aborted",
            "missing-child",
            "duplicate",
            "duplicate-key",
            "malformed",
        ):
            with self.subTest(fault=fault):
                self.events = copy.deepcopy(original)
                if fault == "last":
                    del self.events[-1]["lastMessage"]
                elif fault == "truncated":
                    self.events.pop()
                elif fault == "exit":
                    self.events[-2]["finished"]["exitCode"] = {"name": "BUILD_FAILURE", "code": 1}
                elif fault == "aborted":
                    self.events[1] = {"id": self.events[1]["id"], "aborted": {"reason": "ANALYSIS_FAILURE"}}
                elif fault == "missing-child":
                    self.events[0]["children"].append({"progress": {"opaqueCount": 17}})
                elif fault == "duplicate":
                    self.events.insert(2, self.events[1])
                self.save()
                if fault == "duplicate-key":
                    self.bep.write_text('{"id":{},"id":{}}\n', encoding="utf-8")
                elif fault == "malformed":
                    self.bep.write_text(self.bep.read_text(encoding="utf-8") + "{", encoding="utf-8")
                with self.assertRaises(ValueError):
                    read_reports(self.bep, [self.owner])

    def test_missing_duplicate_wrong_owner_aspect_or_full_configuration_cannot_bind_an_index(self):
        original = copy.deepcopy(self.events)
        for fault in ("owner", "aspect", "configuration", "missing-config", "duplicate-owner", "bin-dir"):
            with self.subTest(fault=fault):
                self.events = copy.deepcopy(original)
                target = self.events[4]["id"]["targetCompleted"]
                if fault == "owner":
                    target["label"] = "//lib/sample:other"
                elif fault == "aspect":
                    target["aspect"] = "//bazel/lint:other.bzl%not_lint_aspect"
                elif fault == "configuration":
                    target["configuration"]["id"] = self.config_id[:7]
                elif fault == "missing-config":
                    self.events.pop(1)
                elif fault == "duplicate-owner":
                    duplicate = copy.deepcopy(self.events[4])
                    duplicate["id"]["targetCompleted"]["configuration"]["id"] = "other-full-id"
                    self.events.insert(5, duplicate)
                else:
                    self.events[1]["configuration"]["makeVariable"]["BINDIR"] = "bazel-out/other-dbg/bin"
                self.save()
                with self.assertRaises(ValueError):
                    read_reports(self.bep, [self.owner])

    def test_malformed_nested_bep_fields_fail_with_a_validation_error(self):
        original = copy.deepcopy(self.events)
        for fault in ("target", "target-config", "configuration", "set", "group", "file"):
            with self.subTest(fault=fault):
                self.events = copy.deepcopy(original)
                if fault == "target":
                    self.events[4]["id"]["targetCompleted"] = []
                elif fault == "target-config":
                    self.events[4]["id"]["targetCompleted"]["configuration"] = []
                elif fault == "configuration":
                    self.events[1]["id"]["configuration"] = []
                elif fault == "set":
                    self.events[2]["id"]["namedSet"] = []
                elif fault == "group":
                    self.events[4]["completed"]["outputGroup"] = [None]
                else:
                    self.events[2]["namedSetOfFiles"]["files"] = [None]
                with self.assertRaises(ValueError):
                    self.read()

    def test_missing_start_or_mixed_output_execroots_do_not_establish_current_inventory(self):
        original = copy.deepcopy(self.events)
        self.events.pop(0)
        with self.assertRaises(ValueError):
            self.read()
        self.events = copy.deepcopy(original)
        name = self.index["reports"][0]["report"]
        elsewhere = self.root / "other-execroot" / name
        elsewhere.parent.mkdir(parents=True)
        elsewhere.write_text((self.root / "execroot/_main" / name).read_text(encoding="utf-8"), encoding="utf-8")
        self.events[2]["namedSetOfFiles"]["files"][0]["uri"] = elsewhere.as_uri()
        with self.assertRaises(ValueError):
            self.read()

    def test_cycles_missing_sets_duplicate_files_and_incomplete_groups_are_rejected(self):
        original = copy.deepcopy(self.events)
        for fault in ("cycle", "missing", "duplicate-file", "incomplete", "missing-group", "duplicate-group"):
            with self.subTest(fault=fault):
                self.events = copy.deepcopy(original)
                if fault == "cycle":
                    self.events[2]["namedSetOfFiles"]["fileSets"] = [{"id": "0"}]
                elif fault == "missing":
                    self.events[3]["namedSetOfFiles"]["fileSets"] = [{"id": "missing"}]
                elif fault == "duplicate-file":
                    self.events[2]["namedSetOfFiles"]["files"].append(self.files[0])
                else:
                    groups = self.events[4]["completed"]["outputGroup"]
                    if fault == "incomplete":
                        groups[0]["incomplete"] = True
                    elif fault == "missing-group":
                        groups[0]["name"] = "default"
                    else:
                        groups.append(copy.deepcopy(groups[0]))
                with self.assertRaises(ValueError):
                    self.read()

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

    def test_index_and_report_must_match_owner_native_configuration_and_entire_context(self):
        original = copy.deepcopy(self.index)
        for fault in (
            "schema",
            "owner",
            "configuration",
            "identity-owner",
            "identity-output",
            "identity-mode",
            "unaccounted",
            "duplicate",
            "foreign-report",
            "foreign-log",
        ):
            with self.subTest(fault=fault):
                self.index = copy.deepcopy(original)
                if fault in ("schema", "owner", "configuration"):
                    self.index[fault] = {
                        "schema": 2,
                        "owner": "@@//lib/sample:other",
                        "configuration": "bazel-out/other/bin",
                    }[fault]
                elif fault.startswith("identity-"):
                    key = fault.removeprefix("identity-")
                    key = "outputs" if key == "output" else key
                    self.index["reports"][0]["identity"][key] = {
                        "owner": "@@//lib/sample:other",
                        "outputs": ["bazel-out/other/bin/Sample.o"],
                        "mode": "RELAXED",
                    }[key]
                elif fault == "unaccounted":
                    self.index["sources"].append("include/ao/sample/Unpaired.h")
                elif fault == "duplicate":
                    self.index["reports"].append(copy.deepcopy(self.index["reports"][0]))
                else:
                    key = fault.removeprefix("foreign-")
                    self.index["reports"][0][key] = self.configuration + "/lib/other/stale." + key
                with self.assertRaises(ValueError):
                    self.read()

    def test_malformed_missing_or_false_clean_report_is_rejected(self):
        entry = self.index["reports"][0]
        report_path = self.root / "execroot/_main" / entry["report"]
        for content in (
            "{",
            "[]",
            '{"schema":1,"schema":1}',
            json.dumps({"schema": 1}),
            json.dumps({"schema": 1, "identity": self.identity, "outcome": "clean", "tool_exit": 0}),
            self.report_json(self.identity, includes=["/usr/include/stdio.h"]),
            self.report_json(self.identity, includes=["lib/sample/Sample.h", "lib/sample/Sample.h"]),
            self.report_json(self.identity, "clean", -11),
            self.report_json(self.identity, "warning", 0),
        ):
            with self.subTest(content=content):
                report_path.write_text(content, encoding="utf-8")
                with self.assertRaises(ValueError):
                    self.read()
        report_path.unlink()
        with self.assertRaises((ValueError, OSError)):
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

    def test_requested_owner_set_must_be_exact_nonempty_and_unique(self):
        self.save()
        for owners in ([], [self.owner, self.owner], [self.owner, "//lib/missing:missing"]):
            with self.subTest(owners=owners), self.assertRaises(ValueError):
                read_reports(self.bep, owners)


if __name__ == "__main__":
    unittest.main()
