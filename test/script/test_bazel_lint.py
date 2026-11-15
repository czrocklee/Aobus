"""Contracts for the production Bazel lint portal orchestration."""

import argparse
import contextlib
import io
import json
import os
import subprocess
import sys
import tempfile
import unittest
from dataclasses import replace
from pathlib import Path
from unittest.mock import patch

from ao.__main__ import make_parser
from ao.command import tidy as tidy_command
from ao.core import bazel_lint, bazel_lint_reports
from ao.core import bazel_native as native

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
  <source-file name="//lib/sample:Sample.cpp"/>
  <source-file name="//lib/sample:SampleLinux.cpp"/>
  <source-file name="//lib/sample:Grouped.cpp"/>
  <source-file name="//lib/sample:Aliased.cpp"/>
  <source-file name="//lib/other:Other.cpp"/>
  <source-file name="//lib/utility:AtomicFile.cpp"/>
  <source-file name="//lib/utility:AtomicFileWindows.cpp"/>
  <source-file name="//app/sample:Main.cpp"/>
  <source-file name="//app/foo:BarWindows.cpp"/>
  <source-file name="//lib/foo:BarPosix.cpp"/>
  <source-file name="//test:winui/ThemeTest.cpp"/>
  <source-file name="//app/macos-appkit:Foo.mm"/>
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

# The complete ao check graph: rules plus the source-file targets that make
# authored checkout inputs distinguishable from generated ones.
PROJECT_XML = """<?xml version="1.1" encoding="UTF-8" standalone="no"?>
<query version="2">
  <rule class="cc_library" name="//lib/sample:sample">
    <list name="srcs">
      <label value="//lib/sample:Sample.cpp"/>
      <label value="//lib/sample:Sample.h"/>
    </list>
    <list name="hdrs">
      <label value="//include/ao/sample:Sample.h"/>
    </list>
    <list name="textual_hdrs">
      <label value="//lib/sample:Internal.h"/>
    </list>
  </rule>
  <rule class="cc_library" name="//lib/sample:header_only">
    <list name="hdrs">
      <label value="//include/ao/sample:Only.h"/>
    </list>
  </rule>
  <rule class="cc_library" name="//lib/generated:generated">
    <list name="srcs">
      <label value="//lib/generated:Generated.cpp"/>
    </list>
    <list name="hdrs">
      <label value="//lib/generated:Generated.h"/>
    </list>
  </rule>
  <rule class="cc_library" name="//test/lint:negative">
    <list name="srcs">
      <label value="//test/lint:HeaderReplay.cpp"/>
    </list>
  </rule>
  <rule class="cc_test" name="//test:winui_test">
    <list name="srcs">
      <label value="//test:winui/ThemeTest.cpp"/>
    </list>
  </rule>
  <source-file name="//lib/sample:Sample.cpp"/>
  <source-file name="//lib/sample:Sample.h"/>
  <source-file name="//include/ao/sample:Sample.h"/>
  <source-file name="//lib/sample:Internal.h"/>
  <source-file name="//include/ao/sample:Only.h"/>
  <source-file name="//test/lint:HeaderReplay.cpp"/>
  <source-file name="//test:winui/ThemeTest.cpp"/>
  <generated-file name="//lib/generated:Generated.cpp"/>
</query>
"""

# Complete-graph orchestration fixtures: every rule kind the project gate
# builds, with authored files present for the scope's existence filter.
PROJECT_BUILD_XML = """<?xml version="1.1" encoding="UTF-8" standalone="no"?>
<query version="2">
  <rule class="cc_library" name="//lib/alpha:alpha">
    <list name="srcs"><label value="//lib/alpha:Alpha.cpp"/></list>
    <list name="hdrs"><label value="//include/ao/alpha:Alpha.h"/></list>
  </rule>
  <rule class="cc_library" name="//lib/utility:utility">
    <list name="srcs">
      <label value="//lib/utility:AtomicFile.cpp"/>
      <label value="//lib/utility:AtomicFileWindows.cpp"/>
    </list>
  </rule>
  <rule class="cc_test" name="//test/zeta:zeta">
    <list name="srcs"><label value="//test/zeta:Zeta.cpp"/></list>
  </rule>
  <source-file name="//lib/alpha:Alpha.cpp"/>
  <source-file name="//include/ao/alpha:Alpha.h"/>
  <source-file name="//lib/utility:AtomicFile.cpp"/>
  <source-file name="//lib/utility:AtomicFileWindows.cpp"/>
  <source-file name="//test/zeta:Zeta.cpp"/>
</query>
"""

PROJECT_CONSUMER_XML = """<?xml version="1.1" encoding="UTF-8" standalone="no"?>
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
    <list name="deps"><label value="//lib/util:util"/></list>
  </rule>
  <source-file name="//include:ao/util/Span.h"/>
  <source-file name="//lib/util:Clock.cpp"/>
  <source-file name="//test/unit:SpanTest.cpp"/>
</query>
"""

PROJECT_INCOMPATIBLE_XML = """<?xml version="1.1" encoding="UTF-8" standalone="no"?>
<query version="2">
  <rule class="cc_library" name="//tool/thing:thing">
    <list name="srcs"><label value="//tool/thing:Thing.cpp"/></list>
  </rule>
  <source-file name="//tool/thing:Thing.cpp"/>
</query>
"""

# A declaration another native platform owns: the inherited policy defers it
# and the complete gate rejects the empty remainder instead of passing clean.
PROJECT_FOREIGN_XML = """<?xml version="1.1" encoding="UTF-8" standalone="no"?>
<query version="2">
  <rule class="cc_library" name="//tool/thing:thing">
    <list name="srcs"><label value="//tool/thing:DeviceMacos.cpp"/></list>
  </rule>
  <source-file name="//tool/thing:DeviceMacos.cpp"/>
</query>
"""

PROJECT_DEAD_XML = """<?xml version="1.1" encoding="UTF-8" standalone="no"?>
<query version="2">
  <rule class="cc_library" name="//include:dead_headers">
    <list name="hdrs"><label value="//include:Dead.h"/></list>
  </rule>
  <rule class="cc_library" name="//lib/alpha:alpha">
    <list name="srcs"><label value="//lib/alpha:Alpha.cpp"/></list>
  </rule>
  <source-file name="//include:Dead.h"/>
  <source-file name="//lib/alpha:Alpha.cpp"/>
</query>
"""

PROJECT_DEFERRED_XML = """<?xml version="1.1" encoding="UTF-8" standalone="no"?>
<query version="2">
  <rule class="cc_library" name="//lib/sample:sample">
    <list name="srcs"><label value="//lib/sample:Sample.cpp"/></list>
    <list name="textual_hdrs"><label value="//lib/sample:Internal.h"/></list>
  </rule>
  <rule class="cc_library" name="//lib/utility:utility">
    <list name="srcs">
      <label value="//lib/utility:AtomicFile.cpp"/>
      <label value="//lib/utility:PortableRing.cpp"/>
    </list>
  </rule>
  <source-file name="//lib/sample:Sample.cpp"/>
  <source-file name="//lib/sample:Internal.h"/>
  <source-file name="//lib/utility:AtomicFile.cpp"/>
  <source-file name="//lib/utility:PortableRing.cpp"/>
</query>
"""

# A broad header glob declares every platform's headers on every host, so the
# configured closure stays conservative: platform-owned headers defer under
# the inherited policy while portable dead declarations still fail.
PROJECT_PLATFORM_XML = """<?xml version="1.1" encoding="UTF-8" standalone="no"?>
<query version="2">
  <rule class="cc_library" name="//lib/audio:audio">
    <list name="srcs">
      <label value="//lib/audio:Audio.cpp"/>
      <label value="//lib/audio:backend/CoreAudioBackend.h"/>
    </list>
  </rule>
  <rule class="cc_library" name="//lib/sample:sample">
    <list name="srcs"><label value="//lib/sample:Sample.cpp"/></list>
    <list name="textual_hdrs"><label value="//lib/sample:PortableDead.h"/></list>
  </rule>
  <source-file name="//lib/audio:Audio.cpp"/>
  <source-file name="//lib/audio:backend/CoreAudioBackend.h"/>
  <source-file name="//lib/sample:Sample.cpp"/>
  <source-file name="//lib/sample:PortableDead.h"/>
</query>
"""


