"""Contracts for the production Bazel lint portal orchestration."""

import argparse
import contextlib
import io
import json
import tempfile
import unittest
from dataclasses import replace
from pathlib import Path
from unittest.mock import patch

from ao.__main__ import make_parser
from ao.command import bazel as bazel_command
from ao.command import tidy as tidy_command
from ao.core import bazel_lint, bazel_lint_reports

QUERY_XML = """<?xml version="1.1" encoding="UTF-8" standalone="no"?>
<query version="2">
  <rule class="cc_library" location="/checkout/lib/sample/BUILD:1:1" name="//lib/sample:sample">
    <string name="name" value="sample"/>
    <list name="srcs">
      <label value="//lib/sample:Sample.cpp"/>
      <label value="//lib/sample:SampleLinux.cpp"/>
      <label value="//lib/sample:Sample.h"/>
      <label value="//lib/sample:group"/>
    </list>
    <list name="hdrs">
      <label value="//include/ao/sample:Sample.h"/>
    </list>
    <list name="textual_hdrs">
      <label value="//lib/sample:Internal.h"/>
    </list>
    <list name="deps">
      <label value="//lib/other:other"/>
    </list>
  </rule>
  <rule class="filegroup" name="//lib/sample:group">
    <list name="srcs">
      <label value="//lib/sample:Grouped.cpp"/>
      <label value="//lib/sample:alias"/>
    </list>
  </rule>
  <rule class="alias" name="//lib/sample:alias">
    <label name="actual" value="//lib/sample:Aliased.cpp"/>
  </rule>
  <rule class="cc_library" name="//lib/other:other">
    <list name="srcs">
      <label value="//lib/other:Other.cpp"/>
    </list>
  </rule>
  <rule class="cc_library" name="//lib/utility:utility">
    <list name="srcs">
      <label value="//lib/utility:AtomicFile.cpp"/>
      <label value="//lib/utility:AtomicFileWindows.cpp"/>
    </list>
  </rule>
  <rule class="cc_library" name="//lib/sample:header_only">
    <list name="hdrs">
      <label value="//lib/sample:Only.h"/>
    </list>
  </rule>
  <rule class="cc_binary" name="//app/sample:app">
    <list name="srcs">
      <label value="//app/sample:Main.cpp"/>
      <label value="//app/foo:BarWindows.cpp"/>
    </list>
    <list name="hdrs">
      <label value="//app/include/ao/foo:Bar.h"/>
    </list>
  </rule>
  <rule class="cc_library" name="//lib/foo:foo">
    <list name="srcs">
      <label value="//lib/foo:BarPosix.cpp"/>
    </list>
    <list name="hdrs">
      <label value="//include/ao/foo:Bar.h"/>
    </list>
  </rule>
  <rule class="cc_test" name="//test:winui_test">
    <list name="srcs">
      <label value="//test:winui/ThemeTest.cpp"/>
    </list>
  </rule>
  <rule class="appkit_objc_library" name="//app/macos-appkit:appkit">
    <list name="srcs">
      <label value="//app/macos-appkit:Foo.mm"/>
    </list>
  </rule>
  <source-file name="lib/sample/Sample.cpp"/>
  <source-file name="lib/sample/SampleLinux.cpp"/>
  <source-file name="lib/sample/Grouped.cpp"/>
  <source-file name="lib/sample/Aliased.cpp"/>
  <source-file name="lib/other/Other.cpp"/>
  <source-file name="lib/utility/AtomicFile.cpp"/>
  <source-file name="lib/utility/AtomicFileWindows.cpp"/>
  <source-file name="app/sample/Main.cpp"/>
  <source-file name="app/foo/BarWindows.cpp"/>
  <source-file name="lib/foo/BarPosix.cpp"/>
  <source-file name="test/winui/ThemeTest.cpp"/>
  <source-file name="app/macos-appkit/Foo.mm"/>
</query>
"""


# Headers and fragments without a companion, reached directly and through
# header-only libraries by owners with native sources.
CONSUMER_XML = """<?xml version="1.1" encoding="UTF-8" standalone="no"?>
<query version="2">
  <rule class="cc_library" name="//include:util_headers">
    <list name="hdrs"><label value="//include:ao/util/Span.h"/></list>
  </rule>
  <rule class="cc_library" name="//lib/util:util">
    <list name="srcs"><label value="//lib/util:Clock.cpp"/></list>
    <list name="deps"><label value="//include:util_headers"/></list>
  </rule>
  <rule class="cc_test" name="//test/unit:unit">
    <list name="srcs"><label value="//test/unit:SpanTest.cpp"/></list>
    <list name="deps"><label value="//lib/util:util"/><label value="@@external+//:dependency"/></list>
  </rule>
  <rule class="cc_library" name="//app/include:catalog_headers">
    <list name="hdrs"><label value="//app/include:ao/i18n/Inventory.def"/></list>
  </rule>
  <rule class="cc_library" name="//app/include:i18n_headers">
    <list name="hdrs"><label value="//app/include:ao/i18n/Ids.h"/></list>
    <list name="deps"><label value="//app/include:catalog_headers"/></list>
  </rule>
  <rule class="alias" name="//app/include:i18n">
    <label name="actual" value="//app/include:i18n_headers"/>
  </rule>
  <rule class="cc_library" name="//app/i18n:i18n">
    <list name="srcs"><label value="//app/i18n:Catalog.cpp"/></list>
    <list name="deps"><label value="//app/include:i18n"/></list>
  </rule>
  <rule class="cc_binary" name="//tool/catalog:compiler">
    <list name="srcs"><label value="//tool/catalog:Main.cpp"/></list>
    <list name="deps"><label value="//app/include:catalog_headers"/></list>
  </rule>
</query>
"""
SPAN = "include/ao/util/Span.h"
INVENTORY = "app/include/ao/i18n/Inventory.def"


