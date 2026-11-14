"""Coverage protocol regressions with isolated profiles and strict source reports.

Only the LLVM reader/process and native workspace boundaries are faked. The
collector, pair matching, parsing, archive receipts, hit merge and admission stay
real; these fixtures are not evidence that Bazel consumed the coverage seam.
"""

import argparse
import contextlib
import importlib.util
import io
import json
import os
import shutil
import subprocess
import tempfile
import time
import unittest
import zipfile
from pathlib import Path
from unittest import mock

from ao.core import bazel_coverage
from ao.core.bazel_native import BazelWorkspaceError
from ao.core.paths import PROJECT_ROOT

_SPEC = importlib.util.spec_from_file_location("aobus_coverage_collector", PROJECT_ROOT / "bazel/coverage/collect.py")
assert _SPEC is not None and _SPEC.loader is not None
collector = importlib.util.module_from_spec(_SPEC)
_SPEC.loader.exec_module(collector)

# Exact LLVM B11* bytes retained from a native zero-function translation unit.
_ZERO_FUNCTION_NOTES = bytes.fromhex("6f6e6367 2a313142 ffffffff 01000000 2e000000 00000000 00000000 00000000")
_ZERO_FUNCTION_PROFILE = bytes.fromhex(
    "61646367 2a313142 ffffffff 000000a1 02000000 01000000 00000000 00000000 00000000"
)


class CollectionFixture:
    def __init__(self, root):
        assert root.is_absolute() and root.resolve().is_relative_to(Path(tempfile.gettempdir()).resolve())
        self.root = root / "execroot"
        self.root.mkdir()
        self.coverage = root / "coverage"
        self.coverage.mkdir()
        self.output = root / "outputs" / "aobus-coverage"
        self.output.mkdir(parents=True)
        self.tool = root / "llvm-cov"
        self.tool.write_text("selected LLVM reader", encoding="utf-8")
        self.manifest = self.root / "manifest.txt"
        self.source = self.root / "app/runtime/Value.cpp"
        self.source.parent.mkdir(parents=True)
        self.source.write_text("int value() {\n  return 3;\n}\n", encoding="utf-8")
        self.environment = {
            "AOBUS_COVERAGE_ATTEMPT": "run-fixture",
            "TEST_TARGET": "//test:fixture",
        }
        self.notes = []
        self.extractions = []

    def add(self, name, *, data=True, matching=True):
        relative = Path("bazel-out/dbg/bin/test/_objs/fixture") / (name + ".gcno")
        notes = self.root / relative
        notes.parent.mkdir(parents=True, exist_ok=True)
        notes.write_bytes(b"oncg408*same" + b"notes")
        self.notes.append(relative.as_posix())
        self.manifest.write_text("\n".join(["app/runtime/Value.cpp", *self.notes]) + "\n", encoding="utf-8")
        profile = self.coverage / self.root.relative_to(self.root.anchor) / relative.with_suffix(".gcda")
        if data:
            profile.parent.mkdir(parents=True, exist_ok=True)
            profile.write_bytes(b"adcg408*" + (b"same" if matching else b"old!") + b"counters")
        return notes, profile

    def reader(self, argv, **kwargs):
        if argv == [str(self.tool), "gcov", "--version"]:
            return subprocess.CompletedProcess(argv, 0, "LLVM version 22.1.8\n", "")
        if argv[:4] != [str(self.tool), "gcov", "-i", "-o"]:
            raise AssertionError(f"unexpected command: {argv}")
        pair = Path(kwargs["cwd"])
        self.extractions.append(argv)
        (pair / "Value.cpp.gcov").write_text(
            "file:app/runtime/Value.cpp\nfunction:1,2,value\nlcount:1,0\nlcount:2,3\n", encoding="utf-8"
        )
        return subprocess.CompletedProcess(argv, 0, "Created report\n", "")

    def empty_reader(self, argv, **kwargs):
        result = self.reader(argv, **kwargs)
        if "cwd" not in kwargs:
            return result
        (Path(kwargs["cwd"]) / "Value.cpp.gcov").write_bytes(b"")
        return subprocess.CompletedProcess(argv, 0, "", "")

    def collect(self, reader=None):
        with mock.patch.object(collector.subprocess, "run", side_effect=reader or self.reader):
            collector.collect(self.root, self.coverage, self.manifest, self.tool, self.output, self.environment)
        return json.loads((self.output / "receipt.json").read_text(encoding="utf-8"))

    def archive(self):
        (self.output / "test.json").write_text(
            json.dumps({"attempt": self.environment["AOBUS_COVERAGE_ATTEMPT"], "exit": 0}), encoding="utf-8"
        )
        path = self.output.parent.parent / "outputs.zip"
        with zipfile.ZipFile(path, "w") as archive:
            for source in sorted(self.output.parent.rglob("*")):
                if source.is_file():
                    archive.write(source, source.relative_to(self.output.parent))
        return path