def bazel_action_of(command):
    """Return the Bazel action of a recorded command, after the governed rc selection."""
    return next(argument for argument in command[1:] if not argument.startswith("-"))


def bazel_expression_of(command):
    """Return the query expression that follows the action of a recorded command."""
    action = bazel_action_of(command)
    return command[command.index(action) + 1]


def canonical_consumer_parameters(mapping):
    """Return the canonical compact sorted ASCII JSON the producer must write."""
    return json.dumps(mapping, sort_keys=True, separators=(",", ":"), ensure_ascii=True)


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
    def test_tidy_parser_wires_scope_check_and_explicit_lint_owners(self):
        args = make_parser().parse_args(
            [
                "tidy",
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
                "--tidy-arg=--quiet",
                "--header-filter",
                "^include",
                "--debug",
                "-j",
                "3",
            ]
        )
        self.assertEqual(args.owner_targets, ["//lib/sample:sample", "//lib/other:other"])
        self.assertEqual(args.files, ["lib/sample/Sample.cpp"])
        self.assertEqual(args.folder, ["app"])
        self.assertEqual(args.commit, "HEAD~1")
        self.assertEqual(args.check, "aobus-example")
        self.assertEqual(args.clang_tidy_arg, ["--quiet"])
        self.assertEqual(args.header_filter, "^include")
        self.assertTrue(args.debug)
        self.assertEqual(args.jobs, 3)

    def test_tidy_rejects_fix_requests_before_any_preparation(self):
        for arguments in (["--fix"], ["--tidy-arg=--fix"], ["--tidy-arg=--export-fixes=out.yaml"]):
            with self.subTest(arguments=arguments), patch("sys.stderr"), self.assertRaises(SystemExit) as exited:
                make_parser().parse_args(["tidy", *arguments])
            self.assertEqual(exited.exception.code, 2)

    def test_tidy_dispatch_reaches_the_lint_owner_before_workspace_preparation(self):
        with tempfile.TemporaryDirectory(prefix="ao-tidy-lint-dispatch-") as temporary:
            root = Path(temporary)
            source = root / "lib" / "sample" / "Sample.cpp"
            source.parent.mkdir(parents=True)
            source.write_text("// lint dispatch fixture\n", encoding="utf-8")
            args = make_parser().parse_args(
                ["tidy", "lib/sample/Sample.cpp", "--check", "aobus-example", "--target", "//lib/sample:sample"]
            )
            with (
                patch.object(tidy_command, "PROJECT_ROOT", root),
                patch.object(bazel_lint, "run", return_value=0) as lint_run,
                patch.object(bazel_lint.bazel_workspace, "workspace_path", side_effect=AssertionError("workspace")),
                contextlib.redirect_stdout(io.StringIO()),
                contextlib.redirect_stderr(io.StringIO()),
            ):
                self.assertEqual(tidy_command.run_command(args), 0)
            lint_run.assert_called_once()
            self.assertEqual(lint_run.call_args.args[0]._resolved_sources, ["lib/sample/Sample.cpp"])
            self.assertEqual(lint_run.call_args.args[0].owner_targets, ["//lib/sample:sample"])
            self.assertEqual(lint_run.call_args.args[0].check, "aobus-example")

    def test_explicit_owner_lint_does_not_fall_back_to_an_empty_changed_scope(self):
        args = make_parser().parse_args(
            ["tidy", "--config", "release", "--target", "//test/lint:cache_clean", "--check", "aobus-example"]
        )
        with (
            patch.object(tidy_command.tidyengine, "resolve_scope", return_value=([], False)) as scope,
            patch.object(bazel_lint, "run", return_value=7) as lint_run,
        ):
            self.assertTrue(tidy_command.requires_build_environment(args))
            self.assertEqual(tidy_command.run_command(args), 7)
        lint_run.assert_called_once_with(args)
        scope.assert_not_called()


class TidyProjectStageTest(unittest.TestCase):
    """run_lint_stages routes only the native C++ stage to the complete graph."""

    def args(self, root: Path) -> argparse.Namespace:
        return argparse.Namespace(
            config="debug",
            path=str(root / "tree"),
            vcpkg_prefix=None,
            jobs=2,
            remote_cache=None,
            check=None,
            output=None,
            debug=False,
        )

    def test_project_lint_runs_once_on_an_empty_cpp_scope(self):
        with tempfile.TemporaryDirectory(prefix="ao-tidy-project-") as temporary:
            root = Path(temporary)
            with (
                patch.object(tidy_command, "PROJECT_ROOT", root),
                patch.object(tidy_command.bazel_lint, "run_project", return_value=1) as project,
                patch.object(tidy_command.bazel_lint, "run") as scoped,
            ):
                # The stage collapses any nonzero native gate to its own failure.
                self.assertEqual(tidy_command.run_lint_stages(self.args(root), [], False, project_lint=True), 1)
            project.assert_called_once()
            scoped.assert_not_called()

    def test_project_lint_replaces_the_cpp_stage_and_keeps_other_stages_scoped(self):
        with tempfile.TemporaryDirectory(prefix="ao-tidy-project-") as temporary:
            root = Path(temporary)
            for name in ("script/module.py", "BUILD", "lib/utility/Base64.cpp"):
                path = root / name
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text("# fixture\n", encoding="utf-8")
            files = ["script/module.py", "BUILD", "lib/utility/Base64.cpp"]
            with (
                patch.object(tidy_command, "PROJECT_ROOT", root),
                patch.object(tidy_command.bazel_lint, "run_project", return_value=1) as project,
                patch.object(tidy_command.bazel_lint, "run") as scoped,
                patch.object(tidy_command.pythoncheck, "run_paths", return_value=0) as python,
                patch.object(tidy_command.buildifier, "run", return_value=0) as buildifier_run,
                contextlib.redirect_stdout(io.StringIO()),
            ):
                self.assertEqual(tidy_command.run_lint_stages(self.args(root), files, False, project_lint=True), 1)
            project.assert_called_once()
            scoped.assert_not_called()
            python.assert_called_once_with(["script/module.py"], python=None, sink=None)
            buildifier_run.assert_called_once_with(["BUILD"], check=True, lint=True, sink=None)

    def test_scoped_cpp_lint_stays_the_default(self):
        with tempfile.TemporaryDirectory(prefix="ao-tidy-project-") as temporary:
            root = Path(temporary)
            source = root / "lib" / "utility" / "Base64.cpp"
            source.parent.mkdir(parents=True)
            source.write_text("// fixture\n", encoding="utf-8")
            with (
                patch.object(tidy_command, "PROJECT_ROOT", root),
                patch.object(tidy_command.bazel_lint, "run", return_value=0) as scoped,
                patch.object(tidy_command.bazel_lint, "run_project") as project,
            ):
                self.assertEqual(tidy_command.run_lint_stages(self.args(root), [str(source)], False), 0)
                self.assertEqual(tidy_command.run_lint_stages(self.args(root), [], False), 0)
            scoped.assert_called_once()
            project.assert_not_called()

    def test_project_lint_carries_optional_completion_flags_to_the_native_gate(self):
        with tempfile.TemporaryDirectory(prefix="ao-tidy-project-") as temporary:
            root = Path(temporary)
            args = self.args(root)
            args.clang = True
            args.gtk = "off"
            with (
                patch.object(tidy_command, "PROJECT_ROOT", root),
                patch.object(tidy_command.bazel_lint, "run_project", return_value=0) as project,
            ):
                self.assertEqual(tidy_command.run_lint_stages(args, [], False, project_lint=True), 0)
            forwarded = project.call_args.args[0]
            self.assertIs(forwarded.clang, True)
            self.assertEqual(forwarded.gtk, "off")
            # Absent optional fields stay absent: the standalone parser's
            # namespace contract is unchanged.
            self.assertNotIn("compiler", vars(forwarded))
            self.assertNotIn("system_media", vars(forwarded))


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

    def test_query_words_use_literal_delimiters_without_json_escapes(self):
        for label, expected in (
            ("//lib:owner(1)", '"//lib:owner(1)"'),
            ('//lib:na"me', "'//lib:na\"me'"),
            ("//lib:can't", '"//lib:can\'t"'),
            ("//lib:café", '"//lib:café"'),
        ):
            with self.subTest(label=label):
                self.assertEqual(bazel_lint._query_word(label), expected)
        with self.assertRaisesRegex(ValueError, "both quote characters"):
            bazel_lint._query_word("//lib:both'\"quotes")

    def test_trusted_target_labels_admit_documented_punctuation(self):
        self.path.write_text(
            """<query>
  <rule class="cc_library" name="//lib/odd:odd#rule$1">
    <list name="srcs"><label value="//lib/odd:Source=File.cpp"/></list>
    <list name="hdrs"><label value="//lib/odd:Header@2x.h"/></list>
  </rule>
  <rule class="cc_library" name="//lib/odd:odd(rule)">
    <list name="srcs"><label value="//lib/odd:Source (1).cpp"/></list>
  </rule>
  <source-file name="//lib/odd:Source=File.cpp"/>
  <source-file name="@@//lib/odd:Header@2x.h"/>
  <source-file name="//lib/odd:Source (1).cpp"/>
  <source-file name="//lib/odd:na&quot;me.cpp"/>
  <source-file name="@external//x:y.h"/>
</query>""",
            encoding="utf-8",
        )
        graph = bazel_lint.parse_query_xml(self.path)
        owner = "//lib/odd:odd#rule$1"
        self.assertIn(owner, graph.rules)
        self.assertIn("//lib/odd:odd(rule)", graph.rules)
        self.assertEqual(graph.source_owners("lib/odd/Source=File.cpp"), (owner,))
        self.assertEqual(graph.declared_paths(owner), ("lib/odd/Source=File.cpp", "lib/odd/Header@2x.h"))
        self.assertEqual(graph.source_owners("lib/odd/Source (1).cpp"), ("//lib/odd:odd(rule)",))
        # Canonical @@ and bare spellings are one checkout file; external
        # repository labels are not authored project inputs.
        self.assertEqual(
            graph.checkout_files,
            frozenset(
                {
                    "lib/odd/Source=File.cpp",
                    "lib/odd/Header@2x.h",
                    "lib/odd/Source (1).cpp",
                    'lib/odd/na"me.cpp',
                }
            ),
        )

    def test_structurally_invalid_target_labels_fail_closed(self):
        for name in ("//lib/odd:..", "//lib/odd", "//lib/odd:a:b", "//lib/odd:./x.cpp"):
            with self.subTest(name=name), self.assertRaises(ValueError):
                bazel_lint.parse_query_xml(self._xml(name))

    def _xml(self, name: str) -> Path:
        self.path.write_text(f'<query><rule class="cc_library" name="{name}"/></query>', encoding="utf-8")
        return self.path

    def test_checkout_source_files_follow_the_trusted_label_grammar(self):
        self.assertIn("lib/sample/Sample.cpp", self.graph.checkout_files)
        self.assertNotIn("lib/sample/Unknown.cpp", self.graph.checkout_files)
        self.path.write_text(PROJECT_XML, encoding="utf-8")
        graph = bazel_lint.parse_query_xml(self.path)
        self.assertIn("test/winui/ThemeTest.cpp", graph.checkout_files)
        self.assertNotIn("lib/generated/Generated.cpp", graph.checkout_files)


