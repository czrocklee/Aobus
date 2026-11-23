"""Coverage regressions for wrong or silently incomplete line measurements."""

import contextlib
import gzip
import importlib.util
import io
import json
import os
import subprocess
import tempfile
import unittest
import zipfile
from pathlib import Path
from unittest import mock

from ao.core.bazel_native import BazelWorkspaceError
from ao.core.paths import PROJECT_ROOT
from ao.portal import bazel_attempt, bazel_coverage, bazel_test_ownership, build_env
from ao.portal.measurements import CoverageRequest

_SPEC = importlib.util.spec_from_file_location("aobus_coverage_collector", PROJECT_ROOT / "bazel/coverage/collect.py")
assert _SPEC is not None and _SPEC.loader is not None
collector = importlib.util.module_from_spec(_SPEC)
_SPEC.loader.exec_module(collector)

# Exact LLVM B11* bytes retained from a native zero-function translation unit.
_ZERO_FUNCTION_NOTES = bytes.fromhex("6f6e6367 2a313142 ffffffff 01000000 2e000000 00000000 00000000 00000000")
_ZERO_FUNCTION_PROFILE = bytes.fromhex(
    "61646367 2a313142 ffffffff 000000a1 02000000 01000000 00000000 00000000 00000000"
)


def _file(root, name, text):
    path = root / name
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text, encoding="utf-8")
    return path


class CollectionFixture:
    def __init__(self, root):
        self.root = root / "execroot"
        self.coverage = root / "coverage"
        self.coverage.mkdir()
        self.output = root / "outputs/aobus-coverage"
        self.output.mkdir(parents=True)
        self.tool = _file(root, "llvm-cov", "selected LLVM reader")
        self.manifest = self.root / "manifest.txt"
        self.source = _file(self.root, "app/runtime/Value.cpp", "int value() {\n  return 3;\n}\n")
        self.environment = {
            "AOBUS_COVERAGE_ATTEMPT": "run-fixture",
            "AOBUS_COVERAGE_COMPILER": "clang",
            "TEST_TARGET": "//test:fixture",
        }
        self.notes = []

    def add(self, name, *, data=True):
        relative = Path("bazel-out/dbg/bin/test/_objs/fixture") / (name + ".gcno")
        notes = self.root / relative
        notes.parent.mkdir(parents=True, exist_ok=True)
        notes.write_bytes(b"oncg408*samenotes")
        self.notes.append(relative.as_posix())
        self.manifest.write_text("\n".join(["app/runtime/Value.cpp", *self.notes]) + "\n", encoding="utf-8")
        profile = self.coverage / self.root.relative_to(self.root.anchor) / relative.with_suffix(".gcda")
        if data:
            profile.parent.mkdir(parents=True, exist_ok=True)
            profile.write_bytes(b"adcg408*samecounters")
        return notes, profile

    def reader(self, argv, **kwargs):
        if argv == [str(self.tool), "gcov", "--version"]:
            return subprocess.CompletedProcess(argv, 0, "LLVM version 22.1.8\n", "")
        pair = Path(kwargs["cwd"])
        if argv != [str(self.tool), "gcov", "-i", "-o", str(pair), str(pair / "object.gcda")]:
            raise AssertionError(f"unexpected gcov command: {argv}")
        _file(pair, "Value.cpp.gcov", "file:app/runtime/Value.cpp\nfunction:1,2,value\nlcount:1,0\nlcount:2,3\n")
        return subprocess.CompletedProcess(argv, 0, "", "")

    def collect(self, reader=None):
        with mock.patch.object(collector.subprocess, "run", side_effect=reader or self.reader):
            collector.collect(self.root, self.coverage, self.manifest, self.tool, self.output, self.environment)
        return bazel_coverage.parse_lcov((self.output / "coverage.lcov").read_text(encoding="utf-8"))

    def archive(self, path):
        _file(self.output, "test.json", json.dumps({"attempt": self.environment["AOBUS_COVERAGE_ATTEMPT"], "exit": 0}))
        path.parent.mkdir(parents=True, exist_ok=True)
        with zipfile.ZipFile(path, "w") as archive:
            for source in self.output.iterdir():
                entries = source.rglob("*") if source.is_dir() else (source,)
                for entry in entries:
                    if entry.is_file():
                        archive.write(entry, entry.relative_to(self.output.parent))


class StrictCollectionTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix="aobus-coverage-")
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name).resolve()
        self.fixture = CollectionFixture(self.root)

    def test_generated_gperf_locations_do_not_count_as_authored_lines(self):
        # GCC attributed generated lookup code outside the DSL's line bounds.
        dsl = "lib/media/file/mp4/AtomDispatch.gperf"
        _file(self.fixture.root, dsl, "entry\n" * 32)
        document = {
            "format_version": "2",
            "files": [
                {"file": dsl, "lines": [{"line_number": 64, "count": 0}]},
                {"file": "app/runtime/Value.cpp", "lines": [{"line_number": 2, "count": 3}]},
            ],
        }
        data, identities = collector.gcov_json(json.dumps(document), self.fixture.root, {})
        self.assertEqual(
            bazel_coverage.production_report([data], identities, self.fixture.root), {"app/runtime/Value.cpp": {2: 3}}
        )

    def test_gcc_parent_relative_header_spelling_keeps_the_authored_line(self):
        # CompletionService includes completion/../TrackField.h; gcov kept that spelling.
        reported = "app/include/ao/rt/completion/../TrackField.h"
        canonical = "app/include/ao/rt/TrackField.h"
        _file(self.fixture.root, canonical, "inline int field() { return 0; }\n")
        document = {"format_version": "2", "files": [{"file": reported, "lines": [{"line_number": 1, "count": 0}]}]}
        strict, identities = collector.gcov_json(json.dumps(document), self.fixture.root, {})
        stock = bazel_coverage.parse_lcov(f"SF:{reported}\nDA:1,0\nend_of_record\n")
        expected = {canonical: {1: 0}}
        self.assertEqual(strict, expected)
        self.assertEqual(
            bazel_coverage.production_report([stock], identities, self.fixture.root, normalize_authored_parents=True),
            expected,
        )

    def test_gcc_zero_hit_header_line_survives_merging_two_profiles(self):
        fixture = self.fixture
        fixture.tool = _file(self.root, "gcov", "selected GCC reader")
        fixture.environment["AOBUS_COVERAGE_COMPILER"] = "gcc"
        _file(fixture.root, "include/Value.h", "inline int value(int n) {\n  if (n) return 1;\n  return 0;\n}\n")
        alias = "bazel-out/dbg/bin/_virtual_includes/value/Value.h"
        standard = _file(self.root, "standard-mappings.txt", f"{alias}:include/Value.h\n")
        fixture.environment["COVERAGE_REPORTED_TO_ACTUAL_SOURCES_FILE"] = str(standard)
        for name in ("first", "second"):
            notes, profile = fixture.add(name)
            notes.write_bytes(bytes.fromhex("6f6e6367 2a323642 ffffffff") + b"notes")
            profile.write_bytes(bytes.fromhex("61646367 2a323642 ffffffff") + b"counters")

        def reader(argv, **kwargs):
            if argv == [str(fixture.tool), "--version"]:
                return subprocess.CompletedProcess(argv, 0, "gcov (GCC) 16.2.0\nJSON format version: 2\n", "")
            pair = Path(kwargs["cwd"])
            self.assertEqual(argv, [str(fixture.tool), "--json-format", "-o", str(pair), str(pair / "object.gcda")])
            report = {
                "format_version": "2",
                "files": [{"file": alias, "lines": [{"line_number": 2, "count": 1}, {"line_number": 3, "count": 0}]}],
            }
            (pair / "object.gcov.json.gz").write_bytes(gzip.compress(json.dumps(report).encode()))
            return subprocess.CompletedProcess(argv, 0, "", "")

        self.assertEqual(fixture.collect(reader), {"include/Value.h": {2: 2, 3: 0}})

    def test_unlinked_archive_notes_do_not_block_an_observed_profile(self):
        self.fixture.add("linked")
        self.fixture.add("unlinked", data=False)
        self.assertEqual(self.fixture.collect(), {"app/runtime/Value.cpp": {1: 0, 2: 3}})

    def test_zero_function_profile_does_not_discard_the_measured_profile(self):
        fixture = self.fixture
        fixture.add("a-measured")
        notes, profile = fixture.add("b-zero-functions")
        notes.write_bytes(_ZERO_FUNCTION_NOTES)
        profile.write_bytes(_ZERO_FUNCTION_PROFILE)

        def reader(argv, **kwargs):
            result = fixture.reader(argv, **kwargs)
            if "profile-00001" in str(kwargs.get("cwd", "")):
                (Path(kwargs["cwd"]) / "Value.cpp.gcov").write_bytes(b"")
            return result

        self.assertEqual(fixture.collect(reader), {"app/runtime/Value.cpp": {1: 0, 2: 3}})
        self.assertEqual(
            json.loads((fixture.output / "receipt.json").read_text(encoding="utf-8"))["consumed_profiles"], 2
        )

    def test_missing_notes_for_an_observed_counter_do_not_publish_partial_coverage(self):
        self.fixture.add("good")
        (self.fixture.coverage / "unlisted.gcda").write_bytes(b"adcg408*samecounter")
        with self.assertRaises(ValueError):
            self.fixture.collect()
        self.assertFalse((self.fixture.output / "coverage.lcov").exists())

    def test_nonzero_extraction_does_not_publish_partial_coverage(self):
        fixture = self.fixture
        fixture.add("a-good")
        fixture.add("b-bad")

        def reader(argv, **kwargs):
            result = fixture.reader(argv, **kwargs)
            if "profile-00001" in str(kwargs.get("cwd", "")):
                return subprocess.CompletedProcess(argv, 7, result.stdout, "bad counters")
            return result

        with self.assertRaises(ValueError):
            fixture.collect(reader)
        self.assertFalse((fixture.output / "coverage.lcov").exists())

    def test_no_current_profiles_do_not_publish_empty_coverage(self):
        self.fixture.add("unlinked", data=False)
        with self.assertRaises(ValueError):
            self.fixture.collect()
        self.assertFalse((self.fixture.output / "coverage.lcov").exists())

    def test_synthetic_function_location_zero_is_not_an_executable_line(self):
        text = "file:app/runtime/Value.cpp\nfunction:0,1,__cxx_global_var_init\nlcount:2,3\n"
        data, _ = collector.intermediate(text, self.fixture.root, {})
        self.assertEqual(data, {"app/runtime/Value.cpp": {2: 3}})

    def test_standard_map_keeps_two_same_named_headers_separate(self):
        fixture = self.fixture
        fixture.add("linked")
        for path in ("include/one/Value.h", "include/two/Value.h"):
            _file(fixture.root, path, "inline int value() { return 3; }\n")
        alias = "bazel-out/dbg/bin/_virtual_includes/one/Value.h"
        generated = "bazel-out/dbg/bin/app/codegen/Tags.h"
        standard = _file(self.root, "standard-mappings.txt", f"{alias}:include/one/Value.h\n")
        fixture.environment["COVERAGE_REPORTED_TO_ACTUAL_SOURCES_FILE"] = str(standard)

        def reader(argv, **kwargs):
            result = fixture.reader(argv, **kwargs)
            if "cwd" in kwargs:
                _file(
                    Path(kwargs["cwd"]),
                    "Value.cpp.gcov",
                    f"file:{alias}\nlcount:1,2\nfile:include/two/Value.h\nlcount:1,0\nfile:{generated}\nlcount:7000,1\n",
                )
            return result

        fixture.collect(reader)
        archive = self.root / "outputs.zip"
        fixture.archive(archive)
        data, sources = bazel_coverage._collection(archive, self.root / "retained", "run-fixture", "//test:fixture")
        self.assertEqual(
            bazel_coverage.production_report([data], sources, fixture.root),
            {"include/one/Value.h": {1: 2}, "include/two/Value.h": {1: 0}},
        )


