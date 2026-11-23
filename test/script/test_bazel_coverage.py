"""Coverage regressions for wrong or silently incomplete line measurements."""

import contextlib
import gzip
import hashlib
import importlib.util
import io
import json
import os
import re
import subprocess
import tempfile
import unittest
import zipfile
from pathlib import Path
from unittest import mock

from ao.__main__ import main
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
        self.line_hits = 3

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
        _file(
            pair,
            "Value.cpp.gcov",
            f"file:app/runtime/Value.cpp\nfunction:1,2,value\nlcount:1,0\nlcount:2,{self.line_hits}\n",
        )
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


class CoverageCommandTests(unittest.TestCase):
    label = "//test:fixture"
    configuration = "a" * 64
    suite = "//:coverage_core_tests"

    def setUp(self):
        temporary = self.enterContext(tempfile.TemporaryDirectory(prefix="aobus-coverage-cli-"))
        # Keep the build tree and its sibling lock file inside the temporary directory.
        self.tree = Path(temporary).resolve() / "tree"
        self.output_archive = self.tree / "testlogs/outputs.zip"
        execution = self.tree / "execution"
        execution.mkdir(parents=True)
        self.fixture = CollectionFixture(execution)
        java = self.tree / "jdk"
        _file(java, "bin/java", "fixture JVM")
        _file(java, "lib/jrt-fs.jar", "fixture runtime")
        prepared = self.tree / "prepared"
        _file(prepared, "request.json", "{}")
        _file(prepared, "aobus.bazelrc", "")
        self.executable = self.tree / "bazel"
        reader = self.fixture.tool
        reader_identity = {"path": str(reader), "sha256": hashlib.sha256(reader.read_bytes()).hexdigest()}
        self.enterContext(mock.patch.object(bazel_coverage, "PROJECT_ROOT", self.fixture.root))
        self.enterContext(mock.patch.object(bazel_coverage.native, "native_platform", return_value="linux"))
        workspace = bazel_coverage.bazel_workspace
        self.enterContext(mock.patch.object(workspace, "_linux_server_javabase", return_value=java))
        self.enterContext(mock.patch.object(workspace, "prepare_workspace", return_value=prepared))
        self.enterContext(mock.patch.object(workspace, "bazel_executable", return_value=self.executable))
        compiler_identity = {"files": [reader_identity]}
        self.enterContext(mock.patch.object(bazel_coverage, "_compiler_identity", return_value=compiler_identity))
        self.enterContext(
            mock.patch.object(bazel_coverage, "nix_cc_wrapper", return_value={"tools": {"gcov": str(reader)}})
        )
        self.fixture.add("linked")

    def _bazel(self, argv, *, stdout, stderr, **kwargs):
        self.assertEqual(argv[0], str(self.executable))
        # The launcher emits startup options as --flag or --flag=value, never
        # a separate value token, so the first non-option is the subcommand.
        command, *arguments = argv[next(i for i, arg in enumerate(argv[1:], 1) if not arg.startswith("--")) :]
        operands = [arg for arg in arguments if not arg.startswith("--")]
        selection = f"set({self.label})"
        if command == "query" and operands == [f"tests({self.suite})"] and "--output=label" in arguments:
            stdout.write((self.label + "\n").encode())
        elif command == "cquery" and operands == [selection] and "--output=jsonproto" in arguments:
            target = {"type": "RULE", "rule": {"name": self.label, "ruleClass": "cc_test"}}
            stdout.write(
                json.dumps(
                    {
                        "results": [{"target": target, "configurationId": 1}],
                        "configurations": [{"id": 1, "checksum": self.configuration}],
                    }
                ).encode()
            )
        elif command == "aquery" and operands == [f"deps({selection})"] and "--output=jsonproto" in arguments:
            action = {"targetId": 1, "configurationId": 1}
            compile_args = ["clang++", "-fprofile-arcs", "-ftest-coverage", "-fprofile-update=atomic"]
            stdout.write(
                json.dumps(
                    {
                        "targets": [{"id": 1, "label": self.label}],
                        "configuration": [{"id": 1}],
                        "actions": [
                            action | {"mnemonic": "CppCompile", "arguments": compile_args},
                            action | {"mnemonic": "CppLink", "arguments": ["clang++", "--coverage"]},
                        ],
                    }
                ).encode()
            )
        elif command == "coverage" and operands == [self.suite] and "--compiler=clang" in arguments:
            (bep,) = [Path(arg.split("=", 1)[1]) for arg in arguments if arg.startswith("--build_event_json_file=")]
            self._write_coverage(bep, stderr)
            if self.interrupt_wait:
                raise KeyboardInterrupt
            return subprocess.CompletedProcess(argv, self.child_exit)
        else:
            self.fail(f"unexpected Bazel invocation: {argv}")
        return subprocess.CompletedProcess(argv, 0)

    def _write_coverage(self, bep, stderr):
        self.fixture.environment["AOBUS_COVERAGE_ATTEMPT"] = bep.parent.name
        self.fixture.output = self.tree / "outputs" / bep.parent.name / "aobus-coverage"
        self.fixture.output.mkdir(parents=True)
        hits = 7 if self.child_exit else 3
        self.fixture.line_hits = hits
        self.fixture.collect()
        self.fixture.archive(self.output_archive)
        report = f"SF:app/runtime/Value.cpp\nDA:1,0\nDA:2,{hits}\nend_of_record\n"
        stock = _file(self.tree, "testlogs/test.lcov", report)
        relative = "bazel-out/_coverage/_coverage_report.dat"
        combined = _file(self.fixture.root, relative, report)
        timestamp = str(combined.stat().st_mtime_ns // 1_000_000)
        stderr.write(f"INFO: LCOV coverage report is located at {combined}\n and execpath is {relative}\n".encode())
        if self.child_exit:
            # Even usable test artifacts cannot make a nonzero Bazel exit pass.
            stderr.write(b"Bazel fixture failed after writing coverage data\n")
        identity = {"label": self.label, "configuration": {"id": self.configuration}}
        finished = {"finishTimeMillis": timestamp, "exitCode": {"code": self.bep_exit}}
        action_summary = {"actionData": [{"mnemonic": "CoverageReport", "actionsExecuted": 1}]}
        events = [
            {"id": {"started": {}}, "started": {"buildToolVersion": "9.2.0", "startTimeMillis": timestamp}},
            {"id": {"workspace": {}}, "workspaceInfo": {"localExecRoot": str(self.fixture.root)}},
            {
                "id": {"testResult": identity | {"run": 1, "shard": 1, "attempt": 1}},
                "testResult": {
                    "status": "PASSED",
                    "testActionOutput": [
                        {"name": path.name, "uri": path.as_uri()} for path in (self.output_archive, stock)
                    ],
                },
            },
            {"id": {"testSummary": identity}, "testSummary": {"overallStatus": "PASSED", "totalRunCount": 1}},
            {"id": {"buildFinished": {}}, "finished": finished},
            {"id": {"buildMetrics": {}}, "buildMetrics": {"actionSummary": action_summary}, "lastMessage": True},
        ]
        if self.missing_last_message:
            del events[-1]["lastMessage"]
        rows = [json.dumps(event) for event in events]
        if self.truncated:
            rows[-1] = '{"id":'
        bep.write_text("\n".join(rows), encoding="utf-8")

    def _measure(self, *, child_exit=0, bep_exit=0, truncated=False, missing_last_message=False, interrupt_wait=False):
        self.child_exit = child_exit
        self.bep_exit = bep_exit
        self.truncated = truncated
        self.missing_last_message = missing_last_message
        self.interrupt_wait = interrupt_wait
        return self._run("measure", "coverage")

    def _run(self, *command):
        coverage = self.tree / "coverage"
        previous = set(coverage.glob("run-*/receipt.json"))
        stdout, stderr = io.StringIO(), io.StringIO()
        with (
            mock.patch.dict(os.environ, clear=True),
            mock.patch.object(bazel_coverage.subprocess, "run", side_effect=self._bazel),
            contextlib.redirect_stdout(stdout),
            contextlib.redirect_stderr(stderr),
        ):
            status = main([*command, "--path", str(self.tree), "--suite", "core", "--compiler", "clang"])
        (receipt,) = set(coverage.glob("run-*/receipt.json")) - previous
        return status, stdout.getvalue(), stderr.getvalue(), receipt

    @staticmethod
    def _path_markers(output, marker):
        return [Path(path) for path in re.findall(rf"(?:^|; ){marker}: (.+)$", output, re.MULTILINE)]

    @staticmethod
    def _coverage_phase(receipt):
        (phase,) = [phase for phase in receipt["phases"] if phase["name"] == "coverage"]
        return phase

    def _assert_no_published_report(self, stdout, receipt):
        self.assertEqual(self._path_markers(stdout, "report"), [])
        self.assertNotIn("report", receipt)
        self.assertNotIn("report_sha256", receipt)

    def _assert_diagnostic_archive_retained(self, receipt, receipt_path):
        retention = receipt["artifact_retention"]
        self.assertEqual(retention["status"], "diagnostic-only")
        (archive,) = retention["archives"]
        self.assertTrue(archive["diagnostic_only"])
        retained = Path(archive["retained"])
        self.assertEqual(retained.parent, receipt_path.parent)
        self.assertEqual(retained.read_bytes(), self.output_archive.read_bytes())

    def test_failed_retries_retain_diagnostics_without_publishing_a_successful_report(self):
        status, stdout, stderr, successful_receipt = self._measure()
        self.assertEqual(status, 0, stderr)
        saved_receipt = successful_receipt.read_bytes()
        success = json.loads(saved_receipt)
        self.assertEqual((success["status"], success["exit"], success["bazel_exit"]), ("passed", 0, 0))
        report = Path(success["report"])
        self.assertEqual(report.parent, successful_receipt.parent)
        saved_report = report.read_bytes()
        self.assertEqual(bazel_coverage.parse_lcov(saved_report.decode()), {"app/runtime/Value.cpp": {1: 0, 2: 3}})
        self.assertEqual(self._path_markers(stdout, "report"), [report])

        # The complete BEP reports success; its verdict cannot override the process exit.
        for truncated, bep_exit in ((False, 0), (True, 23)):
            with self.subTest(truncated_bep=truncated):
                status, stdout, stderr, receipt_path = self._measure(
                    child_exit=23, bep_exit=bep_exit, truncated=truncated
                )
                self.assertNotEqual(status, 0)
                failure = json.loads(receipt_path.read_bytes())
                self._assert_no_published_report(stdout, failure)
                self.assertEqual(
                    (failure["status"], failure["exit"], failure.get("bazel_exit")), ("failed", status, 23), stderr
                )
                phase = self._coverage_phase(failure)
                self.assertEqual(phase["exit"], 23)
                phase_stderr = Path(phase["stderr"])
                self.assertEqual(phase_stderr.parent, receipt_path.parent)
                self.assertIn("Bazel fixture failed", phase_stderr.read_text(encoding="utf-8"))
                self.assertEqual(self._path_markers(stderr, "receipt"), [receipt_path])
                self._assert_diagnostic_archive_retained(failure, receipt_path)
                self.assertEqual(report.read_bytes(), saved_report)
                self.assertEqual(successful_receipt.read_bytes(), saved_receipt)

    def test_truncated_bep_after_zero_bazel_exit_retains_diagnostics_without_a_report(self):
        status, stdout, stderr, receipt_path = self._measure(truncated=True)

        self.assertNotEqual(status, 0)
        receipt = json.loads(receipt_path.read_bytes())
        self.assertEqual((receipt["status"], receipt["exit"], receipt["bazel_exit"]), ("failed", status, 0), stderr)
        self.assertEqual(self._coverage_phase(receipt)["exit"], 0)
        self.assertIn("bep_error", receipt)
        self.assertEqual(self._path_markers(stderr, "receipt"), [receipt_path])
        self._assert_diagnostic_archive_retained(receipt, receipt_path)
        self._assert_no_published_report(stdout, receipt)
        self.assertFalse(receipt_path.with_name("coverage.lcov").exists())

    def test_successful_child_without_a_terminal_bep_marker_does_not_publish_a_report(self):
        status, stdout, stderr, receipt_path = self._measure(missing_last_message=True)

        self.assertNotEqual(status, 0)
        receipt = json.loads(receipt_path.read_bytes())
        self.assertEqual((receipt["status"], receipt["exit"], receipt["bazel_exit"]), ("failed", status, 0), stderr)
        self.assertIn("bep_error", receipt)
        self.assertEqual(self._path_markers(stderr, "receipt"), [receipt_path])
        self._assert_no_published_report(stdout, receipt)
        self.assertFalse(receipt_path.with_name("coverage.lcov").exists())

    def test_sources_write_failure_leaves_lcov_without_passing_the_attempt(self):
        coverage = self.tree / "coverage"
        write_json = bazel_coverage.native.write_json
        error = "fixture could not write sources.json"

        def fail_sources_write(path, value):
            if path.name == "sources.json" and path.parent.parent == coverage:
                self.assertTrue(path.with_name("coverage.lcov").is_file())
                raise OSError(error)
            write_json(path, value)

        with mock.patch.object(bazel_coverage.native, "write_json", side_effect=fail_sources_write):
            status, stdout, stderr, receipt_path = self._measure()

        self.assertNotEqual(status, 0)
        receipt = json.loads(receipt_path.read_bytes())
        self.assertEqual((receipt["status"], receipt["exit"], receipt["bazel_exit"]), ("failed", status, 0), stderr)
        self.assertIn(error, receipt["error"])
        self.assertEqual(self._path_markers(stderr, "receipt"), [receipt_path])
        report = receipt_path.with_name("coverage.lcov")
        self.assertEqual(
            bazel_coverage.parse_lcov(report.read_text(encoding="utf-8")), {"app/runtime/Value.cpp": {1: 0, 2: 3}}
        )
        self._assert_no_published_report(stdout, receipt)

    def test_interrupted_wait_does_not_snapshot_or_publish_usable_coverage_outputs(self):
        status, stdout, stderr, receipt_path = self._measure(interrupt_wait=True)

        self.assertEqual(status, 130, stderr)
        bep = receipt_path.with_name("coverage.bep.json")
        self.assertTrue(json.loads(bep.read_text(encoding="utf-8").splitlines()[-1])["lastMessage"])
        self.assertTrue(zipfile.is_zipfile(self.output_archive))
        receipt = json.loads(receipt_path.read_bytes())
        self.assertEqual((receipt["status"], receipt["exit"]), ("interrupted", 130))
        self.assertNotIn("bazel_exit", receipt)
        self.assertIsNone(self._coverage_phase(receipt)["exit"])
        self.assertEqual(receipt["writer_state"], "unknown")
        self.assertEqual(receipt["artifact_retention"]["status"], "not-attempted")
        self.assertNotIn("test_output_archives", receipt)
        self.assertEqual(list(receipt_path.parent.rglob("*.zip")), [])
        self._assert_no_published_report(stdout, receipt)
        self.assertFalse(receipt_path.with_name("coverage.lcov").exists())

    def test_instrumentation_check_records_analysis_passed_without_a_report(self):
        # Stub the analysis proof; this case checks the command's receipt, not the action graph.
        with mock.patch.object(bazel_coverage, "_analyze") as analyze:
            status, stdout, stderr, receipt_path = self._run("check", "instrumentation")

        self.assertEqual(status, 0, stderr)
        analyze.assert_called_once()
        receipt = json.loads(receipt_path.read_bytes())
        self.assertEqual((receipt["status"], receipt["exit"], receipt["mode"]), ("analysis-passed", 0, "analysis-only"))
        self.assertEqual(self._path_markers(stdout, "receipt"), [receipt_path])
        self._assert_no_published_report(stdout, receipt)
        self.assertFalse(receipt_path.with_name("coverage.lcov").exists())


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