class ProjectScopeTest(unittest.TestCase):
    """Complete owner/path derivation from the actual project graph."""

    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="ao-lint-project-scope-")
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.path = self.root / "query.xml"
        self.path.write_text(PROJECT_XML, encoding="utf-8")
        self.graph = bazel_lint.parse_query_xml(self.path)

    def authored(self, *names: str) -> None:
        for name in names:
            path = self.root / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text("// test input\n", encoding="utf-8")

    def test_scope_gates_declared_checkout_paths_and_builds_every_declaring_owner(self):
        with patch.object(bazel_lint, "PROJECT_ROOT", self.root):
            self.authored(
                "lib/sample/Sample.cpp",
                "lib/sample/Sample.h",
                "include/ao/sample/Sample.h",
                "lib/sample/Internal.h",
                "include/ao/sample/Only.h",
                "test/lint/HeaderReplay.cpp",
                "test/winui/ThemeTest.cpp",
            )
            owners, gated = bazel_lint._project_scope(self.graph)
        self.assertEqual(owners, ("//lib/sample:header_only", "//lib/sample:sample", "//test:winui_test"))
        self.assertEqual(
            gated,
            frozenset(
                {
                    "lib/sample/Sample.cpp",
                    "lib/sample/Sample.h",
                    "include/ao/sample/Sample.h",
                    "lib/sample/Internal.h",
                    "include/ao/sample/Only.h",
                    "test/winui/ThemeTest.cpp",
                }
            ),
        )
        # Header-only owners join the build so their configured inventories
        # carry the select authority; generated-only and canary rules neither
        # join the build nor the set.
        self.assertNotIn("//lib/generated:generated", owners)
        self.assertNotIn("//test/lint:negative", owners)
        self.assertNotIn("lib/generated/Generated.cpp", gated)
        self.assertNotIn("test/lint/HeaderReplay.cpp", gated)

    def test_graph_declarations_without_an_authored_file_never_gate(self):
        with patch.object(bazel_lint, "PROJECT_ROOT", self.root):
            owners, gated = bazel_lint._project_scope(self.graph)
        self.assertEqual(owners, ())
        self.assertEqual(gated, frozenset())


class CqueryRowsTest(unittest.TestCase):
    def test_exact_rows_are_parsed_with_bazel_label_spellings(self):
        self.assertEqual(
            bazel_lint.parse_cquery_rows("@@//lib/sample:sample|False\n//lib/other:other|True\n"),
            {"//lib/sample:sample": False, "//lib/other:other": True},
        )


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
                "@@//tool/catalog:compiler": [["tool/catalog/Main.cpp", INVENTORY]],
                "@@//lib/util:util": [
                    ["lib/util/Clock.cpp", "include/ao/util/Bits.h"],
                    ["lib/util/Clock.cpp", SPAN],
                ],
            },
        )


class LintConfigurationTest(unittest.TestCase):
    def test_fixed_configuration_has_no_scope_and_is_rewritten_only_on_content_change(self):
        with tempfile.TemporaryDirectory(prefix="ao-lint-config-") as temporary:
            path = Path(temporary) / "configuration" / "lint.json"
            with patch.object(native, "source_root", return_value="/checkout"):
                configuration = bazel_lint._lint_configuration("aobus-example", "^include", ["--quiet"])
            self.assertEqual(
                configuration,
                {
                    "schema": 1,
                    "source_root": "/checkout",
                    "check": "aobus-example",
                    "header_filter": "^include",
                    "clang_tidy_args": ["--quiet"],
                    "alpha": False,
                },
            )
            self.assertNotIn("scope", configuration)
            self.assertTrue(bazel_lint._write_lint_config(path, configuration))
            self.assertFalse(bazel_lint._write_lint_config(path, configuration))
            self.assertEqual(json.loads(path.read_text(encoding="utf-8")), configuration)


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
        self.read_error: Exception | None = None
        self.commands: list[list[str]] = []
        self.prepare_calls: list[dict[str, object]] = []
        self.execute_calls: list[dict[str, object]] = []
        self.read_calls: list[tuple[Path, list[str]]] = []
        self.read_consumers: list[str] = []
        self.read_profiles: list[str] = []
        self.log_sequence = 0
        # The owned transport is faked below; the focused rc-isolation case
        # still needs the real production function at the process boundary.
        self.real_run_process = bazel_lint._run_process

        patches = [
            patch.object(bazel_lint, "PROJECT_ROOT", self.checkout),
            patch.object(tidy_command, "PROJECT_ROOT", self.checkout),
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
            # A real baseline rc, created once. A consumer line left by an
            # earlier run is deliberately preserved: each report pass must
            # rewrite it, so no pre-query reset is needed.
            rc.write_text(
                "build " + json.dumps("--config=" + config, ensure_ascii=False) + "\n"
                "build --experimental_disk_cache_gc_max_size=50G\n",
                encoding="utf-8",
            )
        return prepared

    def fake_execute(
        self, executable, tree, prepared, action, config, targets, *, jobs=2, remote_cache=None, isolate_rc=False
    ):
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
                # The consumer mapping the report pass selected through the
                # named config line in the real generated rc.
                "consumers": decode_consumer_parameters(prepared),
            }
        )
        return self.build_status

    def fake_read_reports(self, bep, owners, *, consumers="{}", profile="lint"):
        self.read_calls.append((bep, list(owners)))
        # The literal consumer payload and analysis kind each pass hands to the reader.
        self.read_consumers.append(consumers)
        self.read_profiles.append(profile)
        if self.read_error is not None:
            raise self.read_error
        # A consumer flow reads one inventory per report pass.
        return self.pass_inventories.pop(0) if self.pass_inventories else self.inventories

    def fake_process(self, command, stdout, stderr):
        # Replace only the owned transport, not subprocess.run shared with the
        # standard library's lazy native platform probes.
        self.commands.append(list(command))
        action = bazel_action_of(command)
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

    def touch(self, *names: str) -> None:
        """Create authored checkout files the complete graph scope must see."""
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