class CoverageRunnerTests(unittest.TestCase):
    def test_run_under_preserves_runfiles_layout_and_test_exit(self):
        with tempfile.TemporaryDirectory(prefix="aobus-coverage-") as temporary:
            root = Path(temporary).resolve()
            fixture = CollectionFixture(root)
            notes, _ = fixture.add("linked", data=False)
            profile = fixture.coverage / notes.relative_to(fixture.root).with_suffix(".gcda")
            fixture.output = root / "fresh-outputs/aobus-coverage"
            fixture.output.parent.mkdir()
            runfiles = root / "runfiles/_main"
            runfiles.mkdir(parents=True)
            destination = _file(fixture.root, "coverage.dat", "stock outer owns this output")
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
            }
            arguments = ["test/fixture", "positional-test-argument"]
            calls = []

            def process(argv, **kwargs):
                if argv != arguments:
                    return fixture.reader(argv, **kwargs)
                calls.append(argv)
                self.assertEqual(kwargs["cwd"], runfiles)
                self.assertEqual(kwargs["env"]["GCOV_PREFIX"], str(fixture.coverage))
                profile.parent.mkdir(parents=True)
                profile.write_bytes(b"adcg408*samecounters")
                return subprocess.CompletedProcess(argv, 3)

            with (
                mock.patch.dict(os.environ, environment, clear=True),
                mock.patch.object(collector.Path, "cwd", return_value=runfiles),
                mock.patch.object(collector.subprocess, "run", side_effect=process),
            ):
                self.assertEqual(collector.main(arguments), 3)
            self.assertEqual(calls, [arguments])
            self.assertEqual(destination.read_text(encoding="utf-8"), "stock outer owns this output")
            self.assertTrue((fixture.output / "coverage.lcov").is_file())