class StrictCollectionTests(unittest.TestCase):
    def test_all_observed_pairs_are_consumed_and_unlinked_archive_notes_are_allowed(self):
        with tempfile.TemporaryDirectory(prefix="aobus-coverage-") as temporary:
            fixture = CollectionFixture(Path(temporary))
            fixture.add("linked")
            fixture.add("unlinked", data=False)
            receipt = fixture.collect()
            self.assertEqual(receipt["status"], "passed")
            self.assertEqual(receipt["consumed_profiles"], 1)
            self.assertEqual(len(fixture.extractions), 1)
            report = bazel_coverage.parse_lcov((fixture.output / "coverage.lcov").read_text(encoding="utf-8"))
            self.assertEqual(report, {"app/runtime/Value.cpp": {1: 0, 2: 3}})
            self.assertEqual((fixture.output / "profile-00000/object.gcda").read_bytes()[:4], b"adcg")
            self.assertIn("app/runtime/Value.cpp", receipt["sources"])

    def test_zero_function_notes_prove_empty_extraction_without_skipping_the_profile(self):
        with tempfile.TemporaryDirectory(prefix="aobus-coverage-") as temporary:
            fixture = CollectionFixture(Path(temporary))
            fixture.add("a-measured")
            notes, profile = fixture.add("b-zero-functions")
            notes.write_bytes(_ZERO_FUNCTION_NOTES)
            profile.write_bytes(_ZERO_FUNCTION_PROFILE)

            def reader(argv, **kwargs):
                if "profile-00001" in str(kwargs.get("cwd", "")):
                    return fixture.empty_reader(argv, **kwargs)
                return fixture.reader(argv, **kwargs)

            receipt = fixture.collect(reader)
            self.assertEqual(receipt["status"], "passed")
            self.assertEqual(receipt["consumed_profiles"], 2)
            self.assertEqual(len(fixture.extractions), 2)
            record = receipt["profiles"][1]
            self.assertEqual(record["status"], "passed")
            self.assertEqual(record["executable_functions"], 0)
            self.assertEqual(record["empty_report_reason"], "proved zero-function translation unit")
            self.assertEqual(record["exit"], 0)
            self.assertEqual(record["notes_sha256"], collector.digest(notes))
            self.assertEqual(record["profile_sha256"], collector.digest(profile))
            raw = fixture.output / record["reports"][0]["path"]
            self.assertEqual(raw.read_bytes(), b"")
            self.assertEqual(record["reports"][0]["sha256"], collector.digest(raw))
            data, _ = bazel_coverage._collection(
                fixture.archive(), Path(temporary) / "retained", "run-fixture", "//test:fixture"
            )
            self.assertEqual(data, {"app/runtime/Value.cpp": {1: 0, 2: 3}})

    def test_only_zero_function_profiles_do_not_produce_a_successful_empty_aggregate(self):
        with tempfile.TemporaryDirectory(prefix="aobus-coverage-") as temporary:
            fixture = CollectionFixture(Path(temporary))
            notes, profile = fixture.add("zero-functions")
            notes.write_bytes(_ZERO_FUNCTION_NOTES)
            profile.write_bytes(_ZERO_FUNCTION_PROFILE)
            with self.assertRaisesRegex(ValueError, "no executable coverage records"):
                fixture.collect(fixture.empty_reader)
            receipt = json.loads((fixture.output / "receipt.json").read_text(encoding="utf-8"))
            self.assertEqual(receipt["status"], "failed")
            self.assertEqual(receipt["profiles"][0]["executable_functions"], 0)
            self.assertFalse((fixture.output / "coverage.lcov").exists())

    def test_missing_notes_for_one_observed_counter_fails_despite_good_profile(self):
        with tempfile.TemporaryDirectory(prefix="aobus-coverage-") as temporary:
            fixture = CollectionFixture(Path(temporary))
            fixture.add("a-good")
            missing = fixture.coverage / "unlisted.gcda"
            missing.write_bytes(b"adcg408*samecounter")
            with self.assertRaisesRegex(ValueError, "gcno for observed"):
                fixture.collect()
            receipt = json.loads((fixture.output / "receipt.json").read_text(encoding="utf-8"))
            self.assertEqual(receipt["status"], "failed")
            self.assertEqual(len(receipt["observed_profiles"]), 2)
            self.assertFalse((fixture.output / "coverage.lcov").exists())

    def test_nonzero_second_extraction_is_not_masked_by_first_valid_report(self):
        with tempfile.TemporaryDirectory(prefix="aobus-coverage-") as temporary:
            fixture = CollectionFixture(Path(temporary))
            fixture.add("a-good")
            fixture.add("b-bad")

            def reader(argv, **kwargs):
                result = fixture.reader(argv, **kwargs)
                if "profile-00001" in str(kwargs.get("cwd", "")):
                    return subprocess.CompletedProcess(argv, 7, result.stdout, "bad counters")
                return result

            with self.assertRaisesRegex(ValueError, "extraction failed \\(7\\)"):
                fixture.collect(reader)
            receipt = json.loads((fixture.output / "receipt.json").read_text(encoding="utf-8"))
            self.assertEqual(receipt["profiles"][0]["status"], "passed")
            self.assertEqual(receipt["profiles"][1]["exit"], 7)
            self.assertEqual(receipt["status"], "failed")

    def test_all_profiles_missing_is_unavailable_not_zero_or_complete(self):
        with tempfile.TemporaryDirectory(prefix="aobus-coverage-") as temporary:
            fixture = CollectionFixture(Path(temporary))
            fixture.add("unlinked", data=False)
            with self.assertRaisesRegex(ValueError, "no current-test gcda"):
                fixture.collect()

    def test_synthetic_function_location_zero_is_valid_metadata_not_an_executable_line(self):
        with tempfile.TemporaryDirectory(prefix="aobus-coverage-") as temporary:
            fixture = CollectionFixture(Path(temporary))
            text = (
                "file:app/runtime/Value.cpp\nfunction:0,1,__cxx_global_var_init\n"
                "function:0,0,another_synthetic_symbol\nlcount:2,3\n"
            )
            data, _ = collector.intermediate(text, fixture.root, {})
            self.assertEqual(data, {"app/runtime/Value.cpp": {2: 3}})
            for row in (
                "function:-1,1,synthetic",
                "function:00,1,synthetic",
                "function:x,1,synthetic",
                "function:0,-1,synthetic",
                "function:0,bad,synthetic",
                "function:0,1,",
                "lcount:0,1",
            ):
                with self.subTest(row=row), self.assertRaises(ValueError):
                    collector.intermediate(f"file:app/runtime/Value.cpp\n{row}\nlcount:2,3\n", fixture.root, {})

    def test_generated_only_profile_is_checked_but_cannot_become_a_production_measurement(self):
        with tempfile.TemporaryDirectory(prefix="aobus-coverage-") as temporary:
            fixture = CollectionFixture(Path(temporary))
            fixture.add("generated")

            def reader(argv, **kwargs):
                result = fixture.reader(argv, **kwargs)
                if "cwd" in kwargs:
                    (Path(kwargs["cwd"]) / "Value.cpp.gcov").write_text(
                        "file:bazel-out/dbg/bin/app/generated.cpp\nlcount:7000,1\n",
                        encoding="utf-8",
                    )
                return result

            receipt = fixture.collect(reader)
            self.assertEqual(receipt["status"], "passed")
            self.assertEqual(receipt["consumed_profiles"], 1)
            self.assertEqual(receipt["sources"], {})
            data = bazel_coverage.parse_lcov((fixture.output / "coverage.lcov").read_text(encoding="utf-8"))
            with self.assertRaisesRegex(ValueError, "no executable production"):
                bazel_coverage.production_report([data], {}, fixture.root)

    def test_virtual_headers_use_metadata_and_preserve_equal_basenames(self):
        with tempfile.TemporaryDirectory(prefix="aobus-coverage-") as temporary:
            root = Path(temporary)
            for relative in ("include/one/Value.h", "include/two/Value.h"):
                path = root / relative
                path.parent.mkdir(parents=True)
                path.write_text("inline int value() { return 3; }\n", encoding="utf-8")
            mappings = {"bazel-out/dbg/bin/_virtual_includes/one/Value.h": "include/one/Value.h"}
            text = (
                "file:bazel-out/dbg/bin/_virtual_includes/one/Value.h\nlcount:1,2\n"
                "file:include/two/Value.h\nlcount:1,0\n"
            )
            data, _ = collector.intermediate(text, root, mappings)
            self.assertEqual(data, {"include/one/Value.h": {1: 2}, "include/two/Value.h": {1: 0}})
            with self.assertRaisesRegex(ValueError, "lacks authored-source metadata"):
                collector.intermediate(text, root, {})

    def test_standard_aliases_report_authored_sources_and_parse_generated_records_without_reporting_them(self):
        alias = "bazel-out/dbg/bin/_virtual_includes/value/Value.h"
        generated_alias = "bazel-out/dbg/bin/_virtual_includes/generated/Tags.h"
        generated = "bazel-out/dbg/bin/app/codegen/Tags.h"
        for malformed in (False, True):
            with self.subTest(malformed=malformed), tempfile.TemporaryDirectory(prefix="aobus-coverage-") as temporary:
                root = Path(temporary)
                fixture = CollectionFixture(root)
                fixture.add("linked")
                mappings = {alias: "app/runtime/Value.cpp", generated_alias: generated}
                standard = root / "standard-mappings.txt"
                standard.write_text(f"{alias}:app/runtime/Value.cpp\n{generated_alias}:{generated}\n", encoding="utf-8")
                fixture.environment["COVERAGE_REPORTED_TO_ACTUAL_SOURCES_FILE"] = str(standard)

                def reader(argv, *, fixture=fixture, malformed=malformed, **kwargs):
                    result = fixture.reader(argv, **kwargs)
                    if "cwd" in kwargs:
                        row = "lcount:0,1" if malformed else "lcount:7000,2"
                        with (Path(kwargs["cwd"]) / "Value.cpp.gcov").open("a", encoding="utf-8") as report:
                            report.write(f"file:{alias}\nlcount:2,5\nfile:{generated_alias}\n{row}\n")
                    return result

                if malformed:
                    with self.assertRaisesRegex(ValueError, "malformed line record"):
                        fixture.collect(reader)
                    continue
                receipt = fixture.collect(reader)
                self.assertEqual(receipt["schema"], 2)
                self.assertEqual(receipt["source_mapping_contract"], "declared-standard")
                self.assertEqual(receipt["source_mappings"], mappings)
                self.assertEqual(receipt["standard_source_mappings"]["mappings"], mappings)
                self.assertNotIn("action_source_mappings", receipt)
                self.assertEqual(receipt["sources"], {"app/runtime/Value.cpp": collector.digest(fixture.source)})
                archive = fixture.archive()
                data, sources = bazel_coverage._collection(
                    archive,
                    root / "retained",
                    "run-fixture",
                    "//test:fixture",
                    expected_source_mappings=mappings,
                )
                self.assertEqual(data[generated], {7000: 2})
                self.assertEqual(
                    bazel_coverage.production_report([data], sources, fixture.root),
                    {
                        "app/runtime/Value.cpp": {1: 0, 2: 8},
                    },
                )
                # The configured aquery expectations are an independent oracle:
                # a standard map that disagrees with them cannot pass.
                with self.assertRaises(ValueError):
                    bazel_coverage._collection(
                        archive,
                        root / "wrong-map",
                        "run-fixture",
                        "//test:fixture",
                        expected_source_mappings={alias: "lib/storage/Other.h"},
                    )


