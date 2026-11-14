"""Tests for the bounded lexical registration detector and the tag audit."""

import contextlib
import tempfile
import unittest
from pathlib import Path

from ao.core import testaudit


class LexicalDetectionTest(unittest.TestCase):
    def _scan_text(
        self, body: str, name: str = "ExampleTest.cpp"
    ) -> tuple[list[testaudit.TestCase], list[testaudit.Issue]]:
        with tempfile.TemporaryDirectory() as temp_dir:
            path = Path(temp_dir) / name
            path.write_text(body, encoding="utf-8")
            return testaudit.scan_test_file(path)

    def test_parse_test_cases_handles_multiline_invocations(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            path = Path(temp_dir) / "ExampleTest.cpp"
            path.write_text(
                "\n".join(
                    [
                        "#include <catch2/catch_test_macros.hpp>",
                        "",
                        'TEST_CASE("Thing - does work",',
                        '          "[concurrency]")',
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
        self.assertEqual(cases[0].tags, ("concurrency",))
        self.assertEqual(cases[0].tags_literal, "[concurrency]")
        self.assertEqual(cases[0].line, 3)

    def test_ordinary_registration_without_tags_is_the_canonical_form(self):
        cases, issues = self._scan_text('TEST_CASE("Thing - does work") {}\n')

        self.assertEqual(issues, [])
        self.assertEqual(len(cases), 1)
        self.assertEqual(cases[0].tags, ())
        self.assertIsNone(cases[0].tags_literal)
        self.assertIsNone(cases[0].tags_span)
        self.assertEqual(testaudit._audit_case(cases[0]), [])

    def test_template_registration_keeps_the_mandatory_empty_tags_argument(self):
        cases, issues = self._scan_text('TEMPLATE_TEST_CASE("Scalar - round trips", "", float, double) {}\n')

        self.assertEqual(issues, [])
        self.assertEqual(len(cases), 1)
        self.assertTrue(cases[0].template)
        self.assertEqual(cases[0].tags, ())
        self.assertEqual(cases[0].tags_literal, "")
        self.assertEqual(testaudit._audit_case(cases[0]), [])

    def test_tags_span_covers_exactly_the_tags_literal(self):
        source = 'TEMPLATE_TEST_CASE(\n    "Scalar - round trips",\n    "",\n    float,\n    double) {}\n'
        cases, issues = self._scan_text(source)

        self.assertEqual(issues, [])
        self.assertEqual(len(cases), 1)
        start, end = cases[0].tags_span
        self.assertEqual(source[start:end], '""')

    def test_skips_comments_strings_chars_and_raw_strings(self):
        body = "\n".join(
            [
                '// TEST_CASE("commented", "[concurrency]")',
                '/* TEST_CASE("blocked", "[concurrency]") */',
                'auto const mentioned = "TEST_CASE(\\"quoted\\", \\"[concurrency]\\")";',
                "auto const character = '\"';",
                'auto const raw = R"(TEST_CASE("raw", "[concurrency]"))";',
                'auto const custom = R"delim(TEST_CASE("custom", "[concurrency]"))delim";',
                'auto const multiline = u8R"(',
                'TEST_CASE("multiline", "[concurrency]")',
                ')";',
                "",
            ]
        )
        cases, issues = self._scan_text(body)

        self.assertEqual(issues, [])
        self.assertEqual(cases, [])

    def test_digit_separators_and_encoded_char_literals_do_not_hide_registrations(self):
        body = "\n".join(
            [
                "constexpr auto kMask = 0x00FF'FFFFU;",
                "constexpr auto kCount = 1'000ms;",
                "constexpr auto kOpen = u8'(';",
                "constexpr auto kWide = L'a';",
                "auto const text = std::string{'x'}; // 1'000 in a comment",
                'TEST_CASE("Decoder - rejects malformed config", "[concurrency]") {}',
                "",
            ]
        )
        cases, issues = self._scan_text(body)

        self.assertEqual(issues, [])
        self.assertEqual([case.name for case in cases], ["Decoder - rejects malformed config"])

    def test_template_numeric_argument_keeps_following_registrations_detectable(self):
        body = "\n".join(
            [
                "TEMPLATE_TEST_CASE(",
                '    "Traits - carries a multi-separator constant",',
                '    "",',
                "    (std::integral_constant<unsigned long long, 0xDEAD'BEEF'CAFE>)) {}",
                'TEST_CASE("Traits - still detects the following case", "[concurrency]") {}',
                "",
            ]
        )
        cases, issues = self._scan_text(body)

        self.assertEqual(issues, [])
        self.assertEqual(
            [case.name for case in cases],
            ["Traits - carries a multi-separator constant", "Traits - still detects the following case"],
        )
        self.assertTrue(cases[0].template)
        self.assertEqual(cases[0].tags_literal, "")
        start, end = cases[0].tags_span
        self.assertEqual(body[start:end], '""')
        self.assertEqual(cases[1].line, 5)
        start, end = cases[1].tags_span
        self.assertEqual(body[start:end], '"[concurrency]"')

    def test_malformed_literal_argument_reports_and_keeps_scanning(self):
        # An unclosed ordinary literal inside an invocation is reported for
        # that invocation only; later legitimate cases stay detectable.
        body = "\n".join(
            [
                'TEST_CASE("Traits - rejects malformed tags", "unclosed tags',
                ") {}",
                'TEST_CASE("Traits - still finds the later case") {}',
                "",
            ]
        )
        cases, issues = self._scan_text(body)

        self.assertEqual([case.name for case in cases], ["Traits - still finds the later case"])
        self.assertEqual(len(issues), 1)
        self.assertEqual(issues[0].kind, "unsupported-syntax")
        self.assertEqual(issues[0].line, 1)
        self.assertIn("the tags argument must be a single string literal", issues[0].message)

    def test_comment_trivia_between_macro_and_parenthesis_still_registers(self):
        cases, issues = self._scan_text('TEST_CASE /* why */("Thing - does work") {}\n')

        self.assertEqual(issues, [])
        self.assertEqual([case.name for case in cases], ["Thing - does work"])

    def test_macro_mentions_without_invocations_are_ignored(self):
        cases, issues = self._scan_text('constexpr auto* kScenarioName = "SCENARIO";\nauto TEST_CASE = 0;\n')

        self.assertEqual(issues, [])
        self.assertEqual(cases, [])

    def test_unsupported_registration_families_fail_instead_of_skipping(self):
        # Names verified against the pinned Catch2 3.16.0 headers.
        for macro in (
            "CATCH_TEMPLATE_PRODUCT_TEST_CASE_METHOD_SIG",
            "CATCH_TEMPLATE_TEST_CASE",
            "CATCH_TEST_CASE",
            "CATCH_TEST_CASE_PERSISTENT_FIXTURE",
            "METHOD_AS_TEST_CASE",
            "REGISTER_TEST_CASE",
            "SCENARIO",
            "SCENARIO_METHOD",
            "TEMPLATE_LIST_TEST_CASE",
            "TEMPLATE_PRODUCT_TEST_CASE",
            "TEMPLATE_PRODUCT_TEST_CASE_METHOD_SIG",
            "TEMPLATE_PRODUCT_TEST_CASE_SIG",
            "TEMPLATE_TEST_CASE_METHOD",
            "TEMPLATE_TEST_CASE_METHOD_SIG",
            "TEMPLATE_TEST_CASE_SIG",
            "TEST_CASE_METHOD",
            "TEST_CASE_PERSISTENT_FIXTURE",
        ):
            with self.subTest(macro=macro):
                cases, issues = self._scan_text(f'{macro}("thing", "[concurrency]") {{}}\n')

                self.assertEqual(cases, [])
                self.assertEqual([issue.kind for issue in issues], ["unsupported-syntax"])
                self.assertIn(macro, issues[0].message)

    def test_expression_and_non_literal_arguments_fail(self):
        for invocation, message in (
            ("TEST_CASE(componentName) {}\n", "the name argument must be a single string literal"),
            ('TEST_CASE("a" "b") {}\n', "the name argument must be a single string literal"),
            ('TEST_CASE(R"(raw name)") {}\n', "raw string arguments are not supported"),
            ('TEST_CASE("name", tagLiteral) {}\n', "the tags argument must be a single string literal"),
            ('TEST_CASE("name") {}\n', None),
        ):
            with self.subTest(invocation=invocation):
                cases, issues = self._scan_text(invocation)

                if message is None:
                    self.assertEqual(issues, [])
                    self.assertEqual(len(cases), 1)
                    continue
                self.assertEqual(cases, [])
                self.assertIn(message, " ".join(issue.message for issue in issues))

    def test_comments_inside_invocations_fail(self):
        cases, issues = self._scan_text('TEST_CASE("Thing - does work", /* kept tags */ "[concurrency]") {}\n')

        self.assertEqual(cases, [])
        self.assertEqual([issue.kind for issue in issues], ["unsupported-syntax"])
        self.assertIn("comments inside a registration invocation", issues[0].message)

    def test_wrong_argument_counts_fail(self):
        for invocation, message in (
            ('TEST_CASE("name", "[concurrency]", "extra") {}\n', "TEST_CASE accepts a name literal"),
            ('TEMPLATE_TEST_CASE("name", "") {}\n', "TEMPLATE_TEST_CASE requires a name literal"),
            ('TEMPLATE_TEST_CASE("name") {}\n', "TEMPLATE_TEST_CASE requires a name literal"),
        ):
            with self.subTest(invocation=invocation):
                cases, issues = self._scan_text(invocation)

                self.assertEqual(cases, [])
                self.assertIn(message, " ".join(issue.message for issue in issues))

    def test_unterminated_invocation_fails(self):
        cases, issues = self._scan_text('TEST_CASE("name",\n')

        self.assertEqual(cases, [])
        self.assertIn("unterminated registration invocation", " ".join(issue.message for issue in issues))

    def test_registration_in_headers_and_fragments_fails_placement(self):
        for name in ("ExampleTest.h", "Example.def", "Include.hpp"):
            with self.subTest(name=name):
                with tempfile.TemporaryDirectory() as temp_dir:
                    path = Path(temp_dir) / name
                    path.write_text('TEST_CASE("Thing - does work", "[concurrency]") {}\n', encoding="utf-8")
                    issues = testaudit.audit_files([path])

                self.assertEqual([issue.kind for issue in issues], ["unsupported-placement"])
                self.assertIn("headers or include fragments", issues[0].message)

    def test_nonconventional_registration_filenames_fail_placement(self):
        for name in ("FooSupport.cpp", "FooCases.mm", "TrackTableBenchmark.mm"):
            with self.subTest(name=name), tempfile.TemporaryDirectory() as temporary:
                root = Path(temporary)
                path = root / "test" / "query" / name
                path.parent.mkdir(parents=True)
                path.write_text('TEST_CASE("Foo - preserves the contract") {}\n', encoding="utf-8")
                for selection in ([], ["test/query"], [f"test/query/{name}"]):
                    with self.subTest(selection=selection):
                        issues = testaudit.audit_paths(selection, root)
                        self.assertEqual([issue.kind for issue in issues], ["unsupported-placement"])
                        self.assertIn("*Benchmark.cpp", issues[0].message)

    def test_conventional_registration_filenames_and_registration_free_helpers_are_valid(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            folder = root / "test" / "query"
            folder.mkdir(parents=True)
            for name in ("FooTest.cpp", "FooTest.mm", "FooBenchmark.cpp"):
                (folder / name).write_text('TEST_CASE("Foo - preserves the contract") {}\n', encoding="utf-8")
            (folder / "FooSupport.cpp").write_text("void helper() {}\n", encoding="utf-8")
            self.assertEqual(testaudit.audit_paths([], root), [])

    def test_header_without_registrations_stays_clean(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            path = Path(temp_dir) / "ExampleTest.h"
            path.write_text(
                '// TEST_CASE("mentioned", "[concurrency]")\ninline constexpr auto kMention = "TEST_CASE";\n',
                encoding="utf-8",
            )
            self.assertEqual(testaudit.audit_files([path]), [])


class LineSpliceTest(unittest.TestCase):
    """Backslash-newline splices: honored lexically, unsupported inside invocations."""

    def _scan_text(
        self, body: str, name: str = "ExampleTest.cpp"
    ) -> tuple[list[testaudit.TestCase], list[testaudit.Issue]]:
        with tempfile.TemporaryDirectory() as temp_dir:
            path = Path(temp_dir) / name
            # Preserve authored CRLF instead of doubling CR in Windows text mode.
            path.write_bytes(body.encode("utf-8"))
            return testaudit.scan_test_file(path)

    def test_spliced_supported_invocations_fail_closed(self):
        # A splice in the macro name, between the macro and its parenthesis,
        # or inside an argument must be reported, never silently missed.
        for invocation in (
            'TEST_\\\nCASE("Thing - works") {}\n',
            'TEST_CASE\\\n("Thing - works") {}\n',
            'TEST_CASE \\\n("Thing - works") {}\n',
            'TEMPLATE_\\\nTEST_CASE("Thing - works", "", int) {}\n',
            'TEST_CASE("Thing \\\n- works") {}\n',
            'TEST_CASE("Thing - works", \\\n"[concurrency]") {}\n',
            'TEST_CASE("Thing - works", "[conc\\\nurrency]") {}\n',
            'TEST_CASE /* why \\\nhere */("Thing - works") {}\n',
        ):
            with self.subTest(invocation=invocation):
                cases, issues = self._scan_text(invocation)

                self.assertEqual(cases, [])
                self.assertEqual([issue.kind for issue in issues], ["unsupported-syntax"])
                self.assertEqual(issues[0].line, 1)
                self.assertIn("line splice", issues[0].message)

    def test_crlf_and_trailing_whitespace_splices_are_recognized(self):
        for invocation in (
            'TEST_\\ \r\nCASE("Thing - works") {}\r\n',
            'TEST_CASE \\\r\n("Thing - works") {}\r\n',
        ):
            with self.subTest(invocation=invocation):
                cases, issues = self._scan_text(invocation)

                self.assertEqual(cases, [])
                self.assertEqual([issue.kind for issue in issues], ["unsupported-syntax"])
                self.assertEqual(issues[0].line, 1)
                self.assertIn("line splice", issues[0].message)

        body = '#define K 1 \\\r\n    + 2\r\nTEST_CASE("Thing - works") {}\r\n'
        cases, issues = self._scan_text(body)

        self.assertEqual(issues, [])
        self.assertEqual([case.name for case in cases], ["Thing - works"])
        self.assertEqual(cases[0].line, 3)

    def test_corpus_tags_spans_slice_the_original_text(self):
        for path in testaudit.discover_test_files():
            source = path.read_text(encoding="utf-8")
            for case in testaudit.parse_test_cases(path):
                if case.tags_span is None:
                    continue
                start, end = case.tags_span
                sliced = source[start:end]
                self.assertEqual(sliced, f'"{case.tags_literal}"', f"{path}:{case.line}")


class TagContractTest(unittest.TestCase):
    def _case(self, name: str, tags: tuple[str, ...], *, template: bool = False, tags_literal: str | None):
        return testaudit.TestCase(
            path=Path("/repo/test/query/ParserTest.cpp"),
            line=12,
            name=name,
            tags=tags,
            template=template,
            tags_literal=tags_literal,
        )

    def test_audit_accepts_the_allowed_tags(self):
        for tags, literal in (
            (("concurrency",), "[concurrency]"),
            ((".manual",), "[.manual]"),
            (("concurrency", ".manual"), "[concurrency][.manual]"),
        ):
            with self.subTest(literal=literal):
                case = self._case("Parser - rejects incomplete predicate", tags, tags_literal=literal)

                self.assertEqual(testaudit._audit_case(case), [])

    def test_audit_accepts_untagged_and_template_registrations(self):
        cases = (
            self._case("Parser - rejects incomplete predicate", (), tags_literal=None),
            self._case(
                "Parser - round trips",
                (),
                template=True,
                tags_literal="",
            ),
        )

        for case in cases:
            self.assertEqual(testaudit._audit_case(case), [])

    def test_duplicates_and_incomplete_tag_strings_fail(self):
        for literal, kinds in (
            ("[concurrency][concurrency]", {"tag-duplicate"}),
            ("[concurrency] pace", {"tag"}),
            ("concurrency", {"tag"}),
            ("[]", {"tag"}),
            ("[concurrency][]", {"tag"}),
            ("", {"tag"}),
        ):
            with self.subTest(literal=literal):
                case = self._case(
                    "Parser - rejects incomplete predicate",
                    tuple(testaudit.TAG_RE.findall(literal)),
                    tags_literal=literal if literal else "",
                )

                issues = testaudit._audit_case(case)
                self.assertEqual({issue.kind for issue in issues}, kinds)

    def test_template_tags_argument_is_mandatory_and_may_carry_retained_tags(self):
        for tags, literal, expected in (
            ((), None, ["tag"]),
            ((), "", []),
            (("concurrency",), "[concurrency]", []),
            ((".manual",), "[.manual]", []),
            (("concurrency", ".manual"), "[concurrency][.manual]", []),
            (("stress",), "[stress]", ["tag"]),
            (("concurrency",), "[concurrency] junk", ["tag"]),
            ((), "[]", ["tag"]),
            (("concurrency",), "[concurrency][]", ["tag"]),
            (("concurrency", "concurrency"), "[concurrency][concurrency]", ["tag-duplicate"]),
        ):
            with self.subTest(literal=literal):
                case = self._case("Parser - round trips", tags, template=True, tags_literal=literal)

                issues = testaudit._audit_case(case)
                self.assertEqual([issue.kind for issue in issues], expected)
                if literal is None:
                    self.assertIn("must keep the mandatory empty tags argument", issues[0].message)

    def test_ordinary_empty_tags_argument_is_not_canonical(self):
        case = self._case("Parser - rejects incomplete predicate", (), tags_literal="")

        issues = testaudit._audit_case(case)

        self.assertEqual([issue.kind for issue in issues], ["tag"])
        self.assertIn("omit the tags argument", issues[0].message)


class NameContractTest(unittest.TestCase):
    def _case(self, name: str):
        return testaudit.TestCase(
            Path("/repo/test/query/ParserTest.cpp"), 12, name, ("concurrency",), tags_literal="[concurrency]"
        )

    def test_accepts_component_behavior_names(self):
        self.assertEqual(testaudit._audit_case(self._case("Parser - rejects incomplete predicate")), [])

    def test_accepts_function_level_names(self):
        self.assertEqual(testaudit._audit_case(self._case("base64Decode rejects invalid padding")), [])

    def test_reports_legacy_names(self):
        issues = testaudit._audit_case(self._case("Engine Basic Playback"))

        self.assertEqual(
            [(issue.kind, issue.message) for issue in issues],
            [
                (
                    "name",
                    'test name should use "Component - behavior" or the documented function-level form',
                )
            ],
        )


class DiscoveryRoutingTest(unittest.TestCase):
    def test_relative_source_roots_keep_all_routes_and_workload_policy_consistent(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary) / "configured-source"
            ordinary = root / "test" / "core" / "ExampleTest.cpp"
            workload = root / "test" / "perf" / "baseline" / "ExampleTest.cpp"
            for path in (ordinary, workload):
                path.parent.mkdir(parents=True, exist_ok=True)
            ordinary.write_text('TEST_CASE("Example - works") {}\n', encoding="utf-8")
            workload.write_text('TEST_CASE("Workload - works", "[concurrency]") {}\n', encoding="utf-8")
            expected = sorted(testaudit.absolute_path(path) for path in (ordinary, workload))
            with contextlib.chdir(temporary):
                relative_root = Path("configured-source")
                self.assertEqual(testaudit.discover_test_files(relative_root), expected)
                self.assertTrue(testaudit.is_audit_candidate("test/core/ExampleTest.cpp", relative_root))
                for selection in ([], ["test"], ["test/perf/baseline/ExampleTest.cpp"]):
                    with self.subTest(selection=selection):
                        issues = testaudit.audit_paths(selection, relative_root)
                        self.assertEqual([(issue.path, issue.kind) for issue in issues], [(expected[1], "tag")])
                self.assertEqual(testaudit.audit_paths(["test/core/ExampleTest.cpp"], relative_root), [])

    def test_discovery_is_content_driven_beyond_the_registration_filenames(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            benchmark = root / "test" / "tui" / "TrackTableBenchmark.cpp"
            ordinary = root / "test" / "query" / "ParserTest.cpp"
            header = root / "test" / "query" / "ParserTest.h"
            support = root / "test" / "helper" / "Probe.cpp"
            fixture = root / "test" / "lint" / "fixture" / "aobus-check" / "BasicTest.cpp"
            for path in (benchmark, ordinary, header, support):
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text('TEST_CASE("Thing - does work") {}\n', encoding="utf-8")
            fixture.parent.mkdir(parents=True)
            fixture.write_text('TEST_CASE("broken", "[unit]") {}\n', encoding="utf-8")

            discovered = testaudit.discover_test_files(root)
            self.assertEqual(discovered, [support, ordinary, header, benchmark])

            # The same detector feeds every route: directory and explicit-file
            # selections reach the benchmark source too.
            self.assertEqual(
                testaudit.resolve_files([str(root / "test" / "tui")], root),
                [benchmark],
            )
            self.assertEqual(testaudit.resolve_files([str(benchmark)], root), [benchmark])
            self.assertEqual(testaudit.audit_paths([str(benchmark)], root), [])

    def test_comment_trivia_registrations_reach_every_route(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            probed = root / "test" / "query" / "ParserTest.cpp"
            probed.parent.mkdir(parents=True)
            probed.write_text(
                'TEST_CASE /* name argument follows */("Parser - rejects incomplete predicate") {}\n',
                encoding="utf-8",
            )

            self.assertEqual(testaudit.resolve_files([str(probed)], root), [probed])
            self.assertEqual(testaudit.parse_test_cases(probed)[0].name, "Parser - rejects incomplete predicate")
            self.assertEqual(testaudit.audit_paths([str(root / "test" / "query")], root), [])

    def test_workload_regions_carry_no_tags(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            baseline = root / "test" / "perf" / "baseline" / "PerformanceBaselineTest.cpp"
            benchmark = root / "test" / "tui" / "TrackTableBenchmark.cpp"
            untagged = root / "test" / "perf" / "baseline" / "UntaggedWorkloadTest.cpp"
            report = root / "test" / "perf" / "report" / "PerformanceReportTest.cpp"
            for path in (baseline, benchmark, untagged, report):
                path.parent.mkdir(parents=True, exist_ok=True)
            baseline.write_text(
                'TEST_CASE("PerformanceBaseline - phase 0 baseline", "[perf][baseline]") {}\n', encoding="utf-8"
            )
            benchmark.write_text(
                'TEST_CASE("bench: flat build - full scales with N", "[concurrency]") {}\n', encoding="utf-8"
            )
            untagged.write_text('TEST_CASE("PerformanceBaseline - untimed work") {}\n', encoding="utf-8")
            # Report correctness is ordinary core, not a workload.
            report.write_text(
                'TEST_CASE("PerformanceReport - reports the sampled schema", "[concurrency]") {}\n',
                encoding="utf-8",
            )

            issues = testaudit.audit_paths([str(root / "test" / "perf"), str(benchmark)], root)

        self.assertEqual({(issue.path, issue.kind) for issue in issues}, {(baseline, "tag"), (benchmark, "tag")})
        self.assertTrue(all("carry no tags" in issue.message for issue in issues))

    def test_workload_benchmark_rule_does_not_match_a_nested_near_miss(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            path = root / "test" / "core" / "tui" / "TrackTableBenchmark.cpp"
            path.parent.mkdir(parents=True)
            path.write_text('TEST_CASE("TrackTable - controlled contract", "[concurrency]") {}\n', encoding="utf-8")
            self.assertEqual(testaudit.audit_paths([str(path)], root), [])

    def test_benchmark_registrations_are_audited_for_tags(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            benchmark = root / "test" / "tui" / "TrackTableBenchmark.cpp"
            benchmark.parent.mkdir(parents=True)
            benchmark.write_text(
                'TEST_CASE("bench: flat build - full scales with N", "[benchmark][tui][track-table]") {}\n',
                encoding="utf-8",
            )

            issues = testaudit.audit_paths([str(benchmark)], root)

        self.assertEqual({issue.kind for issue in issues}, {"tag"})
        self.assertEqual({issue.path for issue in issues}, {benchmark})
        self.assertIn("carry no tags", issues[0].message)

    def test_is_audit_candidate_routes_hygiene_selections(self):
        for name, expected in (
            ("test/query/ParserTest.cpp", True),
            ("test/query/ParserTest.h", True),
            ("test/query/ParserTest.CPP", False),
            ("test/query/ParserTest.HPP", False),
            ("test/query/ParserTest.hh", False),
            ("test/i18n/MessageInventory.def", True),
            ("test/tui/TrackTableBenchmark.cpp", True),
            ("test/lint/fixture/aobus-check/BasicTest.cpp", False),
            ("lib/audio/Player.cpp", False),
            ("script/ao/core/testaudit.py", False),
        ):
            with self.subTest(name=name):
                self.assertIs(testaudit.is_audit_candidate(name, Path("/repo")), expected)

    def test_repository_test_tags_follow_the_current_contract(self):
        issues = testaudit.audit_files(testaudit.discover_test_files())
        self.assertEqual(issues, [], "\n".join(issue.format() for issue in issues))


if __name__ == "__main__":
    unittest.main()