class ProductionReportTests(unittest.TestCase):
    def test_lcov_accumulates_duplicate_line_and_source_records_without_losing_zero_hits(self):
        text = (
            "SF:lib/audio/Ctor.cpp\nDA:5,2\nDA:5,3\nDA:7,0\nLF:2\nLH:1\nend_of_record\n"
            "SF:lib/audio/Ctor.cpp\nDA:7,4\nDA:9,0\nLF:2\nLH:1\nend_of_record\n"
        )
        self.assertEqual(bazel_coverage.parse_lcov(text), {"lib/audio/Ctor.cpp": {5: 5, 7: 4, 9: 0}})

    def test_scope_retains_zero_hits_and_does_not_match_a_sibling_prefix(self):
        with tempfile.TemporaryDirectory(prefix="aobus-coverage-") as temporary:
            root = Path(temporary)
            report = {"app/runtime/Foo.cpp": {1: 1, 2: 0}, "app/runtime-tools/Bar.cpp": {1: 2}}
            for name in report:
                _file(root, name, "one\ntwo\n")
            identities = bazel_coverage._source_identity(root)
            selected = bazel_coverage.production_report([report], identities, root, ["app/runtime/"])
            self.assertEqual(selected, {"app/runtime/Foo.cpp": {1: 1, 2: 0}})

    def test_empty_production_data_does_not_claim_full_coverage(self):
        with tempfile.TemporaryDirectory(prefix="aobus-coverage-") as temporary:
            with self.assertRaises(ValueError):
                bazel_coverage.production_report([{"test/FooTest.cpp": {1: 1}}], {}, Path(temporary))

    def test_header_lines_hit_merge_across_suites(self):
        with tempfile.TemporaryDirectory(prefix="aobus-coverage-") as temporary:
            root = Path(temporary)
            header = _file(root, "include/Value.h", "one\ntwo\nthree\n")
            reports = [
                {"include/Value.h": {1: 0, 2: 2}, "test/ValueTest.cpp": {1: 8}},
                {"include/Value.h": {1: 5, 2: 1, 3: 0}},
            ]
            merged = bazel_coverage.production_report(reports, {"include/Value.h": collector.digest(header)}, root)
            self.assertEqual(merged, {"include/Value.h": {1: 5, 2: 3, 3: 0}})

    def test_every_requested_scope_needs_measured_lines(self):
        with tempfile.TemporaryDirectory(prefix="aobus-coverage-") as temporary:
            root = Path(temporary)
            header = _file(root, "include/Value.h", "one\n")
            with self.assertRaises(ValueError):
                bazel_coverage.production_report(
                    [{"include/Value.h": {1: 3}}],
                    {"include/Value.h": collector.digest(header)},
                    root,
                    ["include", "app/runtime"],
                )

    def test_lcov_function_only_baseline_does_not_add_lines(self):
        zero = "SF:app/Value.h\nFN:0,__cxx_global_var_init\nFNDA:3,__cxx_global_var_init\nLH:0\nLF:0\nend_of_record\n"
        measured = "SF:app/Value.cpp\nDA:1,3\nend_of_record\n"
        self.assertEqual(bazel_coverage.parse_lcov(zero + measured), {"app/Value.h": {}, "app/Value.cpp": {1: 3}})