class RunOrchestrationTest(_OrchestrationHarness, unittest.TestCase):
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
                "alpha": False,
            },
        )
        self.assertEqual(self.prepare_calls[0]["lint_configuration"], lint_path)
        self.assertEqual(self.prepare_calls[0]["prefix"], "/vcpkg")
        self.assertEqual(self.execute_calls[0]["jobs"], 3)
        self.assertEqual(self.execute_calls[0]["remote_cache"], "https://cache.example")
        targets = self.execute_calls[0]["targets"]
        self.assertIn("//lib/sample:sample", targets)
        self.assertIn("--aspects=//bazel/lint:lint.bzl%lint_aspect", targets)
        self.assertIn("--config=aobus_lint_consumers", targets)
        self.assertFalse(any(target.startswith("--aspects_parameters=") for target in targets))
        self.assertIn("--output_groups=aobus_lint", targets)
        self.assertIn("--remote_download_outputs=all", targets)
        self.assertIn("--experimental_build_event_upload_strategy=local", targets)
        self.assertIn("--norun_validations", targets)
        self.assertTrue(any(target.startswith("--build_event_json_file=") for target in targets))
        self.assertEqual(self.read_calls[0][1], [owner])
        self.assertEqual(self.read_consumers[0], "{}")

        query = next(command for command in self.commands if bazel_action_of(command) == "query")
        self.assertIn('kind("rule", //...)', query)
        self.assertIn("--output=xml", query)
        cquery = next(command for command in self.commands if bazel_action_of(command) == "cquery")
        self.assertEqual(bazel_expression_of(cquery), 'config(set("//lib/sample:sample"), target)')
        self.assertIn(
            '--starlark:expr=str(target.label) + "|" + str("IncompatiblePlatformProvider" in providers(target))',
            cquery,
        )
        # The graph and compatibility queries never select the private report
        # config and never carry the consumer mapping on their command line.
        for command in self.commands:
            self.assertNotIn("--config=aobus_lint_consumers", command)
            self.assertFalse(any(part.startswith(_CONSUMER_PARAMETER_OPTION) for part in command))
        self.assertFalse(any("aquery" in command for command in self.commands))
        receipt = json.loads((self.tree / "lint").glob("run-*/run.receipt.json").__next__().read_text(encoding="utf-8"))
        self.assertEqual(receipt["selected"], ["lib/sample/Sample.cpp"])

    def test_fastbuild_lints_in_its_true_context_and_unknown_configs_fail_closed(self):
        owner = "@@//lib/sample:sample"
        self.inventories = (self.owner_inventory(owner, self.report(owner, "lib/sample/Sample.cpp")),)
        self.assertEqual(
            self.run_quiet(self.args(files=[str(self.checkout / "lib/sample/Sample.cpp")], config="fastbuild")),
            0,
        )
        # The prepared rc and the report build both carry the requested
        # fastbuild context, never a Debug or opt projection of it.
        self.assertEqual(self.prepare_calls[0]["config"], "fastbuild")
        self.assertEqual(self.execute_calls[0]["config"], "fastbuild")
        with patch.object(bazel_lint.bazel_workspace, "workspace_path", side_effect=AssertionError("workspace")):
            self.assertEqual(
                self.run_quiet(self.args(files=[str(self.checkout / "lib/sample/Sample.cpp")], config="profile")),
                2,
            )

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

    def test_scoped_lint_forwards_explicit_feature_selections_to_the_prepared_workspace(self):
        owner = "@@//lib/sample:sample"
        self.cquery_text = "@@//lib/sample:sample|False\n"
        self.inventories = (self.owner_inventory(owner, self.report(owner, "lib/sample/Sample.cpp")),)
        # A fixed Linux profile keeps the cross-host assertion deterministic.
        linux = argparse.Namespace(name="linux")
        with patch.object(bazel_lint.bazel_workflows.builddir, "platform_profile", return_value=linux):
            status = self.run_quiet(
                self.args(files=[str(self.checkout / "lib/sample/Sample.cpp")], clang=True, gtk="off")
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
        cquery = next(command for command in self.commands if bazel_action_of(command) == "cquery")
        self.assertEqual(bazel_expression_of(cquery), 'config(set("//lib/sample:sample" "//lib/other:other"), target)')

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
        cquery = next(command for command in self.commands if bazel_action_of(command) == "cquery")
        self.assertEqual(
            bazel_expression_of(cquery), 'config(set("//lib/sample:sample" "//app/macos-appkit:appkit"), target)'
        )
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
        self.assertTrue(any(bazel_action_of(command) == "cquery" for command in self.commands))
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

        parameters = canonical_consumer_parameters({owner: [["lib/util/Clock.cpp", SPAN]]})
        self.assertEqual([call["targets"][0] for call in self.execute_calls], ["//lib/util:util", "//lib/util:util"])
        self.assertEqual(
            [call["consumers"] for call in self.execute_calls],
            [{}, {owner: [["lib/util/Clock.cpp", SPAN]]}],
        )
        self.assertEqual(self.read_consumers, ["{}", parameters])
        # The whole mapping stays in the rc line, never on the build command.
        self.assertIn("--config=aobus_lint_consumers", self.execute_calls[1]["targets"])
        self.assertFalse(any(parameters in target for target in self.execute_calls[1]["targets"]))
        self.assertEqual(sum(bazel_action_of(command) == "cquery" for command in self.commands), 1)
        receipt = json.loads((self.tree / "lint").glob("run-*/run.receipt.json").__next__().read_text(encoding="utf-8"))
        self.assertEqual(receipt["consumers"], {SPAN: {"owner": owner, "source": "lib/util/Clock.cpp"}})
        self.assertEqual(Path(receipt["bep"]).name, "consumers.bep.json")
        pass_receipt = json.loads(
            (self.tree / "lint").glob("run-*/consumers.receipt.json").__next__().read_text(encoding="utf-8")
        )
        self.assertEqual(pass_receipt["aspect"], bazel_lint_reports.lint_aspect_id(parameters))
        self.assertEqual(pass_receipt["aspect_parameters"], parameters)
        # The next run rewrites the stale consumer line itself: each report
        # pass writes its own mapping, so no pre-query reset is needed.
        self.pass_inventories = [(self.owner_inventory(owner, source),)]
        self.run_quiet(self.args(files=[str(self.checkout / "lib/util/Clock.cpp")]))
        self.assertEqual(self.execute_calls[-1]["consumers"], {})
        self.assertEqual(self.read_consumers[-1], "{}")

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

        cqueries = [bazel_expression_of(command) for command in self.commands if bazel_action_of(command) == "cquery"]
        self.assertEqual(
            cqueries,
            [
                'config(set("//app/i18n:i18n"), target)',
                'config(set("//tool/catalog:compiler"), target)',
            ],
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
            {compiler: [["tool/catalog/Main.cpp", INVENTORY]]},
        )
        parameters = canonical_consumer_parameters({compiler: [["tool/catalog/Main.cpp", INVENTORY]]})
        self.assertEqual(self.read_consumers, ["{}", "{}", parameters])
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
            [bazel_expression_of(command) for command in self.commands if bazel_action_of(command) == "cquery"],
            ['config(set("//lib/util:util"), target)', 'config(set("//test/unit:unit"), target)'],
        )
        self.assertEqual(self.read_calls[-1][1], [owner])
        self.assertEqual(self.execute_calls[-1]["consumers"], {owner: [[source.identity.source, SPAN]]})
        parameters = canonical_consumer_parameters({owner: [[source.identity.source, SPAN]]})
        self.assertEqual(self.read_consumers[-1], parameters)

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
        self.assertEqual(sum(bazel_action_of(command) == "cquery" for command in self.commands), 2)
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
        self.assertFalse(any(bazel_action_of(command) == "cquery" for command in self.commands))
        self.assertFalse(self.execute_calls)

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

    def test_empty_scope_returns_clean_without_preparing_a_workspace(self):
        args = self.args()
        args._resolved_sources = []
        self.assertEqual(self.run_quiet(args), 0)
        self.assertFalse(self.prepare_calls)
        self.assertFalse(self.commands)


