"""Fixture tests for the public concept-report denominator."""

from __future__ import annotations

import shutil
import tempfile
import unittest
from pathlib import Path

from ao.core import concept_report, concept_scope


def _decl(
    kind: str,
    name: str,
    header: str = "include/ao/a.h",
    area: str = "core",
    signature: str = "",
) -> concept_report.PublicDeclaration:
    return concept_report.PublicDeclaration(kind, name, header, area, 1, signature)


def _record(
    name: str,
    *,
    tag: str = "class",
    file: str = "/tmp/root/include/ao/a.h",
    line: int = 1,
    complete: bool = True,
    implicit: bool = False,
    bases: list[dict[str, object]] | None = None,
    inner: list[dict[str, object]] | None = None,
) -> dict[str, object]:
    node: dict[str, object] = {
        "kind": "CXXRecordDecl",
        "name": name,
        "tagUsed": tag,
        "isImplicit": implicit,
        "loc": {"file": file, "line": line},
    }
    if complete:
        node["completeDefinition"] = True
    if bases:
        node["bases"] = bases
    if inner:
        node["inner"] = inner
    return node


def _tu(*inner: dict[str, object]) -> dict[str, object]:
    return {"kind": "TranslationUnitDecl", "inner": list(inner)}


def _namespace(name: str, *inner: dict[str, object], file: str = "/tmp/root/include/ao/a.h") -> dict[str, object]:
    return {
        "kind": "NamespaceDecl",
        "name": name,
        "loc": {"file": file, "line": 1},
        "inner": list(inner),
    }