class CoverageAdmissionTests(unittest.TestCase):
    def test_invalid_report_scope_is_rejected_during_cli_preflight(self):
        with (
            mock.patch.object(bazel_coverage.native, "native_platform", return_value="linux"),
            contextlib.redirect_stderr(io.StringIO()),
            self.assertRaises(SystemExit) as failure,
        ):
            build_env.requires_build_env("measure", ["coverage", "--scope", "lib/../test"])
        self.assertEqual(failure.exception.code, 1)

    def test_analysis_rejects_a_report_scope(self):
        with mock.patch.object(bazel_coverage.native, "native_platform", return_value="linux"):
            with self.assertRaises(BazelWorkspaceError):
                bazel_coverage.validate_options(CoverageRequest(analysis_only=True, scope=("lib",)))

    def test_child_environment_blocks_ambient_profile_and_rc_redirection(self):
        hostile = {
            "LLVM_PROFILE_FILE": "/personal/out",
            "GCOV_PREFIX": "/personal/data",
            "GCOV_PREFIX_STRIP": "99",
            "IGNORE_COVERAGE_COLLECTION_FAILURES": "1",
            "bAzElRc": "/personal/config.bazelrc",
            "JAVA_HOME": "/personal/jdk",
            "JAVABIN": "/personal/java",
            "KEEP": "value",
        }
        home = Path("/owned/selected-jdk")
        with mock.patch.dict(os.environ, hostile, clear=True):
            parent = dict(os.environ)
            self.assertEqual(
                bazel_coverage._environment(home),
                {"KEEP": "value", "JAVA_HOME": str(home), "JAVABIN": str(home / "bin/java")},
            )
            self.assertEqual(dict(os.environ), parent)

    def test_analysis_attempt_does_not_launch_coverage(self):
        with tempfile.TemporaryDirectory(prefix="aobus-coverage-") as temporary:
            root = Path(temporary).resolve()
            attempt = bazel_coverage._Attempt(root, CoverageRequest(analysis_only=True))
            attempt.java_home = attempt.server_java_home = root
            with (
                mock.patch.object(
                    bazel_coverage.bazel_workspace, "invocation", return_value=["bazel", f"--server_javabase={root}"]
                ),
                mock.patch.object(bazel_coverage.subprocess, "run") as process,
                self.assertRaises(BazelWorkspaceError),
            ):
                attempt.bazel("coverage", Path("bazel"), root, "coverage", [])
            process.assert_not_called()