class CoverageRunnerTests(unittest.TestCase):
    def test_run_under_executes_forwarded_test_once_and_preserves_stock_layout_and_output(self):
        for status in (0, 3):
            with self.subTest(status=status), tempfile.TemporaryDirectory(prefix="aobus-coverage-") as temporary:
                fixture = CollectionFixture(Path(temporary))
                notes, _ = fixture.add("linked", data=False)
                profile = fixture.coverage / notes.relative_to(fixture.root).with_suffix(".gcda")
                fixture.output = Path(temporary) / "fresh-outputs" / "aobus-coverage"
                fixture.output.parent.mkdir()
                runfiles = fixture.root / "runfiles" / "_main"
                payload = runfiles / "test/fixture"
                payload.parent.mkdir(parents=True)
                payload.write_text("actual forwarded test payload sentinel", encoding="utf-8")
                destination = fixture.root / "coverage.dat"
                destination.write_text("stock outer owns this output", encoding="utf-8")
                environment = fixture.environment | {
                    "ROOT": str(fixture.root),
                    "COVERAGE_DIR": str(fixture.coverage),
                    "COVERAGE_MANIFEST": str(fixture.manifest),
                    "COVERAGE_OUTPUT_FILE": str(destination),
                    "COVERAGE_GCOV_PATH": str(fixture.tool),
                    "TEST_SRCDIR": str(runfiles.parent),
                    "TEST_WORKSPACE": "_main",
                    "TEST_UNDECLARED_OUTPUTS_DIR": str(fixture.output.parent),
                    "GCOV_PREFIX": str(fixture.coverage),
                    "GCOV_PREFIX_STRIP": "3",
                    "LLVM_PROFILE_FILE": str(fixture.coverage / "%h-%p-%m.profraw"),
                }
                arguments = ["test/fixture", "positional-test-argument"]
                calls = []

                def process(
                    argv,
                    *,
                    fixture=fixture,
                    profile=profile,
                    environment=environment,
                    runfiles=runfiles,
                    arguments=arguments,
                    calls=calls,
                    status=status,
                    **kwargs,
                ):
                    if argv == arguments:
                        calls.append(argv)
                        self.assertEqual(kwargs["cwd"], runfiles)
                        self.assertNotEqual(kwargs["cwd"], fixture.root)
                        self.assertEqual(kwargs["env"], environment)
                        self.assertEqual(
                            (kwargs["cwd"] / argv[0]).read_text(encoding="utf-8"),
                            "actual forwarded test payload sentinel",
                        )
                        profile.parent.mkdir(parents=True)
                        profile.write_bytes(b"adcg408*samecounters")
                        return subprocess.CompletedProcess(argv, status)
                    return fixture.reader(argv, **kwargs)

                with (
                    mock.patch.dict(os.environ, environment, clear=True),
                    mock.patch.object(collector.Path, "cwd", return_value=runfiles),
                    mock.patch.object(collector.subprocess, "run", side_effect=process),
                ):
                    self.assertEqual(collector.main(arguments), status)
                    self.assertEqual(dict(os.environ), environment)
                self.assertEqual(calls, [arguments])
                self.assertEqual(destination.read_text(encoding="utf-8"), "stock outer owns this output")
                self.assertTrue((fixture.output / "coverage.lcov").is_file())
                result = json.loads((fixture.output / "test.json").read_text(encoding="utf-8"))
                self.assertEqual(result["exit"], status)
                self.assertEqual(result["argv"], arguments)
                self.assertEqual(result["root"], str(fixture.root))

    def test_run_under_rejects_missing_root_or_redirected_counter_layout_before_test_execution(self):
        for overrides in ({"ROOT": "relative"}, {"GCOV_PREFIX": "/personal/out"}, {"GCOV_PREFIX_STRIP": "-1"}):
            with self.subTest(overrides=overrides), tempfile.TemporaryDirectory(prefix="aobus-coverage-") as temporary:
                root = Path(temporary)
                environment = {
                    "ROOT": str(root),
                    "COVERAGE_DIR": str(root / "coverage"),
                    "GCOV_PREFIX": str(root / "coverage"),
                    "GCOV_PREFIX_STRIP": "3",
                } | overrides
                with (
                    mock.patch.dict(os.environ, environment, clear=True),
                    mock.patch.object(collector.subprocess, "run") as process,
                    contextlib.redirect_stderr(io.StringIO()),
                ):
                    self.assertEqual(collector.main(["test/fixture"]), 1)
                    process.assert_not_called()


class ProductionReportTests(unittest.TestCase):
    def test_lcov_accumulates_duplicate_line_and_source_records_without_losing_zero_hits(self):
        text = (
            "SF:lib/audio/Ctor.cpp\nDA:5,2\nDA:5,3\nDA:7,0\nLF:2\nLH:1\nend_of_record\n"
            "SF:lib/audio/Ctor.cpp\nDA:7,4\nDA:9,0\nLF:2\nLH:1\nend_of_record\n"
        )
        self.assertEqual(bazel_coverage.parse_lcov(text), {"lib/audio/Ctor.cpp": {5: 5, 7: 4, 9: 0}})

    def test_scoped_reports_keep_file_statistics_and_accept_a_trailing_slash(self):
        with tempfile.TemporaryDirectory(prefix="aobus-coverage-") as temporary:
            root = Path(temporary)
            report = {
                "app/linux-gtk/Foo.cpp": {1: 1, 2: 0},
                "app/linux-gtk/sub/Bar.cpp": {1: 2, 2: 3},
                "lib/core/Baz.cpp": {1: 0},
            }
            identities = {}
            for name in report:
                source = root / name
                source.parent.mkdir(parents=True, exist_ok=True)
                source.write_text("one\ntwo\ncomment\n", encoding="utf-8")
                identities[name] = collector.digest(source)
            for scope in ("app/linux-gtk", "app/linux-gtk/"):
                with self.subTest(scope=scope):
                    selected = bazel_coverage.production_report([report], identities, root, [scope])
                    self.assertEqual(
                        selected,
                        {"app/linux-gtk/Foo.cpp": {1: 1, 2: 0}, "app/linux-gtk/sub/Bar.cpp": {1: 2, 2: 3}},
                    )
                    self.assertEqual(
                        bazel_coverage._lcov(selected),
                        "TN:\nSF:app/linux-gtk/Foo.cpp\nDA:1,1\nDA:2,0\nLF:2\nLH:1\nend_of_record\n"
                        "TN:\nSF:app/linux-gtk/sub/Bar.cpp\nDA:1,2\nDA:2,3\nLF:2\nLH:2\nend_of_record\n",
                    )

    def test_empty_and_unmeasured_production_reports_fail_instead_of_claiming_full_coverage(self):
        with tempfile.TemporaryDirectory(prefix="aobus-coverage-") as temporary:
            root = Path(temporary)
            source = root / "lib/audio/Foo.cpp"
            source.parent.mkdir(parents=True)
            source.write_text("comment\n", encoding="utf-8")
            identities = {"lib/audio/Foo.cpp": collector.digest(source)}
            for report in ({}, {"lib/audio/Foo.cpp": {}}, {"test/FooTest.cpp": {1: 1}}):
                with self.subTest(report=report), self.assertRaisesRegex(ValueError, "no executable production"):
                    bazel_coverage.production_report([report], identities, root)

    def test_header_lines_hit_merge_across_test_objects_and_suites(self):
        with tempfile.TemporaryDirectory(prefix="aobus-coverage-") as temporary:
            root = Path(temporary)
            header = root / "include/Value.h"
            header.parent.mkdir()
            header.write_text("one\ntwo\nthree\n", encoding="utf-8")
            reports = [
                {"include/Value.h": {1: 0, 2: 2}, "test/ValueTest.cpp": {1: 8}, "/nix/store/sdk/Value.h": {1: 4}},
                {"include/Value.h": {1: 5, 2: 1, 3: 0}},
            ]
            merged = bazel_coverage.production_report(reports, {"include/Value.h": collector.digest(header)}, root)
            self.assertEqual(merged, {"include/Value.h": {1: 5, 2: 3, 3: 0}})
            self.assertEqual(bazel_coverage.parse_lcov(bazel_coverage._lcov(merged)), merged)

    def test_every_explicit_scope_requires_measurement_and_application_requires_cpp(self):
        with tempfile.TemporaryDirectory(prefix="aobus-coverage-") as temporary:
            root = Path(temporary)
            header = root / "include/Value.h"
            header.parent.mkdir()
            header.write_text("one\n", encoding="utf-8")
            data = [{"include/Value.h": {1: 3}}]
            identity = {"include/Value.h": collector.digest(header)}
            with self.assertRaisesRegex(ValueError, "scope: app/runtime"):
                bazel_coverage.production_report(data, identity, root, ["include", "app/runtime"])
            with self.assertRaisesRegex(ValueError, "no real app implementation"):
                bazel_coverage.production_report(data, identity, root, require_application=True)
            for scope in ("../include", "/include", "test"):
                with self.subTest(scope=scope), self.assertRaises(ValueError):
                    bazel_coverage.production_report(data, identity, root, [scope])

    def test_lcov_synthetic_function_metadata_zero_does_not_admit_line_zero(self):
        text = "SF:app/Value.cpp\nFN:0,__cxx_global_var_init\nFNDA:1,__cxx_global_var_init\nDA:2,3\nend_of_record\n"
        self.assertEqual(bazel_coverage.parse_lcov(text), {"app/Value.cpp": {2: 3}})
        for malformed in (text.replace("FN:0,", "FN:-1,"), text.replace("DA:2,3", "DA:0,3")):
            with self.subTest(text=malformed), self.assertRaises(ValueError):
                bazel_coverage.parse_lcov(malformed)

    def test_function_only_lcov_records_require_explicit_zero_line_summaries(self):
        zero = (
            "SF:app/Value.h\nFN:21,implicit_member\nFNDA:3,implicit_member\nFNF:1\nFNH:1\nLH:0\nLF:0\nend_of_record\n"
        )
        measured = "SF:app/Value.cpp\nDA:1,3\nend_of_record\n"
        self.assertEqual(bazel_coverage.parse_lcov(zero + measured), {"app/Value.h": {}, "app/Value.cpp": {1: 3}})
        for text in (zero, zero.replace("LF:0\n", "") + measured, zero.replace("LH:0", "LH:1") + measured):
            with self.subTest(text=text), self.assertRaises(ValueError):
                bazel_coverage.parse_lcov(text)


