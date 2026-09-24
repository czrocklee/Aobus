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

    def test_audit_accepts_winui_frontend_layer(self):
        case = testaudit.TestCase(
            path=Path("/repo/test/unit/winui/layout/ThemeSurfaceTest.cpp"),
            line=12,
            name="ThemeSurface - authored slots resolve through the Windows theme",
            tags=("winui", "unit", "theme"),
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

    def test_audit_accepts_independent_metadata_without_count_or_order_limits(self):
        case = testaudit.TestCase(
            path=Path("/repo/test/unit/runtime/AsyncRuntimeTest.cpp"),
            line=12,
            name="AsyncRuntime - cancellation races safely with timer expiry",
            tags=("runtime", "unit", "async", "stress", "concurrency", "timer"),
        )

        self.assertEqual(testaudit._audit_case(case), [])

    def test_audit_requires_stress_to_include_concurrency(self):
        case = testaudit.TestCase(
            path=Path("/repo/test/unit/runtime/AsyncRuntimeTest.cpp"),
            line=12,
            name="AsyncRuntime - cancellation races safely with timer expiry",
            tags=("runtime", "unit", "async", "stress"),
        )

        issues = testaudit._audit_case(case)

        self.assertEqual(
            [(issue.kind, issue.message) for issue in issues],
            [("tag-relation", "[stress] requires [concurrency]")],
        )

    def test_audit_rejects_retired_regression_tag(self):
        case = testaudit.TestCase(
            path=Path("/repo/test/unit/runtime/AsyncRuntimeTest.cpp"),
            line=12,
            name="AsyncRuntime - cancellation races safely with timer expiry",
            tags=("runtime", "unit", "async", "regression"),
        )

        issues = testaudit._audit_case(case)

        self.assertEqual(
            [(issue.kind, issue.message) for issue in issues],
            [("tag-retired", "tag [regression] is retired")],
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
                ("tag-order", "second tag must be [unit] or [integration]"),
                ("tag-scope", "include exactly one [unit] or [integration] scope"),
            ],
        )

    def test_scope_and_component_cannot_be_replaced_by_metadata(self):
        for tags, expected in (
            (("tui", "regression", "text"), {"tag-order", "tag-scope", "tag-retired"}),
            (("tui", "workflow", "text"), {"tag-order", "tag-scope", "tag-retired"}),
            (("tui", "smoke", "text"), {"tag-order", "tag-scope", "tag-retired"}),
            (("tui", "unit", "text", "integration"), {"tag-scope"}),
            (("tui", "unit", "regression"), {"tag-component", "tag-retired"}),
            (("tui", "unit", ".manual"), {"tag-component"}),
            (("tui", "unit"), {"tag-component"}),
            (("tui", "unit", "text", "text"), {"tag-duplicate"}),
        ):
            with self.subTest(tags=tags):
                case = testaudit.TestCase(Path("/repo/TextTest.cpp"), 1, "Text - preserves line breaks", tags)
                self.assertEqual({issue.kind for issue in testaudit._audit_case(case)}, expected)

    def test_parse_template_test_cases_preserves_scope(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            path = Path(temp_dir) / "ScalarTest.cpp"
            path.write_text(
                'TEMPLATE_TEST_CASE("Scalar - round trips", "[utility][unit][scalar][round-trip]", float, double) {}',
                encoding="utf-8",
            )
            cases = testaudit.parse_test_cases(path)
        self.assertEqual(len(cases), 1)
        self.assertEqual(cases[0].tags, ("utility", "unit", "scalar", "round-trip"))
        self.assertEqual(testaudit._audit_case(cases[0]), [])

    def test_repository_test_tags_follow_the_current_contract(self):
        issues = testaudit.audit_files(testaudit.discover_test_files())
        self.assertEqual(issues, [], "\n".join(issue.format() for issue in issues))

    def test_resolve_files_ignores_lint_fixtures(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            real = root / "test" / "unit" / "core" / "RealTest.cpp"
            fixture = root / "test" / "integration" / "lint" / "fixture" / "aobus-check" / "BasicTest.cpp"
            real.parent.mkdir(parents=True)
            fixture.parent.mkdir(parents=True)
            real.touch()
            fixture.touch()
            objective_cpp = real.with_suffix(".mm")
            objective_cpp.write_text('TEST_CASE("bad", "[unknown]") {}\n', encoding="utf-8")
            fixture.with_suffix(".mm").touch()

            files = testaudit.resolve_files([], root)
            self.assertEqual(testaudit.resolve_files([str(objective_cpp)], root), [objective_cpp])
            self.assertEqual(testaudit.resolve_files([str(root / "test")], root), [real, objective_cpp])
            issues = testaudit.audit_paths([str(objective_cpp)], root)
            self.assertEqual({issue.kind for issue in issues}, {"name", "tag-order", "tag-scope", "tag-component"})
            self.assertEqual({issue.path for issue in issues}, {objective_cpp})

        self.assertEqual(files, [real, objective_cpp])


if __name__ == "__main__":
    unittest.main()
