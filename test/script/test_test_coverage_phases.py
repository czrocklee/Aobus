"""Real phase-JSON regressions for the ``ao test``/``ao coverage`` attribution.

Every test here drives the real control flow (``run_command``, ``run_suites``,
``_run_catch2_suite``, ``_run_coverage``) with the real ``ao.core.phases``
writer, pointed at a temporary ``AOBUS_PHASE_LOG_DIR``, while every native
process is mocked. Two contracts are pinned:

* Filtered report handling (feedback item 7): the real producers already
  promote a missing or malformed filtered XML report and an unreported shard to
  a nonzero suite status, preserve exact signed native codes, and allow an
  empty member suite when another suite matches globally. No production fix is
  intended or needed; these tests protect that behavior.
* Phase attribution (feedback item 8): the standalone test/coverage target
  build, the cold coverage configure, and each per-suite coverage invocation
  are recorded as their own native child phases with the actual native exit
  code, closing the root-only attribution gap without changing build, sharding,
  or run-all/first-failure semantics.

Timings are never asserted: mock durations prove boundaries, not speed.
"""

import contextlib
import io
import json
import os
import tempfile
import unittest
from pathlib import Path
from types import SimpleNamespace
from unittest import mock

from ao.__main__ import make_parser, parse_arguments
from ao.command import build as build_command
from ao.command import coverage as coverage_command
from ao.command import test as test_command
from ao.core import builddir, phases
from ao.core.paths import PROJECT_ROOT

# A CMake cache that represents an enabled test graph.
_ENABLED_SUITE_CACHE = {"AOBUS_BUILD_TESTS": "ON"}


def _enabled_suite_cache(_path):
    return _ENABLED_SUITE_CACHE


_EMPTY_CASES_REPORT = (
    '<Catch2TestRun><OverallResultsCases successes="0" failures="0" expectedFailures="0" skips="0"/></Catch2TestRun>'
)
_ONE_CASE_REPORT = (
    '<Catch2TestRun><OverallResultsCases successes="1" failures="0" expectedFailures="0" skips="0"/></Catch2TestRun>'
)
_ONE_SHARD_REPORT = (
    "<Catch2TestRun>"
    '<OverallResults successes="1" failures="0" expectedFailures="0" skips="0"/>'
    '<OverallResultsCases successes="1" failures="0" expectedFailures="0" skips="0"/>'
    "</Catch2TestRun>"
)


def _write_suite_binary(build_dir: Path, suite: str) -> Path:
    """Place the binary for the Linux profile these producer tests execute."""
    target = test_command.SUITES[suite].target
    assert target is not None
    with mock.patch.object(builddir, "platform_profile", return_value=builddir.LINUX_PROFILE):
        binary = builddir.executable(build_dir / "test" / target)
    binary.parent.mkdir(parents=True, exist_ok=True)
    binary.touch()
    return binary


class _PhaseJsonTestCase(unittest.TestCase):
    """Shared harness: real parser, real phase writer, temporary phase log.

    ``measure`` writes a BEGIN row carrying the metadata (tree, suite, jobs,
    count) and a separate END row carrying the status; the reader merges the
    pair by ``phase_id`` into one record per measured phase.
    """

    def parse(self, argv):
        return parse_arguments(make_parser(), argv)

    @contextlib.contextmanager
    def phase_log(self):
        with tempfile.TemporaryDirectory(prefix="aobus-phase-json-") as directory:
            log_dir = Path(directory)
            with mock.patch.dict(os.environ, {phases.LOG_DIR_ENV: str(log_dir)}):
                yield log_dir

    @staticmethod
    def read_records(log_dir: Path) -> list[dict]:
        """Return one schema-checked record per completed measured phase."""
        begins: dict[str, dict] = {}
        records: list[dict] = []
        for path in sorted(log_dir.glob("*.jsonl")):
            for line in path.read_text(encoding="utf-8").splitlines():
                row = json.loads(line)
                assert phases._row_problem(row) is None, row
                if row["kind"] == "BEGIN":
                    begins[row["phase_id"]] = row
                else:
                    record = {**begins[row["phase_id"]], "status": row["status"], "outcome": row["outcome"]}
                    records.append(record)
        return records

    @staticmethod
    def records_for(records: list[dict], phase: str) -> list[dict]:
        return [record for record in records if record["phase"] == phase]

    def single_record(self, records: list[dict], phase: str) -> dict:
        matches = self.records_for(records, phase)
        self.assertEqual(len(matches), 1, f"expected exactly one {phase!r} record, got {matches}")
        return matches[0]


