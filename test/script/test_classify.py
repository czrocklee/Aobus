"""Tests for tidy/analyze file classification and check policy."""

import argparse
import contextlib
import json
import os
import re
import tempfile
import unittest
from pathlib import Path
from unittest import mock

from ao.command import analyze, tidy
from ao.core import bazel_lint_action, tidyconfig
from ao.core.paths import PROJECT_ROOT, absolute_path


class TidyClassifyTest(unittest.TestCase):
    def setUp(self):
        patch = mock.patch.object(tidy, "PROJECT_ROOT", Path("/repo"))
        patch.start()
        self.addCleanup(patch.stop)

    def test_nested_test_and_fixture_components_do_not_relax_or_skip_production(self):
        for relative in (
            "app/x/test/Foo.cpp",
            "lib/test/lint/fixture/Policy.cpp",
            "app/test/lint/Helper.cpp",
            "te\u017ft/Policy.cpp",  # Unicode case folding must not invent the ASCII test prefix.
        ):
            for explicit in (False, True):
                with self.subTest(relative=relative, explicit=explicit):
                    self.assertEqual(tidy.classify(Path("/repo") / relative, explicit), "STRICT")

    def test_unknown_root_test_paths_receive_no_exemption(self):
        self.assertEqual(tidy.classify(Path("/other/test/Policy.cpp"), explicit=False), "STRICT")
        self.assertEqual(tidy.classify(Path("/repo-other/test/lint/fixture/Policy.cpp"), explicit=False), "STRICT")

    def test_case_varied_test_component_matches_native_plugin_policy(self):
        self.assertEqual(
            tidy.classify(Path("/repo/TEST/Policy.cpp"), explicit=False),
            "RELAXED" if os.name == "nt" else "STRICT",
        )

    def test_production_code_is_strict(self):
        self.assertEqual(tidy.classify(Path("/repo/lib/audio/Foo.cpp"), explicit=False), "STRICT")
        self.assertEqual(tidy.classify(Path("/repo/app/linux-gtk/Window.cpp"), explicit=False), "STRICT")
        self.assertEqual(tidy.classify(Path("/repo/app/macos-appkit/DesktopApplication.mm"), explicit=False), "STRICT")
        self.assertEqual(tidy.classify(Path("/repo/include/aobus/Foo.h"), explicit=False), "STRICT")

    def test_lint_checker_sources_are_strict_despite_tool_prefix(self):
        self.assertEqual(tidy.classify(Path("/repo/tool/lint/Check.cpp"), explicit=False), "STRICT")

    def test_test_code_is_relaxed(self):
        self.assertEqual(tidy.classify(Path("/repo/test/audio/FooTest.cpp"), explicit=False), "RELAXED")

    def test_lint_fixtures_only_checked_when_explicit(self):
        fixture = Path("/repo/test/lint/fixture/aobus-check/case.cpp")
        self.assertEqual(tidy.classify(fixture, explicit=True), "STRICT")
        self.assertEqual(tidy.classify(fixture, explicit=False), "IGNORE")

    def test_other_lint_integration_files_are_ignored(self):
        helper = Path("/repo/test/lint/fixture_helper.cpp")
        self.assertEqual(tidy.classify(helper, explicit=True), "IGNORE")