class AuthoredManifestTest(unittest.TestCase):
    """The complete gate's independent Git manifest collector contract."""

    def setUp(self):
        # The collector honors the native VM mirror binding in production; a
        # repository-fixture test must not inherit that ambient selection.
        for key in ("AOBUS_GIT_DIR", "AOBUS_GIT_WORK_TREE"):
            value = os.environ.pop(key, None)
            if value is not None:
                self.addCleanup(os.environ.__setitem__, key, value)

    def test_manifest_keeps_nul_separated_filenames_whole(self):
        # Exercise the documented Git wire format without requiring filenames
        # the host filesystem cannot represent (notably newlines on Windows).
        result = subprocess.CompletedProcess([], 0, stdout=b"Plain.cpp\0We ird.cpp\0New\nline.cpp\0", stderr=b"")
        with patch.object(bazel_lint.subprocess, "run", return_value=result):
            manifest = bazel_lint._authored_manifest()
        self.assertEqual(manifest, ["Plain.cpp", "We ird.cpp", "New\nline.cpp"])

    def test_manifest_failure_fails_closed(self):
        with tempfile.TemporaryDirectory(prefix="ao-authored-manifest-") as temporary:
            # A directory without a repository is a failed inventory, never an
            # empty one that would silently pass the coverage audit.
            with patch.object(bazel_lint, "PROJECT_ROOT", Path(temporary)):
                with self.assertRaises(ValueError):
                    bazel_lint._authored_manifest()


