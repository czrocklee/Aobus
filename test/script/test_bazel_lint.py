"""Contracts for the production Bazel lint portal orchestration."""

import argparse
import contextlib
import io
import json
import os
import shlex
import sys
import tempfile
import unittest
from dataclasses import replace
from pathlib import Path
from types import SimpleNamespace
from unittest.mock import patch
from xml.etree import ElementTree as ET

from ao.__main__ import make_parser
from ao.command import check as check_command
from ao.core import bazel_native as native
from ao.graph import bazel_lint_reports
from ao.portal import bazel_lint, build_dir, git_files, source_analyzer, source_lint, source_scope, tidy_engine
from ao.portal.source_scope import SourceScope

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
  <rule class="objc_library" name="//app/macos-appkit:appkit">
    <list name="srcs">
      <label value="//app/macos-appkit:Foo.mm"/>
    </list>
  </rule>
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
_CONSUMER_CONFIG_PREFIX = "build:aobus_lint_consumers "
_CONSUMER_PARAMETER_OPTION = "--aspects_parameters=aobus_lint_consumers="
_SCOPE_CONFIG_PREFIX = "build:aobus_lint_scope "
_OUTPUT_GROUP_OPTION = "--output_groups="
_FULL_GROUP = "aobus_lint"
_INDEX_GROUP = "aobus_lint_index"
_SOURCES_GROUP = "aobus_lint_sources"


def bazel_action_of(command):
    """Return the Bazel action of a recorded command, after the governed rc selection."""
    return next(argument for argument in command[1:] if not argument.startswith("-"))


def bazel_expression_of(command):
    """Return the query expression that follows the action of a recorded command."""
    action = bazel_action_of(command)
    return command[command.index(action) + 1]


def decode_consumer_parameters(prepared):
    """Decode the named consumer config line of the real generated rc."""
    lines = (prepared / "aobus.bazelrc").read_text(encoding="utf-8").splitlines()
    selected = [line for line in lines if line.startswith(_CONSUMER_CONFIG_PREFIX)]
    if len(selected) != 1:
        raise AssertionError(f"expected exactly one named consumer config line: {selected}")
    option = json.loads(selected[0][len(_CONSUMER_CONFIG_PREFIX) :])
    if not option.startswith(_CONSUMER_PARAMETER_OPTION):
        raise AssertionError(f"unexpected aspects parameter option: {option}")
    return json.loads(option[len(_CONSUMER_PARAMETER_OPTION) :])


class TidyLintParserTest(unittest.TestCase):
    def test_explicit_owner_lint_does_not_fall_back_to_an_empty_changed_scope(self):
        args = make_parser().parse_args(
            [
                "check",
                "lint",
                "--config",
                "release",
                "--target",
                "//test/lint:cache_clean",
                "--tidy-check",
                "aobus-example",
            ]
        )
        with (
            patch.object(source_scope, "resolve", side_effect=AssertionError("no source discovery")) as scope,
            patch.object(bazel_lint, "run", return_value=7) as lint_run,
        ):
            self.assertTrue(check_command.requires_build_environment(args))
            self.assertEqual(check_command.run_command(args), 7)
        lint_run.assert_called_once()
        self.assertIsInstance(lint_run.call_args.args[0], source_lint.LintRequest)
        self.assertEqual(lint_run.call_args.args[0].owner_targets, ("//test/lint:cache_clean",))
        scope.assert_not_called()


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

    def test_optional_nonownership_label_does_not_block_actual_source_owners(self):
        self.path.write_text(
            '<query><rule class="cc_library" name="//lib/sample:sample">'
            '<list name="srcs"><label value="//lib/sample:Sample.cpp"/></list></rule>'
            '<rule class="native_context_test" name="//test:wrapped">'
            '<label name="gtk_enabled"/></rule></query>',
            encoding="utf-8",
        )
        graph = bazel_lint.parse_query_xml(self.path)
        self.assertEqual(graph.source_owners("lib/sample/Sample.cpp"), ("//lib/sample:sample",))
        self.assertEqual(graph.native_sources("//lib/sample:sample"), ("lib/sample/Sample.cpp",))

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

    def test_all_header_declarations_reach_consumer_owners(self):
        # The current ownership contract: hdrs, textual_hdrs and non-source
        # ``srcs`` declarations must all reach the declarer map that
        # ``consumer_owners`` answers over.
        for path in ("include/ao/sample/Sample.h", "lib/sample/Internal.h", "lib/sample/Sample.h"):
            with self.subTest(path=path):
                self.assertEqual(self.graph.consumer_owners(path), ("//lib/sample:sample",))

    def test_consumer_owners_reach_declarers_through_dependencies_and_have_sources(self):
        self.path.write_text(CONSUMER_XML, encoding="utf-8")
        graph = bazel_lint.parse_query_xml(self.path)
        self.assertEqual(graph.consumer_owners(SPAN), ("//lib/util:util", "//test/unit:unit"))
        self.assertEqual(graph.consumer_owners(INVENTORY), ("//app/i18n:i18n", "//tool/catalog:compiler"))
        self.assertEqual(graph.consumer_owners("app/include/ao/i18n/Ids.h"), ("//app/i18n:i18n",))
        self.assertEqual(graph.consumer_owners("include/ao/util/Undeclared.h"), ())

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