class NativeSourceSelectionTest(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.source = absolute_path(temporary.name) / "checkout"
        self.source.mkdir()
        stack = contextlib.ExitStack()
        self.addCleanup(stack.close)
        stack.enter_context(mock.patch.object(tidy, "PROJECT_ROOT", self.source))
        stack.enter_context(mock.patch.object(analyze, "PROJECT_ROOT", self.source))

    def test_native_spellings_deduplicate_before_tidy_and_analyzer_selection(self):
        physical = self.source / "test/Policy.cpp"
        physical.parent.mkdir()
        physical.write_text("int value;\n", encoding="utf-8")
        alternate = physical.parent / "../test/Policy.cpp"
        for files in ([str(alternate), str(physical)], [str(physical), str(alternate)]):
            self.assertEqual(tidy.split_existing(files), (["test/Policy.cpp"], []))
            self.assertEqual(tidy.classify_existing(files, True), {"STRICT": [], "RELAXED": [physical]})
            args = argparse.Namespace(files=[], all=False, folder=[], commit=None)
            with mock.patch.object(analyze.tidyengine, "resolve_scope", return_value=(files, True)):
                self.assertEqual(analyze.analyzable_sources(args), ([str(physical)], True))

    def test_hardlink_outside_checkout_does_not_gain_test_or_fixture_exemptions(self):
        physical = self.source / "test/lint/fixture/aobus-check/Policy.cpp"
        physical.parent.mkdir(parents=True)
        physical.write_text("int value;\n", encoding="utf-8")
        alias = self.source.parent / "outside-test.cpp"
        alias.hardlink_to(physical)
        self.assertTrue(alias.samefile(physical))
        self.assertEqual(tidy.classify(physical, explicit=False), "IGNORE")
        self.assertFalse(analyze.classify(physical))
        self.assertEqual(tidy.classify(alias, explicit=False), "STRICT")
        self.assertTrue(analyze.classify(alias))


class TidyChecksTest(unittest.TestCase):
    def test_no_selection_uses_full_mode_checks(self):
        self.assertEqual(tidyconfig.checks_for("STRICT", None), tidyconfig.STRICT_CHECKS)
        self.assertEqual(tidyconfig.checks_for("RELAXED", None), tidyconfig.RELAXED_CHECKS)

    def test_selected_check_keeps_mode_disables(self):
        # A check disabled only for tests must stay off in RELAXED even when selected.
        relaxed = tidyconfig.checks_for("RELAXED", "readability-magic-numbers")
        self.assertEqual(relaxed.split(",")[:2], ["-*", "readability-magic-numbers"])
        self.assertEqual(relaxed.split(",").count("-readability-magic-numbers"), 1)
        self.assertTrue(relaxed.endswith("-readability-magic-numbers") or ",-readability-magic-numbers," in relaxed)

    def test_selected_check_active_in_strict(self):
        # STRICT does not disable magic-numbers, so it stays enabled.
        strict = tidyconfig.checks_for("STRICT", "readability-magic-numbers")
        self.assertIn("readability-magic-numbers", strict)
        self.assertNotIn("-readability-magic-numbers", strict)

    def test_mode_disabled_checks_excludes_star(self):
        disables = tidyconfig.mode_disabled_checks("STRICT")
        self.assertNotIn("-*", disables)
        self.assertTrue(all(token.startswith("-") for token in disables))


class TidySourceRootOptionsTest(unittest.TestCase):
    def roots_in(self, config):
        encoded = config.split("key: 'AobusSourceRoots', value: ", 1)[1]
        roots_json, _end = json.JSONDecoder().raw_decode(encoded)
        return json.loads(roots_json)

    def test_default_root_is_the_portal_checkout(self):
        self.assertEqual(self.roots_in(tidyconfig.config_for("-*,aobus-*")), [tidyconfig.PROJECT_ROOT.as_posix()])

    def test_validated_alias_roots_are_preserved_without_filesystem_resolution(self):
        roots = (Path("/ci/test/checkout"), Path("/build/source"), Path("S:/"))
        self.assertEqual(
            self.roots_in(tidyconfig.config_for("-*,aobus-*", source_roots=roots)),
            [root.as_posix() for root in roots],
        )

    def test_root_encoding_preserves_spaces_quotes_semicolons_and_non_ascii(self):
        roots = (Path('/ci/a  b/"quoted";café'), Path(r"C:\source with spaces\checkout"))
        self.assertEqual(
            self.roots_in(tidyconfig.config_for("-*,aobus-*", source_roots=roots)),
            [root.as_posix() for root in roots],
        )

    def test_shared_default_root_selection_preserves_callers_and_aliases(self):
        root = Path("/synthetic/checkout")
        aliases = (Path("S:/"), Path("/owned/source"))
        self.assertEqual(tidyconfig.source_roots_or_default(None, project_root=root), (root,))
        self.assertIs(tidyconfig.source_roots_or_default(aliases, project_root=root), aliases)
        # The analyzer profile derives its diagnostic filter from the action's
        # default runtime roots, so the checkout spelling keeps qualifying.
        for platform in ("linux", "windows"):
            with self.subTest(platform=platform):
                identity = bazel_lint_action.Identity(
                    "@@//lib/audio:audio",
                    "bazel-out/k8-dbg/bin",
                    "lib/audio/Foo.cpp",
                    "lib/audio/Foo.cpp",
                    ("bazel-out/k8-dbg/bin/lib/audio/_objs/audio/Foo.o",),
                    "ANALYZER",
                    platform,
                )
                expression = bazel_lint_action._header_filter(
                    identity, {"header_filter": None}, bazel_lint_action._runtime_roots(), {}
                )
                absolute = (absolute_path(PROJECT_ROOT) / "lib/audio/Foo.h").as_posix()
                for spelling in ("lib/audio/Foo.h", absolute):
                    self.assertIsNotNone(re.search(expression, spelling), spelling)
                native = absolute.replace("/", "\\")
                self.assertEqual(re.search(expression, native) is not None, platform == "windows")
                self.assertIsNone(
                    re.search(expression, absolute_path(PROJECT_ROOT).as_posix() + "-foreign/lib/audio/Foo.h")
                )

    def test_source_root_option_does_not_depend_on_the_mapping_suffix(self):
        base = tidyconfig.CONFIG_BASE.rstrip()[:-1] + ", FormatStyle: none}"
        with mock.patch.object(tidyconfig, "CONFIG_BASE", base):
            config = tidyconfig.config_for("-*,aobus-*")
        self.assertEqual(self.roots_in(config), [tidyconfig.PROJECT_ROOT.as_posix()])
        self.assertTrue(config.endswith(", FormatStyle: none}"))
        self.assertNotIn("SOURCE_ROOTS_JSON", config)


class StaleNolintAuditTest(unittest.TestCase):
    def test_reports_only_checks_disabled_for_the_file_mode(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            source = Path(temp_dir) / "Example.cpp"
            source.write_text(
                "// NOLINTNEXTLINE(misc-multiple-inheritance,readability-qualified-auto)\nint value;\n",
                encoding="utf-8",
            )

            issues = tidy.find_stale_nolint_suppressions({"STRICT": [source], "RELAXED": []})

        self.assertEqual([(issue.line, issue.check) for issue in issues], [(1, "misc-multiple-inheritance")])

    def test_applies_relaxed_only_disables_to_test_sources(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            source = Path(temp_dir) / "ExampleTest.cpp"
            source.write_text(
                "// NOLINT(readability-function-cognitive-complexity)\nint value;\n",
                encoding="utf-8",
            )

            issues = tidy.find_stale_nolint_suppressions({"STRICT": [], "RELAXED": [source]})

        self.assertEqual(
            [(issue.line, issue.check) for issue in issues],
            [(1, "readability-function-cognitive-complexity")],
        )

    def test_reports_nolintbegin_once_without_repeating_nolintend(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            source = Path(temp_dir) / "Example.cpp"
            source.write_text(
                "// NOLINTBEGIN(cppcoreguidelines-pro-bounds-constant-array-index)\n"
                "int value;\n"
                "// NOLINTEND(cppcoreguidelines-pro-bounds-constant-array-index)\n",
                encoding="utf-8",
            )

            issues = tidy.find_stale_nolint_suppressions({"STRICT": [source], "RELAXED": []})

        self.assertEqual(
            [(issue.line, issue.check) for issue in issues], [(1, "cppcoreguidelines-pro-bounds-constant-array-index")]
        )


class TidySplitExistingTest(unittest.TestCase):
    def test_files_outside_the_repository_keep_their_absolute_path(self):
        with tempfile.NamedTemporaryFile(suffix=".py") as outside:
            cpp_files, python_files = tidy.split_existing([outside.name])
            self.assertEqual(cpp_files, [])
            self.assertEqual(python_files, [absolute_path(outside.name).as_posix()])


class AnalyzeClassifyTest(unittest.TestCase):
    def setUp(self):
        patch = mock.patch.object(analyze, "PROJECT_ROOT", Path("/repo"))
        patch.start()
        self.addCleanup(patch.stop)

    def test_nested_or_unknown_root_lint_components_are_not_skipped(self):
        self.assertTrue(analyze.classify(Path("/repo/app/test/lint/fixture/Policy.cpp")))
        self.assertTrue(analyze.classify(Path("/other/test/lint/fixture/Policy.cpp")))

    def test_case_varied_lint_path_matches_native_filesystem_policy(self):
        self.assertEqual(analyze.classify(Path("/repo/TEST/LINT/fixture/Policy.cpp")), os.name != "nt")

    def test_project_sources_are_analyzed(self):
        self.assertTrue(analyze.classify(Path("/repo/lib/audio/Foo.cpp")))
        self.assertTrue(analyze.classify(Path("/repo/test/audio/FooTest.cpp")))

    def test_lint_integration_is_skipped(self):
        self.assertFalse(analyze.classify(Path("/repo/test/lint/fixture/x/case.cpp")))


if __name__ == "__main__":
    unittest.main()