class ProjectLintTest(_OrchestrationHarness, unittest.TestCase):
    """The complete project graph gate ao check runs instead of a scoped stage."""

    def setUp(self):
        super().setUp()
        # The complete gate audits the authored inventory independently, so
        # each test's fixture declares exactly the files it touches; the real
        # Git manifest is covered by its own collector contract test.
        for path in sorted(self.checkout.rglob("*"), reverse=True):
            if path.is_file():
                path.unlink()
        manifest = patch.object(
            bazel_lint,
            "_authored_manifest",
            side_effect=lambda: [
                path.relative_to(self.checkout).as_posix()
                for path in sorted(self.checkout.rglob("*"))
                if path.is_file()
            ],
        )
        manifest.start()
        self.addCleanup(manifest.stop)

    def run_project(self, args):
        with contextlib.redirect_stdout(io.StringIO()) as out, contextlib.redirect_stderr(io.StringIO()) as err:
            status = bazel_lint.run_project(args)
        return status, out.getvalue(), err.getvalue()

    def test_project_lint_gates_the_whole_graph_with_every_compatible_owner_once(self):
        self.query_xml = PROJECT_BUILD_XML
        self.touch(
            "lib/alpha/Alpha.cpp",
            "include/ao/alpha/Alpha.h",
            "lib/utility/AtomicFile.cpp",
            "lib/utility/AtomicFileWindows.cpp",
            "test/zeta/Zeta.cpp",
        )
        alpha = "@@//lib/alpha:alpha"
        utility = "@@//lib/utility:utility"
        zeta = "@@//test/zeta:zeta"
        self.cquery_text = f"{alpha}|False\n{utility}|False\n{zeta}|False\n"
        self.inventories = (
            self.owner_inventory(
                alpha,
                self.report(alpha, "lib/alpha/Alpha.cpp"),
                self.report(alpha, "lib/alpha/Alpha.cpp", header="include/ao/alpha/Alpha.h"),
            ),
            self.owner_inventory(utility, self.report(utility, "lib/utility/AtomicFile.cpp")),
            self.owner_inventory(
                zeta,
                self.report(
                    zeta,
                    "test/zeta/Zeta.cpp",
                    outcome="warning",
                    log="test/zeta/Zeta.cpp:1:1: warning: project [aobus-example]\n",
                ),
            ),
        )
        # A scoped file selection cannot narrow the complete graph gate.
        # This fixture represents a Linux configuration on every test host.
        with patch.object(
            bazel_lint.tidyengine.builddir, "platform_profile", return_value=argparse.Namespace(name="linux")
        ):
            status, _out, err = self.run_project(self.args(files=[str(self.checkout / "lib/alpha/Alpha.cpp")]))
        self.assertEqual(status, 1)
        # The select-omitted source defers only through the inherited
        # platform ownership contract.
        self.assertIn("Deferred files that are incompatible with this platform:", err)
        self.assertIn("  lib/utility/AtomicFileWindows.cpp", err)

        query = next(command for command in self.commands if bazel_action_of(command) == "query")
        self.assertEqual(bazel_expression_of(query), 'kind("rule|source file", //...:*)')
        cquery = next(command for command in self.commands if bazel_action_of(command) == "cquery")
        self.assertEqual(
            bazel_expression_of(cquery),
            'config(set("//lib/alpha:alpha" "//lib/utility:utility" "//test/zeta:zeta"), target)',
        )
        self.assertEqual(len(self.execute_calls), 1)
        self.assertEqual(self.read_calls[0][1], [alpha, utility, zeta])
        receipt = json.loads((self.tree / "lint").glob("run-*/run.receipt.json").__next__().read_text(encoding="utf-8"))
        self.assertEqual(receipt["scope"], "project")
        self.assertEqual(receipt["owners"], ["//lib/alpha:alpha", "//lib/utility:utility", "//test/zeta:zeta"])
        self.assertEqual(
            receipt["selected"],
            ["include/ao/alpha/Alpha.h", "lib/alpha/Alpha.cpp", "lib/utility/AtomicFile.cpp", "test/zeta/Zeta.cpp"],
        )
        self.assertEqual(receipt["deferred"], ["lib/utility/AtomicFileWindows.cpp"])
        self.assertEqual(receipt["consumers"], {})
        # The readable summary keeps the complete gate's scale explainable.
        self.assertIn(
            "Complete native C++ lint graph: 3 compatible owner(s), 4 selected path(s), "
            "0 consumer-bound header(s)/fragment(s), 1 deferred path(s)",
            err,
        )

    def test_project_lint_binds_header_declarations_through_one_consumer_pass(self):
        self.query_xml = PROJECT_CONSUMER_XML
        self.touch(SPAN, "lib/util/Clock.cpp", "test/unit/SpanTest.cpp")
        headers = "@@//include:util_headers"
        util = "@@//lib/util:util"
        unit = "@@//test/unit:unit"
        self.cquery_text = f"{headers}|False\n{util}|False\n{unit}|False\n"
        source = self.report(util, "lib/util/Clock.cpp", includes=(SPAN,))
        test_source = self.report(unit, "test/unit/SpanTest.cpp", includes=(SPAN,))
        header = self.report(
            util,
            "lib/util/Clock.cpp",
            header=SPAN,
            outcome="warning",
            log=f"{SPAN}:1:1: warning: project header [aobus-example]\n",
        )
        self.pass_inventories = [
            (
                self.owner_inventory(headers, sources=(SPAN,)),
                self.owner_inventory(util, source),
                self.owner_inventory(unit, test_source),
            ),
            (
                self.owner_inventory(headers, sources=(SPAN,)),
                self.owner_inventory(util, source, header),
                self.owner_inventory(unit, test_source),
            ),
        ]
        status, _out, _err = self.run_project(self.args())
        self.assertEqual(status, 1)
        self.assertEqual(len(self.execute_calls), 2)
        self.assertEqual(
            [target for target in self.execute_calls[0]["targets"] if target.startswith("//")],
            ["//include:util_headers", "//lib/util:util", "//test/unit:unit"],
        )
        # One consumer pass over the same owners keeps one inventory provenance.
        self.assertEqual(
            [target for target in self.execute_calls[1]["targets"] if target.startswith("//")],
            ["//include:util_headers", "//lib/util:util", "//test/unit:unit"],
        )
        parameters = canonical_consumer_parameters({util: [["lib/util/Clock.cpp", SPAN]]})
        self.assertEqual(self.read_consumers, ["{}", parameters])
        self.assertEqual(self.read_calls[1][1], [headers, util, unit])
        receipt = json.loads((self.tree / "lint").glob("run-*/run.receipt.json").__next__().read_text(encoding="utf-8"))
        self.assertEqual(receipt["consumers"], {SPAN: {"owner": util, "source": "lib/util/Clock.cpp"}})
        self.assertEqual(Path(receipt["bep"]).name, "consumers.bep.json")

    def test_project_lint_rejects_a_complete_gate_without_any_compatible_inputs(self):
        self.query_xml = PROJECT_FOREIGN_XML
        self.touch("tool/thing/DeviceMacos.cpp")
        self.cquery_text = "@@//tool/thing:thing|True\n"
        # A fixed Linux profile keeps the inherited platform policy deterministic.
        linux = argparse.Namespace(name="linux")
        with patch.object(bazel_lint.tidyengine.builddir, "platform_profile", return_value=linux):
            status, _out, err = self.run_project(self.args())
        self.assertEqual(status, 2)
        self.assertFalse(self.execute_calls)
        # Every input is platform-owned elsewhere, so nothing remains to gate.
        self.assertIn("complete project lint has no compatible native C++ inputs", err)
        self.assertIn("Deferred files that are incompatible with this platform:", err)
        self.assertIn("  tool/thing/DeviceMacos.cpp", err)
        receipt = json.loads((self.tree / "lint").glob("run-*/run.receipt.json").__next__().read_text(encoding="utf-8"))
        self.assertEqual(receipt["deferred"], ["tool/thing/DeviceMacos.cpp"])
        self.assertEqual(receipt["selected"], [])

    def test_project_lint_fails_declarations_no_compatible_owner_reports(self):
        self.query_xml = PROJECT_INCOMPATIBLE_XML
        self.touch("tool/thing/Thing.cpp")
        self.cquery_text = "@@//tool/thing:thing|True\n"
        status, _out, err = self.run_project(self.args())
        self.assertEqual(status, 2)
        self.assertFalse(self.execute_calls)
        # Without platform ownership, a foreign owner's portable declaration is
        # an unreported source, never a silent deferral.
        self.assertIn("Uncovered lint source: tool/thing/Thing.cpp", err)
        receipt = json.loads((self.tree / "lint").glob("run-*/run.receipt.json").__next__().read_text(encoding="utf-8"))
        self.assertEqual(receipt["deferred"], [])
        self.assertEqual(receipt["selected"], ["tool/thing/Thing.cpp"])

    def test_project_lint_fails_declarations_no_translation_unit_reaches(self):
        self.query_xml = PROJECT_DEAD_XML
        self.touch("include/Dead.h", "lib/alpha/Alpha.cpp")
        dead = "@@//include:dead_headers"
        alpha = "@@//lib/alpha:alpha"
        self.cquery_text = f"{dead}|False\n{alpha}|False\n"
        self.inventories = (
            self.owner_inventory(dead, sources=("include/Dead.h",)),
            self.owner_inventory(alpha, self.report(alpha, "lib/alpha/Alpha.cpp")),
        )
        status, _out, err = self.run_project(self.args())
        self.assertEqual(status, 2)
        # The header-only declarer joins the build; its configured declaration
        # without any including translation unit stays a failure.
        self.assertIn("//include:dead_headers", self.execute_calls[0]["targets"])
        self.assertIn("include/Dead.h: no analyzed translation unit includes it", err)
        receipt = json.loads((self.tree / "lint").glob("run-*/run.receipt.json").__next__().read_text(encoding="utf-8"))
        self.assertEqual(receipt["selected"], ["include/Dead.h", "lib/alpha/Alpha.cpp"])
        self.assertEqual(receipt["deferred"], [])

    def test_project_lint_fails_unreported_select_branches_and_dead_declarations(self):
        self.query_xml = PROJECT_DEFERRED_XML
        self.touch("lib/sample/Sample.cpp", "lib/sample/Internal.h", "lib/utility/PortableRing.cpp")
        sample = "@@//lib/sample:sample"
        utility = "@@//lib/utility:utility"
        self.cquery_text = f"{sample}|False\n{utility}|False\n"
        self.inventories = (
            self.owner_inventory(
                sample,
                self.report(sample, "lib/sample/Sample.cpp"),
                sources=("lib/sample/Sample.cpp", "lib/sample/Internal.h"),
            ),
            self.owner_inventory(utility, self.report(utility, "lib/utility/AtomicFile.cpp")),
        )
        status, _out, err = self.run_project(self.args())
        self.assertEqual(status, 2)
        # An inactive select branch has no report and no platform ownership: it
        # fails instead of silently passing as covered elsewhere, and the
        # configured textual header no translation unit includes still fails.
        self.assertIn("Uncovered lint source: lib/utility/PortableRing.cpp", err)
        self.assertIn("lib/sample/Internal.h: no analyzed translation unit includes it", err)
        receipt = json.loads((self.tree / "lint").glob("run-*/run.receipt.json").__next__().read_text(encoding="utf-8"))
        self.assertEqual(receipt["deferred"], [])

    def test_project_lint_defers_configured_platform_headers_but_fails_portable_declarations(self):
        self.query_xml = PROJECT_PLATFORM_XML
        self.touch(
            "lib/audio/Audio.cpp",
            "lib/audio/backend/CoreAudioBackend.h",
            "lib/sample/Sample.cpp",
            "lib/sample/PortableDead.h",
        )
        audio = "@@//lib/audio:audio"
        sample = "@@//lib/sample:sample"
        self.cquery_text = f"{audio}|False\n{sample}|False\n"
        self.inventories = (
            self.owner_inventory(
                audio,
                self.report(audio, "lib/audio/Audio.cpp"),
                sources=("lib/audio/Audio.cpp", "lib/audio/backend/CoreAudioBackend.h"),
            ),
            self.owner_inventory(
                sample,
                self.report(sample, "lib/sample/Sample.cpp"),
                sources=("lib/sample/Sample.cpp", "lib/sample/PortableDead.h"),
            ),
        )
        # A fixed Linux profile keeps the inherited platform policy deterministic.
        linux = argparse.Namespace(name="linux")
        with patch.object(bazel_lint.tidyengine.builddir, "platform_profile", return_value=linux):
            status, _out, err = self.run_project(self.args())
        self.assertEqual(status, 2)
        # The broad declared closure is conservative: a configured but
        # unincluded platform-owned header defers under the inherited policy,
        # while a portable configured declaration with no consumer fails.
        self.assertIn("Deferred files that are incompatible with this platform:", err)
        self.assertIn("  lib/audio/backend/CoreAudioBackend.h", err)
        self.assertIn("lib/sample/PortableDead.h: no analyzed translation unit includes it", err)
        receipt = json.loads((self.tree / "lint").glob("run-*/run.receipt.json").__next__().read_text(encoding="utf-8"))
        self.assertEqual(receipt["deferred"], ["lib/audio/backend/CoreAudioBackend.h"])
        self.assertEqual(
            receipt["selected"],
            ["lib/audio/Audio.cpp", "lib/sample/PortableDead.h", "lib/sample/Sample.cpp"],
        )

    def test_project_lint_fails_stale_suppressions_over_the_complete_selection(self):
        self.query_xml = PROJECT_BUILD_XML
        self.touch(
            "lib/alpha/Alpha.cpp",
            "include/ao/alpha/Alpha.h",
            "lib/utility/AtomicFile.cpp",
            "lib/utility/AtomicFileWindows.cpp",
        )
        self.touch("test/zeta/Zeta.cpp")
        (self.checkout / "test/zeta/Zeta.cpp").write_text("// NOLINT(readability-magic-numbers)\n", encoding="utf-8")
        status, _out, err = self.run_project(self.args())
        self.assertEqual(status, 1)
        self.assertIn("stale NOLINT suppressions", err)
        self.assertIn("test/zeta/Zeta.cpp:1: readability-magic-numbers", err)
        self.assertFalse(self.execute_calls)
        self.assertFalse(any(bazel_action_of(command) == "cquery" for command in self.commands))

    def test_project_lint_forwards_explicit_feature_selections_to_the_prepared_workspace(self):
        self.query_xml = PROJECT_BUILD_XML
        self.touch("lib/alpha/Alpha.cpp", "include/ao/alpha/Alpha.h")
        alpha = "@@//lib/alpha:alpha"
        self.cquery_text = alpha + "|False\n"
        self.inventories = (
            self.owner_inventory(
                alpha,
                self.report(alpha, "lib/alpha/Alpha.cpp"),
                self.report(alpha, "lib/alpha/Alpha.cpp", header="include/ao/alpha/Alpha.h"),
            ),
        )
        # A fixed Linux profile keeps the cross-host assertion deterministic.
        linux = argparse.Namespace(name="linux")
        with patch.object(bazel_lint.bazel_workflows.builddir, "platform_profile", return_value=linux):
            status, _out, _err = self.run_project(self.args(clang=True, gtk="off", system_media="on"))
        self.assertEqual(status, 0)
        self.assertEqual(
            self.prepare_calls[0]["graph_options"],
            ("--//bazel:gtk_enabled=false", "--//bazel:system_media_enabled=true", "--compiler=clang"),
        )

    def test_project_lint_quotes_discovered_owner_labels_in_queries(self):
        self.query_xml = (
            '<query><rule class="cc_library" name="//lib/odd:odd@rule">'
            '<list name="srcs"><label value="//lib/odd:Source=File.cpp"/></list></rule>'
            '<source-file name="//lib/odd:Source=File.cpp"/></query>'
        )
        self.touch("lib/odd/Source=File.cpp")
        odd = "@@//lib/odd:odd@rule"
        self.cquery_text = odd + "|False\n"
        self.inventories = (self.owner_inventory(odd, self.report(odd, "lib/odd/Source=File.cpp")),)
        status, _out, _err = self.run_project(self.args())
        self.assertEqual(status, 0)
        # A punctuation-bearing discovered label joins the query expression as
        # one quoted word instead of being rejected or injected.
        cquery = next(command for command in self.commands if bazel_action_of(command) == "cquery")
        self.assertEqual(bazel_expression_of(cquery), 'config(set("//lib/odd:odd@rule"), target)')
        self.assertIn("//lib/odd:odd@rule", self.execute_calls[0]["targets"])

    def test_project_lint_fails_an_authored_source_no_rule_declares(self):
        self.query_xml = PROJECT_BUILD_XML
        self.touch(
            "lib/alpha/Alpha.cpp",
            "include/ao/alpha/Alpha.h",
            "lib/utility/AtomicFile.cpp",
            "lib/utility/AtomicFileWindows.cpp",
            "test/zeta/Zeta.cpp",
            "lib/orphan/Mystery.cpp",
        )
        status, _out, err = self.run_project(self.args())
        # The independent authored inventory catches a physical file no Bazel
        # rule declares, before any expensive native work.
        self.assertEqual(status, 2)
        self.assertIn("authored C++ sources no Bazel lint owner declares", err)
        self.assertIn("  lib/orphan/Mystery.cpp", err)
        self.assertFalse(any(bazel_action_of(command) == "cquery" for command in self.commands))
        self.assertFalse(self.execute_calls)

    def test_project_lint_fails_authored_sources_outside_every_listed_root(self):
        self.query_xml = PROJECT_BUILD_XML
        self.touch(
            "lib/alpha/Alpha.cpp",
            "include/ao/alpha/Alpha.h",
            "lib/utility/AtomicFile.cpp",
            "lib/utility/AtomicFileWindows.cpp",
            "test/zeta/Zeta.cpp",
            "RootNew.cpp",
            "newcomponent/Widget.cpp",
        )
        status, _out, err = self.run_project(self.args())
        # The full manifest is not limited to any folder list: an undeclared
        # root file or a brand-new component fails the complete gate too.
        self.assertEqual(status, 2)
        self.assertIn("authored C++ sources no Bazel lint owner declares", err)
        self.assertIn("  RootNew.cpp", err)
        self.assertIn("  newcomponent/Widget.cpp", err)
        self.assertFalse(self.execute_calls)

    def test_project_lint_fails_closed_when_authored_sources_have_no_lint_owner(self):
        self.query_xml = (
            '<query><rule class="filegroup" name="//lib:group">'
            '<list name="srcs"><label value="//lib:Grouped.cpp"/></list></rule>'
            '<source-file name="//lib:Grouped.cpp"/></query>'
        )
        self.touch("lib/Grouped.cpp")
        status, _out, err = self.run_project(self.args())
        # A filegroup is not a lint owner; the authored audit fails closed
        # before the empty-graph check can read as a clean pass.
        self.assertEqual(status, 2)
        self.assertIn("authored C++ sources no Bazel lint owner declares", err)
        self.assertIn("  lib/Grouped.cpp", err)
        self.assertFalse(any(bazel_action_of(command) == "cquery" for command in self.commands))
        self.assertFalse(self.execute_calls)


