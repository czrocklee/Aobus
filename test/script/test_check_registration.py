"""Fresh configured-source ownership checks, independent of filename discovery."""

import argparse
import contextlib
import io
import tempfile
import unittest
from pathlib import Path
from unittest import mock

from ao.command import check
from ao.command.build import BuildResult
from ao.core import builddir


class ConfiguredTestRegistrationTest(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        self.source = self.root / "configured-source"
        self.build = self.root / "build"
        self.build.mkdir()
        self.write("test/core/ExampleTest.cpp", 'TEST_CASE("Example - works") {}\n')
        (self.build / "CMakeCache.txt").write_text(
            f"CMAKE_HOME_DIRECTORY:INTERNAL={self.source.as_posix()}\n", encoding="utf-8"
        )
        self.records = [("test/core/ExampleTest.cpp", "configured", "ao_core_test")]
        self.manifest()

    def write(self, relative, text):
        path = self.source / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text, encoding="utf-8")
        return path

    def manifest(self):
        (self.build / "aobus-test-sources.tsv").write_text(
            "source\tstate\towner-or-disabled-reason\n" + "".join("\t".join(record) + "\n" for record in self.records),
            encoding="utf-8",
        )

    def verify(self):
        with contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()):
            check._verify_test_source_manifest(self.build)

    def test_reads_the_configured_source_view_and_accepts_benchmark_owner(self):
        self.write("test/tui/TrackTableBenchmark.cpp", 'TEST_CASE("TrackTable - benchmark rows") {}\n')
        self.records.append(("test/tui/TrackTableBenchmark.cpp", "configured", "ao_tui_bench"))
        self.manifest()
        self.verify()

    def test_configured_source_home_is_normalized_before_discovery(self):
        (self.build / "CMakeCache.txt").write_text(
            "CMAKE_HOME_DIRECTORY:INTERNAL=configured-source\n", encoding="utf-8"
        )
        with (
            contextlib.chdir(self.root),
            mock.patch.object(
                check.testaudit, "discover_test_files", wraps=check.testaudit.discover_test_files
            ) as discover,
        ):
            self.verify()
        discover.assert_called_once_with(check.absolute_path(self.source))

    def test_empty_discovery_cannot_validate_an_existing_manifest(self):
        self.write("test/core/ExampleTest.cpp", "void helper() {}\n")
        with self.assertRaises(SystemExit):
            self.verify()

    def test_unconventional_registration_filename_cannot_escape_ownership(self):
        self.write("test/core/ExtraCases.cpp", 'TEST_CASE("Extra - works") {}\n')
        with self.assertRaises(SystemExit):
            self.verify()

    def test_nonconventional_registration_filename_fails_even_with_a_manifest_record(self):
        self.write("test/core/FooSupport.cpp", 'TEST_CASE("Foo - preserves the contract") {}\n')
        self.records.append(("test/core/FooSupport.cpp", "configured", "ao_core_test"))
        self.manifest()
        errors = io.StringIO()
        with (
            contextlib.redirect_stdout(io.StringIO()),
            contextlib.redirect_stderr(errors),
            self.assertRaises(SystemExit),
        ):
            check._verify_test_source_manifest(self.build)
        self.assertEqual(errors.getvalue().count("unsupported-placement:"), 1)
        self.assertIn("*Benchmark.cpp", errors.getvalue())

    def test_benchmark_without_an_owner_fails(self):
        self.write("test/tui/TrackTableBenchmark.cpp", 'TEST_CASE("TrackTable - benchmark rows") {}\n')
        with self.assertRaises(SystemExit):
            self.verify()

    def test_explicit_disabled_reason_accounts_for_inactive_source(self):
        self.write("test/macos/NativeTest.mm", 'TEST_CASE("Native - works") {}\n')
        self.records.append(("test/macos/NativeTest.mm", "disabled", "non-macOS host"))
        self.manifest()
        self.verify()

    def test_duplicate_or_simultaneously_configured_disabled_record_fails(self):
        for state, detail in (("configured", "ao_tui_test"), ("disabled", "disabled frontend")):
            with self.subTest(state=state):
                self.records = [
                    ("test/core/ExampleTest.cpp", "configured", "ao_core_test"),
                    ("test/core/ExampleTest.cpp", state, detail),
                ]
                self.manifest()
                with self.assertRaises(SystemExit):
                    self.verify()

    def test_invalid_records_and_missing_registered_source_fail(self):
        for record in (
            ("test/core/ExampleTest.cpp", "unexpected", "ao_core_test"),
            ("test/core/ExampleTest.cpp", "disabled", ""),
            ("test/core/MissingTest.cpp", "configured", "ao_core_test"),
            ("test/../outside.cpp", "configured", "ao_core_test"),
        ):
            with self.subTest(record=record):
                self.records = [record]
                self.manifest()
                with self.assertRaises(SystemExit):
                    self.verify()

    def test_missing_manifest_or_source_view_fails_closed(self):
        manifest = self.build / "aobus-test-sources.tsv"
        manifest.unlink()
        with self.assertRaises(SystemExit):
            self.verify()
        self.manifest()
        (self.build / "CMakeCache.txt").unlink()
        with self.assertRaises(SystemExit):
            self.verify()

    def test_invalid_header_fails(self):
        (self.build / "aobus-test-sources.tsv").write_text("not a manifest\n", encoding="utf-8")
        with self.assertRaises(SystemExit):
            self.verify()

    def test_unsupported_macro_family_cannot_become_an_empty_discovery_success(self):
        self.write("test/core/ExtraCases.cpp", 'CATCH_TEST_CASE("Extra - works") {}\n')
        with self.assertRaises(SystemExit):
            self.verify()

    def test_header_and_fragment_registration_fail_placement(self):
        for suffix in (".h", ".hpp", ".def"):
            with self.subTest(suffix=suffix):
                path = self.write(f"test/core/Extra{suffix}", 'TEST_CASE("Extra - works") {}\n')
                with self.assertRaises(SystemExit):
                    self.verify()
                path.unlink()

    def test_header_and_fragment_lexical_errors_are_reported_once(self):
        for suffix in (".h", ".hpp", ".def"):
            for supported_case in (False, True):
                with self.subTest(suffix=suffix, supported_case=supported_case):
                    body = 'CATCH_TEST_CASE("Unsupported - works") {}\n'
                    if supported_case:
                        body = 'TEST_CASE("Extra - works") {}\n' + body
                    path = self.write(f"test/core/Extra{suffix}", body)
                    _, lexical_issues = check.testaudit.scan_test_file(path)
                    self.assertEqual(len(lexical_issues), 1)
                    errors = io.StringIO()
                    with (
                        contextlib.redirect_stdout(io.StringIO()),
                        contextlib.redirect_stderr(errors),
                        self.assertRaises(SystemExit),
                    ):
                        check._verify_test_source_manifest(self.build)
                    self.assertEqual(errors.getvalue().count(lexical_issues[0].format(self.source)), 1)
                    self.assertEqual(errors.getvalue().count("unsupported-placement:"), int(supported_case))
                    path.unlink()

    def test_comments_strings_and_deliberate_lint_fixtures_are_not_owners(self):
        self.write(
            "test/core/Examples.cpp",
            '// TEST_CASE("False - comment") {}\nconst char* sample = R"---(TEST_CASE("False - raw string"))---";\n',
        )
        self.write("test/lint/fixture/example/BadTest.cpp", "TEST_CASE(dynamic_name) {}\n")
        self.verify()

    def test_registered_source_without_a_declaration_is_not_a_second_registry(self):
        self.write("test/core/EmptyTest.cpp", "void helper() {}\n")
        self.records.append(("test/core/EmptyTest.cpp", "configured", "ao_core_test"))
        self.manifest()
        self.verify()

    def test_check_verifies_native_manifest_after_build_before_running_suites(self):
        parser = argparse.ArgumentParser()
        with mock.patch.object(builddir, "platform_profile", return_value=builddir.LINUX_PROFILE):
            check.register(parser.add_subparsers())
            args = parser.parse_args(["check"])
            result = BuildResult(self.build, self.build / "build.log", "gcc")
            events = []
            original_verify = check._verify_test_source_manifest

            def verify(build):
                events.append("manifest")
                original_verify(build)

            with (
                mock.patch.object(
                    check.build, "do_build", side_effect=lambda *a, **kw: (events.append("build"), result)[1]
                ),
                mock.patch.object(check, "_verify_test_source_manifest", side_effect=verify),
                mock.patch.object(check.dependency_policy, "verified_report"),
                mock.patch.object(check.test, "configured_suites_for", return_value=("core",)),
                mock.patch.object(
                    check.test, "run_suites", side_effect=lambda *a, **kw: (events.append("suites"), 0)[1]
                ),
                mock.patch.object(check.build, "print_summary"),
                contextlib.redirect_stdout(io.StringIO()),
            ):
                self.assertEqual(check.run_command(args), 0)
            self.assertEqual(events, ["build", "manifest", "suites"])


if __name__ == "__main__":
    unittest.main()