class CoverageAdmissionTests(unittest.TestCase):
    def test_unsupported_host_or_profile_rejected_without_tree_or_environment_selection(self):
        for platform, config in (
            ("macos", "debug"),
            ("windows", "debug"),
            ("linux", "release"),
            ("linux", "asan"),
            ("linux", "tsan"),
        ):
            with (
                self.subTest(platform=platform, config=config),
                mock.patch.object(bazel_coverage.native, "native_platform", return_value=platform),
                mock.patch.object(bazel_coverage.bazel_workspace, "workspace_path") as select,
                mock.patch.object(bazel_coverage.bazel_workspace, "_linux_server_javabase") as java,
            ):
                with self.assertRaises(BazelWorkspaceError):
                    bazel_coverage.run(argparse.Namespace(config=config))
                select.assert_not_called()
                java.assert_not_called()

    def test_filters_and_native_prefix_are_explicitly_rejected_not_claimed_supported(self):
        for overrides in (
            {"filter": "Value*"},
            {"vcpkg_prefix": "/ready/sdk"},
            {"analysis_only": True, "scope": ["lib"]},
            {"analysis_only": True, "filter": "Value*"},
        ):
            args = argparse.Namespace(config="debug", vcpkg_prefix=None, filter=None)
            vars(args).update(overrides)
            with (
                mock.patch.object(bazel_coverage.native, "native_platform", return_value="linux"),
                mock.patch.object(bazel_coverage.bazel_workspace, "workspace_path") as select,
            ):
                with self.assertRaises(BazelWorkspaceError):
                    bazel_coverage.run(args)
                select.assert_not_called()

    def test_child_environment_cannot_redirect_profiles_and_parent_is_unchanged(self):
        hostile = {
            "LLVM_PROFILE_FILE": "/personal/out",
            "GCOV_PREFIX": "/personal/data",
            "GCOV_PREFIX_STRIP": "99",
            "IGNORE_COVERAGE_COLLECTION_FAILURES": "1",
            "JAVA_HOME": "/personal/jdk",
            "JAVABIN": "/personal/java",
            "KEEP": "value",
        }
        home = Path("/owned/selected-jdk")
        with mock.patch.dict(os.environ, hostile, clear=True):
            self.assertEqual(
                bazel_coverage._environment(home),
                {"KEEP": "value", "JAVA_HOME": str(home), "JAVABIN": str(home / "bin/java")},
            )
            self.assertEqual(dict(os.environ), hostile)

    def test_child_environment_ignores_ambient_bazelrc_and_keeps_selected_java(self):
        hostile = {
            "BAZELRC": "/personal/first.bazelrc",
            "bAzElRc": "/personal/second.bazelrc",
            "LLVM_PROFILE_FILE": "/personal/out",
            "GCOV_PREFIX": "/personal/data",
            "JAVA_HOME": "/personal/jdk",
            "JAVABIN": "/personal/java",
            "KEEP": "sentinel",
        }
        home = Path("/owned/selected-jdk")
        with mock.patch.dict(os.environ, hostile, clear=True):
            ambient = dict(os.environ)
            environment = bazel_coverage._environment(home)
            self.assertFalse(any(key.casefold() == "bazelrc" for key in environment))
            self.assertEqual(
                environment,
                {"KEEP": "sentinel", "JAVA_HOME": str(home), "JAVABIN": str(home / "bin/java")},
            )
            self.assertEqual(dict(os.environ), ambient)


def _actions():
    return {
        "targets": [
            {"id": 1, "label": "//app/runtime:runtime"},
            {"id": 2, "label": "//test:fixture"},
            {"id": 3, "label": "//tool:catalog"},
        ],
        "configuration": [{"id": 1, "isTool": False}, {"id": 2, "isTool": True}],
        "actions": [
            {
                "targetId": 1,
                "configurationId": 1,
                "mnemonic": "CppCompile",
                "arguments": [
                    "clang++",
                    "-O0",
                    "-g",
                    "-fprofile-arcs",
                    "-ftest-coverage",
                    "-fprofile-update=atomic",
                    "-c",
                    "app/runtime/Value.cpp",
                ],
            },
            {
                "targetId": 2,
                "configurationId": 1,
                "mnemonic": "CppCompile",
                "arguments": [
                    "clang++",
                    "-O0",
                    "-g",
                    "-fprofile-arcs",
                    "-ftest-coverage",
                    "-fprofile-update=atomic",
                    "-c",
                    "test/ValueTest.cpp",
                ],
            },
            {
                "targetId": 3,
                "configurationId": 2,
                "mnemonic": "CppCompile",
                "arguments": ["clang++", "-O2", "-g0", "-c", "tool/catalog.cpp"],
            },
            {
                "targetId": 1,
                "configurationId": 1,
                "mnemonic": "CppArchive",
                "arguments": ["llvm-ar", "rcs", "runtime.a", "Value.o"],
            },
            {
                "targetId": 2,
                "configurationId": 1,
                "mnemonic": "CppLink",
                "arguments": ["clang++", "--coverage", "ValueTest.o", "runtime.a", "-o", "test"],
            },
        ],
    }


def _symlink_actions(source="lib/storage/detail/Failure.h"):
    document = _actions()
    alias = "bazel-out/dbg/bin/lib/storage/_virtual_includes/private/detail/Failure.h"
    fragments, artifacts = [], []
    for identifier, value in enumerate((alias, source), 1):
        parent = 0
        for label in value.split("/"):
            fragment = {"id": len(fragments) + 1, "label": label, "parentId": parent}
            fragments.append(fragment)
            parent = fragment["id"]
        artifacts.append({"id": identifier, "pathFragmentId": parent})
    document.update(
        pathFragments=fragments,
        artifacts=artifacts,
        depSetOfFiles=[{"id": 1, "directArtifactIds": [2]}, {"id": 2, "transitiveDepSetIds": [1]}],
    )
    document["actions"].append(
        {
            "mnemonic": "Symlink",
            "targetId": 1,
            "configurationId": 1,
            "outputIds": [1],
            "primaryOutputId": 1,
            "inputDepSetIds": [2],
        }
    )
    return document, alias


class CoverageAnalysisTests(unittest.TestCase):
    def test_analysis_attempt_refuses_execution_commands_before_launching_a_process(self):
        with tempfile.TemporaryDirectory(prefix="aobus-coverage-") as temporary:
            root = Path(temporary).resolve()
            args = argparse.Namespace(analysis_only=True, coverage_suite="default", scope=[])
            attempt = bazel_coverage._Attempt(root, args)
            with mock.patch.object(bazel_coverage.subprocess, "run") as process:
                for action in ("build", "test", "coverage", "run"):
                    with self.subTest(action=action), self.assertRaisesRegex(BazelWorkspaceError, "cannot execute"):
                        attempt.bazel("forbidden", Path("/fixture/bazel"), root, action, [])
                process.assert_not_called()
            self.assertEqual(attempt.receipt["phases"], [])