class _OrchestrationHarness:
    """The owned transport fakes shared by the scoped and project gates."""

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
        self.commands: list[list[str]] = []
        self.prepare_calls: list[dict[str, object]] = []
        self.execute_calls: list[dict[str, object]] = []
        self.read_calls: list[tuple[Path, list[str], tuple[str, ...]]] = []
        self.read_profiles: list[str] = []
        self.log_sequence = 0
        # The owned transport is faked below; the focused rc-isolation case
        # still needs the real production function at the process boundary.
        self.real_run_process = bazel_lint._run_process

        patches = [
            patch.object(bazel_lint, "PROJECT_ROOT", self.checkout),
            patch.object(bazel_lint, "os", SimpleNamespace(name="posix")),
            patch.object(tidy_engine, "PROJECT_ROOT", self.checkout),
            patch.object(bazel_lint.bazel_workspace, "workspace_path", side_effect=lambda path, config: self.tree),
            patch.object(bazel_lint.bazel_workspace, "bazel_executable", return_value=Path("bazel")),
            patch.object(native, "require_supported_source"),
            patch.object(native, "source_root", return_value=str(self.checkout)),
            patch.object(bazel_lint.bazel_workspace, "prepare_workspace", side_effect=self.fake_prepare),
            # Only the selected-Nix-JDK boundary is faked; the rc isolation
            # logic of bazel_workspace.invocation stays real.
            patch.object(
                bazel_lint.bazel_workspace, "_linux_server_javabase", return_value=Path("/nix/store/fixture-jdk")
            ),
            patch.object(bazel_lint.bazel_workspace, "execute", side_effect=self.fake_execute),
            patch.object(bazel_lint.bazel_lint_reports, "read_reports", side_effect=self.fake_read_reports),
            patch.object(bazel_lint, "build_tree_lock", return_value=contextlib.nullcontext()),
            patch.object(bazel_lint, "_run_process", side_effect=self.fake_process),
        ]
        for patcher in patches:
            patcher.start()
            self.addCleanup(patcher.stop)

    def fake_prepare(self, tree, config, *, prefix=None, graph_options=None, lint_configuration=None):
        self.prepare_calls.append(
            {
                "tree": tree,
                "config": config,
                "prefix": prefix,
                "graph_options": graph_options,
                "lint_configuration": lint_configuration,
            }
        )
        prepared = tree / "configuration"
        prepared.mkdir(parents=True, exist_ok=True)
        rc = prepared / "aobus.bazelrc"
        if not rc.is_file():
            # A real baseline rc, created once. Prior consumer and output
            # selection lines persist until the next report pass rewrites them.
            rc.write_text(
                "build " + json.dumps("--config=" + config, ensure_ascii=False) + "\n"
                "build --experimental_disk_cache_gc_max_size=50G\n",
                encoding="utf-8",
            )
        return prepared

    def fake_execute(
        self, executable, tree, prepared, action, config, targets, *, jobs=2, remote_cache=None, isolate_rc=False
    ):
        self.assertEqual((executable, action), (Path("bazel"), "build"))
        group_options = [option for option in targets if option.startswith(_OUTPUT_GROUP_OPTION)]
        if "--config=aobus_lint_scope" in targets:
            self.assertFalse(group_options)
            lines = (prepared / "aobus.bazelrc").read_text(encoding="utf-8").splitlines()
            group_options = [
                option
                for line in lines
                if line.startswith(_SCOPE_CONFIG_PREFIX)
                for option in shlex.split(line.removeprefix(_SCOPE_CONFIG_PREFIX))
            ]
        self.assertEqual(len(group_options), 1, group_options)
        self.assertTrue(group_options[0].startswith(_OUTPUT_GROUP_OPTION), group_options)
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
                "isolate_rc": isolate_rc,
                "output_groups": tuple(group_options[0].removeprefix(_OUTPUT_GROUP_OPTION).split(",")),
                # The consumer mapping the report pass selected through the
                # named config line in the real generated rc.
                "consumers": decode_consumer_parameters(prepared),
            }
        )
        return self.build_status

    def fake_read_reports(self, bep, owners, *, consumers="{}", profile="lint", output_groups=(_FULL_GROUP,)):
        self.read_calls.append((bep, list(owners), tuple(output_groups)))
        self.read_profiles.append(profile)
        build = self.execute_calls[-1]
        self.assertEqual(json.loads(consumers), build["consumers"])
        self.assertCountEqual(output_groups, build["output_groups"])
        self.assertCountEqual(owners, ["@@" + word for word in build["targets"] if word.startswith("//")])
        inventory = self.pass_inventories.pop(0) if self.pass_inventories else self.inventories
        self.assertCountEqual([item.owner for item in inventory], owners)

        def materialized(report):
            group = bazel_lint_reports.main_output_group(
                report.identity.main, windows=report.identity.platform == "windows"
            )
            return (
                _FULL_GROUP in output_groups
                or (_SOURCES_GROUP in output_groups and report.identity.source == report.identity.main)
                or group in output_groups
            )

        # The index still describes the full owner when a pass materializes no TU reports.
        return tuple(
            replace(
                item,
                reports=tuple(report for report in item.reports if materialized(report)),
                _omitted_native_tu=any(
                    report.identity.source == report.identity.main and not materialized(report)
                    for report in item.reports
                ),
            )
            for item in inventory
        )

    def assert_report_groups(self, *passes):
        self.assertEqual(len(self.read_calls), len(passes))
        for (bep, _, groups), (name, expected) in zip(self.read_calls, passes, strict=True):
            self.assertEqual(bep.name, f"{name}.bep.json")
            self.assertCountEqual(groups, expected)

    def fake_process(self, command, stdout, stderr):
        # Replace only the owned transport, not subprocess.run shared with the
        # standard library's lazy native platform probes.
        self.commands.append(list(command))
        action = bazel_action_of(command)
        self.assertEqual(command[0], "bazel")
        expression = bazel_expression_of(command)
        if action == "query" and "--output=xml" in command:
            xml = self.query_xml
            if expression.startswith('kind("rule", set(') and expression.endswith("))"):
                labels = shlex.split(expression.removeprefix('kind("rule", set(')[:-2])
                root = ET.fromstring(xml)
                for rule in root.findall("rule"):
                    if rule.attrib["name"] not in labels:
                        root.remove(rule)
                xml = ET.tostring(root, encoding="unicode")
            else:
                self.assertEqual(expression, 'kind("rule", //...)')
            stdout.write_text(xml, encoding="utf-8")
            stderr.write_text("query failure\n" if self.query_status else "", encoding="utf-8")
            return self.query_status
        if action == "cquery" and "--output=starlark" in command:
            self.assertTrue(expression.startswith("config(set(") and expression.endswith("), target)"), expression)
            labels = shlex.split(expression.removeprefix("config(set(").removesuffix("), target)"))
            rows = self.cquery_texts.pop(0) if self.cquery_texts else self.cquery_text
            self.assertCountEqual(labels, [row.split("|")[0].removeprefix("@@") for row in rows.splitlines()])
            stdout.write_text(rows, encoding="utf-8")
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
        values["tidy_check"] = values.pop("check")
        values["tidy_args"] = values.pop("clang_tidy_arg")
        for key in ("files", "folder", "owner_targets", "tidy_args", "_resolved_sources"):
            if isinstance(values.get(key), list):
                values[key] = tuple(values[key])
        return source_lint._NativeLintRequest(**values)

    def run_quiet(self, args):
        with contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()):
            return bazel_lint.run(args)

    def touch(self, *names: str) -> None:
        """Create authored checkout files the scope fixtures select."""
        for name in names:
            path = self.checkout / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text("// test input\n", encoding="utf-8")

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
        return bazel_lint_reports.Report(
            identity,
            outcome,
            1 if outcome == "compiler_error" else 0,
            self.root / "report.json",
            log_path,
            tuple(includes),
        )

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