class BazelLintParserTest(unittest.TestCase):
    def test_lint_parser_wires_targets_scope_and_check(self):
        args = make_parser().parse_args(
            [
                "bazel",
                "lint",
                "lib/sample/Sample.cpp",
                "--folder",
                "app",
                "--commit",
                "HEAD~1",
                "--check",
                "aobus-example",
                "--target",
                "//lib/sample:sample",
                "--target",
                "//lib/other:other",
                "--clang-tidy-arg=--quiet",
                "--debug",
                "--jobs",
                "3",
            ]
        )
        self.assertEqual(args.owner_targets, ["//lib/sample:sample", "//lib/other:other"])
        self.assertEqual(args.files, ["lib/sample/Sample.cpp"])
        self.assertEqual(args.folder, ["app"])
        self.assertEqual(args.commit, "HEAD~1")
        self.assertEqual(args.check, "aobus-example")
        self.assertEqual(args.clang_tidy_arg, ["--quiet"])
        self.assertTrue(args.debug)
        self.assertEqual(args.jobs, 3)

    def test_lint_parser_rejects_fix_and_export_before_preparation(self):
        for arguments in (["--fix"], ["--clang-tidy-arg=--fix"], ["--clang-tidy-arg=--export-fixes=out.yaml"]):
            with self.subTest(arguments=arguments), patch("sys.stderr"), self.assertRaises(SystemExit) as exited:
                make_parser().parse_args(["bazel", "lint", *arguments])
            self.assertEqual(exited.exception.code, 2)

    def test_lint_dispatch_reaches_the_lint_owner_before_workspace_preparation(self):
        args = make_parser().parse_args(
            [
                "bazel",
                "lint",
                "lib/sample/Sample.cpp",
                "--folder",
                "app",
                "--commit",
                "HEAD~1",
                "--check",
                "aobus-example",
                "--target",
                "//lib/sample:sample",
            ]
        )
        with (
            patch.object(bazel_lint, "run", return_value=0) as lint_run,
            patch.object(bazel_lint.bazel_workspace, "workspace_path", side_effect=AssertionError("workspace")),
        ):
            self.assertEqual(bazel_command.run(args), 0)
        lint_run.assert_called_once_with(args)


class QueryGraphTest(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="ao-lint-graph-")
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.path = self.root / "query.xml"
        self.path.write_text(QUERY_XML, encoding="utf-8")
        self.graph = bazel_lint.parse_query_xml(self.path)

    def test_source_ownership_follows_actual_srcs_through_filegroups_and_aliases(self):
        self.assertEqual(self.graph.source_owners("lib/sample/Sample.cpp"), ("//lib/sample:sample",))
        self.assertEqual(self.graph.source_owners("lib/sample/Grouped.cpp"), ("//lib/sample:sample",))
        self.assertEqual(self.graph.source_owners("lib/sample/Aliased.cpp"), ("//lib/sample:sample",))
        self.assertEqual(self.graph.source_owners("lib/other/Other.cpp"), ("//lib/other:other",))
        self.assertEqual(self.graph.source_owners("lib/sample/Only.h"), ())

    def test_dependencies_are_not_treated_as_lint_scope(self):
        self.assertNotIn("//lib/other:other", self.graph.source_owners("lib/sample/Sample.cpp"))
        self.assertEqual(
            self.graph.native_sources("//lib/sample:sample"),
            ("lib/sample/Sample.cpp", "lib/sample/SampleLinux.cpp", "lib/sample/Grouped.cpp", "lib/sample/Aliased.cpp"),
        )

    def test_header_ownership_requires_a_same_component_companion_source(self):
        self.assertEqual(self.graph.header_owners("include/ao/sample/Sample.h"), ("//lib/sample:sample",))
        self.assertEqual(self.graph.header_owners("app/include/ao/foo/Bar.h"), ("//app/sample:app",))
        self.assertEqual(self.graph.header_owners("include/ao/foo/Bar.h"), ("//lib/foo:foo",))
        self.assertEqual(self.graph.header_owners("include/ao/sample/Unpaired.h"), ())
        self.assertEqual(self.graph.header_owners("lib/sample/Only.h"), ())

    def test_declared_headers_include_hdrs_textual_headers_and_non_source_srcs(self):
        self.assertEqual(
            set(self.graph.declared_headers("//lib/sample:sample")),
            {"include/ao/sample/Sample.h", "lib/sample/Internal.h", "lib/sample/Sample.h"},
        )

    def test_consumer_owners_reach_declarers_through_dependencies_and_have_sources(self):
        self.path.write_text(CONSUMER_XML, encoding="utf-8")
        graph = bazel_lint.parse_query_xml(self.path)
        self.assertEqual(graph.consumer_owners(SPAN), ("//lib/util:util", "//test/unit:unit"))
        self.assertEqual(graph.consumer_owners(INVENTORY), ("//app/i18n:i18n", "//tool/catalog:compiler"))
        self.assertEqual(graph.consumer_owners("app/include/ao/i18n/Ids.h"), ("//app/i18n:i18n",))
        self.assertEqual(graph.consumer_owners("include/ao/util/Undeclared.h"), ())

    def test_first_pass_holds_back_consumers_beyond_the_nearest_and_companion_owners(self):
        self.path.write_text(CONSUMER_XML, encoding="utf-8")
        graph = bazel_lint.parse_query_xml(self.path)
        self.assertEqual(
            bazel_lint._map_selected(graph, [SPAN, INVENTORY, "lib/util/Clock.cpp"]),
            (
                ["//app/i18n:i18n", "//lib/util:util"],
                {SPAN: ("//test/unit:unit",), INVENTORY: ("//tool/catalog:compiler",)},
            ),
        )
        # Without a nearer package every possible consumer joins the first pass.
        self.assertEqual(
            bazel_lint._nearest(("//test/unit:unit", "//tool/catalog:compiler"), SPAN),
            ("//test/unit:unit", "//tool/catalog:compiler"),
        )
        self.assertEqual(
            bazel_lint._map_selected(self.graph, ["include/ao/sample/Sample.h"]),
            (["//lib/sample:sample"], {"include/ao/sample/Sample.h": ()}),
        )

    def test_malformed_duplicate_and_cyclic_query_graphs_fail_closed(self):
        for text in (
            "<not-query/>",
            "<query version='2'><rule class='cc_library' name='not-a-label'/></query>",
            (
                "<query version='2'><rule class='cc_library' name='//lib/sample:sample'/>"
                "<rule class='cc_library' name='//lib/sample:sample'/></query>"
            ),
        ):
            with self.subTest(text=text):
                self.path.write_text(text, encoding="utf-8")
                with self.assertRaises(ValueError):
                    bazel_lint.parse_query_xml(self.path)
        self.path.write_text(
            (
                "<query version='2'><rule class='filegroup' name='//lib/sample:group'>"
                "<list name='srcs'><label value='//lib/sample:group'/></list></rule></query>"
            ),
            encoding="utf-8",
        )
        with self.assertRaises(ValueError):
            bazel_lint.parse_query_xml(self.path).native_sources("//lib/sample:group")

    def test_bare_target_labels_reject_patterns_external_repositories_and_duplicates(self):
        self.assertEqual(
            bazel_lint._owner_targets(["//lib/sample:sample", "@//lib/other:other"]),
            ["//lib/sample:sample", "//lib/other:other"],
        )
        self.assertEqual(bazel_lint._owner_targets(["@@//lib/sample:sample"]), ["//lib/sample:sample"])
        for values in (
            ["//lib/sample:all"],
            ["//..."],
            ["@repo//lib/sample:sample"],
            ["lib/sample:sample"],
            ["//lib/sample:sample", "//lib/sample:sample"],
        ):
            with self.subTest(values=values), self.assertRaises(ValueError):
                bazel_lint._owner_targets(values)

    def test_target_validation_admits_native_rule_kinds_before_configured_coverage(self):
        for target in (
            "//lib/sample:sample",
            "//lib/sample:header_only",
            "//lib/other:other",
            "//app/sample:app",
            "//test:winui_test",
            "//app/macos-appkit:appkit",
        ):
            with self.subTest(target=target):
                bazel_lint._validate_targets(self.graph, [target])
        for target in ("//lib/sample:alias", "//lib/sample:missing"):
            with self.subTest(target=target), self.assertRaises(ValueError):
                bazel_lint._validate_targets(self.graph, [target])