class CoveragePipelineTests(unittest.TestCase):
    def test_declared_symlink_aliases_use_exact_ids_and_ignore_exec_configurations(self):
        document, alias = _symlink_actions()
        document["actions"].append(dict(document["actions"][-1]))
        document["actions"].append(document["actions"][-1] | {"configurationId": 2, "inputDepSetIds": [999]})
        self.assertEqual(bazel_coverage._action_mappings(document), {alias: "lib/storage/detail/Failure.h"})
        for source in ("test/Helper.h", "external/sdk/Header.h", "bazel-out/dbg/bin/app/codegen/Tags.h"):
            with self.subTest(source=source):
                generated, alias = _symlink_actions(source)
                self.assertEqual(bazel_coverage._action_mappings(generated), {alias: source})

    def test_pipeline_uses_configured_fresh_receipts_and_combined_agreement(self):
        for outcome in (
            "passed",
            "processwrapper-sandbox",
            "local",
            "future-strategy",
            "missing-execution-info",
            "missing-strategy",
            "malformed-execution-info",
            "malformed-strategy",
            "strategy-association",
            "malformed-second-result",
            "missing-config-result",
            "virtual-alias",
            "disagreement",
            "stock-disagreement",
            "native-failure",
            "map-changed",
            "map-receipt-changed",
            "source-changed",
        ):
            with (
                self.subTest(outcome=outcome),
                tempfile.TemporaryDirectory(prefix="aobus-coverage-") as temporary,
            ):
                root = Path(temporary).resolve()
                fixture = CollectionFixture(root)
                fixture.add("linked")
                authored = root / "checkout"
                authored_source = authored / fixture.source.relative_to(fixture.root)
                authored_source.parent.mkdir(parents=True)
                shutil.copyfile(fixture.source, authored_source)
                unmeasured = authored / "lib/storage/Baseline.h"
                unmeasured.parent.mkdir(parents=True)
                unmeasured.write_text("inline int baseline();\n", encoding="utf-8")
                actions, alias = _symlink_actions()
                if outcome == "virtual-alias":
                    header = authored / "lib/storage/detail/Failure.h"
                    header.parent.mkdir(parents=True)
                    header.write_text("inline int failure() {\n  return 1; }\n", encoding="utf-8")
                else:
                    actions = _actions()
                standard = root / "standard-mappings.txt"
                standard.write_text(
                    f"{alias}:lib/storage/detail/Failure.h\n" if outcome == "virtual-alias" else "", encoding="utf-8"
                )
                fixture.environment["COVERAGE_REPORTED_TO_ACTUAL_SOURCES_FILE"] = str(standard)
                successful = outcome in {
                    "passed",
                    "processwrapper-sandbox",
                    "local",
                    "future-strategy",
                    "missing-execution-info",
                    "missing-strategy",
                    "strategy-association",
                    "virtual-alias",
                }
                strategy = {
                    "processwrapper-sandbox": "processwrapper-sandbox",
                    "local": "local",
                    "future-strategy": "future-worker-v2",
                    "missing-execution-info": None,
                    "missing-strategy": None,
                }.get(outcome, "linux-sandbox")
                tree = root / "tree"
                base = root / "base"
                base.mkdir()
                (base / "aobus.bazelrc").write_text("build --config=debug\n", encoding="utf-8")
                (base / "request.json").write_text(
                    json.dumps({"schema": 1, "host": "linux", "source": str(authored), "prefix": None}),
                    encoding="utf-8",
                )
                checksum = "d" * 64
                labels = "//test:fixture //test:other" if outcome == "strategy-association" else "//test:fixture"
                commands = []
                configurations = []
                server_java_home = root / "selected JDK"
                java_home = server_java_home / "lib/openjdk" if os.name != "nt" else server_java_home
                java = java_home / "bin/java"
                java.parent.mkdir(parents=True)
                java.write_bytes(b"selected Java executable sentinel")
                jar = java_home / "lib/jrt-fs.jar"
                jar.parent.mkdir(parents=True)
                jar.write_bytes(b"Java runtime capability sentinel")
                if os.name != "nt":
                    (server_java_home / "bin").symlink_to(java_home / "bin", target_is_directory=True)
                args = argparse.Namespace(
                    path=str(tree),
                    config="debug",
                    jobs=2,
                    remote_cache=None,
                    vcpkg_prefix=None,
                    filter=None,
                    scope=["app/runtime"],
                    coverage_suite="application",
                )

                def process(
                    argv,
                    *,
                    fixture=fixture,
                    commands=commands,
                    checksum=checksum,
                    labels=labels,
                    root=root,
                    tree=tree,
                    java_home=java_home,
                    server_java_home=server_java_home,
                    authored=authored,
                    configurations=configurations,
                    outcome=outcome,
                    actions=actions,
                    alias=alias,
                    strategy=strategy,
                    successful=successful,
                    **kwargs,
                ):
                    self.assertEqual(argv[0], str(Path("/fixture/bazel")))
                    self.assertTrue({"--nosystem_rc", "--nohome_rc", "--noworkspace_rc"} <= set(argv))
                    self.assertIn(f"--server_javabase={server_java_home}", argv)
                    self.assertEqual(kwargs["env"]["JAVA_HOME"], str(java_home))
                    self.assertEqual(kwargs["env"]["JAVABIN"], str(java_home / "bin/java"))
                    rc_path = Path([value.split("=", 1)[1] for value in argv if value.startswith("--bazelrc=")][-1])
                    configuration = rc_path.read_text(encoding="utf-8")
                    self.assertIn("common " + json.dumps(f"--repo_env=JAVA_HOME={java_home}"), configuration)
                    self.assertNotIn("--java_runtime_version", configuration)
                    self.assertNotIn("--tool_java_runtime_version", configuration)
                    self.assertIn("build --//bazel/coverage:runtime_enabled=true", configuration)
                    self.assertIn(json.dumps(f"--test_env=AOBUS_COVERAGE_SOURCE_ROOT={authored}"), configuration)
                    self.assertEqual(configuration.count("JAVA_HOME"), 1)
                    self.assertNotIn("JAVABIN", configuration)
                    self.assertNotIn("AOBUS_COVERAGE_ACTION_MAPPINGS", configuration)
                    self.assertNotIn("java_executable", configuration)
                    self.assertNotIn("action_source_mappings", configuration)
                    configurations.append(configuration)
                    action = next(item for item in argv[1:] if not item.startswith("-"))
                    if action != "query":
                        self.assertTrue(
                            {
                                "--combined_report=lcov",
                                "--test_timeout=300,600,1200,3600",
                                "--run_under=//bazel/coverage:coverage_support",
                                "--incompatible_bazel_test_exec_run_under",
                                "--coverage_output_generator=//bazel/coverage:merger",
                                "--coverage_report_generator=//bazel/coverage:merger",
                            }
                            <= set(argv)
                        )
                        self.assertEqual(argv.count("--combined_report=lcov"), 1)
                    commands.append(action)
                    stream = kwargs["stdout"]
                    if action == "query":
                        self.assertIn("tests(//:coverage_application_tests)", argv)
                        stream.write((labels.replace(" ", "\n") + "\n").encode())
                    elif action == "cquery":
                        self.assertIn(f"set({labels})", argv)
                        selected = {
                            "results": [
                                {
                                    "target": {
                                        "type": "RULE",
                                        "rule": {"name": "//test:fixture", "ruleClass": "cc_test"},
                                    },
                                    "configurationId": 1,
                                }
                            ],
                            "configurations": [{"id": 1, "checksum": checksum}],
                        }
                        if outcome in {"strategy-association", "malformed-second-result", "missing-config-result"}:
                            selected["results"].append(selected["results"][0] | {"configurationId": 2})
                            selected["configurations"].append({"id": 2, "checksum": checksum[:-1] + "e"})
                        if outcome == "strategy-association":
                            selected["results"].append(
                                {
                                    "target": {
                                        "type": "RULE",
                                        "rule": {"name": "//test:other", "ruleClass": "cc_test"},
                                    },
                                    "configurationId": 1,
                                }
                            )
                        stream.write(json.dumps(selected).encode())
                    elif action == "aquery":
                        self.assertIn(f"deps(set({labels}))", argv)
                        stream.write(json.dumps(actions).encode())
                    elif action == "coverage":
                        self.assertIn("//:coverage_application_tests", argv)
                        self.assertTrue(
                            {
                                "--nocache_test_results",
                                "--test_sharding_strategy=disabled",
                                "--runs_per_test=1",
                                "--flaky_test_attempts=1",
                                "--//bazel:asan_enabled=false",
                                "--//bazel:tsan_enabled=false",
                            }
                            <= set(argv)
                        )
                        bep = Path(
                            next(item.split("=", 1)[1] for item in argv if item.startswith("--build_event_json_file="))
                        )
                        fixture.environment.update(
                            AOBUS_COVERAGE_ATTEMPT=bep.parent.name,
                            AOBUS_COVERAGE_SOURCE_ROOT=str(authored),
                        )

                        def reader(argv, **kwargs):
                            result = fixture.reader(argv, **kwargs)
                            if outcome == "virtual-alias" and "cwd" in kwargs:
                                with (Path(kwargs["cwd"]) / "Value.cpp.gcov").open("a", encoding="utf-8") as report:
                                    report.write(f"file:{alias}\nlcount:2,7\n")
                            return result

                        fixture.collect(reader)
                        if outcome == "map-changed":
                            (fixture.output / "source-mappings.txt").write_text(
                                "changed standard mappings\n", encoding="utf-8"
                            )
                        if outcome == "source-changed":
                            (authored / "app/runtime/Value.cpp").write_text("changed\n", encoding="utf-8")
                        if outcome == "map-receipt-changed":
                            receipt_path = fixture.output / "receipt.json"
                            receipt = json.loads(receipt_path.read_text(encoding="utf-8"))
                            receipt["source_mappings"]["injected/Header.h"] = "lib/Injected.h"
                            collector.save(receipt_path, receipt)
                        archive = tree / "state/output/testlogs/fixture/test.outputs/outputs.zip"
                        archive.parent.mkdir(parents=True)
                        shutil.copyfile(fixture.archive(), archive)
                        started = time.time_ns() // 1_000_000 - 1000
                        execroot = tree / "state/output/execroot/_main"
                        raw = execroot / "bazel-out/_coverage/_coverage_report.dat"
                        raw.parent.mkdir(parents=True)
                        kwargs["stderr"].write(
                            (
                                f"INFO: LCOV coverage report is located at {raw}\n"
                                " and execpath is bazel-out/_coverage/_coverage_report.dat\n"
                            ).encode()
                        )
                        text = (fixture.output / "coverage.lcov").read_text(encoding="utf-8")
                        stock = archive.parent / "test.lcov"
                        reported = text.replace("SF:app/runtime/Value.cpp", f"SF:{fixture.source}")
                        if outcome == "virtual-alias":
                            reported = reported.replace("SF:lib/storage/detail/Failure.h", f"SF:{alias}")
                            text = text.replace("SF:lib/storage/detail/Failure.h", f"SF:{alias}")
                        stock.write_text(
                            reported.replace("DA:2,3", "DA:2,4") if outcome == "stock-disagreement" else reported,
                            encoding="utf-8",
                        )
                        # Bazel merges every instrumented target's zero-line
                        # baseline record into the combined report.
                        baseline = "TN:\nSF:lib/storage/Baseline.h\nFNF:0\nFNH:0\nLH:0\nLF:0\nend_of_record\n"
                        combined = text
                        if outcome == "disagreement":
                            combined = text.replace("DA:2,3", "DA:2,4")
                        elif outcome == "strategy-association":
                            combined = text.replace("DA:2,3", "DA:2,9")
                        raw.write_text(combined + baseline, encoding="utf-8")
                        identity = {"label": "//test:fixture", "configuration": {"id": checksum}}
                        events = [
                            {
                                "id": {"started": {}},
                                "started": {"startTimeMillis": str(started), "buildToolVersion": "9.2.0"},
                            },
                            {"id": {"workspace": {}}, "workspaceInfo": {"localExecRoot": str(execroot)}},
                            {
                                "id": {"testResult": identity | {"run": 1, "shard": 1, "attempt": 1}},
                                "testResult": {
                                    "status": "PASSED",
                                    "executionInfo": {"strategy": strategy},
                                    "testActionOutput": [
                                        {"name": "test.outputs__outputs.zip", "uri": archive.as_uri()},
                                        {"name": "test.lcov", "uri": stock.as_uri()},
                                    ],
                                },
                            },
                            {
                                "id": {"testSummary": identity},
                                "testSummary": {"overallStatus": "PASSED", "totalRunCount": 1, "totalNumCached": 0},
                            },
                            {
                                "id": {"buildFinished": {}},
                                "finished": {"finishTimeMillis": str(time.time_ns() // 1_000_000 + 1000)},
                            },
                            {
                                "id": {"buildMetrics": {}},
                                "buildMetrics": {
                                    "actionSummary": {
                                        "actionData": [{"mnemonic": "CoverageReport", "actionsExecuted": "2"}]
                                    }
                                },
                                "lastMessage": True,
                            },
                        ]
                        result = events[2]["testResult"]
                        if outcome == "missing-execution-info":
                            result.pop("executionInfo")
                        elif outcome == "missing-strategy":
                            result["executionInfo"] = {"cachedRemotely": False}
                        elif outcome == "malformed-execution-info":
                            result["executionInfo"] = None
                        elif outcome == "malformed-strategy":
                            result["executionInfo"] = {"strategy": ""}
                        elif outcome in {"strategy-association", "malformed-second-result", "missing-config-result"}:
                            other = identity | {"configuration": {"id": checksum[:-1] + "e"}}
                            if outcome != "missing-config-result":
                                # Result order differs from the sorted summary rows.
                                events.insert(
                                    2 if outcome == "strategy-association" else 3,
                                    {
                                        "id": {"testResult": other | {"run": 1, "shard": 1, "attempt": 1}},
                                        "testResult": result
                                        | {"executionInfo": {"strategy": "local" if successful else False}},
                                    },
                                )
                            events.insert(
                                4,
                                {
                                    "id": {"testSummary": other},
                                    "testSummary": {
                                        "overallStatus": "PASSED",
                                        "totalRunCount": 1,
                                        "totalNumCached": 0,
                                    },
                                },
                            )
                        if outcome == "strategy-association":
                            other_archive = archive.with_name("other-outputs.zip")
                            receipt_path = fixture.output / "receipt.json"
                            receipt = json.loads(receipt_path.read_text(encoding="utf-8"))
                            receipt["test"] = "//test:other"
                            collector.save(receipt_path, receipt)
                            shutil.copyfile(fixture.archive(), other_archive)
                            other_label = identity | {"label": "//test:other"}
                            events.insert(
                                2,
                                {
                                    "id": {"testResult": other_label | {"run": 1, "shard": 1, "attempt": 1}},
                                    "testResult": {
                                        "status": "PASSED",
                                        "executionInfo": {"strategy": "processwrapper-sandbox"},
                                        "testActionOutput": [
                                            {"name": "test.outputs__outputs.zip", "uri": other_archive.as_uri()},
                                            {"name": "test.lcov", "uri": stock.as_uri()},
                                        ],
                                    },
                                },
                            )
                            events.insert(
                                3,
                                {
                                    "id": {"testSummary": other_label},
                                    "testSummary": {
                                        "overallStatus": "PASSED",
                                        "totalRunCount": 1,
                                        "totalNumCached": 0,
                                    },
                                },
                            )
                        bep.write_text("\n".join(json.dumps(event) for event in events), encoding="utf-8")
                    else:
                        raise AssertionError(f"unexpected action: {argv}")
                    return subprocess.CompletedProcess(
                        argv, 3 if action == "coverage" and outcome == "native-failure" else 0
                    )

                collect = bazel_coverage._collection

                def checked_collection(*arguments, collect=collect, **keywords):
                    saved = json.loads((arguments[1].parent / "receipt.json").read_text(encoding="utf-8"))
                    self.assertIn("test_results", saved)
                    self.assertTrue(all("execution_strategy" in row for row in saved["test_results"]))
                    return collect(*arguments, **keywords)

                with (
                    mock.patch.object(bazel_coverage, "PROJECT_ROOT", authored),
                    mock.patch.object(
                        bazel_coverage,
                        "_compiler_identity",
                        return_value={
                            "schema": 1,
                            "files": [{"path": str(fixture.tool), "sha256": collector.digest(fixture.tool)}],
                        },
                    ),
                    mock.patch.object(bazel_coverage.native, "native_platform", return_value="linux"),
                    mock.patch.object(
                        bazel_coverage.bazel_workspace, "prepare_workspace", return_value=base
                    ) as prepare,
                    mock.patch.object(
                        bazel_coverage.bazel_workspace, "bazel_executable", return_value=Path("/fixture/bazel")
                    ),
                    mock.patch.object(
                        bazel_coverage.bazel_workspace, "_linux_server_javabase", return_value=server_java_home
                    ),
                    mock.patch.dict(os.environ, {"JAVA_HOME": "/personal/jdk", "JAVABIN": "/personal/java"}),
                    mock.patch(
                        "ao.command.test.virtual_gtk_displays",
                        return_value=contextlib.nullcontext([{"DISPLAY": ":fixture"}]),
                    ),
                    mock.patch(
                        "ao.core.bazel_test_services.private_pipewire",
                        return_value=contextlib.nullcontext({"AOBUS_REQUIRE_PIPEWIRE": "1"}),
                    ),
                    mock.patch.object(bazel_coverage.subprocess, "run", side_effect=process),
                    mock.patch.object(bazel_coverage, "_collection", side_effect=checked_collection) as collection,
                    contextlib.redirect_stdout(io.StringIO()),
                    contextlib.redirect_stderr(io.StringIO()),
                ):
                    self.assertEqual(bazel_coverage.run(args), int(not successful))
                    prepare.assert_called_once_with(tree, "debug")
                    self.assertEqual(os.environ["JAVA_HOME"], "/personal/jdk")
                    self.assertEqual(os.environ["JAVABIN"], "/personal/java")
                self.assertEqual(commands, ["query", "cquery", "aquery", "coverage"])
                self.assertEqual(len(configurations), 4)
                self.assertTrue(all(configuration == configurations[0] for configuration in configurations))
                receipts = list((tree / "coverage").glob("run-*/receipt.json"))
                self.assertEqual(len(receipts), 1)
                receipt = json.loads(receipts[0].read_text(encoding="utf-8"))
                self.assertEqual(receipt["status"], "passed" if successful else "failed")
                self.assertEqual(receipt["bazel_exit"], 3 if outcome == "native-failure" else 0)
                self.assertEqual(receipt["java_runtime"]["source_home"], str(server_java_home))
                self.assertEqual(receipt["java_runtime"]["home"], str(java_home))
                self.assertEqual(receipt["java_runtime"]["executable"], str(java))
                self.assertEqual(receipt["java_runtime"]["source_executable"], str(server_java_home / "bin/java"))
                self.assertEqual(receipt["java_runtime"]["sha256"], collector.digest(java))
                self.assertEqual(receipt["java_runtime"]["sha256"], collector.digest(server_java_home / "bin/java"))
                if os.name != "nt":
                    self.assertNotEqual(receipt["java_runtime"]["source_home"], receipt["java_runtime"]["home"])
                self.assertIn("not the complete JDK", receipt["java_runtime"]["identity_scope"])
                self.assertIn("Java compilation toolchain unchanged", receipt["java_runtime"]["selection"])
                retained = receipt["test_output_archives"]
                self.assertEqual(
                    len(retained), {"strategy-association": 3, "malformed-second-result": 2}.get(outcome, 1)
                )
                self.assertTrue(Path(retained[0]["retained"]).is_file())
                rc = (receipts[0].parent / "configuration/aobus.bazelrc").read_text(encoding="utf-8")
                self.assertIn("--test_env=AOBUS_REQUIRE_PIPEWIRE=1", rc)
                self.assertEqual((receipts[0].parent / "coverage.lcov").is_file(), successful)
                if successful:
                    expectations = receipt["source_mapping_expectations"]
                    self.assertNotIn("path", expectations)
                    self.assertEqual(expectations["aliases"], int(outcome == "virtual-alias"))
                    self.assertEqual(collector.digest(Path(expectations["aquery"])), expectations["aquery_sha256"])
                    self.assertEqual(len(receipt["stock_test_reports"]), 3 if outcome == "strategy-association" else 1)
                    self.assertTrue(receipt["strict_test_reports"][0]["stock_agreement"])
                    self.assertIn("stock_combined_report", receipt)
                    self.assertEqual((receipt["lines"], receipt["covered"]), (2, 1))
                    statistics = json.loads((receipts[0].parent / "sources.json").read_text(encoding="utf-8"))
                    self.assertEqual(
                        statistics,
                        {
                            "metric": "line",
                            "lines": 2,
                            "covered": 1,
                            "sources": [
                                {
                                    "source": "app/runtime/Value.cpp",
                                    "lines": 2,
                                    "covered": 1,
                                    "sha256": collector.digest(authored_source),
                                }
                            ],
                        },
                    )
                    self.assertEqual(
                        bazel_coverage.parse_lcov(Path(receipt["report"]).read_text(encoding="utf-8")),
                        {"app/runtime/Value.cpp": {1: 0, 2: 9 if outcome == "strategy-association" else 3}},
                    )
                elif outcome == "stock-disagreement":
                    self.assertIn("stock per-test LCOV disagrees", receipt["error"])
                if outcome in {
                    "malformed-execution-info",
                    "malformed-strategy",
                    "malformed-second-result",
                    "missing-config-result",
                }:
                    expected_error = (
                        "missing selected coverage test artifacts"
                        if outcome == "missing-config-result"
                        else "malformed coverage test execution"
                    )
                    self.assertIn(expected_error, receipt["error"])
                    self.assertNotIn("test_results", receipt)
                    self.assertNotIn("stock_test_reports", receipt)
                    collection.assert_not_called()
                elif successful or outcome in {"disagreement", "stock-disagreement", "map-receipt-changed"}:
                    expected_results = [
                        {
                            "label": "//test:fixture",
                            "configuration": checksum,
                            "status": "PASSED",
                            "runs": 1,
                            "cached": 0,
                            "execution_strategy": strategy,
                        }
                    ]
                    if outcome == "strategy-association":
                        expected_results.extend(
                            [
                                expected_results[0]
                                | {"configuration": checksum[:-1] + "e", "execution_strategy": "local"},
                                expected_results[0]
                                | {"label": "//test:other", "execution_strategy": "processwrapper-sandbox"},
                            ]
                        )
                    self.assertEqual(receipt["test_results"], expected_results)


# The pinned Bazel 9.2 producer serializes local artifact paths by carrying the
# native path's UTF-8 bytes as Latin-1 characters in its internal Java string
# and URI-quoting that string with the JDK constructor: URI delimiters and
# internal characters 0x00..0xA0 are percent-escaped as the character's own
# UTF-8 bytes, while 0xA1..0xFF stay raw. Native host paths are normalized to
# the PathConverter's posix form (forward slashes, leading "/") before the
# internal transform. This fixture encoder reproduces that wire for arbitrary
# temporary roots; CurrentArtifactTests pins it against literal goldens from
# the pinned JDK URI oracle and the raw wire captured from the actual failed
# Bazel 9.2 coverage run, never against the production decoder.
_BAZEL_URI_ALLOWED = frozenset("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789/:@&=+$,;-_.!~*'()")


def _internal(native):
    """The Bazel internal string for a native path: UTF-8 bytes as Latin-1."""
    return Path(native).as_posix().encode("utf-8").decode("latin-1")


def _bazel_uri(native):
    """The genuine Bazel 9.2 file-URI producer, distinct from raw execroots."""
    internal = _internal(native)
    if not internal.startswith("/"):
        internal = "/" + internal
    quoted = []
    for char in internal:
        if char in _BAZEL_URI_ALLOWED or "\xa1" <= char <= "\xff":
            quoted.append(char)
        else:
            quoted.extend(f"%{byte:02X}" for byte in char.encode("utf-8"))
    return "file://" + "".join(quoted)


class CoverageDiagnosticRetentionTests(unittest.TestCase):
    def _run(self, root, exit_status, *, tail='{"id":', interrupted=False, unrooted=False, unicode_archive=False):
        root = root.resolve()
        assert root.is_absolute() and root.is_relative_to(Path(tempfile.gettempdir()).resolve())
        tree = root / "tree"
        logs = (
            "coverage-覆盖-Résumé/test.outputs/outputs.zip" if unicode_archive else "fixture/test.outputs/outputs.zip"
        )
        archive = tree / "state/output/testlogs" / logs
        archive.parent.mkdir(parents=True)
        with zipfile.ZipFile(archive, "w") as output:
            output.writestr("aobus-coverage/profile-00000/object.gcda", b"completed profile fixture")
            output.writestr("aobus-coverage/receipt.json", '{"status":"passed"}')
        original_hash = collector.digest(archive)
        java_home = root / "selected JDK"
        java = java_home / "bin/java"
        java.parent.mkdir(parents=True)
        java.write_bytes(b"selected Java executable sentinel")
        jar = java_home / "lib/jrt-fs.jar"
        jar.parent.mkdir(parents=True)
        jar.write_bytes(b"Java runtime capability sentinel")
        base = root / "configuration"
        base.mkdir()
        (base / "aobus.bazelrc").write_text("build --config=debug\n", encoding="utf-8")
        (base / "request.json").write_text(
            json.dumps({"schema": 1, "host": "linux", "source": str(root), "prefix": None}), encoding="utf-8"
        )
        checksum = "e" * 64
        configured = {
            "results": [
                {
                    "target": {"type": "RULE", "rule": {"name": "//test:fixture", "ruleClass": "cc_test"}},
                    "configurationId": 1,
                }
            ],
            "configurations": [{"id": 1, "checksum": checksum}],
        }
        args = argparse.Namespace(
            path=str(tree),
            config="debug",
            jobs=2,
            remote_cache=None,
            vcpkg_prefix=None,
            filter=None,
            scope=[],
            coverage_suite="application",
        )
        invoke = bazel_coverage._Attempt.bazel

        def phase(attempt, name, executable, prepared, action, arguments, **kwargs):
            if action == "coverage":
                return invoke(attempt, name, executable, prepared, action, arguments, **kwargs)
            if action == "query" and arguments[0] == "tests(//:coverage_application_tests)":
                text = "//test:fixture\n"
            elif action == "cquery" and arguments[0] == "set(//test:fixture)":
                text = json.dumps(configured)
            elif action == "aquery" and arguments[0] == "deps(set(//test:fixture))":
                text = json.dumps(_actions())
            else:
                raise AssertionError(f"unexpected coverage fixture phase: {action} {arguments}")
            path = attempt.directory / f"{name}.json"
            path.write_text(text, encoding="utf-8")
            return path

        def process(argv, **kwargs):
            self.assertEqual(argv[0], str(Path("/fixture/bazel")))
            self.assertIn("coverage", argv)
            bep = Path(next(value.split("=", 1)[1] for value in argv if value.startswith("--build_event_json_file=")))
            wire = _bazel_uri(str(archive)) if unicode_archive else archive.as_uri()
            files = [{"name": "test.outputs__outputs.zip", "uri": wire}]
            if unrooted:
                outside = root / "outside-owned-tree.zip"
                outside.write_bytes(b"must not be consumed")
                files.insert(0, {"name": "other.outputs.zip", "uri": outside.as_uri()})
            completed = {
                "id": {"testResult": {"label": "//test:fixture", "configuration": {"id": checksum}}},
                "testResult": {"status": "PASSED", "testActionOutput": files},
            }
            started = {
                "id": {"started": {}},
                "started": {"startTimeMillis": "1", "buildToolVersion": "9.2.0"},
            }
            bep.write_text(
                "\n".join(json.dumps(event) for event in (started, completed)) + "\n" + tail, encoding="utf-8"
            )
            if interrupted:
                raise KeyboardInterrupt
            return subprocess.CompletedProcess(argv, exit_status)

        with (
            mock.patch.object(bazel_coverage, "PROJECT_ROOT", root),
            mock.patch.object(bazel_coverage, "_compiler_identity", return_value={"schema": 1, "files": []}),
            mock.patch.object(bazel_coverage.native, "native_platform", return_value="linux"),
            mock.patch.object(bazel_coverage.bazel_workspace, "prepare_workspace", return_value=base),
            mock.patch.object(bazel_coverage.bazel_workspace, "bazel_executable", return_value=Path("/fixture/bazel")),
            mock.patch.object(bazel_coverage.bazel_workspace, "_linux_server_javabase", return_value=java_home),
            mock.patch("ao.command.test.virtual_gtk_displays", return_value=contextlib.nullcontext([{}])),
            mock.patch("ao.core.bazel_test_services.private_pipewire", return_value=contextlib.nullcontext({})),
            mock.patch.object(bazel_coverage._Attempt, "bazel", new=phase),
            mock.patch.object(bazel_coverage.subprocess, "run", side_effect=process),
            mock.patch.object(bazel_coverage, "_retain_archives", wraps=bazel_coverage._retain_archives) as retain,
            mock.patch.object(bazel_coverage, "_events", wraps=bazel_coverage._events) as parse,
            mock.patch.object(bazel_coverage.bazel_check, "_test_results") as results,
            mock.patch.object(bazel_coverage, "_collection") as collection,
            mock.patch.object(bazel_coverage.zipfile, "ZipFile") as consume_archive,
            contextlib.redirect_stdout(io.StringIO()),
            contextlib.redirect_stderr(io.StringIO()),
        ):
            self.assertEqual(bazel_coverage.run(args), 130 if interrupted else 1)
            results.assert_not_called()
            collection.assert_not_called()
            consume_archive.assert_not_called()
            self.assertEqual(retain.call_count, 0 if interrupted else 1)
            self.assertEqual(parse.call_count, 0 if interrupted else 1)
        receipts = list((tree / "coverage").glob("run-*/receipt.json"))
        self.assertEqual(len(receipts), 1)
        directory = receipts[0].parent
        self.assertFalse((directory / "coverage.lcov").exists())
        self.assertTrue(tree.is_dir())
        return json.loads(receipts[0].read_text(encoding="utf-8")), directory, original_hash

    def test_nonzero_exit_survives_bad_bep_and_rejected_unrooted_archive(self):
        with tempfile.TemporaryDirectory(prefix="aobus-coverage-") as temporary:
            receipt, _, original_hash = self._run(Path(temporary), 3, unrooted=True)
            self.assertEqual(receipt["bazel_exit"], 3)
            self.assertEqual(receipt["status"], "failed")
            self.assertIn("Bazel coverage exited 3", receipt["error"])
            self.assertTrue(receipt["bep_error"])
            retention = receipt["artifact_retention"]
            self.assertEqual(retention["status"], "diagnostic-only")
            self.assertEqual(len(retention["archives"]), 1)
            self.assertEqual(retention["archives"][0]["sha256"], original_hash)
            self.assertTrue(any("outside the owned tree" in error for error in retention["errors"]))

    def test_interrupted_unknown_writer_never_consumes_bep_or_archives(self):
        with tempfile.TemporaryDirectory(prefix="aobus-coverage-") as temporary:
            receipt, directory, _ = self._run(Path(temporary), None, interrupted=True)
            self.assertEqual(receipt["status"], "interrupted")
            self.assertEqual(receipt["exit"], 130)
            self.assertEqual(receipt["writer_state"], "unknown")
            self.assertIsNone(receipt["phases"][-1]["exit"])
            self.assertEqual(receipt["artifact_retention"]["status"], "not-attempted")
            self.assertIn("owner confirmation", receipt["retry_requires"])
            self.assertEqual(list(directory.glob("test-output-*.zip")), [])


class CurrentArtifactTests(unittest.TestCase):
    def test_local_file_uri_round_trips_native_paths_without_double_decoding(self):
        with tempfile.TemporaryDirectory(prefix="aobus-coverage-") as temporary:
            root = Path(temporary)
            path = root / "report %20 ü.dat"
            path.write_text("current local artifact", encoding="utf-8")
            # The genuine Bazel 9.2 producer wire carries the internal string's
            # raw Latin-1 characters and quotes the literal percent as %25:
            # unquoting once preserves the literal "%20" text of the native name.
            self.assertEqual(bazel_coverage._artifact({"uri": _bazel_uri(str(path))}, root=root), path)
            # A standard UTF-8 percent-encoded URI is a different producer's
            # spelling of the same native path and is not admitted.
            with self.assertRaises(ValueError):
                bazel_coverage._artifact({"uri": path.as_uri()}, root=root)
            for uri in ("https://example.invalid/report.dat", "file://remote/report.dat", "file:relative.dat"):
                with self.subTest(uri=uri), self.assertRaisesRegex(ValueError, "current local BEP"):
                    bazel_coverage._artifact({"uri": uri})

    def test_combined_report_is_bound_to_this_invocations_declaration_action_and_time_window(self):
        def invocation(tree, start, finish, *, executed=2, extra=()):
            execroot = tree / "state/output/execroot/_main"
            return [
                {"id": {"started": {}}, "started": {"startTimeMillis": str(start)}},
                {"id": {"workspace": {}}, "workspaceInfo": {"localExecRoot": str(execroot)}},
                {"id": {"namedSet": {"id": "0"}}, "namedSetOfFiles": {"files": []}},
                *extra,
                {"id": {"buildFinished": {}}, "finished": {"finishTimeMillis": str(finish)}},
                {
                    "id": {"buildMetrics": {}},
                    "buildMetrics": {
                        "actionSummary": {"actionData": [{"mnemonic": "CoverageReport", "actionsExecuted": executed}]}
                    },
                    "lastMessage": True,
                },
            ]

        def declaration(path, execpath="bazel-out/_coverage/_coverage_report.dat"):
            return f"INFO: LCOV coverage report is located at {path}\n and execpath is {execpath}\n"

        with tempfile.TemporaryDirectory(prefix="aobus-coverage-") as temporary:
            tree = Path(temporary) / "tree"
            report = tree / "state/output/execroot/_main/bazel-out/_coverage/_coverage_report.dat"
            baseline = report.with_name("_baseline_report.dat")
            report.parent.mkdir(parents=True)
            report.write_text("current", encoding="utf-8")
            baseline.write_text("baseline", encoding="utf-8")
            written = report.stat().st_mtime_ns // 1_000_000
            diagnostics = Path(temporary) / "coverage.stderr.log"
            current = declaration(baseline, "bazel-out/_coverage/_baseline_report.dat") + declaration(report)
            diagnostics.write_text("progress\n" + current + "INFO: Build completed\n", encoding="utf-8")
            events = invocation(tree, written - 5, written + 5)
            self.assertEqual(bazel_coverage._combined_artifact(events, diagnostics, tree), report)

            # A stale report from an earlier invocation, an undeclared or
            # duplicated declaration, an unexecuted report action, a path
            # outside the owned tree, or ambiguous BEP framing never pass.
            failures = {
                "stale": (invocation(tree, written + 1, written + 9), current),
                "late": (invocation(tree, written - 9, written - 1), current),
                "undeclared": (events, declaration(baseline, "bazel-out/_coverage/_baseline_report.dat")),
                "duplicate": (events, current + declaration(report)),
                "other-path": (events, declaration(tree / "other/_coverage_report.dat")),
                "not-executed": (invocation(tree, written - 5, written + 5, executed=0), current),
                "outside-tree": (invocation(Path(temporary) / "foreign", written - 5, written + 5), current),
                "second-workspace": (
                    invocation(
                        tree,
                        written - 5,
                        written + 5,
                        extra=[{"id": {"workspace": {}}, "workspaceInfo": {"localExecRoot": "/elsewhere"}}],
                    ),
                    current,
                ),
            }
            for case, (case_events, text) in failures.items():
                with self.subTest(case=case):
                    diagnostics.write_text(text, encoding="utf-8")
                    with self.assertRaises(ValueError):
                        bazel_coverage._combined_artifact(case_events, diagnostics, tree)
            diagnostics.write_text(current, encoding="utf-8")
            report.unlink()
            with self.assertRaisesRegex(ValueError, "missing or outside"):
                bazel_coverage._combined_artifact(events, diagnostics, tree)

    def test_artifact_decodes_the_actual_raw_mojibake_wire_to_the_native_unicode_path(self):
        # The actual failed run's testActionOutput wire mixes raw Latin-1
        # characters with %C2%XX escapes for C1 controls. Decoding resolves the
        # native Unicode path and never falls back to the literal mojibake
        # spelling, even when a file with that exact name exists adjacent.
        with tempfile.TemporaryDirectory(prefix="aobus-coverage-") as temporary:
            root = Path(temporary)
            relative = (
                "coverage-覆盖-Résumé/state/output/execroot/_main"
                "/bazel-out/k8-dbg/testlogs/test/async_test/test.outputs/outputs.zip"
            )
            path = root / relative
            path.parent.mkdir(parents=True)
            path.write_bytes(b"current local unicode artifact")
            namesake = Path(_internal(str(path)))
            namesake.parent.mkdir(parents=True, exist_ok=True)
            namesake.write_bytes(b"mojibake namesake decoy")
            wire = _bazel_uri(str(path))
            self.assertIn("è¦%C2%86", wire)
            self.assertEqual(bazel_coverage._artifact({"uri": wire}, root=root), path)
            path.unlink()
            with self.assertRaisesRegex(ValueError, "missing BEP coverage artifact"):
                bazel_coverage._artifact({"uri": wire}, root=root)
