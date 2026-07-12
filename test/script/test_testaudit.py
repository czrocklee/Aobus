"""Tests for the Catch2 naming and tag audit."""

import tempfile
import unittest
from pathlib import Path

from ao.core import testaudit


class TestAuditTest(unittest.TestCase):
    def test_parse_test_cases_handles_multiline_invocations(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            path = Path(temp_dir) / "ExampleTest.cpp"
            path.write_text(
                "\n".join(
                    [
                        "#include <catch2/catch_test_macros.hpp>",
                        "",
                        'TEST_CASE("Thing - does work",',
                        '          "[core][unit][thing]")',
                        "{",
                        "}",
                        "",
                    ]
                ),
                encoding="utf-8",
            )

            cases = testaudit.parse_test_cases(path)

        self.assertEqual(len(cases), 1)
        self.assertEqual(cases[0].name, "Thing - does work")
        self.assertEqual(cases[0].tags, ("core", "unit", "thing"))

    def test_audit_accepts_project_style_name_and_tags(self):
        case = testaudit.TestCase(
            path=Path("/repo/test/unit/query/ParserTest.cpp"),
            line=12,
            name="Parser - rejects incomplete predicate",
            tags=("query", "unit", "parser"),
        )

        self.assertEqual(testaudit._audit_case(case), [])

    def test_audit_accepts_function_level_name(self):
        case = testaudit.TestCase(
            path=Path("/repo/test/unit/utility/Base64Test.cpp"),
            line=12,
            name="base64Decode rejects invalid padding",
            tags=("utility", "unit", "base64"),
        )

        self.assertEqual(testaudit._audit_case(case), [])

    def test_audit_accepts_hidden_kebab_case_tag(self):
        case = testaudit.TestCase(
            path=Path("/repo/test/integration/audio/WasapiProviderTest.cpp"),
            line=12,
            name="WasapiProvider - renders through a real endpoint",
            tags=("audio", "integration", "wasapi", ".manual"),
        )

        self.assertEqual(testaudit._audit_case(case), [])

    def test_audit_accepts_five_tags_only_for_concurrency_stress(self):
        case = testaudit.TestCase(
            path=Path("/repo/test/unit/runtime/AsyncRuntimeTest.cpp"),
            line=12,
            name="AsyncRuntime - cancellation races safely with timer expiry",
            tags=("runtime", "regression", "async", "concurrency", "stress"),
        )

        self.assertEqual(testaudit._audit_case(case), [])

    def test_audit_requires_stress_to_follow_concurrency(self):
        case = testaudit.TestCase(
            path=Path("/repo/test/unit/runtime/AsyncRuntimeTest.cpp"),
            line=12,
            name="AsyncRuntime - cancellation races safely with timer expiry",
            tags=("runtime", "regression", "async", "stress"),
        )

        issues = testaudit._audit_case(case)

        self.assertEqual(
            [(issue.kind, issue.message) for issue in issues],
            [("tag-order", "[stress] must be the final tag and immediately follow [concurrency]")],
        )

    def test_audit_reports_legacy_name_and_tag_drift(self):
        case = testaudit.TestCase(
            path=Path("/repo/test/unit/audio/EngineTest.cpp"),
            line=4,
            name="Engine Basic Playback",
            tags=("playback", "audio", "engine", "slow", "dryrun"),
        )

        issues = testaudit._audit_case(case)

        self.assertEqual(
            [(issue.kind, issue.message) for issue in issues],
            [
                (
                    "name",
                    'test name should use "Component - behavior" or the documented function-level form',
                ),
                ("tag-order", "first tag should be a known layer tag, got [playback]"),
                ("tag-order", "second tag should be a known test type tag, got [audio]"),
                (
                    "tag-count",
                    "prefer three or four tags; five are reserved for a final [concurrency][stress] pair",
                ),
            ],
        )

    def test_resolve_files_ignores_lint_fixtures(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            real = root / "test" / "unit" / "core" / "RealTest.cpp"
            fixture = root / "test" / "integration" / "lint" / "fixture" / "aobus-check" / "BasicTest.cpp"
            real.parent.mkdir(parents=True)
            fixture.parent.mkdir(parents=True)
            real.touch()
            fixture.touch()

            files = testaudit.resolve_files([], root)

        self.assertEqual(files, [real])


if __name__ == "__main__":
    unittest.main()