class CqueryRowsTest(unittest.TestCase):
    def test_exact_rows_are_parsed_with_bazel_label_spellings(self):
        self.assertEqual(
            bazel_lint.parse_cquery_rows(
                "@@//lib/sample:sample|False\n//lib/other:other|True\n",
                ["//lib/sample:sample", "//lib/other:other"],
            ),
            {"//lib/sample:sample": False, "//lib/other:other": True},
        )

    def test_unknown_missing_duplicate_or_malformed_rows_fail_closed(self):
        for text in (
            "//lib/sample:sample|False\n//lib/other:other|False\n",
            "//lib/sample:sample|False\n//lib/sample:sample|True\n",
            "//lib/sample:sample|maybe\n",
            "//lib/sample:sample\n",
        ):
            with self.subTest(text=text), self.assertRaises(ValueError):
                bazel_lint.parse_cquery_rows(text, ["//lib/sample:sample"])
        with self.assertRaises(ValueError):
            bazel_lint.parse_cquery_rows("//lib/sample:sample|False\n", ["//lib/sample:sample", "//lib/other:other"])


class ConsumerSelectionTest(unittest.TestCase):
    def inventory(self, owner, *sources, includes=()):
        configuration = "bazel-out/k8-dbg/bin"
        reports = tuple(
            bazel_lint_reports.Report(
                bazel_lint_reports.Identity(
                    owner, configuration, source, main, (configuration + "/x.o",), "STRICT", "linux"
                ),
                "clean",
                0,
                Path("report.json"),
                Path("report.log"),
                tuple(includes),
            )
            for source, main in ((source, source) for source in sources)
        )
        return bazel_lint_reports.OwnerInventory(
            owner, configuration, "config-id", reports, tuple(sources), (), Path("execroot")
        )

    def test_same_component_then_longest_prefix_then_labels_choose_the_consumer(self):
        inventories = (
            self.inventory("@@//test/unit:unit", "test/unit/SpanTest.cpp", includes=(SPAN, INVENTORY)),
            self.inventory("@@//lib/util:util", "lib/util/Zeta.cpp", "lib/util/Clock.cpp", includes=(SPAN,)),
            self.inventory("@@//app/runtime:runtime", "app/runtime/Player.cpp", includes=(INVENTORY,)),
            self.inventory("@@//tool/catalog:compiler", "tool/catalog/Main.cpp", includes=(INVENTORY,)),
        )
        self.assertEqual(
            bazel_lint._choose_consumers([SPAN, INVENTORY, "include/ao/util/Unused.h"], inventories),
            {
                SPAN: ("@@//lib/util:util", "lib/util/Clock.cpp"),
                INVENTORY: ("@@//app/runtime:runtime", "app/runtime/Player.cpp"),
            },
        )

    def test_only_source_mains_offer_their_includes_as_consumer_evidence(self):
        inventory = self.inventory("@@//lib/util:util", "lib/util/Clock.cpp")
        header = replace(
            inventory.reports[0],
            identity=replace(inventory.reports[0].identity, main="include/ao/util/Clock.h"),
            includes=(SPAN,),
        )
        self.assertEqual(bazel_lint._choose_consumers([SPAN], (replace(inventory, reports=(header,)),)), {})

    def test_selection_groups_consumers_by_owner_for_the_repository_rule(self):
        self.assertEqual(
            bazel_lint._consumer_selection(
                {
                    SPAN: ("@@//lib/util:util", "lib/util/Clock.cpp"),
                    "include/ao/util/Bits.h": ("@@//lib/util:util", "lib/util/Clock.cpp"),
                    INVENTORY: ("@@//tool/catalog:compiler", "tool/catalog/Main.cpp"),
                }
            ),
            {
                "schema": 1,
                "consumers": {
                    "@@//tool/catalog:compiler": [["tool/catalog/Main.cpp", INVENTORY]],
                    "@@//lib/util:util": [
                        ["lib/util/Clock.cpp", "include/ao/util/Bits.h"],
                        ["lib/util/Clock.cpp", SPAN],
                    ],
                },
            },
        )


class LintConfigurationTest(unittest.TestCase):
    def test_fixed_configuration_has_no_scope_and_is_rewritten_only_on_content_change(self):
        with tempfile.TemporaryDirectory(prefix="ao-lint-config-") as temporary:
            path = Path(temporary) / "configuration" / "lint.json"
            with patch.object(bazel_lint.bazel_workspace, "_source_root", return_value="/checkout"):
                configuration = bazel_lint._lint_configuration("aobus-example", "^include", ["--quiet"])
            self.assertEqual(
                configuration,
                {
                    "schema": 1,
                    "source_root": "/checkout",
                    "check": "aobus-example",
                    "header_filter": "^include",
                    "clang_tidy_args": ["--quiet"],
                },
            )
            self.assertNotIn("scope", configuration)
            self.assertTrue(bazel_lint._write_lint_config(path, configuration))
            self.assertFalse(bazel_lint._write_lint_config(path, configuration))
            self.assertEqual(json.loads(path.read_text(encoding="utf-8")), configuration)


