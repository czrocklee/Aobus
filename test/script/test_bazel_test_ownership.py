"""Declared test ownership and configured-target evidence; nothing here compiles or runs product tests."""

import tempfile
import unittest
from pathlib import Path

from ao.core.bazel_lint import QueryGraph, Rule
from ao.core.bazel_test_ownership import (
    ConfiguredTarget,
    discover_registration_sources,
    extract_compatibility,
    extract_configured_targets,
    ownership_errors,
    registration_owners,
)

_CHECKSUM = "a" * 64
_OTHER_CHECKSUM = "b" * 64
_SOURCE = "test/unit/ValueTest.cpp"


def _graph(**rules):
    """Declared rules as query XML reports them: kind plus label-valued attributes."""
    return QueryGraph(
        {
            label: Rule(label, kind, {name: tuple(values) for name, values in attributes.items()})
            for label, (kind, attributes) in rules.items()
        }
    )


_UNIT = "//test:unit"
_SUPPORT = "//test:support"
_BENCH = "//test:bench"
_BENCH_SOURCE = "test/perf/LoadBenchmark.cpp"
_GRAPH = _graph(
    **{
        _UNIT: ("cc_test", {"srcs": ("//test:unit/ValueTest.cpp",), "deps": (_SUPPORT,)}),
        _SUPPORT: ("cc_library", {"srcs": ("//test:unit/SharedTest.cpp",)}),
        _BENCH: ("cc_binary", {"srcs": ("//test:perf/LoadBenchmark.cpp",)}),
        "//test:sources": ("filegroup", {"srcs": ("//test:unit/OrphanTest.cpp",)}),
    }
)


class DeclaredOwnershipTest(unittest.TestCase):
    def test_owner_reaches_registered_sources_through_linked_libraries(self):
        sources = [_SOURCE, "test/unit/SharedTest.cpp"]
        self.assertEqual(registration_owners(_GRAPH, sources), [_UNIT])
        self.assertEqual(ownership_errors(_GRAPH, sources, reached=[_UNIT], executed=[_UNIT], incompatible=[]), [])

    def test_source_without_an_executable_owner_fails_even_inside_a_filegroup(self):
        self.assertEqual(
            ownership_errors(_GRAPH, ["test/unit/OrphanTest.cpp"], reached=[], executed=[], incompatible=[]),
            ["test/unit/OrphanTest.cpp: no declared executable owner"],
        )

    def test_built_test_outside_the_executed_selection_fails_and_an_incompatible_one_is_excused(self):
        self.assertEqual(
            ownership_errors(_GRAPH, [_SOURCE], reached=[_UNIT], executed=[], incompatible=[]),
            [f"{_UNIT}: test is not in the executed test selection"],
        )
        self.assertEqual(ownership_errors(_GRAPH, [_SOURCE], reached=[], executed=[], incompatible=[_UNIT]), [])

    def test_registration_binary_needs_only_to_be_built_by_the_gate(self):
        self.assertEqual(ownership_errors(_GRAPH, [_BENCH_SOURCE], reached=[_BENCH], executed=[], incompatible=[]), [])
        self.assertEqual(
            ownership_errors(_GRAPH, [_BENCH_SOURCE], reached=[], executed=[], incompatible=[]),
            [f"{_BENCH}: test executable is absent from the completion roots"],
        )