class FilteredReportPhaseContractTest(_PhaseJsonTestCase):
    """Item 7: the real failure promotion and signed-status contracts, as JSON.

    The defensive ``matched is None`` guard in ``run_suites`` is unreachable
    from the real producers with ``(status=0, matched=None)`` for a filter; the
    suite record is already nonzero by then. These tests keep it that way.
    """

    @staticmethod
    def _native_run_stub(status=0, *, report_xml: str | None = None):
        """Replace one native Catch2 process: return a status, maybe write XML."""

        def stub(argv, **_kwargs):
            argument = next((item for item in argv if item.startswith("xml::out=")), None)
            if argument is not None and report_xml is not None:
                Path(argument[len("xml::out=") :]).write_text(report_xml, encoding="utf-8")
            return status

        return stub

    def _run_suites_with_filter(
        self, suites, build_dir, run_stub, *, test_filter="[focused]", wrap_command=False
    ) -> tuple[int, str, list[dict]]:
        output = io.StringIO()
        # The records must be read while the temporary log directory exists.
        with self.phase_log() as log_dir:
            with (
                mock.patch.object(builddir, "platform_profile", return_value=builddir.LINUX_PROFILE),
                mock.patch.object(test_command, "shard_count", return_value=1),
                mock.patch.object(test_command, "run", side_effect=run_stub),
                contextlib.redirect_stdout(output),
            ):
                if wrap_command:
                    # Mirror the portal: the command record owns the root span.
                    with phases.measure("command", command="test") as root:
                        status = test_command.run_suites(suites, build_dir, test_filter=test_filter)
                        root.status = status
                else:
                    status = test_command.run_suites(suites, build_dir, test_filter=test_filter)
            records = self.read_records(log_dir)
        return status, output.getvalue(), records

    def test_missing_filtered_xml_report_records_failing_suite_and_command_phases(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            build_dir = Path(temp_dir)
            _write_suite_binary(build_dir, "core")

            # Native exit 0 but no readable XML report: the producer must not
            # let the filter pass; the command row mirrors the failure.
            status, text, records = self._run_suites_with_filter(
                ("core",), build_dir, self._native_run_stub(0), wrap_command=True
            )

        self.assertEqual(status, 1)
        suite = self.single_record(records, "suite")
        self.assertEqual(
            (suite["suite"], suite["tree"], suite["jobs"], suite["count"], suite["status"], suite["outcome"]),
            ("core", "native", 1, 1, 1, "fail"),
        )
        command = self.single_record(records, "command")
        self.assertEqual((command["command"], command["status"], command["outcome"]), ("test", 1, "fail"))
        self.assertIn("no readable Catch2 XML report", text)

    def test_malformed_filtered_xml_report_records_a_failing_suite_phase(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            build_dir = Path(temp_dir)
            _write_suite_binary(build_dir, "core")

            status, _text, records = self._run_suites_with_filter(
                ("core",), build_dir, self._native_run_stub(0, report_xml="<not-xml")
            )

        self.assertEqual(status, 1)
        suite = self.single_record(records, "suite")
        self.assertEqual((suite["status"], suite["outcome"]), (1, "fail"))

    def test_missing_filtered_xml_report_preserves_the_signed_native_status(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            build_dir = Path(temp_dir)
            _write_suite_binary(build_dir, "core")

            status, _text, records = self._run_suites_with_filter(("core",), build_dir, self._native_run_stub(-11))

        # A crashed native step keeps its signed code; the missing report adds
        # no normalization on top of it.
        self.assertEqual(status, -11)
        suite = self.single_record(records, "suite")
        self.assertEqual((suite["status"], suite["outcome"]), (-11, "fail"))

    def test_unreported_shard_records_a_failing_suite_phase(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            build_dir = Path(temp_dir)
            _write_suite_binary(build_dir, "core")

            def spawn(argv, *, cwd=None, env=None, stdout=None, stderr=None):
                index = int(argv[argv.index("--shard-index") + 1])
                if index == 0:
                    argument = next(item for item in argv if item.startswith("xml::out="))
                    Path(argument[len("xml::out=") :]).write_text(_ONE_SHARD_REPORT, encoding="utf-8")
                stdout.write(f"shard {index} console output\n".encode())
                return mock.Mock(returncode=0, wait=mock.Mock(return_value=0))

            with self.phase_log() as log_dir:
                with (
                    mock.patch.object(builddir, "platform_profile", return_value=builddir.LINUX_PROFILE),
                    mock.patch.object(test_command, "shard_count", return_value=2),
                    mock.patch.object(test_command.subprocess, "Popen", side_effect=spawn),
                    contextlib.redirect_stdout(io.StringIO()) as output,
                ):
                    status = test_command.run_suites(("core",), build_dir, test_filter="[focused]")
                records = self.read_records(log_dir)

        self.assertEqual(status, 1)
        suite = self.single_record(records, "suite")
        self.assertEqual((suite["status"], suite["outcome"], suite["jobs"]), (1, "fail", 2))
        self.assertIn("no readable report from shard(s) 2 of 2", output.getvalue())

    def test_unfiltered_signed_failure_is_recorded_verbatim(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            build_dir = Path(temp_dir)
            _write_suite_binary(build_dir, "core")

            status, _text, records = self._run_suites_with_filter(
                ("core",), build_dir, self._native_run_stub(17), test_filter=""
            )

        self.assertEqual(status, 17)
        suite = self.single_record(records, "suite")
        self.assertEqual((suite["status"], suite["outcome"]), (17, "fail"))

    def test_unfiltered_success_records_a_passing_suite_phase(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            build_dir = Path(temp_dir)
            _write_suite_binary(build_dir, "core")

            status, _text, records = self._run_suites_with_filter(
                ("core",), build_dir, self._native_run_stub(0), test_filter=""
            )

        # Without a filter, matched=None is a legitimate success.
        self.assertEqual(status, 0)
        suite = self.single_record(records, "suite")
        self.assertEqual((suite["status"], suite["outcome"]), (0, "ok"))
        self.assertEqual((suite["jobs"], suite["count"]), (1, 1))

    def test_empty_member_suite_with_a_global_match_records_passing_phases(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            build_dir = Path(temp_dir)
            _write_suite_binary(build_dir, "core")
            _write_suite_binary(build_dir, "tui")

            def run_stub(argv, **_kwargs):
                report_xml = _ONE_CASE_REPORT if "ao_tui_test" in argv[0] else _EMPTY_CASES_REPORT
                argument = next(item for item in argv if item.startswith("xml::out="))
                Path(argument[len("xml::out=") :]).write_text(report_xml, encoding="utf-8")
                return 0

            status, _text, records = self._run_suites_with_filter(("core", "tui"), build_dir, run_stub)

        self.assertEqual(status, 0)
        suites = {record["suite"]: record for record in self.records_for(records, "suite")}
        self.assertEqual(set(suites), {"core", "tui"})
        for record in suites.values():
            self.assertEqual((record["status"], record["outcome"]), (0, "ok"))


class StandaloneBuildAttributionTest(_PhaseJsonTestCase):
    """Item 8 for ``ao test``: the in-place target build gets its own record."""

    def _run_test_command(self, argv, *, native_build_status=0, suites_status=0, sync=None):
        with tempfile.TemporaryDirectory() as temp_dir:
            build_dir = Path(temp_dir)
            args = self.parse([*argv, "-p", str(build_dir)])
            with self.phase_log() as log_dir:
                with (
                    mock.patch.object(builddir, "platform_profile", return_value=builddir.LINUX_PROFILE),
                    mock.patch.object(build_command, "validate_build_tree", return_value="gcc"),
                    mock.patch.object(test_command.compiler_cache, "read_cmake_cache", new=_enabled_suite_cache),
                    mock.patch.object(test_command.workspace_cache, "compiler_build_dir", return_value=build_dir),
                    mock.patch.object(build_command, "sync_compiler_cache", side_effect=sync),
                    mock.patch.object(build_command, "parallel_build_jobs", return_value=31),
                    mock.patch.object(test_command, "run", return_value=native_build_status) as run,
                    mock.patch.object(test_command, "run_suites", return_value=suites_status),
                    contextlib.redirect_stdout(io.StringIO()),
                ):
                    status = test_command.run_command(args)
                records = self.read_records(log_dir)
        return status, run, records, build_dir

    def test_target_build_is_attributed_below_the_root_command_record(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            build_dir = Path(temp_dir)
            args = self.parse(["test", "--core", "-p", str(build_dir)])
            with self.phase_log() as log_dir:
                with (
                    mock.patch.object(builddir, "platform_profile", return_value=builddir.LINUX_PROFILE),
                    mock.patch.object(build_command, "validate_build_tree", return_value="gcc"),
                    mock.patch.object(test_command.compiler_cache, "read_cmake_cache", new=_enabled_suite_cache),
                    mock.patch.object(test_command.workspace_cache, "compiler_build_dir", return_value=build_dir),
                    mock.patch.object(build_command, "sync_compiler_cache"),
                    mock.patch.object(build_command, "parallel_build_jobs", return_value=31),
                    mock.patch.object(test_command, "run", return_value=0) as run,
                    mock.patch.object(test_command, "_run_catch2_suite", return_value=test_command._SuiteOutcome(0)),
                    mock.patch.object(test_command, "shard_count", return_value=1),
                    contextlib.redirect_stdout(io.StringIO()),
                ):
                    with phases.measure("command", command="test") as root:
                        status = test_command.run_command(args)
                        root.status = status
                records = self.read_records(log_dir)

        self.assertEqual(status, 0)
        command = self.single_record(records, "command")
        self.assertEqual(command["status"], 0)
        build = self.single_record(records, "build")
        self.assertEqual((build["tree"], build["jobs"], build["status"], build["outcome"]), ("native", 31, 0, "ok"))
        self.assertEqual(build["parent_id"], command["phase_id"])
        suite = self.single_record(records, "suite")
        self.assertEqual((suite["suite"], suite["jobs"], suite["count"], suite["status"]), ("core", 1, 1, 0))
        self.assertEqual(suite["parent_id"], command["phase_id"])
        # The measured invocation is still the existing incremental build.
        run.assert_called_once_with(
            ["cmake", "--build", str(build_dir), "--parallel", "31", "--target", "ao_core_test"],
            cwd=PROJECT_ROOT,
        )

    def test_target_build_failure_keeps_its_native_status_before_die_normalization(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            build_dir = Path(temp_dir)
            args = self.parse(["test", "--core", "-p", str(build_dir)])
            with self.phase_log() as log_dir:
                with (
                    mock.patch.object(builddir, "platform_profile", return_value=builddir.LINUX_PROFILE),
                    mock.patch.object(build_command, "validate_build_tree", return_value="gcc"),
                    mock.patch.object(test_command.compiler_cache, "read_cmake_cache", new=_enabled_suite_cache),
                    mock.patch.object(test_command.workspace_cache, "compiler_build_dir", return_value=build_dir),
                    mock.patch.object(build_command, "sync_compiler_cache"),
                    mock.patch.object(build_command, "parallel_build_jobs", return_value=31),
                    mock.patch.object(test_command, "run", return_value=17),
                    mock.patch.object(test_command, "run_suites", return_value=0),
                    contextlib.redirect_stdout(io.StringIO()),
                    contextlib.redirect_stderr(io.StringIO()),
                ):
                    with self.assertRaisesRegex(SystemExit, "1"):
                        test_command.run_command(args)
                records = self.read_records(log_dir)

        build = self.single_record(records, "build")
        self.assertEqual((build["status"], build["outcome"]), (17, "fail"))
        self.assertEqual(self.records_for(records, "suite"), [])

    def test_no_build_skips_the_build_phase(self):
        status, run, records, _build_dir = self._run_test_command(["test", "--core", "-n"], suites_status=0)

        self.assertEqual(status, 0)
        self.assertEqual(self.records_for(records, "build"), [])
        run.assert_not_called()

    def test_sync_reconfigure_is_not_duplicated_by_the_build_phase(self):
        def sync(_build_dir, *, unsupported_reason=None):
            # The real sync_compiler_cache measures its own reconfigure when
            # the managed cache settings changed; reproduce that emission.
            with phases.measure("configure", tree="native") as phase:
                phase.status = 0

        status, _run, records, _build_dir = self._run_test_command(["test", "--core"], sync=sync)

        self.assertEqual(status, 0)
        configure = self.single_record(records, "configure")
        self.assertEqual((configure["tree"], configure["status"]), ("native", 0))
        build = self.single_record(records, "build")
        self.assertEqual(build["status"], 0)


class CoverageAttributionTest(_PhaseJsonTestCase):
    """Item 8 for ``ao coverage``: configure, build, and suite attribution."""

    def _run_coverage(
        self,
        build_dir: Path,
        argv: list[str],
        *,
        cache_file: bool,
        run_side_effect,
        suite_status=0,
        sync=None,
        workspace=None,
    ):
        if cache_file:
            (build_dir / "CMakeCache.txt").write_text("CMAKE_CXX_FLAGS:STRING=--coverage\n", encoding="utf-8")
        args = self.parse([*argv, "-p", str(build_dir)])
        with self.phase_log() as log_dir:
            with (
                mock.patch.object(builddir, "platform_profile", return_value=builddir.LINUX_PROFILE),
                mock.patch.object(coverage_command.workspace_cache, "prepare", return_value=workspace),
                mock.patch.object(coverage_command.compiler_cache, "cmake_launcher_arguments", return_value=[]),
                mock.patch.object(build_command, "sync_compiler_cache", side_effect=sync),
                mock.patch.object(coverage_command, "coverage_cxx_flags", return_value="--coverage"),
                mock.patch.object(coverage_command, "run", side_effect=run_side_effect) as run,
                mock.patch.object(coverage_command, "run_suite", return_value=suite_status) as run_suite,
                mock.patch.object(coverage_command, "collect_coverage", return_value={}),
                mock.patch.object(coverage_command, "report"),
                contextlib.redirect_stdout(io.StringIO()),
                contextlib.redirect_stderr(io.StringIO()),
            ):
                status = coverage_command.run_command(args)
            records = self.read_records(log_dir)
        return status, run, run_suite, records

    def test_cold_coverage_records_configure_build_and_suite_phases(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            build_dir = Path(temp_dir)
            workspace = SimpleNamespace(source_dir=PROJECT_ROOT, cmake_arguments=[])
            status, run, run_suite, records = self._run_coverage(
                build_dir,
                ["coverage", "--tui", "-j", "4"],
                cache_file=False,
                run_side_effect=[0, 0],
                workspace=workspace,
            )

        self.assertEqual(status, 0)
        configure = self.single_record(records, "configure")
        self.assertEqual((configure["tree"], configure["status"], configure["outcome"]), ("native", 0, "ok"))
        build = self.single_record(records, "build")
        self.assertEqual((build["tree"], build["jobs"], build["status"]), ("native", 4, 0))
        suite = self.single_record(records, "suite")
        self.assertEqual((suite["suite"], suite["jobs"], suite["count"], suite["status"]), ("tui", 1, 1, 0))
        # The cold configure still comes from the coverage source configuration.
        configure_call, build_call = (call.args[0] for call in run.call_args_list)
        self.assertEqual(configure_call[1:5], ["-S", str(PROJECT_ROOT), "--preset", "linux-debug"])
        self.assertIn("-DCMAKE_CXX_FLAGS=--coverage", configure_call)
        self.assertIn("-DCMAKE_EXE_LINKER_FLAGS=--coverage", configure_call)
        self.assertEqual(build_call, ["cmake", "--build", str(build_dir), "-j4", "--target", "ao_tui_test"])
        run_suite.assert_called_once_with("tui", build_dir, test_filter="")

    def test_nonpositive_coverage_limits_still_reach_the_native_build(self):
        for jobs in (0, -2):
            with self.subTest(jobs=jobs), tempfile.TemporaryDirectory() as temp_dir:
                build_dir = Path(temp_dir)
                status, native, _suite, records = self._run_coverage(
                    build_dir,
                    ["coverage", "--core", "-j", str(jobs)],
                    cache_file=True,
                    run_side_effect=[0],
                )
                self.assertEqual(status, 0)
                self.assertIn(f"-j{jobs}", native.call_args.args[0])
                measured = self.single_record(records, "build")
                self.assertNotIn("jobs", measured)
                self.assertEqual(measured["status"], 0)

    def test_warm_coverage_skips_the_cold_configure_phase(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            build_dir = Path(temp_dir)
            status, run, _run_suite, records = self._run_coverage(
                build_dir,
                ["coverage", "--core", "-j", "3"],
                cache_file=True,
                run_side_effect=[0],
                suite_status=0,
            )

        self.assertEqual(status, 0)
        self.assertEqual(self.records_for(records, "configure"), [])
        build = self.single_record(records, "build")
        self.assertEqual((build["tree"], build["jobs"], build["status"]), ("native", 3, 0))
        suite = self.single_record(records, "suite")
        self.assertEqual((suite["suite"], suite["status"]), ("core", 0))
        self.assertEqual(len(run.call_args_list), 1)

    def test_warm_coverage_does_not_duplicate_the_sync_configure_phase(self):
        def sync(_build_dir, *, unsupported_reason=None):
            # The real sync_compiler_cache measures its own reconfigure when
            # the managed cache settings changed; reproduce that emission.
            with phases.measure("configure", tree="native") as phase:
                phase.status = 0

        with tempfile.TemporaryDirectory() as temp_dir:
            build_dir = Path(temp_dir)
            status, _run, _run_suite, records = self._run_coverage(
                build_dir,
                ["coverage", "--core", "-j", "2"],
                cache_file=True,
                run_side_effect=[0],
                sync=sync,
            )

        self.assertEqual(status, 0)
        configure = self.single_record(records, "configure")
        self.assertEqual((configure["tree"], configure["status"]), ("native", 0))
        self.assertEqual(self.single_record(records, "build")["status"], 0)

    def test_coverage_records_every_suite_status_and_the_first_failure(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            build_dir = Path(temp_dir)
            args = self.parse(["coverage", "--all", "-p", str(build_dir), "-j", "2"])
            (build_dir / "CMakeCache.txt").write_text("CMAKE_CXX_FLAGS:STRING=--coverage\n", encoding="utf-8")
            with self.phase_log() as log_dir:
                with (
                    mock.patch.object(builddir, "platform_profile", return_value=builddir.LINUX_PROFILE),
                    mock.patch.object(build_command, "sync_compiler_cache"),
                    mock.patch.object(coverage_command, "run", return_value=0),
                    mock.patch.object(coverage_command, "run_suite", side_effect=[0, 7, 0, -11]) as run_suite,
                    mock.patch.object(coverage_command, "collect_coverage", return_value={}),
                    mock.patch.object(coverage_command, "report"),
                    contextlib.redirect_stdout(io.StringIO()),
                    contextlib.redirect_stderr(io.StringIO()),
                ):
                    status = coverage_command.run_command(args)
                records = self.read_records(log_dir)

        # Run-all and first-failure semantics are unchanged: every suite ran,
        # each kept its own status, and the first failure wins the command.
        self.assertEqual(status, 7)
        self.assertEqual([call.args[0] for call in run_suite.call_args_list], ["core", "tui", "cli", "gtk"])
        suites = {record["suite"]: record["status"] for record in self.records_for(records, "suite")}
        self.assertEqual(suites, {"core": 0, "tui": 7, "cli": 0, "gtk": -11})

    def test_cold_configure_failure_keeps_its_native_status_before_die_normalization(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            build_dir = Path(temp_dir)
            workspace = SimpleNamespace(source_dir=PROJECT_ROOT, cmake_arguments=[])
            with self.phase_log() as log_dir:
                with (
                    mock.patch.object(builddir, "platform_profile", return_value=builddir.LINUX_PROFILE),
                    mock.patch.object(coverage_command.workspace_cache, "prepare", return_value=workspace),
                    mock.patch.object(coverage_command.compiler_cache, "cmake_launcher_arguments", return_value=[]),
                    mock.patch.object(coverage_command, "coverage_cxx_flags", return_value="--coverage"),
                    mock.patch.object(coverage_command, "run", return_value=17),
                    contextlib.redirect_stdout(io.StringIO()),
                    contextlib.redirect_stderr(io.StringIO()),
                ):
                    with self.assertRaisesRegex(SystemExit, "1"):
                        coverage_command.run_command(self.parse(["coverage", "-p", str(build_dir)]))
                records = self.read_records(log_dir)

        configure = self.single_record(records, "configure")
        self.assertEqual((configure["status"], configure["outcome"]), (17, "fail"))
        self.assertEqual(self.records_for(records, "build"), [])

    def test_coverage_build_failure_keeps_its_signed_native_status(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            build_dir = Path(temp_dir)
            (build_dir / "CMakeCache.txt").write_text("CMAKE_CXX_FLAGS:STRING=--coverage\n", encoding="utf-8")
            with self.phase_log() as log_dir:
                with (
                    mock.patch.object(builddir, "platform_profile", return_value=builddir.LINUX_PROFILE),
                    mock.patch.object(build_command, "sync_compiler_cache"),
                    mock.patch.object(coverage_command, "run", return_value=-11),
                    contextlib.redirect_stdout(io.StringIO()),
                    contextlib.redirect_stderr(io.StringIO()),
                ):
                    with self.assertRaisesRegex(SystemExit, "1"):
                        coverage_command.run_command(self.parse(["coverage", "-p", str(build_dir)]))
                records = self.read_records(log_dir)

        build = self.single_record(records, "build")
        self.assertEqual((build["status"], build["outcome"]), (-11, "fail"))
        self.assertEqual(self.records_for(records, "suite"), [])


class ToolingSuitePhaseTest(_PhaseJsonTestCase):
    """A targetless selection records no build phase at all."""

    def test_tooling_suite_records_no_build_phase(self):
        args = self.parse(["test", "--tooling"])
        with self.phase_log() as log_dir:
            with (
                mock.patch.object(builddir, "platform_profile", return_value=builddir.LINUX_PROFILE),
                mock.patch.object(test_command, "run_suites", return_value=0),
                mock.patch.object(test_command, "run", return_value=0) as run,
                contextlib.redirect_stdout(io.StringIO()),
            ):
                status = test_command.run_command(args)
            records = self.read_records(log_dir)

        self.assertEqual(status, 0)
        self.assertEqual(self.records_for(records, "build"), [])
        run.assert_not_called()


if __name__ == "__main__":
    unittest.main()