class RunOrchestrationTest(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="ao-bazel-lint-")
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.checkout = self.root / "checkout"
        self.tree = self.root / "tree"
        self.checkout.mkdir()
        self.tree.mkdir()
        for name in (
            "lib/sample/Sample.cpp",
            "lib/sample/Grouped.cpp",
            "lib/sample/Aliased.cpp",
            "lib/other/Other.cpp",
            "lib/utility/AtomicFile.cpp",
            "lib/utility/AtomicFileWindows.cpp",
            "include/ao/sample/Sample.h",
            "include/ao/sample/Unpaired.h",
            "app/macos-appkit/Foo.mm",
            "app/macos-appkit/Foo.h",
            "app/macos-appkit/Unmapped.mm",
            "test/winui/ThemeTest.cpp",
            "app/include/ao/foo/Bar.h",
            "app/foo/BarWindows.cpp",
            "include/ao/foo/Bar.h",
            "lib/foo/BarPosix.cpp",
            "lib/sample/Foo.def",
            "lib/sample/Unknown.cpp",
            SPAN,
            "lib/util/Clock.cpp",
            "test/unit/SpanTest.cpp",
            INVENTORY,
            "app/i18n/Catalog.cpp",
            "tool/catalog/Main.cpp",
        ):
            path = self.checkout / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text("// test input\n", encoding="utf-8")

        self.query_xml = QUERY_XML
        self.query_status = 0
        self.cquery_text = "@@//lib/sample:sample|False\n"
        self.cquery_status = 0
        self.cquery_texts: list[str] = []
        self.build_status = 0
        self.inventories: tuple[bazel_lint_reports.OwnerInventory, ...] = ()
        self.pass_inventories: list[tuple[bazel_lint_reports.OwnerInventory, ...]] = []
        self.read_error: Exception | None = None
        self.commands: list[list[str]] = []
        self.prepare_calls: list[dict[str, object]] = []
        self.execute_calls: list[dict[str, object]] = []
        self.read_calls: list[tuple[Path, list[str]]] = []
        self.log_sequence = 0

        patches = [
            patch.object(bazel_lint, "PROJECT_ROOT", self.checkout),
            patch.object(tidy_command, "PROJECT_ROOT", self.checkout),
            patch.object(bazel_lint.bazel_workspace, "workspace_path", side_effect=lambda path, config: self.tree),
            patch.object(bazel_lint.bazel_workspace, "bazel_executable", return_value=Path("bazel")),
            patch.object(bazel_lint.bazel_workspace, "require_supported_source"),
            patch.object(bazel_lint.bazel_workspace, "_source_root", return_value=str(self.checkout)),
            patch.object(bazel_lint.bazel_workspace, "prepare_workspace", side_effect=self.fake_prepare),
            patch.object(bazel_lint.bazel_workspace, "execute", side_effect=self.fake_execute),
            patch.object(bazel_lint.bazel_lint_reports, "read_reports", side_effect=self.fake_read_reports),
            patch.object(bazel_lint, "build_tree_lock", return_value=contextlib.nullcontext()),
            patch.object(bazel_lint, "_run_process", side_effect=self.fake_process),
        ]
        for patcher in patches:
            patcher.start()
            self.addCleanup(patcher.stop)

    def fake_prepare(self, tree, config, *, prefix=None, lint_configuration=None, lint_consumers=None):
        self.prepare_calls.append(
            {
                "tree": tree,
                "config": config,
                "prefix": prefix,
                "lint_configuration": lint_configuration,
                "lint_consumers": lint_consumers,
            }
        )
        prepared = tree / "configuration"
        prepared.mkdir(parents=True, exist_ok=True)
        return prepared

    def fake_execute(self, executable, tree, prepared, action, config, targets, *, jobs=2, remote_cache=None):
        self.execute_calls.append(
            {
                "executable": executable,
                "tree": tree,
                "prepared": prepared,
                "action": action,
                "config": config,
                "targets": list(targets),
                "jobs": jobs,
                "remote_cache": remote_cache,
                # The consumer selection each report pass loads.
                "consumers": json.loads((tree / "configuration/lint-consumers.json").read_text(encoding="utf-8")),
            }
        )
        return self.build_status

    def fake_read_reports(self, bep, owners):
        self.read_calls.append((bep, list(owners)))
        if self.read_error is not None:
            raise self.read_error
        # A consumer flow reads one inventory per report pass.
        return self.pass_inventories.pop(0) if self.pass_inventories else self.inventories

    def fake_process(self, command, stdout, stderr):
        # Replace only the owned transport, not subprocess.run shared with the
        # standard library's lazy native platform probes.
        self.commands.append(list(command))
        action = command[2]
        if action == "query":
            stdout.write_text(self.query_xml, encoding="utf-8")
            stderr.write_text("query failure\n" if self.query_status else "", encoding="utf-8")
            return self.query_status
        if action == "cquery":
            stdout.write_text(self.cquery_texts.pop(0) if self.cquery_texts else self.cquery_text, encoding="utf-8")
            stderr.write_text("cquery failure\n" if self.cquery_status else "", encoding="utf-8")
            return self.cquery_status
        raise AssertionError(f"unexpected Bazel query command: {command}")

    def args(self, **overrides):
        values = {
            "files": [],
            "all": False,
            "folder": [],
            "commit": None,
            "check": None,
            "debug": False,
            "output": None,
            "jobs": 2,
            "path": str(self.tree),
            "config": "debug",
            "vcpkg_prefix": None,
            "owner_targets": [],
            "clang_tidy_arg": [],
            "header_filter": None,
            "remote_cache": None,
        }
        values.update(overrides)
        return argparse.Namespace(**values)

    def run_quiet(self, args):
        with contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()):
            return bazel_lint.run(args)

    def report(self, owner, source, *, header=None, outcome="clean", log="", includes=()):
        configuration = "bazel-out/k8-dbg/bin"
        main = header or source
        identity = bazel_lint_reports.Identity(
            owner,
            configuration,
            source,
            main,
            (configuration + "/lib/sample/_objs/sample/Sample.o",),
            "STRICT",
            "linux",
        )
        log_path = self.root / f"log-{self.log_sequence}.log"
        self.log_sequence += 1
        log_path.write_text(log, encoding="utf-8")
        return bazel_lint_reports.Report(identity, outcome, 0, self.root / "report.json", log_path, tuple(includes))

    def owner_inventory(self, owner, *reports, sources=None):
        configuration = "bazel-out/k8-dbg/bin"
        paths = (
            tuple(sources)
            if sources is not None
            else tuple(
                dict.fromkeys(path for report in reports for path in (report.identity.source, report.identity.main))
            )
        )
        return bazel_lint_reports.OwnerInventory(
            owner, configuration, "config-id", tuple(reports), paths, (), self.root / "execroot"
        )

    def test_source_scope_writes_fixed_configuration_and_gates_the_current_report(self):
        owner = "@@//lib/sample:sample"
        self.inventories = (self.owner_inventory(owner, self.report(owner, "lib/sample/Sample.cpp")),)
        status = self.run_quiet(
            self.args(
                files=[str(self.checkout / "lib/sample/Sample.cpp")],
                check="aobus-example",
                header_filter="^include",
                clang_tidy_arg=["--quiet"],
                jobs=3,
                vcpkg_prefix="/vcpkg",
                remote_cache="https://cache.example",
            )
        )
        self.assertEqual(status, 0)

        lint_path = self.tree / "configuration" / "lint.json"
        self.assertEqual(
            json.loads(lint_path.read_text(encoding="utf-8")),
            {
                "schema": 1,
                "source_root": str(self.checkout),
                "check": "aobus-example",
                "header_filter": "^include",
                "clang_tidy_args": ["--quiet"],
            },
        )
        self.assertEqual(self.prepare_calls[0]["lint_configuration"], lint_path)
        self.assertEqual(self.prepare_calls[0]["prefix"], "/vcpkg")
        self.assertEqual(self.execute_calls[0]["jobs"], 3)
        self.assertEqual(self.execute_calls[0]["remote_cache"], "https://cache.example")
        targets = self.execute_calls[0]["targets"]
        self.assertIn("//lib/sample:sample", targets)
        self.assertIn("--aspects=//bazel/lint:lint.bzl%lint_aspect", targets)
        self.assertIn("--output_groups=aobus_lint", targets)
        self.assertIn("--remote_download_outputs=all", targets)
        self.assertIn("--experimental_build_event_upload_strategy=local", targets)
        self.assertIn("--norun_validations", targets)
        self.assertTrue(any(target.startswith("--build_event_json_file=") for target in targets))
        self.assertEqual(self.read_calls[0][1], [owner])

        query = next(command for command in self.commands if command[2] == "query")
        self.assertIn('kind("rule", //...)', query)
        self.assertIn("--output=xml", query)
        cquery = next(command for command in self.commands if command[2] == "cquery")
        self.assertEqual(cquery[3], "config(set(//lib/sample:sample), target)")
        self.assertIn(
            '--starlark:expr=str(target.label) + "|" + str("IncompatiblePlatformProvider" in providers(target))',
            cquery,
        )
        self.assertFalse(any("aquery" in command for command in self.commands))
        receipt = json.loads((self.tree / "lint").glob("run-*/run.receipt.json").__next__().read_text(encoding="utf-8"))
        self.assertEqual(receipt["selected"], ["lib/sample/Sample.cpp"])

    def test_debug_publishes_action_evidence_without_changing_analysis_policy(self):
        owner = "@@//lib/sample:sample"
        self.inventories = (self.owner_inventory(owner, self.report(owner, "lib/sample/Sample.cpp")),)
        self.assertEqual(self.run_quiet(self.args(owner_targets=["//lib/sample:sample"])), 0)
        policy_path = self.tree / "configuration" / "lint.json"
        original_policy = json.loads(policy_path.read_text(encoding="utf-8"))
        self.assertEqual(self.run_quiet(self.args(owner_targets=["//lib/sample:sample"], debug=True)), 0)
        self.assertEqual(json.loads(policy_path.read_text(encoding="utf-8")), original_policy)
        self.assertNotIn("--build_event_publish_all_actions", self.execute_calls[0]["targets"])
        self.assertIn("--build_event_publish_all_actions", self.execute_calls[1]["targets"])

    def test_target_only_selects_source_and_paired_header_reports(self):
        owner = "@@//lib/sample:sample"
        source = self.report(owner, "lib/sample/Sample.cpp")
        header = self.report(
            owner,
            "lib/sample/Sample.cpp",
            header="include/ao/sample/Sample.h",
            outcome="warning",
            log="include/ao/sample/Sample.h:1:1: warning: header [aobus-example]\n",
        )
        self.inventories = (self.owner_inventory(owner, source, header),)
        self.assertEqual(self.run_quiet(self.args(owner_targets=["//lib/sample:sample"])), 1)
        receipt = json.loads((self.tree / "lint").glob("run-*/run.receipt.json").__next__().read_text(encoding="utf-8"))
        self.assertEqual(receipt["selected"], ["include/ao/sample/Sample.h", "lib/sample/Sample.cpp"])

    def test_explicit_owner_query_is_bounded_and_uses_native_inventory_for_grouped_sources(self):
        self.query_xml = """<query><rule class="cc_library" name="//lib/sample:sample">
        <list name="srcs"><label value="//lib/sample:group"/></list></rule></query>"""
        owner = "@@//lib/sample:sample"
        self.inventories = (self.owner_inventory(owner, self.report(owner, "lib/sample/Grouped.cpp")),)
        self.assertEqual(self.run_quiet(self.args(owner_targets=["//lib/sample:sample"])), 0)
        query = next(command for command in self.commands if command[2] == "query")
        self.assertEqual(query[3], 'kind("rule", set(//lib/sample:sample))')
        receipt = json.loads((self.tree / "lint").glob("run-*/run.receipt.json").__next__().read_text(encoding="utf-8"))
        self.assertEqual(receipt["selected"], ["lib/sample/Grouped.cpp"])

    def test_target_only_requires_a_source_translation_unit_report_per_owner(self):
        owner = "@@//lib/sample:sample"
        header = self.report(owner, "lib/sample/Sample.cpp", header="include/ao/sample/Sample.h")
        self.inventories = (self.owner_inventory(owner, header),)
        self.assertEqual(self.run_quiet(self.args(owner_targets=["//lib/sample:sample"])), 2)

    def test_header_scope_gates_the_header_as_main_not_its_companion_source(self):
        owner = "@@//lib/sample:sample"
        source = self.report(
            owner,
            "lib/sample/Sample.cpp",
            outcome="warning",
            log="lib/sample/Sample.cpp:1:1: warning: source [aobus-example]\n",
        )
        header = self.report(owner, "lib/sample/Sample.cpp", header="include/ao/sample/Sample.h")
        self.inventories = (self.owner_inventory(owner, source, header),)
        self.assertEqual(
            self.run_quiet(self.args(files=[str(self.checkout / "include/ao/sample/Sample.h")])),
            0,
        )
        receipt = json.loads((self.tree / "lint").glob("run-*/run.receipt.json").__next__().read_text(encoding="utf-8"))
        self.assertEqual(receipt["selected"], ["include/ao/sample/Sample.h"])

    def test_source_scope_gates_source_findings_even_when_a_header_report_is_clean(self):
        owner = "@@//lib/sample:sample"
        source = self.report(
            owner,
            "lib/sample/Sample.cpp",
            outcome="warning",
            log="lib/sample/Sample.cpp:1:1: warning: source [aobus-example]\n",
        )
        header = self.report(owner, "lib/sample/Sample.cpp", header="include/ao/sample/Sample.h")
        self.inventories = (self.owner_inventory(owner, source, header),)
        self.assertEqual(
            self.run_quiet(self.args(files=[str(self.checkout / "lib/sample/Sample.cpp")])),
            1,
        )

    def test_cquery_uses_one_set_expression_for_multiple_owners(self):
        sample = "@@//lib/sample:sample"
        other = "@@//lib/other:other"
        self.cquery_text = "@@//lib/sample:sample|False\n@@//lib/other:other|False\n"
        self.inventories = (
            self.owner_inventory(sample, self.report(sample, "lib/sample/Sample.cpp")),
            self.owner_inventory(other, self.report(other, "lib/other/Other.cpp")),
        )
        self.assertEqual(
            self.run_quiet(self.args(owner_targets=["//lib/sample:sample", "//lib/other:other"])),
            0,
        )
        cquery = next(command for command in self.commands if command[2] == "cquery")
        self.assertEqual(cquery[3], "config(set(//lib/sample:sample //lib/other:other), target)")

    def test_actual_incompatible_owners_are_filtered_before_the_report_build(self):
        sample = "@@//lib/sample:sample"
        self.cquery_text = "@@//lib/sample:sample|False\n@@//app/macos-appkit:appkit|True\n"
        self.inventories = (self.owner_inventory(sample, self.report(sample, "lib/sample/Sample.cpp")),)
        self.assertEqual(
            self.run_quiet(self.args(owner_targets=["//lib/sample:sample", "//app/macos-appkit:appkit"])),
            0,
        )
        self.assertNotIn("//app/macos-appkit:appkit", self.execute_calls[0]["targets"])
        self.assertEqual(self.read_calls[0][1], [sample])
        cquery = next(command for command in self.commands if command[2] == "cquery")
        self.assertEqual(cquery[3], "config(set(//lib/sample:sample //app/macos-appkit:appkit), target)")
        receipt = json.loads(
            (self.tree / "lint").glob("run-*/cquery.receipt.json").__next__().read_text(encoding="utf-8")
        )
        self.assertEqual(receipt["incompatible"], ["//app/macos-appkit:appkit"])

    def test_all_explicit_owners_incompatible_fails_without_a_report_build(self):
        self.cquery_text = "@@//app/macos-appkit:appkit|True\n"
        self.assertEqual(
            self.run_quiet(self.args(owner_targets=["//app/macos-appkit:appkit"])),
            2,
        )
        self.assertFalse(self.execute_calls)

    def test_explicit_target_without_native_translation_units_fails_from_current_inventory(self):
        owner = "@@//lib/sample:header_only"
        self.cquery_text = owner + "|False\n"
        self.inventories = (self.owner_inventory(owner),)
        self.assertEqual(self.run_quiet(self.args(owner_targets=["//lib/sample:header_only"])), 2)
        self.assertTrue(any(command[2] == "cquery" for command in self.commands))
        self.assertTrue(self.execute_calls)

    def test_file_scope_cannot_hide_an_empty_explicit_owner_beside_a_covering_owner(self):
        owner = "@@//lib/sample:sample"
        empty = "@@//lib/sample:header_only"
        self.cquery_text = owner + "|False\n" + empty + "|False\n"
        self.inventories = (
            self.owner_inventory(owner, self.report(owner, "lib/sample/Sample.cpp")),
            self.owner_inventory(empty),
        )
        self.assertEqual(
            self.run_quiet(
                self.args(
                    owner_targets=["//lib/sample:sample", "//lib/sample:header_only"],
                    files=[str(self.checkout / "lib/sample/Sample.cpp")],
                )
            ),
            2,
        )

    def test_target_plus_file_scope_does_not_defer_an_uncovered_compatible_file(self):
        owner = "@@//lib/sample:sample"
        self.inventories = (self.owner_inventory(owner, self.report(owner, "lib/sample/Sample.cpp")),)
        self.assertEqual(
            self.run_quiet(
                self.args(
                    owner_targets=["//lib/sample:sample"],
                    files=[str(self.checkout / "lib/other/Other.cpp")],
                )
            ),
            2,
        )

    def test_unpaired_compatible_header_fails_even_in_a_batch_scope(self):
        self.assertEqual(
            self.run_quiet(self.args(files=[str(self.checkout / "include/ao/sample/Unpaired.h")])),
            2,
        )
        self.assertFalse(self.execute_calls)

    def test_platform_incompatible_unowned_file_is_deferred_without_a_report_build(self):
        with patch.object(bazel_lint, "_platform_incompatible", return_value=True):
            self.assertEqual(
                self.run_quiet(self.args(files=[str(self.checkout / "app/macos-appkit/Unmapped.mm")])),
                0,
            )
        self.assertFalse(self.execute_calls)

    def test_platform_incompatible_source_omitted_from_actual_configured_srcs_is_deferred(self):
        owner = "@@//lib/utility:utility"
        self.cquery_text = "@@//lib/utility:utility|False\n"
        self.inventories = (self.owner_inventory(owner, self.report(owner, "lib/utility/AtomicFile.cpp")),)
        with patch.object(bazel_lint, "_platform_incompatible", return_value=True):
            self.assertEqual(
                self.run_quiet(self.args(files=[str(self.checkout / "lib/utility/AtomicFileWindows.cpp")])),
                0,
            )
        self.assertEqual(self.read_calls[0][1], [owner])
        receipt = json.loads((self.tree / "lint").glob("run-*/run.receipt.json").__next__().read_text(encoding="utf-8"))
        self.assertEqual(receipt["selected"], [])
        self.assertEqual(receipt["deferred"], ["lib/utility/AtomicFileWindows.cpp"])

    def test_platform_incompatible_header_omitted_from_actual_configured_hdrs_is_deferred(self):
        owner = "@@//app/macos-appkit:appkit"
        self.cquery_text = "@@//app/macos-appkit:appkit|False\n"
        self.inventories = (self.owner_inventory(owner, self.report(owner, "app/macos-appkit/Foo.mm")),)
        with patch.object(bazel_lint, "_platform_incompatible", return_value=True):
            self.assertEqual(
                self.run_quiet(self.args(files=[str(self.checkout / "app/macos-appkit/Foo.h")])),
                0,
            )
        self.assertTrue(self.execute_calls)
        receipt = json.loads((self.tree / "lint").glob("run-*/run.receipt.json").__next__().read_text(encoding="utf-8"))
        self.assertEqual(receipt["deferred"], ["app/macos-appkit/Foo.h"])

    def test_configured_unpaired_platform_incompatible_header_without_a_consumer_is_deferred(self):
        owner = "@@//app/macos-appkit:appkit"
        self.cquery_text = "@@//app/macos-appkit:appkit|False\n"
        inventory = self.owner_inventory(
            owner,
            self.report(owner, "app/macos-appkit/Foo.mm"),
            sources=("app/macos-appkit/Foo.mm", "app/macos-appkit/Foo.h"),
        )
        self.inventories = (
            replace(
                inventory,
                uncovered=(bazel_lint_reports.Uncovered("app/macos-appkit/Foo.h", "no companion"),),
            ),
        )
        with patch.object(bazel_lint, "_platform_incompatible", return_value=True):
            self.assertEqual(
                self.run_quiet(self.args(files=[str(self.checkout / "app/macos-appkit/Foo.h")])),
                0,
            )
        self.assertEqual(len(self.execute_calls), 1)
        receipt = json.loads((self.tree / "lint").glob("run-*/run.receipt.json").__next__().read_text(encoding="utf-8"))
        self.assertEqual(receipt["deferred"], ["app/macos-appkit/Foo.h"])

    def test_unpaired_header_is_analyzed_as_a_main_in_a_consumer_that_included_it(self):
        self.query_xml = CONSUMER_XML
        owner = "@@//lib/util:util"
        self.cquery_text = owner + "|False\n"
        source = self.report(owner, "lib/util/Clock.cpp", includes=(SPAN, "lib/util/Clock.h"))
        header = self.report(
            owner,
            "lib/util/Clock.cpp",
            header=SPAN,
            outcome="warning",
            log=f"{SPAN}:1:1: warning: header [aobus-example]\n",
        )
        self.pass_inventories = [
            (self.owner_inventory(owner, source),),
            (self.owner_inventory(owner, source, header),),
        ]
        self.assertEqual(self.run_quiet(self.args(files=[str(self.checkout / SPAN)])), 1)

        selection = self.tree / "configuration" / "lint-consumers.json"
        self.assertEqual(self.prepare_calls[0]["lint_consumers"], selection)
        self.assertEqual([call["targets"][0] for call in self.execute_calls], ["//lib/util:util", "//lib/util:util"])
        self.assertEqual(
            [call["consumers"] for call in self.execute_calls],
            [
                {"schema": 1, "consumers": {}},
                {"schema": 1, "consumers": {owner: [["lib/util/Clock.cpp", SPAN]]}},
            ],
        )
        self.assertEqual(sum(command[2] == "cquery" for command in self.commands), 1)
        receipt = json.loads((self.tree / "lint").glob("run-*/run.receipt.json").__next__().read_text(encoding="utf-8"))
        self.assertEqual(receipt["consumers"], {SPAN: {"owner": owner, "source": "lib/util/Clock.cpp"}})
        self.assertEqual(Path(receipt["bep"]).name, "consumers.bep.json")
        # The next run starts from no consumers before loading the graph.
        self.pass_inventories = [(self.owner_inventory(owner, source),)]
        self.run_quiet(self.args(files=[str(self.checkout / "lib/util/Clock.cpp")]))
        self.assertEqual(self.execute_calls[-1]["consumers"], {"schema": 1, "consumers": {}})

    def test_held_back_consumers_build_only_when_the_nearest_reports_miss_the_fragment(self):
        self.query_xml = CONSUMER_XML
        nearest = "@@//app/i18n:i18n"
        compiler = "@@//tool/catalog:compiler"
        self.cquery_texts = [nearest + "|False\n", compiler + "|False\n"]
        catalog = self.report(nearest, "app/i18n/Catalog.cpp", includes=("app/include/ao/i18n/Ids.h",))
        main = self.report(compiler, "tool/catalog/Main.cpp", includes=(INVENTORY,))
        fragment = self.report(compiler, "tool/catalog/Main.cpp", header=INVENTORY)
        self.pass_inventories = [
            (self.owner_inventory(nearest, catalog),),
            (self.owner_inventory(nearest, catalog), self.owner_inventory(compiler, main)),
            (self.owner_inventory(nearest, catalog), self.owner_inventory(compiler, main, fragment)),
        ]
        self.assertEqual(self.run_quiet(self.args(files=[str(self.checkout / INVENTORY)])), 0)

        cqueries = [command[3] for command in self.commands if command[2] == "cquery"]
        self.assertEqual(
            cqueries, ["config(set(//app/i18n:i18n), target)", "config(set(//tool/catalog:compiler), target)"]
        )
        self.assertEqual(
            [[target for target in call["targets"] if target.startswith("//")] for call in self.execute_calls],
            [
                ["//app/i18n:i18n"],
                ["//app/i18n:i18n", "//tool/catalog:compiler"],
                ["//app/i18n:i18n", "//tool/catalog:compiler"],
            ],
        )
        self.assertEqual(
            self.execute_calls[2]["consumers"],
            {"schema": 1, "consumers": {compiler: [["tool/catalog/Main.cpp", INVENTORY]]}},
        )
        self.assertTrue((self.tree / "lint").glob("run-*/cquery-consumers.receipt.json").__next__().is_file())

    def test_incompatible_nearest_owner_does_not_discard_a_compatible_held_consumer(self):
        self.query_xml = CONSUMER_XML
        owner = "@@//test/unit:unit"
        self.cquery_texts = ["//lib/util:util|True\n", owner + "|False\n"]
        source = self.report(owner, "test/unit/SpanTest.cpp", includes=(SPAN,))
        header = self.report(
            owner,
            "test/unit/SpanTest.cpp",
            header=SPAN,
            outcome="warning",
            log=f"{SPAN}:1:1: warning: selected header [aobus-example]\n",
        )
        self.pass_inventories = [
            (self.owner_inventory(owner, source),),
            (self.owner_inventory(owner, source, header),),
        ]
        self.assertEqual(self.run_quiet(self.args(files=[str(self.checkout / SPAN)])), 1)
        self.assertEqual(
            [command[3] for command in self.commands if command[2] == "cquery"],
            ["config(set(//lib/util:util), target)", "config(set(//test/unit:unit), target)"],
        )
        self.assertEqual(self.read_calls[-1][1], [owner])
        self.assertEqual(
            self.execute_calls[-1]["consumers"], {"schema": 1, "consumers": {owner: [[source.identity.source, SPAN]]}}
        )

    def test_platform_named_header_is_not_deferred_before_held_consumers_are_checked(self):
        header_path = "app/windows-winui/include/ao/winui/Unpaired.h"
        path = self.checkout / header_path
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text("// portable header\n", encoding="utf-8")
        self.query_xml = (
            CONSUMER_XML.replace("//include:ao/util/Span.h", "//app/windows-winui:include/ao/winui/Unpaired.h")
            .replace("//include:util_headers", "//app/windows-winui:headers")
            .replace("//lib/util:util", "//app/windows-winui:native")
        )
        owner = "@@//test/unit:unit"
        self.cquery_texts = ["//app/windows-winui:native|True\n", owner + "|False\n"]
        source = self.report(owner, "test/unit/SpanTest.cpp", includes=(header_path,))
        header = self.report(
            owner,
            "test/unit/SpanTest.cpp",
            header=header_path,
            outcome="warning",
            log=f"{header_path}:1:1: warning: portable header [aobus-example]\n",
        )
        self.pass_inventories = [
            (self.owner_inventory(owner, source),),
            (self.owner_inventory(owner, source, header),),
        ]
        self.assertEqual(self.run_quiet(self.args(files=[str(path)])), 1)
        receipt = json.loads(next((self.tree / "lint").glob("run-*/run.receipt.json")).read_text(encoding="utf-8"))
        self.assertEqual(receipt["selected"], [header_path])
        self.assertEqual(receipt["deferred"], [])
        self.assertEqual(len(self.execute_calls), 2)

    def test_portable_header_fails_only_after_every_held_owner_is_incompatible(self):
        self.query_xml = CONSUMER_XML
        self.cquery_texts = ["//lib/util:util|True\n", "//test/unit:unit|True\n"]
        self.assertEqual(self.run_quiet(self.args(files=[str(self.checkout / SPAN)])), 2)
        self.assertEqual(sum(command[2] == "cquery" for command in self.commands), 2)
        self.assertFalse(self.execute_calls)

    def test_compatible_header_that_no_analyzed_unit_includes_fails_with_its_reason(self):
        self.query_xml = CONSUMER_XML
        owner = "@@//lib/util:util"
        test = "@@//test/unit:unit"
        self.cquery_texts = [owner + "|False\n", test + "|False\n"]
        self.pass_inventories = [
            (self.owner_inventory(owner, self.report(owner, "lib/util/Clock.cpp")),),
            (
                self.owner_inventory(owner, self.report(owner, "lib/util/Clock.cpp")),
                self.owner_inventory(test, self.report(test, "test/unit/SpanTest.cpp")),
            ),
        ]
        stderr = io.StringIO()
        with contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(stderr):
            self.assertEqual(bazel_lint.run(self.args(files=[str(self.checkout / SPAN)])), 2)
        self.assertEqual(len(self.execute_calls), 2)
        self.assertIn(f"{SPAN}: no analyzed translation unit includes it", stderr.getvalue())

    def test_portable_winui_model_source_is_gated_when_actual_coverage_exists(self):
        owner = "@@//test:winui_test"
        self.cquery_text = "@@//test:winui_test|False\n"
        self.inventories = (self.owner_inventory(owner, self.report(owner, "test/winui/ThemeTest.cpp")),)
        with patch.object(bazel_lint, "_platform_incompatible", return_value=True):
            self.assertEqual(
                self.run_quiet(self.args(files=[str(self.checkout / "test/winui/ThemeTest.cpp")])),
                0,
            )
        self.assertEqual(self.read_calls[0][1], [owner])
        receipt = json.loads((self.tree / "lint").glob("run-*/run.receipt.json").__next__().read_text(encoding="utf-8"))
        self.assertEqual(receipt["deferred"], [])

    def test_compatible_unowned_source_fails_instead_of_being_deferred(self):
        self.assertEqual(
            self.run_quiet(self.args(files=[str(self.checkout / "lib/sample/Unknown.cpp")])),
            2,
        )
        self.assertFalse(self.execute_calls)

    def test_public_configuration_guard_rejects_raw_options_before_workspace_and_tool_lookup(self):
        for argument in (
            "--fix",
            "--export-fixes=out.yaml",
            "--list-checks",
            "--config=other",
            "--help-hidden",
            "--vfsoverlay=/tmp/missing",
            "--warnings-as-errors=*",
        ):
            with (
                self.subTest(argument=argument),
                patch.object(bazel_lint.bazel_workspace, "workspace_path", side_effect=AssertionError("workspace")),
                patch.object(bazel_lint.bazel_workspace, "bazel_executable", side_effect=AssertionError("tool")),
            ):
                self.assertEqual(
                    self.run_quiet(
                        self.args(
                            files=[str(self.checkout / "lib/sample/Sample.cpp")],
                            clang_tidy_arg=[argument],
                        )
                    ),
                    2,
                )

    def test_explicit_missing_and_outside_checkout_paths_fail_before_workspace(self):
        outside = self.root / "outside.cpp"
        outside.write_text("// outside\n", encoding="utf-8")
        for path in (self.checkout / "lib/sample/Missing.cpp", outside):
            with (
                self.subTest(path=path),
                patch.object(bazel_lint.bazel_workspace, "workspace_path", side_effect=AssertionError("workspace")),
                patch.object(bazel_lint.bazel_workspace, "bazel_executable", side_effect=AssertionError("tool")),
            ):
                self.assertEqual(self.run_quiet(self.args(files=[str(path)])), 2)

    def test_query_failure_stops_before_compatibility_or_report_build(self):
        self.query_status = 1
        self.assertEqual(
            self.run_quiet(self.args(files=[str(self.checkout / "lib/sample/Sample.cpp")])),
            2,
        )
        self.assertFalse(any(command[2] == "cquery" for command in self.commands))
        self.assertFalse(self.execute_calls)

    def test_unknown_cquery_rows_fail_closed_without_a_report_build(self):
        self.cquery_text = "@@//lib/sample:sample|False\n@@//lib/other:other|False\n"
        self.assertEqual(
            self.run_quiet(self.args(files=[str(self.checkout / "lib/sample/Sample.cpp")])),
            2,
        )
        self.assertFalse(self.execute_calls)
        receipt = json.loads(
            (self.tree / "lint").glob("run-*/cquery.receipt.json").__next__().read_text(encoding="utf-8")
        )
        self.assertIn("unknown cquery row", receipt["error"])

    def test_report_build_failure_is_not_gated_as_a_clean_inventory(self):
        self.build_status = 1
        self.assertEqual(
            self.run_quiet(self.args(files=[str(self.checkout / "lib/sample/Sample.cpp")])),
            2,
        )
        self.assertFalse(self.read_calls)
        receipt = json.loads(
            (self.tree / "lint").glob("run-*/build.receipt.json").__next__().read_text(encoding="utf-8")
        )
        self.assertEqual(receipt["status"], 1)

    def test_malformed_or_incomplete_current_reports_fail_closed(self):
        self.read_error = ValueError("aborted BEP event")
        self.assertEqual(
            self.run_quiet(self.args(files=[str(self.checkout / "lib/sample/Sample.cpp")])),
            2,
        )

    def test_empty_scope_returns_clean_without_preparing_a_workspace(self):
        args = self.args()
        args._resolved_sources = []
        self.assertEqual(self.run_quiet(args), 0)
        self.assertFalse(self.prepare_calls)
        self.assertFalse(self.commands)


if __name__ == "__main__":
    unittest.main()