class PerFileProvenanceTest(_OrchestrationHarness, unittest.TestCase):
    def setUp(self):
        super().setUp()
        for module in (source_lint, source_analyzer, source_scope, git_files):
            self.enterContext(patch.object(module, "PROJECT_ROOT", self.checkout))
        self.enterContext(patch.object(build_dir, "platform_profile", return_value=build_dir.LINUX_PROFILE))
        self.enterContext(patch.object(git_files, "prettier_sources", return_value=[]))
        self.touch("app/macos-appkit/Other.cpp")

    def run_service(self, kind, scope, *, report_only=False, **options):
        request_type = source_lint.LintRequest if kind == "lint" else source_analyzer.AnalyzerRequest
        request = request_type(path=str(self.tree), tidy_check="aobus-example", report_only=report_only, **options)
        run = source_lint.run if kind == "lint" else source_analyzer.run
        with (
            contextlib.redirect_stdout(io.StringIO()) as output,
            contextlib.redirect_stderr(io.StringIO()) as diagnostics,
        ):
            status = run(scope, request)
        return status, output.getvalue(), diagnostics.getvalue()

    def inventory(self, kind, *, main="lib/sample/Sample.cpp", owner="@@//lib/sample:sample", **diagnostic):
        report = self.report(owner, main, **diagnostic)
        if kind == "analyzer":
            report = replace(report, identity=replace(report.identity, mode="ANALYZER"))
        return (self.owner_inventory(owner, report),)

    def test_explicit_linux_file_does_not_make_scanned_macos_directory_explicit(self):
        scope = source_scope.resolve(source_scope.Selection(paths=("lib/sample/Sample.cpp", "app/macos-appkit")))
        self.assertEqual(scope.explicit_files, ("lib/sample/Sample.cpp",))
        scanned = set(scope.files) - set(scope.explicit_files)
        self.assertIn("app/macos-appkit/Foo.mm", scanned)
        self.cquery_text = "@@//app/macos-appkit:appkit|True\n@@//lib/sample:sample|False\n"
        for kind in ("lint", "analyzer"):
            with self.subTest(kind=kind):
                self.inventories = self.inventory(kind)
                status, _output, diagnostics = self.run_service(kind, scope)
                self.assertEqual(status, 0, diagnostics)
                receipt = json.loads(
                    next((self.tree / kind).glob("run-*/run.receipt.json")).read_text(encoding="utf-8")
                )
                self.assertEqual(receipt["selected"], ["lib/sample/Sample.cpp"])
                self.assertEqual(set(receipt["deferred"]), scanned)

    def gtk_scope(self):
        gtk_test = "test/linux-gtk/list/ListTest.cpp"
        bus_support = "test/linux-gtk/GtkSessionBusTestSupport.cpp"
        self.touch(gtk_test, bus_support)
        self.query_xml = """<query version="2">
          <rule class="cc_test" name="//test:gtk_test.native">
            <list name="srcs"><label value="//test:linux-gtk/list/ListTest.cpp"/></list>
          </rule>
          <rule class="cc_library" name="//test:gtk_session_bus_test_support">
            <list name="srcs"><label value="//test:linux-gtk/GtkSessionBusTestSupport.cpp"/></list>
          </rule>
        </query>"""
        self.cquery_text = "@@//test:gtk_test.native|True\n@@//test:gtk_session_bus_test_support|False\n"
        self.inventories = self.inventory(
            "lint",
            main=bus_support,
            owner="@@//test:gtk_session_bus_test_support",
            outcome="warning",
            log=f"{bus_support}:3:1: warning: GTK support finding [aobus-example]\n",
        )
        return gtk_test, bus_support

    def test_gtk_off_folder_defers_configured_out_tests_and_keeps_compatible_support(self):
        gtk_test, bus_support = self.gtk_scope()
        scope = source_scope.resolve(source_scope.Selection(paths=("test/linux-gtk",)))
        status, output, diagnostics = self.run_service("lint", scope, gtk="off")
        self.assertEqual(status, 1, diagnostics)
        self.assertIn(gtk_test, diagnostics)
        self.assertIn(f"{Path(bus_support)}:3:1: warning: GTK support finding", output)

    def test_gtk_off_folder_without_compatible_sources_needs_no_report_build(self):
        gtk_test, _bus_support = self.gtk_scope()
        self.cquery_text = "@@//test:gtk_test.native|True\n"
        scope = source_scope.resolve(source_scope.Selection(paths=("test/linux-gtk/list",)))
        status, _output, diagnostics = self.run_service("lint", scope, gtk="off")
        self.assertEqual(status, 0, diagnostics)
        self.assertIn(gtk_test, diagnostics)
        self.assertFalse(self.execute_calls)

    def test_gtk_off_still_fails_for_an_explicit_configured_out_test(self):
        gtk_test, _bus_support = self.gtk_scope()
        scope = source_scope.resolve(source_scope.Selection(paths=(gtk_test, "test/linux-gtk")))
        status, _output, diagnostics = self.run_service("lint", scope, gtk="off", report_only=True)
        self.assertEqual(status, 2, diagnostics)
        self.assertIn(gtk_test, diagnostics)

    def test_gtk_off_does_not_defer_an_unowned_file_just_because_it_is_gtk(self):
        self.gtk_scope()
        unowned = "test/linux-gtk/UnownedTest.cpp"
        self.touch(unowned)
        scope = source_scope.resolve(source_scope.Selection(paths=("test/linux-gtk",)))
        status, _output, diagnostics = self.run_service("lint", scope, gtk="off")
        self.assertEqual(status, 2, diagnostics)
        self.assertIn(unowned, diagnostics)

    def test_gtk_off_keeps_coverage_required_when_a_possible_owner_is_compatible(self):
        gtk_test, _ = self.gtk_scope()
        # Query unions the srcs branches, but the configured support target
        # only produced a report for its own source in this configuration.
        self.query_xml = self.query_xml.replace(
            '<label value="//test:linux-gtk/GtkSessionBusTestSupport.cpp"/>',
            '<label value="//test:linux-gtk/GtkSessionBusTestSupport.cpp"/>'
            '<label value="//test:linux-gtk/list/ListTest.cpp"/>',
        )
        scope = source_scope.resolve(source_scope.Selection(paths=("test/linux-gtk",)))
        status, _output, diagnostics = self.run_service("lint", scope, gtk="off")
        self.assertEqual(status, 2, diagnostics)
        self.assertIn(gtk_test, diagnostics)

    def test_windows_inventory_case_alias_retains_explicit_membership(self):
        canonical = "app/macos-appkit/Foo.h"
        alias = "APP/MACOS-APPKIT/FOO.H"
        owner = "@@//app/macos-appkit:appkit"
        report = self.report(owner, "app/macos-appkit/Other.cpp")
        report = replace(report, identity=replace(report.identity, platform="windows"))
        inventory = self.owner_inventory(owner, report, sources=(report.identity.source, canonical))
        inventory = replace(inventory, uncovered=(bazel_lint_reports.Uncovered(canonical, "no companion"),))
        with (
            patch.object(bazel_lint, "os", SimpleNamespace(name="nt")),
            patch.object(bazel_lint, "_platform_incompatible", return_value=True),
        ):
            selected = bazel_lint._match_paths({alias}, (inventory,))
            self.assertEqual(selected, {canonical})
            self.assertEqual(
                bazel_lint._deferred_paths(tuple(selected), (inventory,), allow=True, explicit_files=(alias,)), set()
            )
            self.assertEqual(bazel_lint._deferred_paths(tuple(selected), (inventory,), allow=True), {canonical})

    def test_early_unowned_explicit_file_survives_later_deferred_or_unreported_files(self):
        explicit = "app/macos-appkit/Unmapped.mm"
        scope = SourceScope((explicit, "app/macos-appkit/Foo.mm"), (explicit,))
        for kind in ("lint", "analyzer"):
            for later in ("incompatible-owner", "unreported-main"):
                self.cquery_text = "@@//app/macos-appkit:appkit|" + (
                    "True\n" if later == "incompatible-owner" else "False\n"
                )
                self.inventories = self.inventory(
                    kind, main="app/macos-appkit/Other.cpp", owner="@@//app/macos-appkit:appkit"
                )
                for advisory in (False, True):
                    with self.subTest(kind=kind, later=later, advisory=advisory):
                        status, _output, diagnostics = self.run_service(kind, scope, report_only=advisory)
                        self.assertEqual(status, 2, diagnostics)
                        self.assertIn(explicit, diagnostics)

    def test_explicit_foreign_main_missing_from_compatible_inventory_is_not_deferred(self):
        explicit = "app/macos-appkit/Foo.mm"
        scope = SourceScope((explicit,), (explicit,))
        self.cquery_text = "@@//app/macos-appkit:appkit|False\n"
        for kind in ("lint", "analyzer"):
            self.inventories = self.inventory(
                kind, main="app/macos-appkit/Other.cpp", owner="@@//app/macos-appkit:appkit"
            )
            for advisory in (False, True):
                with self.subTest(kind=kind, advisory=advisory):
                    status, _output, diagnostics = self.run_service(kind, scope, report_only=advisory)
                    self.assertEqual(status, 2, diagnostics)
                    self.assertIn(explicit, diagnostics)