class ConceptScopeTest(unittest.TestCase):
    def test_scope_roots_match_the_rfc_measurement_contract(self):
        self.assertEqual(concept_scope.CORE_ROOTS, ("include/ao",))
        self.assertEqual(concept_scope.APPLICATION_ROOTS, ("app/include/ao",))
        self.assertEqual(concept_scope.validate_construction_chains(), [])

    def test_construction_hops_are_crossed_api_boundaries(self):
        cache = next(chain for chain in concept_scope.CONSTRUCTION_CHAINS if chain.leaf == "rt.ResourceByteMemoryCache")
        self.assertEqual(cache.hops, 1)
        self.assertEqual(cache.steps, ("ResourceByteMemoryCache(async::Runtime&, ReadBytes)",))
        activity = next(
            chain for chain in concept_scope.CONSTRUCTION_CHAINS if chain.leaf == "uimodel.ActivityStatusFeedProjection"
        )
        self.assertEqual(activity.hops, 1)
        self.assertEqual(
            activity.steps,
            ("ActivityStatusFeedProjection(MessageCatalog, NotificationFeedState)",),
        )

    def test_header_discovery_stays_inside_the_frozen_roots(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            keep = root / "include" / "ao" / "utility" / "Keep.h"
            skip = root / "lib" / "utility" / "Skip.h"
            app = root / "app" / "include" / "ao" / "rt" / "App.h"
            private = root / "app" / "runtime" / "Hidden.h"
            for path in (keep, skip, app, private):
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text("class X {};\n", encoding="utf-8")

            headers = concept_scope.public_headers(root)

        self.assertEqual(
            [path.relative_to(root).as_posix() for path in headers["core"]],
            ["include/ao/utility/Keep.h"],
        )
        self.assertEqual(
            [path.relative_to(root).as_posix() for path in headers["application"]],
            ["app/include/ao/rt/App.h"],
        )


class ConceptAstTest(unittest.TestCase):
    def setUp(self):
        self.root = Path("/tmp/root")
        self.header = self.root / "include" / "ao" / "a.h"

    def extract(self, ast: dict[str, object], path: Path | None = None) -> concept_report.HeaderParse:
        return concept_report.extract_header(ast, path or self.header, "core", self.root)

    def test_counts_public_types_and_free_functions_in_the_main_file(self):
        ast = _tu(
            _namespace(
                "ao",
                _record("Foo"),
                {
                    "kind": "EnumDecl",
                    "name": "Kind",
                    "loc": {"file": str(self.header), "line": 4},
                },
                {
                    "kind": "TypeAliasDecl",
                    "name": "Alias",
                    "loc": {"file": str(self.header), "line": 5},
                    "type": {"qualType": "int"},
                },
                {
                    "kind": "FunctionDecl",
                    "name": "run",
                    "loc": {"file": str(self.header), "line": 6},
                    "type": {"qualType": "void ()"},
                },
            )
        )

        parsed = self.extract(ast)
        names = {(item.kind, item.name) for item in parsed.declarations}

        self.assertEqual(
            names,
            {
                ("class", "ao::Foo"),
                ("enum", "ao::Kind"),
                ("alias", "ao::Alias"),
                ("function", "ao::run"),
            },
        )

    def test_excludes_implicit_injected_class_names_and_anonymous_namespaces(self):
        ast = _tu(
            _namespace(
                "ao",
                _record("Foo"),
                _record("Foo", implicit=True),
                {
                    "kind": "NamespaceDecl",
                    "name": "",
                    "loc": {"file": str(self.header), "line": 8},
                    "inner": [_record("Hidden", file=str(self.header), line=9)],
                },
            )
        )

        parsed = self.extract(ast)

        self.assertEqual([item.name for item in parsed.declarations], ["ao::Foo"])

    def test_counts_public_methods_and_excludes_private_nested_types(self):
        ast = _tu(
            _namespace(
                "ao",
                _record(
                    "Owner",
                    inner=[
                        {"kind": "AccessSpecDecl", "access": "public"},
                        _record("PublicNested", line=4),
                        {
                            "kind": "CXXMethodDecl",
                            "name": "work",
                            "loc": {"file": str(self.header), "line": 5},
                            "type": {"qualType": "void ()"},
                        },
                        {"kind": "AccessSpecDecl", "access": "private"},
                        _record("Secret", line=7),
                    ],
                ),
            )
        )

        parsed = self.extract(ast)

        self.assertEqual(
            [item.name for item in parsed.declarations],
            ["ao::Owner", "ao::Owner::PublicNested", "ao::Owner::work"],
        )

    def test_counts_a_member_function_template_once_as_a_method(self):
        ast = _tu(
            _namespace(
                "ao",
                _record(
                    "Owner",
                    inner=[
                        {"kind": "AccessSpecDecl", "access": "public"},
                        {
                            "kind": "FunctionTemplateDecl",
                            "name": "convert",
                            "loc": {"file": str(self.header), "line": 4},
                            "inner": [
                                {
                                    "kind": "CXXMethodDecl",
                                    "name": "convert",
                                    "loc": {"file": str(self.header), "line": 5},
                                    "type": {"qualType": "void (T)"},
                                    "inner": [{"kind": "ParmVarDecl", "type": {"qualType": "T"}}],
                                }
                            ],
                        },
                    ],
                ),
            )
        )

        parsed = self.extract(ast)
        methods = [item for item in parsed.declarations if item.kind == "method"]

        self.assertEqual([(item.name, item.signature) for item in methods], [("ao::Owner::convert", "void (T)")])
        self.assertFalse(
            any(item.kind == "function" and item.name.endswith("::convert") for item in parsed.declarations)
        )

    def test_counts_each_overload_and_records_the_treatment(self):
        ast = _tu(
            _namespace(
                "ao",
                {
                    "kind": "FunctionDecl",
                    "name": "g",
                    "loc": {"file": str(self.header), "line": 2},
                    "type": {"qualType": "void ()"},
                },
                {
                    "kind": "FunctionDecl",
                    "name": "g",
                    "loc": {"file": str(self.header), "line": 3},
                    "type": {"qualType": "void (int)"},
                },
            )
        )
        parsed = self.extract(ast)
        functions = [item for item in parsed.declarations if item.kind == "function"]

        self.assertEqual(len(functions), 2)
        self.assertEqual({item.signature for item in functions}, {"void ()", "void (int)"})
        self.assertIn("overload", concept_scope.OVERLOAD_TREATMENT)

    def test_dependency_edges_keep_overload_source_identity(self):
        ast = _tu(
            _namespace(
                "ao",
                _record(
                    "Bar",
                    inner=[
                        {"kind": "AccessSpecDecl", "access": "public"},
                        {
                            "kind": "CXXConstructorDecl",
                            "name": "Bar",
                            "loc": {"file": str(self.header), "line": 3},
                            "type": {"qualType": "void (int)"},
                            "inner": [{"kind": "ParmVarDecl", "type": {"qualType": "int"}}],
                        },
                    ],
                ),
                {
                    "kind": "FunctionDecl",
                    "name": "take",
                    "loc": {"file": str(self.header), "line": 4},
                    "type": {"qualType": "void (Bar)"},
                    "inner": [{"kind": "ParmVarDecl", "type": {"qualType": "Bar"}}],
                },
                {
                    "kind": "FunctionDecl",
                    "name": "take",
                    "loc": {"file": str(self.header), "line": 5},
                    "type": {"qualType": "void (const Bar &)"},
                    "inner": [{"kind": "ParmVarDecl", "type": {"qualType": "const Bar &"}}],
                },
            )
        )

        parsed = self.extract(ast)
        edges = concept_report.resolve_edges([parsed], parsed.declarations)
        take_edges = [edge for edge in edges if edge.source == "ao::take"]

        self.assertEqual(len(take_edges), 2)
        self.assertEqual({edge.source_signature for edge in take_edges}, {"void (Bar)", "void (const Bar &)"})

    def test_member_owner_edge_makes_member_and_free_function_dependency_counts_symmetric(self):
        member_ast = _tu(
            _namespace(
                "ao",
                _record(
                    "Formatter",
                    inner=[
                        {"kind": "AccessSpecDecl", "access": "public"},
                        {
                            "kind": "CXXMethodDecl",
                            "name": "format",
                            "loc": {"file": str(self.header), "line": 4},
                            "type": {"qualType": "void ()"},
                        },
                    ],
                ),
            )
        )
        free_ast = _tu(
            _namespace(
                "ao",
                _record("Formatter"),
                {
                    "kind": "FunctionDecl",
                    "name": "format",
                    "loc": {"file": str(self.header), "line": 4},
                    "type": {"qualType": "void (const Formatter &)"},
                    "inner": [{"kind": "ParmVarDecl", "type": {"qualType": "const Formatter &"}}],
                },
            )
        )

        member = self.extract(member_ast)
        free = self.extract(free_ast)
        member_edges = concept_report.resolve_edges([member], member.declarations)
        free_edges = concept_report.resolve_edges([free], free.declarations)

        self.assertEqual([(edge.target, edge.reason) for edge in member_edges], [("ao::Formatter", "owner")])
        self.assertEqual([(edge.target, edge.reason) for edge in free_edges], [("ao::Formatter", "parameter")])

    def test_external_qualified_types_do_not_alias_project_simple_names(self):
        ast = _tu(
            _namespace(
                "ao",
                _record(
                    "Owner",
                    inner=[
                        {"kind": "AccessSpecDecl", "access": "public"},
                        {
                            "kind": "CXXMethodDecl",
                            "name": "vector",
                            "loc": {"file": str(self.header), "line": 4},
                            "type": {"qualType": "void ()"},
                        },
                    ],
                ),
                {
                    "kind": "FunctionDecl",
                    "name": "take",
                    "loc": {"file": str(self.header), "line": 7},
                    "type": {"qualType": "void (std::vector<int>)"},
                    "inner": [{"kind": "ParmVarDecl", "type": {"qualType": "std::vector<int>"}}],
                },
            )
        )

        parsed = self.extract(ast)
        edges = concept_report.resolve_edges([parsed], parsed.declarations)

        self.assertFalse(any(edge.source == "ao::take" for edge in edges))

    def test_concatenating_headers_does_not_reduce_the_declaration_count(self):
        first = self.extract(_tu(_namespace("ao", _record("Alpha"))))
        second_header = self.root / "include" / "ao" / "b.h"
        second = concept_report.extract_header(
            _tu(_namespace("ao", _record("Beta", file=str(second_header)), file=str(second_header))),
            second_header,
            "core",
            self.root,
        )
        merged_header = self.root / "include" / "ao" / "merged.h"
        merged = concept_report.extract_header(
            _tu(
                _namespace(
                    "ao",
                    _record("Alpha", file=str(merged_header)),
                    _record("Beta", file=str(merged_header), line=4),
                    file=str(merged_header),
                )
            ),
            merged_header,
            "core",
            self.root,
        )

        split = concept_report._dedup_declarations([first, second])
        combined = concept_report._dedup_declarations([merged])

        self.assertEqual(len(split), 2)
        self.assertEqual(len(combined), 2)

    def test_public_bases_fields_aliases_and_parameters_become_edges(self):
        ast = _tu(
            _namespace(
                "ao",
                _record("Bar"),
                _record(
                    "Foo",
                    bases=[{"access": "public", "writtenAccess": "public", "type": {"qualType": "Bar"}}],
                    inner=[
                        {"kind": "AccessSpecDecl", "access": "public"},
                        {
                            "kind": "FieldDecl",
                            "name": "member",
                            "type": {"qualType": "Bar"},
                            "loc": {"file": str(self.header), "line": 8},
                        },
                        {
                            "kind": "TypeAliasDecl",
                            "name": "Alias",
                            "type": {"qualType": "Bar"},
                            "loc": {"file": str(self.header), "line": 9},
                        },
                    ],
                ),
                {
                    "kind": "FunctionDecl",
                    "name": "take",
                    "loc": {"file": str(self.header), "line": 12},
                    "type": {"qualType": "Foo (const Bar &)"},
                    "inner": [{"kind": "ParmVarDecl", "type": {"qualType": "const Bar &"}}],
                },
            )
        )
        parsed = self.extract(ast)
        edges = concept_report.resolve_edges([parsed], parsed.declarations)
        pairs = {(edge.source, edge.target) for edge in edges}
        reasons = {(edge.source, edge.target, edge.reason) for edge in edges}

        self.assertIn(("ao::Foo", "ao::Bar"), pairs)
        self.assertIn(("ao::Foo::Alias", "ao::Bar", "alias"), reasons)
        self.assertIn(("ao::take", "ao::Bar", "parameter"), reasons)
        self.assertIn(("ao::take", "ao::Foo", "return"), reasons)
        self.assertTrue({"base", "field"} & {edge.reason for edge in edges if edge.source == "ao::Foo"})

    def test_private_fields_do_not_create_edges(self):
        ast = _tu(
            _namespace(
                "ao",
                _record("Bar"),
                _record(
                    "Foo",
                    inner=[
                        {"kind": "AccessSpecDecl", "access": "private"},
                        {
                            "kind": "FieldDecl",
                            "name": "hidden",
                            "type": {"qualType": "Bar"},
                            "loc": {"file": str(self.header), "line": 6},
                        },
                    ],
                ),
            )
        )
        parsed = self.extract(ast)
        edges = concept_report.resolve_edges([parsed], parsed.declarations)

        self.assertEqual(edges, [])

    def test_records_default_constructible_bind_unbind_types_and_aggregates(self):
        ast = _tu(
            _namespace(
                "ao",
                _record(
                    "Loader",
                    inner=[
                        {"kind": "AccessSpecDecl", "access": "public"},
                        {
                            "kind": "CXXConstructorDecl",
                            "name": "Loader",
                            "isImplicit": True,
                            "loc": {"file": str(self.header), "line": 3},
                        },
                        {
                            "kind": "CXXMethodDecl",
                            "name": "bind",
                            "loc": {"file": str(self.header), "line": 4},
                            "type": {"qualType": "void ()"},
                        },
                        {
                            "kind": "CXXMethodDecl",
                            "name": "unbind",
                            "loc": {"file": str(self.header), "line": 5},
                            "type": {"qualType": "void ()"},
                        },
                    ],
                ),
                _record(
                    "UiDependencies",
                    tag="struct",
                    inner=[
                        {
                            "kind": "FieldDecl",
                            "name": "one",
                            "type": {"qualType": "int"},
                            "loc": {"file": str(self.header), "line": 10},
                        },
                        {
                            "kind": "FieldDecl",
                            "name": "two",
                            "type": {"qualType": "int"},
                            "loc": {"file": str(self.header), "line": 11},
                        },
                    ],
                ),
            )
        )
        parsed = self.extract(ast)

        self.assertEqual(parsed.bind_unbind, ["ao::Loader"])
        self.assertEqual(parsed.aggregates[0]["name"], "ao::UiDependencies")
        self.assertEqual(parsed.aggregates[0]["fields"], 2)


class ConceptMetricTest(unittest.TestCase):
    def test_percentile_uses_nearest_rank(self):
        self.assertEqual(concept_report.percentile([], 50), 0)
        self.assertEqual(concept_report.percentile([1, 2, 3, 4], 50), 2)
        self.assertEqual(concept_report.percentile([1, 2, 3, 4], 95), 4)
        self.assertEqual(concept_report.percentile([10], 95), 10)

    def test_fan_out_statistics_use_the_header_cohort(self):
        fan_out = {
            "include/ao/a.h": 2,
            "include/ao/b.h": 10,
            "include/ao/c.h": 4,
        }
        stats = concept_report._fan_out_stats(fan_out, list(fan_out))
        self.assertEqual(stats["headers"], 3)
        self.assertEqual(stats["medianTranslationUnits"], 4)
        self.assertEqual(stats["p95TranslationUnits"], 10)

    def test_fan_out_stats_do_not_weight_a_header_by_its_declaration_count(self):
        fan_out = {"include/ao/a.h": 2, "include/ao/b.h": 10}

        stats = concept_report._fan_out_stats(
            fan_out,
            ["include/ao/a.h", "include/ao/a.h", "include/ao/b.h"],
        )

        self.assertEqual(stats["headers"], 2)
        self.assertEqual(stats["medianTranslationUnits"], 2)

    def test_fan_out_rows_preserve_per_header_evidence_and_area(self):
        rows = concept_report._fan_out_rows(
            {"include/ao/a.h": 2, "app/include/ao/b.h": 7},
            {"core": ["include/ao/a.h"], "application": ["app/include/ao/b.h"]},
        )

        self.assertEqual(
            rows,
            [
                {"header": "include/ao/a.h", "area": "core", "translationUnits": 2},
                {"header": "app/include/ao/b.h", "area": "application", "translationUnits": 7},
            ],
        )

    def test_source_aggregate_inventory_covers_collaborator_bags_and_skips_forwards(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            header = root / "app" / "frontend" / "LayoutBuildContext.h"
            header.parent.mkdir(parents=True)
            header.write_text(
                "\n".join(
                    [
                        "struct LayoutBuildContext;",
                        "struct LayoutBuildContext final {",
                        "  int one;",
                        "  std::function<void(int)> callback{};",
                        "  void render() const;",
                        "};",
                        "",
                    ]
                ),
                encoding="utf-8",
            )

            collaborators = root / "app" / "frontend" / "ShellCollaborators.h"
            collaborators.write_text(
                "\n".join(
                    [
                        "struct ShellCollaborators final {",
                        "  Cache* cache = nullptr;",
                        "  Loader* loader = nullptr;",
                        "};",
                        "",
                    ]
                ),
                encoding="utf-8",
            )

            inventory = concept_report._source_aggregate_inventory(root)

        self.assertEqual(
            inventory,
            [
                {
                    "name": "LayoutBuildContext",
                    "kind": "Context",
                    "fields": 2,
                    "header": "app/frontend/LayoutBuildContext.h",
                },
                {
                    "name": "ShellCollaborators",
                    "kind": "Collaborators",
                    "fields": 2,
                    "header": "app/frontend/ShellCollaborators.h",
                },
            ],
        )

    def test_header_includes_are_reported_separately_from_dependency_edges(self):
        parsed = concept_report.HeaderParse(
            header="include/ao/a.h",
            area="core",
            declarations=[_decl("class", "ao::Foo")],
            pending_edges=[concept_report.PendingEdge("ao::Foo", "ao::Bar", "field")],
            project_headers=["include/ao/a.h", "include/ao/b.h"],
            include_bytes=12,
        )
        other = concept_report.HeaderParse(
            header="include/ao/b.h",
            area="core",
            declarations=[_decl("class", "ao::Bar", "include/ao/b.h")],
        )
        declarations = concept_report._dedup_declarations([parsed, other])
        edges = concept_report.resolve_edges([parsed, other], declarations)
        includes = concept_report._include_edges([parsed, other])

        self.assertEqual([(edge.source, edge.target) for edge in edges], [("ao::Foo", "ao::Bar")])
        self.assertEqual(includes[0]["includes"], ["include/ao/b.h"])
        self.assertNotEqual(
            {(item["header"], tuple(item["includes"])) for item in includes},
            {(edge.source, (edge.target,)) for edge in edges},
        )


class ConceptReportAssemblyTest(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        core = self.root / "include/ao/A.h"
        application = self.root / "app/include/ao/B.h"
        for path in (core, application):
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(b"class;\n")
        self.headers = {"core": (core,), "application": (application,)}
        self.parses = [
            concept_report.HeaderParse(
                "include/ao/A.h",
                "core",
                declarations=[_decl("class", "ao::A", "include/ao/A.h")],
                project_headers=["include/ao/A.h"],
                include_bytes=7,
            ),
            concept_report.HeaderParse(
                "app/include/ao/B.h",
                "application",
                declarations=[_decl("class", "ao::B", "app/include/ao/B.h", "application")],
                pending_edges=[concept_report.PendingEdge("ao::B", "ao::A", "field")],
                project_headers=["app/include/ao/B.h", "include/ao/A.h"],
                include_bytes=14,
            ),
        ]
        self.fan_out = {"include/ao/A.h": 2, "app/include/ao/B.h": 0}

    def assemble(self, **changes):
        arguments = dict(
            headers=self.headers,
            parses=self.parses,
            fan_out=self.fan_out,
            provenance={"nativeMethod": "fresh compiler observation"},
            root=self.root,
            chains=[],
        )
        arguments.update(changes)
        return concept_report.assemble_report(self.root / "tree", **arguments)

    def test_schema_four_retains_literal_concept_vector_and_honest_fan_out_name(self):
        report = self.assemble()
        self.assertEqual(report["schemaVersion"], 4)
        self.assertEqual(report["primary"]["core"]["declarations"], 1)
        self.assertEqual(report["primary"]["application"]["dependencyEdges"], 1)
        self.assertEqual(report["primary"]["application"]["includeWeight"]["medianHeaders"], 2)
        self.assertEqual(report["primary"]["application"]["includeWeight"]["medianBytes"], 14)
        self.assertEqual(
            report["primary"]["core"]["compilerIncludeFanOut"],
            {"medianTranslationUnits": 2, "p95TranslationUnits": 2, "headers": 1},
        )
        self.assertEqual(
            report["headerFanOut"],
            [
                {"header": "include/ao/A.h", "area": "core", "translationUnits": 2},
                {"header": "app/include/ao/B.h", "area": "application", "translationUnits": 0},
            ],
        )
        self.assertNotIn("rebuildFanOut", report["primary"]["core"])
        self.assertIn("compiler include fan-out", concept_report.format_summary(report))
        self.assertIn("not last-build", report["scope"]["compilerIncludeFanOut"])

    def test_missing_observation_is_not_a_zero_consumer_header(self):
        with self.assertRaisesRegex(ValueError, "exact public-header denominator"):
            self.assemble(fan_out={"include/ao/A.h": 2})

    def test_added_public_header_cannot_be_silently_omitted(self):
        (self.root / "include/ao/New.h").write_bytes(b"// new\n")
        with self.assertRaisesRegex(ValueError, "complete public-header areas"):
            self.assemble()


class ConceptClangTest(unittest.TestCase):
    def test_live_clang_dump_matches_the_fixture_walker(self):
        clang = shutil.which("clang")
        if clang is None:
            self.skipTest("clang is not on PATH")

        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            header = root / "include" / "ao" / "Probe.h"
            header.parent.mkdir(parents=True)
            header.write_text(
                "\n".join(
                    [
                        "namespace ao {",
                        "class Bar {};",
                        "class Foo : public Bar {",
                        "public:",
                        "  Bar member;",
                        "  using Alias = Bar;",
                        "private:",
                        "  class Hidden {};",
                        "  Bar hidden;",
                        "};",
                        "enum class Kind { A };",
                        "void g();",
                        "void g(int);",
                        "}",
                        "",
                    ]
                ),
                encoding="utf-8",
            )
            ast = concept_report.dump_ast(clang, ["-std=c++26"], header)
            parsed = concept_report.extract_header(ast, header, "core", root)
            names = {(item.kind, item.name) for item in parsed.declarations}
            edges = concept_report.resolve_edges([parsed], parsed.declarations)

        self.assertEqual(
            names,
            {
                ("class", "ao::Bar"),
                ("class", "ao::Foo"),
                ("alias", "ao::Foo::Alias"),
                ("enum", "ao::Kind"),
                ("function", "ao::g"),
            },
        )
        self.assertEqual(len([item for item in parsed.declarations if item.kind == "function"]), 2)
        self.assertIn(("ao::Foo", "ao::Bar"), {(edge.source, edge.target) for edge in edges})
        self.assertNotIn("ao::Foo::Hidden", {item.name for item in parsed.declarations})