class CurrentArtifactTests(unittest.TestCase):
    def test_artifact_uri_must_be_canonical_and_stay_inside_the_selected_tree(self):
        with tempfile.TemporaryDirectory(prefix="aobus-coverage-") as temporary:
            parent = Path(temporary).resolve()
            root = parent / "owned"
            inside = _file(root, "artifact.txt", "inside")
            outside = _file(parent, "outside.txt", "outside")
            self.assertEqual(bazel_attempt.test_output_file({"uri": inside.as_uri()}, root=root), inside)
            self.assertEqual(bazel_attempt.test_output_file({"uri": outside.as_uri()}), outside)
            with self.assertRaisesRegex(ValueError, "outside the owned tree"):
                bazel_attempt.test_output_file({"uri": outside.as_uri()}, root=root)
            with self.assertRaisesRegex(ValueError, "noncanonical"):
                bazel_attempt.test_output_file(
                    {"uri": inside.as_uri().replace("artifact.txt", "%61rtifact.txt")}, root=root
                )
            traversal = root.as_uri() + "/../outside.txt"
            with self.assertRaisesRegex(ValueError, "outside the owned tree"):
                bazel_attempt.test_output_file({"uri": traversal}, root=root)

    def test_missing_second_configuration_is_not_covered_by_first_result(self):
        with tempfile.TemporaryDirectory(prefix="aobus-coverage-") as temporary:
            root = Path(temporary).resolve()
            archive = _file(root, "outputs.zip", "fixture")
            stock = _file(root, "test.lcov", "fixture")
            label = "//test:fixture"
            event = {
                "id": {
                    "testResult": {"label": label, "configuration": {"id": "one"}, "run": 1, "shard": 1, "attempt": 1}
                },
                "testResult": {
                    "status": "PASSED",
                    "testActionOutput": [
                        {"name": archive.name, "uri": archive.as_uri()},
                        {"name": stock.name, "uri": stock.as_uri()},
                    ],
                },
            }
            first = bazel_test_ownership.ConfiguredTarget(label, "one")
            other = bazel_test_ownership.ConfiguredTarget(label, "two")
            self.assertEqual(
                bazel_coverage._test_artifacts([event], frozenset({first}), root=root)[first][:2], (archive, stock)
            )
            with self.assertRaises(ValueError):
                bazel_coverage._test_artifacts([event], frozenset({first, other}), root=root)

    def test_stale_combined_report_is_not_used_for_a_new_invocation(self):
        with tempfile.TemporaryDirectory(prefix="aobus-coverage-") as temporary:
            tree = Path(temporary).resolve()
            execroot = tree / "state/output/execroot/_main"
            relative = "bazel-out/_coverage/_coverage_report.dat"
            report = _file(execroot, relative, "retained report")
            written = report.stat().st_mtime_ns // 1_000_000
            diagnostics = _file(
                tree,
                "coverage.stderr.log",
                f"INFO: LCOV coverage report is located at {report}\n and execpath is {relative}\n",
            )
            events = [
                {"id": {"started": {}}, "started": {"startTimeMillis": str(written - 5000)}},
                {"id": {"workspace": {}}, "workspaceInfo": {"localExecRoot": str(execroot)}},
                {"id": {"buildFinished": {}}, "finished": {"finishTimeMillis": str(written + 5000)}},
                {
                    "buildMetrics": {
                        "actionSummary": {"actionData": [{"mnemonic": "CoverageReport", "actionsExecuted": 1}]}
                    }
                },
            ]
            self.assertEqual(bazel_coverage._combined_artifact(events, diagnostics, tree), report)
            events[0]["started"]["startTimeMillis"] = str(written + 1)
            with self.assertRaises(ValueError):
                bazel_coverage._combined_artifact(events, diagnostics, tree)

    def test_bazel_92_unicode_artifact_decodes_to_native_path_once(self):
        with tempfile.TemporaryDirectory(prefix="aobus-coverage-") as temporary:
            root = Path(temporary).resolve()
            path = _file(root, "coverage-覆盖-Résumé %20/outputs.zip", "current archive")
            # Bazel 9.2's captured wire mixes raw Latin-1 with escapes for C1 bytes.
            wire = root.as_uri() + "/coverage-è¦%C2%86ç%C2%9B%C2%96-RÃ©sumÃ©%20%2520/outputs.zip"
            self.assertEqual(bazel_attempt.test_output_file({"uri": wire}, root=root), path)

    def test_partial_bep_retains_completed_archive_without_proving_success(self):
        with tempfile.TemporaryDirectory(prefix="aobus-coverage-") as temporary:
            root = Path(temporary).resolve()
            archive = _file(root, "testlogs/outputs.zip", "diagnostic archive")
            attempt = root / "attempt"
            attempt.mkdir()
            label = "//test:fixture"
            expected = frozenset({bazel_test_ownership.ConfiguredTarget(label, "one")})
            events = [
                {"id": {"started": {}}, "started": {"buildToolVersion": "9.2.0"}},
                {
                    "id": {"testResult": {"label": label, "configuration": {"id": "one"}}},
                    "testResult": {"testActionOutput": [{"name": archive.name, "uri": archive.as_uri()}]},
                },
            ]
            bep = _file(attempt, "coverage.bep.json", "\n".join(json.dumps(event) for event in events) + '\n{"id":')
            retention = bazel_coverage._retain_archives(bep, attempt, root, expected)
            self.assertEqual(
                [Path(row["retained"]).read_bytes() for row in retention["archives"]], [archive.read_bytes()]
            )
            with self.assertRaises(ValueError):
                bazel_coverage._events(bep)