class RunOrchestrationTest(_OrchestrationHarness, unittest.TestCase):
    def test_report_only_suppresses_findings_but_not_compiler_or_missing_context_failures(self):
        owner = "@@//lib/sample:sample"
        for advisory in (False, True):
            for outcome, expected in (("clean", 0), ("warning", 0 if advisory else 1), ("compiler_error", 2)):
                with self.subTest(advisory=advisory, outcome=outcome):
                    report = self.report(
                        owner,
                        "lib/sample/Sample.cpp",
                        outcome=outcome,
                        log="lib/sample/Sample.cpp:1:1: warning: finding [aobus-example]\n"
                        if outcome == "warning"
                        else "",
                    )
                    self.inventories = (self.owner_inventory(owner, report),)
                    self.assertEqual(
                        self.run_quiet(
                            self.args(files=[str(self.checkout / "lib/sample/Sample.cpp")], report_only=advisory)
                        ),
                        expected,
                    )
        self.inventories = (
            self.owner_inventory(
                owner, self.report(owner, "lib/sample/Sample.cpp", header="include/ao/sample/Sample.h")
            ),
        )
        self.assertEqual(self.run_quiet(self.args(owner_targets=["//lib/sample:sample"], report_only=True)), 2)

    def test_stale_findings_do_not_mask_explicit_missing_compile_context(self):
        owner = "@@//lib/sample:sample"
        source = self.checkout / "lib/sample/Sample.cpp"
        source.write_text("// NOLINT(misc-multiple-inheritance)\nint value;\n", encoding="utf-8")
        self.inventories = (self.owner_inventory(owner, self.report(owner, "lib/sample/Sample.cpp")),)
        for advisory in (False, True):
            with self.subTest(advisory=advisory):
                self.assertEqual(
                    self.run_quiet(
                        self.args(
                            files=[str(source), str(self.checkout / "app/macos-appkit/Unmapped.mm")],
                            report_only=advisory,
                        )
                    ),
                    2,
                )

    def test_named_checker_bypasses_the_unrelated_stale_suppression_audit(self):
        owner = "@@//lib/sample:sample"
        self.inventories = (self.owner_inventory(owner, self.report(owner, "lib/sample/Sample.cpp")),)
        with patch.object(
            bazel_lint,
            "find_stale_nolint_suppressions",
            side_effect=AssertionError("named check must not audit unrelated suppressions"),
        ):
            self.assertEqual(
                self.run_quiet(self.args(files=[str(self.checkout / "lib/sample/Sample.cpp")], check="aobus-example")),
                0,
            )

    def test_advisory_stale_findings_do_not_bypass_native_admission(self):
        source = self.checkout / "lib/sample/Sample.cpp"
        source.write_text("// NOLINT(misc-multiple-inheritance)\nint value;\n", encoding="utf-8")
        with patch.object(
            bazel_lint.bazel_workspace, "prepare_workspace", side_effect=native.BazelWorkspaceError("missing ready SDK")
        ) as prepare:
            self.assertEqual(self.run_quiet(self.args(files=[str(source)], report_only=True)), 2)
        prepare.assert_called_once()

    def test_ambient_rc_selection_cannot_influence_the_native_lint_client(self):
        owner = "@@//lib/sample:sample"
        self.inventories = (self.owner_inventory(owner, self.report(owner, "lib/sample/Sample.cpp")),)
        ambient = self.root / "ambient.bazelrc"
        ambient.write_text("build --aspects_parameters=aobus_lint_consumers=ambient\n", encoding="utf-8")
        child = (
            "import json,os; print(json.dumps({k: v for k, v in os.environ.items() "
            "if k.casefold() == 'bazelrc' or k == 'AOBUS_RC_TEST_SENTINEL'}))"
        )
        with patch.dict(os.environ, {"BAZELRC": str(ambient), "AOBUS_RC_TEST_SENTINEL": "retained"}):
            self.assertEqual(self.run_quiet(self.args(files=[str(self.checkout / "lib/sample/Sample.cpp")])), 0)
            # The real transport at the process boundary: a real child observes
            # the environment production hands to the Bazel client.
            stdout, stderr = self.root / "child.env.json", self.root / "child.stderr"
            self.assertEqual(self.real_run_process([sys.executable, "-c", child], stdout, stderr), 0)
        self.assertEqual(json.loads(stdout.read_text(encoding="utf-8")), {"AOBUS_RC_TEST_SENTINEL": "retained"})
        # Every query launches through the governed rc selection: personal,
        # system and workspace rc files are disabled and only the repository
        # and generated rc files remain.
        for command in self.commands:
            self.assertEqual(command[1:4], ["--nosystem_rc", "--nohome_rc", "--noworkspace_rc"])
            self.assertEqual(
                [argument for argument in command if argument.startswith("--bazelrc=")],
                [
                    f"--bazelrc={native.PROJECT_ROOT / '.bazelrc'}",
                    f"--bazelrc={self.tree / 'configuration' / 'aobus.bazelrc'}",
                ],
            )
        # The report build requests the same isolation as the completion gate.
        self.assertTrue(self.execute_calls)
        self.assertTrue(all(call["isolate_rc"] for call in self.execute_calls))

    def test_scoped_lint_forwards_explicit_feature_selections_to_the_prepared_workspace(self):
        owner = "@@//lib/sample:sample"
        self.cquery_text = "@@//lib/sample:sample|False\n"
        self.inventories = (self.owner_inventory(owner, self.report(owner, "lib/sample/Sample.cpp")),)
        # A fixed Linux profile keeps the cross-host assertion deterministic.
        linux = argparse.Namespace(name="linux")
        with patch.object(bazel_lint.bazel_workflows.build_dir, "platform_profile", return_value=linux):
            status = self.run_quiet(
                self.args(files=[str(self.checkout / "lib/sample/Sample.cpp")], compiler="clang", gtk="off")
            )
        self.assertEqual(status, 0)
        # The scoped portal shares the same prepared analysis interface.
        self.assertEqual(
            self.prepare_calls[0]["graph_options"],
            ("--//bazel:gtk_enabled=false", "--compiler=clang"),
        )

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
        self.assert_report_groups(("build", {_FULL_GROUP}))
        receipt = json.loads((self.tree / "lint").glob("run-*/run.receipt.json").__next__().read_text(encoding="utf-8"))
        self.assertEqual(receipt["selected"], ["include/ao/sample/Sample.h", "lib/sample/Sample.cpp"])

    def test_explicit_owner_query_is_bounded_and_uses_native_inventory_for_grouped_sources(self):
        self.query_xml = """<query><rule class="cc_library" name="//lib/sample:sample">
        <list name="srcs"><label value="//lib/sample:group"/></list></rule></query>"""
        owner = "@@//lib/sample:sample"
        self.inventories = (self.owner_inventory(owner, self.report(owner, "lib/sample/Grouped.cpp")),)
        self.assertEqual(self.run_quiet(self.args(owner_targets=["//lib/sample:sample"])), 0)
        query = next(command for command in self.commands if bazel_action_of(command) == "query")
        self.assertEqual(bazel_expression_of(query), 'kind("rule", set("//lib/sample:sample"))')
        receipt = json.loads((self.tree / "lint").glob("run-*/run.receipt.json").__next__().read_text(encoding="utf-8"))
        self.assertEqual(receipt["selected"], ["lib/sample/Grouped.cpp"])

    def test_scoped_source_requests_its_encoded_main_without_siblings_or_donors(self):
        owner = "@@//lib/sample:sample"
        main = "lib/sample-extra/Selected_Name.cpp"
        self.touch(main)
        self.query_xml = QUERY_XML.replace("//lib/sample:Sample.cpp", "//lib/sample-extra:Selected_Name.cpp")
        selected = self.report(owner, main)
        sibling = self.report(owner, "lib/sample/Grouped.cpp")
        self.inventories = (self.owner_inventory(owner, selected, sibling),)

        self.assertEqual(self.run_quiet(self.args(files=[main])), 0)
        self.assert_report_groups(("build", {_INDEX_GROUP, "aobus_lint_main_lib_ssample_hextra_sSelected__Name_dcpp"}))

    def test_windows_case_alias_materializes_and_gates_the_canonical_main(self):
        owner = "@@//lib/sample:sample"
        canonical = "lib/sample/Sample.cpp"
        alias = "lib/sample/SAMPLE.CPP"
        self.touch(alias)
        report = self.report(
            owner, canonical, outcome="warning", log=f"{canonical}:1:1: warning: finding [aobus-example]\n"
        )
        report = replace(report, identity=replace(report.identity, platform="windows"))
        self.inventories = (self.owner_inventory(owner, report),)
        with patch.object(bazel_lint, "os", SimpleNamespace(name="nt")):
            self.assertEqual(self.run_quiet(self.args(files=[alias], owner_targets=["//lib/sample:sample"])), 1)
        self.assert_report_groups(("build", {_INDEX_GROUP, "aobus_lint_main_lib_ssample_ssample_dcpp"}))
        receipt = json.loads(next((self.tree / "lint").glob("run-*/run.receipt.json")).read_text(encoding="utf-8"))
        self.assertEqual(receipt["selected"], [canonical])

    def test_explicit_owner_header_scope_uses_the_index_to_admit_the_companion_tu(self):
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
            self.run_quiet(self.args(files=["include/ao/sample/Sample.h"], owner_targets=["//lib/sample:sample"])),
            0,
        )
        self.assert_report_groups(("build", {_INDEX_GROUP, "aobus_lint_main_include_sao_ssample_sSample_dh"}))
        receipt = json.loads((self.tree / "lint").glob("run-*/run.receipt.json").__next__().read_text(encoding="utf-8"))
        self.assertEqual(receipt["selected"], ["include/ao/sample/Sample.h"])

    def test_discovered_owner_labels_with_punctuation_are_quoted_in_queries(self):
        # A discovered (not explicitly named) owner joins the cquery expression
        # as one literal word instead of being rejected or injected.
        self.query_xml = (
            '<query><rule class="cc_library" name="//lib/odd:odd@rule">'
            '<list name="srcs"><label value="//lib/odd:Source=File.cpp"/></list></rule></query>'
        )
        self.touch("lib/odd/Source=File.cpp")
        odd = "@@//lib/odd:odd@rule"
        self.cquery_text = odd + "|False\n"
        self.inventories = (self.owner_inventory(odd, self.report(odd, "lib/odd/Source=File.cpp")),)
        self.assertEqual(
            self.run_quiet(self.args(files=[str(self.checkout / "lib/odd/Source=File.cpp")])),
            0,
        )
        cquery = next(command for command in self.commands if bazel_action_of(command) == "cquery")
        self.assertEqual(bazel_expression_of(cquery), 'config(set("//lib/odd:odd@rule"), target)')
        self.assertIn("//lib/odd:odd@rule", self.execute_calls[0]["targets"])
        self.assert_report_groups(("build", {_FULL_GROUP}))

    def test_all_explicit_owners_incompatible_fails_without_a_report_build(self):
        self.cquery_text = "@@//app/macos-appkit:appkit|True\n"
        self.assertEqual(
            self.run_quiet(self.args(owner_targets=["//app/macos-appkit:appkit"])),
            2,
        )
        self.assertFalse(self.execute_calls)

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

    def test_batch_platform_incompatible_source_omitted_from_actual_configured_srcs_is_deferred(self):
        owner = "@@//lib/utility:utility"
        self.cquery_text = "@@//lib/utility:utility|False\n"
        self.inventories = (self.owner_inventory(owner, self.report(owner, "lib/utility/AtomicFile.cpp")),)
        with patch.object(bazel_lint, "_platform_incompatible", return_value=True):
            self.assertEqual(
                self.run_quiet(self.args(_resolved_sources=[str(self.checkout / "lib/utility/AtomicFileWindows.cpp")])),
                0,
            )
        self.assertEqual(self.read_calls[0][1], [owner])
        self.assert_report_groups(("build", {_INDEX_GROUP, "aobus_lint_main_lib_sutility_sAtomicFileWindows_dcpp"}))
        receipt = json.loads((self.tree / "lint").glob("run-*/run.receipt.json").__next__().read_text(encoding="utf-8"))
        self.assertEqual(receipt["selected"], [])
        self.assertEqual(receipt["deferred"], ["lib/utility/AtomicFileWindows.cpp"])

    def test_unpaired_header_replay_keeps_the_direct_source_but_not_its_donor(self):
        direct = "lib/util/Time.cpp"
        self.touch(direct)
        self.query_xml = CONSUMER_XML.replace(
            '<label value="//lib/util:Clock.cpp"/>',
            '<label value="//lib/util:Clock.cpp"/><label value="//lib/util:Time.cpp"/>',
        )
        owner = "@@//lib/util:util"
        self.cquery_text = owner + "|False\n"
        selected = self.report(owner, direct)
        source = self.report(owner, "lib/util/Clock.cpp", includes=(SPAN, "lib/util/Clock.h"))
        header = self.report(
            owner,
            "lib/util/Clock.cpp",
            header=SPAN,
            outcome="warning",
            log=f"{SPAN}:1:1: warning: header [aobus-example]\n",
        )
        before_replay = (self.owner_inventory(owner, selected, source),)
        self.pass_inventories = [before_replay, before_replay, (self.owner_inventory(owner, selected, source, header),)]
        self.assertEqual(self.run_quiet(self.args(files=[direct, SPAN])), 1)

        selected_groups = {
            _INDEX_GROUP,
            "aobus_lint_main_include_sao_sutil_sSpan_dh",
            "aobus_lint_main_lib_sutil_sTime_dcpp",
        }
        self.assert_report_groups(
            ("build", selected_groups),
            ("discovery", selected_groups | {_SOURCES_GROUP}),
            ("consumers", selected_groups),
        )
        self.assertEqual(
            [call["consumers"] for call in self.execute_calls], [{}, {}, {owner: [["lib/util/Clock.cpp", SPAN]]}]
        )
        # The next run must not inherit the previous header replay.
        self.inventories = (self.owner_inventory(owner, source),)
        self.assertEqual(self.run_quiet(self.args(files=[str(self.checkout / "lib/util/Clock.cpp")])), 0)
        self.assertEqual(self.execute_calls[-1]["consumers"], {})
        self.assertCountEqual(self.read_calls[-1][2], {_INDEX_GROUP, "aobus_lint_main_lib_sutil_sClock_dcpp"})

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

        selected_groups = {_INDEX_GROUP, "aobus_lint_main_app_sinclude_sao_si18n_sInventory_ddef"}
        self.assert_report_groups(
            ("build", selected_groups | {_SOURCES_GROUP}),
            ("consumer-owners", selected_groups | {_SOURCES_GROUP}),
            ("consumers", selected_groups),
        )
        self.assertEqual(
            [owners for _, owners, _ in self.read_calls], [[nearest], [nearest, compiler], [nearest, compiler]]
        )
        self.assertEqual(self.execute_calls[-1]["consumers"], {compiler: [["tool/catalog/Main.cpp", INVENTORY]]})

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
            (self.owner_inventory(owner, source),),
            (self.owner_inventory(owner, source, header),),
        ]
        self.assertEqual(self.run_quiet(self.args(files=[str(path)])), 1)
        receipt = json.loads(next((self.tree / "lint").glob("run-*/run.receipt.json")).read_text(encoding="utf-8"))
        self.assertEqual(receipt["selected"], [header_path])
        self.assertEqual(receipt["deferred"], [])
        self.assertEqual(len(self.execute_calls), 3)

    def test_compatible_header_that_no_analyzed_unit_includes_fails_with_its_reason(self):
        self.query_xml = CONSUMER_XML
        owner = "@@//lib/util:util"
        test = "@@//test/unit:unit"
        self.cquery_texts = [owner + "|False\n", test + "|False\n"]
        self.pass_inventories = [
            (self.owner_inventory(owner, self.report(owner, "lib/util/Clock.cpp")),),
            (self.owner_inventory(owner, self.report(owner, "lib/util/Clock.cpp")),),
            (
                self.owner_inventory(owner, self.report(owner, "lib/util/Clock.cpp")),
                self.owner_inventory(test, self.report(test, "test/unit/SpanTest.cpp")),
            ),
        ]
        stderr = io.StringIO()
        with contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(stderr):
            self.assertEqual(bazel_lint.run(self.args(files=[str(self.checkout / SPAN)])), 2)
        selected_groups = {_INDEX_GROUP, "aobus_lint_main_include_sao_sutil_sSpan_dh"}
        self.assert_report_groups(
            ("build", selected_groups),
            ("discovery", selected_groups | {_SOURCES_GROUP}),
            ("consumer-owners", selected_groups | {_SOURCES_GROUP}),
        )
        self.assertIn(f"{SPAN}: no analyzed translation unit includes it", stderr.getvalue())

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
        self.assertFalse(any(bazel_action_of(command) == "cquery" for command in self.commands))
        self.assertFalse(self.execute_calls)

    def test_report_build_failure_is_not_gated_as_a_clean_inventory(self):
        self.build_status = 1
        self.assertEqual(
            self.run_quiet(self.args(files=[str(self.checkout / "lib/sample/Sample.cpp")])),
            2,
        )
        self.assertFalse(self.read_calls)

    def test_empty_scope_returns_clean_without_preparing_a_workspace(self):
        args = self.args(_resolved_sources=())
        self.assertEqual(self.run_quiet(args), 0)
        self.assertFalse(self.prepare_calls)
        self.assertFalse(self.commands)