class ConfiguredTargetEvidenceTest(unittest.TestCase):
    def test_compatibility_requires_consistent_status_and_exact_configured_label_multiplicity(self):
        unit = ConfiguredTarget("//test:unit", _CHECKSUM)
        tool = ConfiguredTarget("//test:unit", _OTHER_CHECKSUM)
        native = ConfiguredTarget("//test:native", _CHECKSUM)
        expected = {unit, tool, native}
        rows = "@@//test:unit|False\n@@//test:native|True\n@@//test:unit|False\n"
        self.assertEqual(extract_compatibility(rows, expected), frozenset({native}))
        for invalid in (
            rows.replace("native|True", "native|maybe"),
            rows.replace("native|True", "unknown|True"),
            rows.replace("unit|False", "unit|True", 1),
            rows + "//test:unit|False\n",
            rows.rsplit("@@//test:unit|False", 1)[0],
            "",
        ):
            with self.subTest(invalid=invalid), self.assertRaisesRegex(ValueError, "compatibility"):
                extract_compatibility(invalid, expected)

    def test_modern_cquery_retains_configurations_ignoring_external_and_source_nodes(self):
        document = {
            "configurations": [{"id": 1, "checksum": _CHECKSUM}, {"id": 2, "checksum": _OTHER_CHECKSUM}],
            "results": [
                {"target": {"type": "RULE", "rule": {"name": "//test:unit"}}, "configurationId": identifier}
                for identifier in (1, 2)
            ]
            + [
                {"target": {"type": "SOURCE_FILE"}},
                {"target": {"type": "RULE", "rule": {"name": "@@sdk//:library", "ruleClass": "alias"}}},
            ],
        }
        self.assertEqual(
            extract_configured_targets(document),
            frozenset({ConfiguredTarget("//test:unit", _CHECKSUM), ConfiguredTarget("//test:unit", _OTHER_CHECKSUM)}),
        )
        document["results"].append(document["results"][0])
        with self.assertRaisesRegex(ValueError, "duplicate cquery"):
            extract_configured_targets(document)

    def test_alias_configured_record_is_rejected_instead_of_guessed_by_label(self):
        document = {
            "configurations": [{"id": 1, "checksum": _CHECKSUM}],
            "results": [
                {
                    "target": {"type": "RULE", "rule": {"name": "//test:alias", "ruleClass": "alias"}},
                    "configurationId": 1,
                }
            ],
        }
        with self.assertRaisesRegex(ValueError, "exclude transparent alias"):
            extract_configured_targets(document)

    def test_constraint_metadata_leaks_fail_closed_without_discarding_profile_named_compile_targets(self):
        for kind in ("constraint_setting", "constraint_value"):
            with self.subTest(kind=kind):
                rule = {"name": "//bazel:debug_profile", "ruleClass": kind}
                document = {
                    "configurations": [{"id": 1, "checksum": _CHECKSUM}],
                    "results": [
                        {"target": {"type": "RULE", "rule": rule}, "configurationId": 1},
                        {"target": {"type": "RULE", "rule": {"name": "@@sdk//:abi", "ruleClass": kind}}},
                    ],
                }
                with self.assertRaisesRegex(ValueError, "exclude native constraint metadata"):
                    extract_configured_targets(document)
                for ordinary_kind in ("cc_library", "constraint_value_wrapper", "profile_request_flag"):
                    rule["ruleClass"] = ordinary_kind
                    self.assertEqual(
                        extract_configured_targets(document),
                        frozenset({ConfiguredTarget("//bazel:debug_profile", _CHECKSUM)}),
                    )
                document["configurations"][0]["checksum"] = "short"
                with self.assertRaisesRegex(ValueError, "full configuration checksum"):
                    extract_configured_targets(document)


class RegistrationDiscoveryTest(unittest.TestCase):
    def test_discovery_uses_existing_lexer_including_guards_manual_and_benchmarks(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            test = root / "test"
            test.mkdir()
            (test / "GuardedTest.cpp").write_text(
                '#if defined(_WIN32)\nTEST_CASE("Native - registered", "[.manual]") {}\n#endif\n', encoding="utf-8"
            )
            (test / "TableBenchmark.cpp").write_text('TEST_CASE("Table - workload") {}\n', encoding="utf-8")
            (test / "FakeTest.cpp").write_text(
                '// TEST_CASE("Comment - ignored")\nconst auto x = R"_(TEST_CASE("Raw - ignored"))_";\n',
                encoding="utf-8",
            )
            sources = discover_registration_sources(root)
            self.assertEqual(set(sources), {"test/GuardedTest.cpp", "test/TableBenchmark.cpp"})
            self.assertEqual(sources["test/GuardedTest.cpp"][0].tags, (".manual",))

    def test_discovery_relates_sources_to_a_root_reached_through_a_link(self):
        with tempfile.TemporaryDirectory() as directory:
            real = Path(directory).resolve() / "real"
            (real / "test").mkdir(parents=True)
            (real / "test" / "LinkedTest.cpp").write_text('TEST_CASE("Linked - registered") {}\n', encoding="utf-8")
            alias = Path(directory) / "alias"
            try:
                alias.symlink_to(real, target_is_directory=True)
            except OSError:
                self.skipTest("the host cannot create a directory symlink")
            self.assertEqual(set(discover_registration_sources(alias)), {"test/LinkedTest.cpp"})

    def test_unsupported_registration_or_header_cannot_disappear_from_audit(self):
        for filename, source in (
            ("Hidden.h", 'TEST_CASE("Hidden - rejected") {}'),
            ("HiddenTest.cpp", 'TEST_CASE_METHOD(Fixture, "Hidden - rejected") {}'),
        ):
            with self.subTest(filename=filename), tempfile.TemporaryDirectory() as directory:
                root = Path(directory)
                (root / "test").mkdir()
                (root / "test" / filename).write_text(source, encoding="utf-8")
                with self.assertRaisesRegex(ValueError, "unsupported"):
                    discover_registration_sources(root)


if __name__ == "__main__":
    unittest.main()