class AnalyzerOrchestrationTest(unittest.TestCase):
    """The explicit clang-analyzer profile over the same native report machinery.

    Only the owned transport is faked (Bazel processes, workspace preparation);
    the analyzer gate runs for real over the inventories the fake passes return.
    """

    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="ao-bazel-analyzer-")
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.checkout = self.root / "checkout"
        self.tree = self.root / "tree"
        self.checkout.mkdir()
        self.tree.mkdir()
        for name in ("lib/sample/Sample.cpp", "app/macos-appkit/Foo.mm", "lib/sample/Unknown.cpp"):
            path = self.checkout / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text("// test input\n", encoding="utf-8")
        self.query_xml = QUERY_XML
        self.query_status = 0
        self.cquery_text = "@@//lib/sample:sample|False\n"
        self.cquery_status = 0
        self.inventories: tuple[bazel_lint_reports.OwnerInventory, ...] = ()
        self.commands: list[list[str]] = []
        self.prepare_calls: list[dict[str, object]] = []
        self.execute_calls: list[dict[str, object]] = []
        self.read_profiles: list[str] = []
        patches = [
            patch.object(bazel_lint, "PROJECT_ROOT", self.checkout),
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

    def fake_prepare(self, tree, config, *, prefix=None, lint_configuration=None):
        self.prepare_calls.append({"config": config, "lint_configuration": lint_configuration})
        prepared = tree / "configuration"
        prepared.mkdir(parents=True, exist_ok=True)
        rc = prepared / "aobus.bazelrc"
        if not rc.is_file():
            rc.write_text("build " + json.dumps("--config=" + config, ensure_ascii=False) + "\n", encoding="utf-8")
        return prepared

    def fake_execute(
        self, executable, tree, prepared, action, config, targets, *, jobs=2, remote_cache=None, isolate_rc=False
    ):
        self.execute_calls.append(
            {"action": action, "config": config, "targets": list(targets), "jobs": jobs, "isolate_rc": isolate_rc}
        )
        return 0

    def fake_read_reports(self, bep, owners, *, consumers="{}", profile="lint"):
        self.read_profiles.append(profile)
        return self.inventories

    def fake_process(self, command, stdout, stderr):
        self.commands.append(list(command))
        if bazel_action_of(command) == "query":
            stdout.write_text(self.query_xml, encoding="utf-8")
            return self.query_status
        if bazel_action_of(command) == "cquery":
            stdout.write_text(self.cquery_text, encoding="utf-8")
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
            "tidy_arg": [],
            "alpha": False,
            "fail_on_diagnostics": False,
            "include_external_diagnostics": False,
        }
        values.update(overrides)
        return argparse.Namespace(**values)

    def run_analyzer(self, args, files, *, explicit=False):
        with contextlib.redirect_stdout(io.StringIO()) as out, contextlib.redirect_stderr(io.StringIO()) as err:
            status = bazel_lint.run_analyzer(args, files, explicit=explicit)
        return status, out.getvalue(), err.getvalue()

    def report(self, main, *, outcome="clean", log=""):
        configuration = "bazel-out/k8-dbg/bin"
        identity = bazel_lint_reports.Identity(
            "@@//lib/sample:sample",
            configuration,
            main,
            main,
            (configuration + "/lib/sample/_objs/sample/Sample.o",),
            "ANALYZER",
            "linux",
        )
        log_path = self.root / f"analyzer-log-{id(main)}-{outcome}.log"
        log_path.write_text(log, encoding="utf-8")
        return bazel_lint_reports.Report(identity, outcome, 1 if outcome == "compiler_error" else 0, log_path, log_path)

    def owner_inventory(self, *reports):
        configuration = "bazel-out/k8-dbg/bin"
        sources = tuple(
            dict.fromkeys(path for report in reports for path in (report.identity.source, report.identity.main))
        )
        return bazel_lint_reports.OwnerInventory(
            "@@//lib/sample:sample", configuration, "config-id", tuple(reports), sources, (), self.root / "execroot"
        )

    def test_analyzer_pass_writes_its_configuration_and_builds_the_analyzer_aspect(self):
        self.inventories = (self.owner_inventory(self.report("lib/sample/Sample.cpp")),)
        status, out, _err = self.run_analyzer(
            self.args(alpha=True, tidy_arg=["--quiet"]), [str(self.checkout / "lib/sample/Sample.cpp")], explicit=True
        )
        self.assertEqual(status, 0)
        configuration_path = self.tree / "configuration" / "analyzer.json"
        self.assertEqual(
            json.loads(configuration_path.read_text(encoding="utf-8")),
            {
                "schema": 1,
                "source_root": str(self.checkout),
                "check": None,
                "header_filter": None,
                "clang_tidy_args": ["--quiet"],
                "alpha": True,
            },
        )
        self.assertEqual(self.prepare_calls[0]["lint_configuration"], configuration_path)
        # The analyzer compiles in its true fastbuild context; --debug stays
        # verbose evidence output and never selects a build mode.
        self.assertEqual(self.prepare_calls[0]["config"], "fastbuild")
        self.assertEqual(self.execute_calls[0]["config"], "fastbuild")
        targets = self.execute_calls[0]["targets"]
        self.assertIn("--aspects=//bazel/lint:lint.bzl%analyzer_aspect", targets)
        self.assertIn("--config=aobus_lint_consumers", targets)
        self.assertEqual(self.read_profiles, ["analyzer"])
        receipt = json.loads(next((self.tree / "analyzer").glob("run-*/run.receipt.json")).read_text(encoding="utf-8"))
        self.assertEqual(receipt["profile"], "analyzer")
        self.assertEqual(receipt["selected"], ["lib/sample/Sample.cpp"])
        self.assertIn("No analyzer diagnostics found.", out)

    def test_analyzer_findings_stay_report_only_until_the_gate_is_requested(self):
        log = "lib/sample/Sample.cpp:1:1: warning: leak [clang-analyzer-cplusplus.NewDeleteLeaks]\n"
        self.inventories = (self.owner_inventory(self.report("lib/sample/Sample.cpp", outcome="warning", log=log)),)
        status, out, _ = self.run_analyzer(self.args(), [str(self.checkout / "lib/sample/Sample.cpp")], explicit=True)
        self.assertEqual(status, 0)
        self.assertIn("Analyzer diagnostics: 1", out)
        self.assertIn("leak", out)
        status, out, _ = self.run_analyzer(
            self.args(fail_on_diagnostics=True), [str(self.checkout / "lib/sample/Sample.cpp")], explicit=True
        )
        self.assertEqual(status, 1)

    def test_analyzer_compiler_errors_stay_nonzero_even_report_only(self):
        log = "lib/sample/Sample.cpp:2:3: error: unknown type [clang-diagnostic-error]\n"
        self.inventories = (
            self.owner_inventory(self.report("lib/sample/Sample.cpp", outcome="compiler_error", log=log)),
        )
        for gate in (False, True):
            with self.subTest(fail_on_diagnostics=gate):
                status, _, err = self.run_analyzer(
                    self.args(fail_on_diagnostics=gate), [str(self.checkout / "lib/sample/Sample.cpp")], explicit=True
                )
                self.assertEqual(status, 1)
                self.assertIn("Analyzer tool failure output:", err)

    def test_explicit_foreign_objective_cpp_fails_without_a_report_build(self):
        self.cquery_text = "@@//app/macos-appkit:appkit|True\n"
        status, _, err = self.run_analyzer(self.args(), [str(self.checkout / "app/macos-appkit/Foo.mm")], explicit=True)
        self.assertEqual(status, 1)
        self.assertIn("Foo.mm", err)
        self.assertFalse(self.execute_calls)
        self.assertNotIn("No analyzer diagnostics found.", _)

    def test_batch_foreign_objective_cpp_is_deferred_while_native_cpp_is_analyzed(self):
        self.cquery_text = "@@//app/macos-appkit:appkit|True\n@@//lib/sample:sample|False\n"
        self.inventories = (self.owner_inventory(self.report("lib/sample/Sample.cpp")),)
        status, out, err = self.run_analyzer(
            self.args(),
            [str(self.checkout / "app/macos-appkit/Foo.mm"), str(self.checkout / "lib/sample/Sample.cpp")],
            explicit=False,
        )
        self.assertEqual(status, 0)
        self.assertIn("incompatible with this platform", err)
        self.assertIn("Foo.mm", err)
        self.assertEqual(len(self.execute_calls), 1)
        self.assertIn("No analyzer diagnostics found.", out)
        receipt = json.loads(next((self.tree / "analyzer").glob("run-*/run.receipt.json")).read_text(encoding="utf-8"))
        self.assertEqual(receipt["selected"], ["lib/sample/Sample.cpp"])
        self.assertEqual(receipt["deferred"], ["app/macos-appkit/Foo.mm"])

    def test_all_foreign_batch_scope_defers_without_claiming_a_clean_analysis(self):
        self.cquery_text = "@@//app/macos-appkit:appkit|True\n"
        status, out, err = self.run_analyzer(
            self.args(), [str(self.checkout / "app/macos-appkit/Foo.mm")], explicit=False
        )
        self.assertEqual(status, 0)
        self.assertIn("incompatible with this platform", err)
        self.assertNotIn("No analyzer diagnostics found.", out)
        self.assertFalse(self.execute_calls)

    def test_unowned_explicit_source_fails_without_a_report_build(self):
        status, _, err = self.run_analyzer(self.args(), [str(self.checkout / "lib/sample/Unknown.cpp")], explicit=True)
        self.assertEqual(status, 1)
        self.assertIn("Unknown.cpp", err)
        self.assertFalse(self.execute_calls)

    def test_explicit_covered_but_unreported_objective_cpp_requires_an_exact_context(self):
        # A compatible owner whose configured build captured no Objective-C++
        # compile action leaves the explicit .mm unreported; the analyzer never
        # borrows a context, so the historical exactness error stays a failure.
        self.cquery_text = "@@//app/macos-appkit:appkit|False\n"
        self.inventories = (self.owner_inventory(self.report("app/macos-appkit/Foo.h")),)
        status, out, err = self.run_analyzer(
            self.args(), [str(self.checkout / "app/macos-appkit/Foo.mm")], explicit=True
        )
        self.assertEqual(status, 1)
        self.assertIn("Objective-C++ analysis requires an exact native compile command:", err)
        self.assertIn("app/macos-appkit/Foo.mm", err)
        self.assertEqual(len(self.execute_calls), 1)
        self.assertNotIn("No analyzer diagnostics found.", out)

    def test_analyzer_rejects_nonpresentation_tidy_arguments_before_the_workspace(self):
        # The analyzer profile shares the lint action's diagnostic-argument
        # policy: only presentation-only raw arguments reach an analysis.
        with patch.object(bazel_lint.bazel_workspace, "workspace_path", side_effect=AssertionError("workspace")):
            for argument in ("--checks=aobus-*", "--warnings-as-errors=*", "--config-file=external.yaml"):
                with self.subTest(argument=argument):
                    status, _, err = self.run_analyzer(
                        self.args(tidy_arg=[argument]),
                        [str(self.checkout / "lib/sample/Sample.cpp")],
                        explicit=True,
                    )
                    self.assertEqual(status, 2)
                    self.assertIn("unsupported Bazel clang-tidy diagnostic argument", err)
        self.assertFalse(self.execute_calls)

    def test_analyzer_scope_validation_and_transport_errors_fail_closed(self):
        with tempfile.TemporaryDirectory() as outside:
            source = Path(outside) / "Outside.cpp"
            source.write_text("// outside\n", encoding="utf-8")
            status, _, err = self.run_analyzer(self.args(), [str(source)], explicit=True)
        self.assertEqual(status, 2)
        self.assertIn("outside the checkout", err)
        self.query_status = 1
        status, _, err = self.run_analyzer(self.args(), [str(self.checkout / "lib/sample/Sample.cpp")], explicit=True)
        self.assertEqual(status, 2)
        self.assertIn("Analyzer failed:", err)


if __name__ == "__main__":
    unittest.main()