class AnalyzerOrchestrationTest(_OrchestrationHarness, unittest.TestCase):
    def run_analyzer(self, args, files, *, explicit_files=()):
        with contextlib.redirect_stdout(io.StringIO()) as out, contextlib.redirect_stderr(io.StringIO()) as err:
            status = bazel_lint.run_analyzer(args, files, explicit_files=explicit_files)
        return status, out.getvalue(), err.getvalue()

    def args(self, **overrides):
        return source_analyzer.AnalyzerRequest(path=str(self.tree), jobs=2, **overrides)

    def report(self, owner, source, *, outcome="clean", log=""):
        report = super().report(owner, source, outcome=outcome, log=log)
        return replace(report, identity=replace(report.identity, mode="ANALYZER"))

    def test_alpha_reaches_the_analyzer_policy_in_its_native_fastbuild_context(self):
        owner = "@@//lib/sample:sample"
        source = "lib/sample/Sample.cpp"
        self.inventories = (self.owner_inventory(owner, self.report(owner, source)),)
        status, _, diagnostics = self.run_analyzer(self.args(alpha=True), [source], explicit_files=(source,))
        self.assertEqual(status, 0, diagnostics)
        prepared = self.prepare_calls[0]
        self.assertTrue(json.loads(prepared["lint_configuration"].read_text(encoding="utf-8"))["alpha"])
        self.assertEqual(prepared["config"], "fastbuild")
        self.assertEqual(self.read_profiles, ["analyzer"])
        self.assert_report_groups(("build", {_FULL_GROUP}))

    def test_analyzer_findings_fail_by_default_and_report_only_is_explicit(self):
        log = "lib/sample/Sample.cpp:1:1: warning: leak [clang-analyzer-cplusplus.NewDeleteLeaks]\n"
        owner = "@@//lib/sample:sample"
        source = "lib/sample/Sample.cpp"
        self.inventories = (self.owner_inventory(owner, self.report(owner, source, outcome="warning", log=log)),)
        status, out, _ = self.run_analyzer(
            self.args(),
            [str(self.checkout / "lib/sample/Sample.cpp")],
            explicit_files=(str(self.checkout / "lib/sample/Sample.cpp"),),
        )
        self.assertEqual(status, 1)
        self.assertIn("Analyzer diagnostics: 1", out)
        self.assertIn("leak", out)
        status, out, _ = self.run_analyzer(
            self.args(report_only=True),
            [str(self.checkout / "lib/sample/Sample.cpp")],
            explicit_files=(str(self.checkout / "lib/sample/Sample.cpp"),),
        )
        self.assertEqual(status, 0)

    def test_analyzer_compiler_errors_stay_nonzero_even_report_only(self):
        log = "lib/sample/Sample.cpp:2:3: error: unknown type [clang-diagnostic-error]\n"
        owner = "@@//lib/sample:sample"
        source = "lib/sample/Sample.cpp"
        self.inventories = (self.owner_inventory(owner, self.report(owner, source, outcome="compiler_error", log=log)),)
        status, _, err = self.run_analyzer(self.args(report_only=True), [source], explicit_files=(source,))
        self.assertEqual(status, 2)
        self.assertIn("unknown type", err)

    def test_explicit_covered_but_unreported_objective_cpp_requires_an_exact_context(self):
        # A compatible owner whose configured build captured no Objective-C++
        # compile action leaves the explicit .mm unreported; the analyzer never
        # borrows a context, so the historical exactness error stays a failure.
        self.cquery_text = "@@//app/macos-appkit:appkit|False\n"
        owner = "@@//app/macos-appkit:appkit"
        self.touch("app/macos-appkit/Other.cpp")
        self.inventories = (self.owner_inventory(owner, self.report(owner, "app/macos-appkit/Other.cpp")),)
        status, out, err = self.run_analyzer(
            self.args(),
            [str(self.checkout / "app/macos-appkit/Foo.mm")],
            explicit_files=(str(self.checkout / "app/macos-appkit/Foo.mm"),),
        )
        self.assertEqual(status, 2)
        self.assertIn("Objective-C++ analysis requires an exact native compile command:", err)
        self.assertIn("app/macos-appkit/Foo.mm", err)
        self.assertEqual(len(self.execute_calls), 1)
        self.assertNotIn("No analyzer diagnostics found.", out)


if __name__ == "__main__":
    unittest.main()
