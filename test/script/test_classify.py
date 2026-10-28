"""Tests for tidy/analyze file classification and check policy."""

import argparse
import contextlib
import io
import json
import os
import tempfile
import unittest
from pathlib import Path
from unittest import mock

from ao.command import analyze, tidy
from ao.core import compiler_cache, tidyconfig, tidyengine, workspace_cache
from ao.core.paths import absolute_path


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


class CompilerViewSourceSelectionTest(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        root = Path(temporary.name)
        self.source = root / "checkout"
        self.view = root / "owned-source-view"
        original = workspace_cache.canonical_portal_path
        environment = {
            compiler_cache.SHARED_WORKSPACES_EFFECTIVE: "1",
            workspace_cache.WINDOWS_SOURCE_VIEW: str(self.view),
        }

        def canonical(path, *, project_root):
            return original(path, project_root=project_root, system="Windows", environ=environment)

        stack = contextlib.ExitStack()
        self.addCleanup(stack.close)
        stack.enter_context(mock.patch.object(tidy, "PROJECT_ROOT", self.source))
        stack.enter_context(mock.patch.object(analyze, "PROJECT_ROOT", self.source))
        stack.enter_context(mock.patch.object(workspace_cache, "canonical_portal_path", side_effect=canonical))

    def source_pair(self, relative):
        physical = self.source / relative
        alias = self.view / relative
        physical.parent.mkdir(parents=True, exist_ok=True)
        alias.parent.mkdir(parents=True, exist_ok=True)
        physical.write_text("int value;\n", encoding="utf-8")
        alias.hardlink_to(physical)
        self.assertTrue(alias.samefile(physical))
        return physical, alias

    def test_owned_alias_is_relaxed_and_deduplicated_before_tidy_selection(self):
        physical, alias = self.source_pair("test/Policy.cpp")
        for files in ([str(alias), str(physical)], [str(physical), str(alias)]):
            self.assertEqual(tidy.split_existing(files), (["test/Policy.cpp"], []))
            self.assertEqual(tidy.classify_existing(files, True), {"STRICT": [], "RELAXED": [physical]})
        self.assertEqual(tidy.classify(alias, explicit=True), "RELAXED")

    def test_owned_fixture_alias_preserves_explicit_and_batch_policies(self):
        physical, alias = self.source_pair("test/lint/fixture/aobus-check/Policy.cpp")
        for path in (physical, alias):
            self.assertEqual(tidy.classify(path, explicit=True), "STRICT")
            self.assertEqual(tidy.classify(path, explicit=False), "IGNORE")
            self.assertFalse(analyze.classify(path))

    def test_analyzer_selects_a_single_physical_test_consumer(self):
        physical, alias = self.source_pair("test/Policy.cpp")
        args = argparse.Namespace(
            path=str(self.source / "database"),
            jobs=2,
            output=None,
            tidy_arg=[],
            debug=False,
            alpha=False,
            check=None,
            include_external_diagnostics=False,
            fail_on_diagnostics=False,
        )
        with (
            mock.patch.object(analyze.tidyengine, "ensure_compile_db"),
            mock.patch.object(analyze.workspace_cache, "validated_source_roots", return_value=(self.source, self.view)),
            mock.patch.object(analyze.tidyengine, "system_include_args", return_value=[]),
            mock.patch.object(analyze.tidyengine, "resolve_scope", return_value=([str(alias), str(physical)], True)),
            mock.patch.object(analyze.tidyengine, "run_parallel", return_value=tidyengine.BatchResult()) as run,
            contextlib.redirect_stdout(io.StringIO()),
        ):
            self.assertEqual(analyze.run_command(args), 0)
        self.assertEqual(run.call_args.args[0], [str(physical)])

    def test_unowned_outside_test_paths_remain_production(self):
        path = self.view.parent / "unowned/test/Policy.cpp"
        self.assertEqual(tidy.classify(path, explicit=False), "STRICT")
        self.assertTrue(analyze.classify(path))


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

    def test_cocoa_checks_supplement_cpp_rules_in_production(self):
        checks = tidyconfig.checks_for("STRICT", None).split(",")
        self.assertIn("objc-super-self", checks)
        self.assertIn("objc-property-declaration", checks)
        self.assertIn("cppcoreguidelines-*", checks)
        self.assertIn("clang-analyzer-osx.*", analyze.analyzer_checks(alpha=False, only=None).split(","))

    def test_mode_disabled_checks_excludes_star(self):
        disables = tidyconfig.mode_disabled_checks("STRICT")
        self.assertNotIn("-*", disables)
        self.assertTrue(all(token.startswith("-") for token in disables))

    def test_clang_22_policy_exclusions_are_mode_specific(self):
        self.assertIn("-misc-multiple-inheritance", tidyconfig.STRICT_CHECKS.split(","))
        self.assertIn("-bugprone-derived-method-shadowing-base-method", tidyconfig.STRICT_CHECKS.split(","))
        self.assertIn("-bugprone-exception-escape", tidyconfig.STRICT_CHECKS.split(","))
        self.assertIn("-bugprone-throwing-static-initialization", tidyconfig.STRICT_CHECKS.split(","))
        self.assertNotIn("-modernize-use-designated-initializers", tidyconfig.STRICT_CHECKS.split(","))
        self.assertIn("-modernize-use-designated-initializers", tidyconfig.RELAXED_CHECKS.split(","))
        self.assertIn("-bugprone-exception-escape", tidyconfig.RELAXED_CHECKS.split(","))
        self.assertIn("-bugprone-throwing-static-initialization", tidyconfig.RELAXED_CHECKS.split(","))

    def test_cognitive_complexity_ignores_macro_expansions(self):
        self.assertIn(
            "{key: 'readability-function-cognitive-complexity.IgnoreMacros', value: true}",
            tidyconfig.CONFIG_BASE,
        )

    def test_boolean_simplification_ignores_macro_expansions(self):
        self.assertIn(
            "{key: 'readability-simplify-boolean-expr.IgnoreMacros', value: true}",
            tidyconfig.CONFIG_BASE,
        )

    def test_contract_macros_are_narrowly_allowlisted(self):
        self.assertIn(
            "^DEBUG_*|^[A-Z_]+_LOG_[A-Z_]+$|^AO_(EXPECTS|ENSURES|INVARIANT|FATAL|RT_INVARIANT)$",
            tidyconfig.CONFIG_BASE,
        )

    def test_constant_and_concept_naming_matches_project_policy(self):
        for category in ("ConstexprVariable", "ClassConstant", "GlobalConstant", "StaticConstant"):
            self.assertIn(
                f"{{key: 'readability-identifier-naming.{category}Case', value: 'CamelCase'}}",
                tidyconfig.CONFIG_BASE,
            )
            self.assertIn(
                f"{{key: 'readability-identifier-naming.{category}Prefix', value: 'k'}}",
                tidyconfig.CONFIG_BASE,
            )
            self.assertNotIn(f"readability-identifier-naming.{category}IgnoredRegexp", tidyconfig.CONFIG_BASE)
        self.assertIn(
            "{key: 'readability-identifier-naming.ConceptCase', value: 'CamelCase'}",
            tidyconfig.CONFIG_BASE,
        )
        self.assertNotIn("readability-identifier-naming.LocalConstant", tidyconfig.CONFIG_BASE)

    def test_winrt_framework_shapes_are_narrowly_allowlisted(self):
        self.assertIn(
            "|GetAt|Size|IndexOf|GetMany|First|Current|HasCurrent|MoveNext|GetXamlType|GetXmlnsDefinitions)",
            tidyconfig.CONFIG_BASE,
        )
        self.assertIn(
            r"{key: 'bugprone-suspicious-include.IgnoredRegex', value: '.*\.g\.cpp$'}",
            tidyconfig.CONFIG_BASE,
        )
        self.assertIn(r".*[/\\]winrt[/\\].*", tidyconfig.CONFIG_BASE)

    def test_specialization_provider_header_is_narrowly_ignored_by_include_cleaner(self):
        self.assertIn(r".*ao[/\\]utility[/\\]StrongTypeFormatter\.h$", tidyconfig.CONFIG_BASE)

    def test_boost_process_definition_header_is_narrowly_ignored_by_include_cleaner(self):
        self.assertIn(r".*boost[/\\]process[/\\]v2[/\\]process\.hpp$", tidyconfig.CONFIG_BASE)

    def test_darwin_posix_provider_headers_are_narrowly_ignored_by_include_cleaner(self):
        self.assertIn(r".*sys[/\\]_types[/\\]_pid_t\.h$", tidyconfig.CONFIG_BASE)
        self.assertIn(r".*sys[/\\]_types[/\\]_sigset_t\.h$", tidyconfig.CONFIG_BASE)
        self.assertIn(r".*sys[/\\]fcntl\.h$", tidyconfig.CONFIG_BASE)
        self.assertIn(r".*sys[/\\]signal\.h$", tidyconfig.CONFIG_BASE)
        self.assertNotIn(r".*sys[/\\]_types[/\\].*", tidyconfig.CONFIG_BASE)


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

    def test_empty_roots_fail_consistently_in_config_and_diagnostic_filters(self):
        calls = (
            lambda: tidyconfig.config_for("-*,aobus-*", source_roots=()),
            lambda: tidy.project_header_filter(("test",), source_roots=()),
            lambda: tidy.exact_header_filter([Path("test/Example.h")], source_roots=()),
            lambda: tidy.path_line_filter([Path("test/Example.h")], source_roots=()),
            lambda: analyze.project_header_filter(source_roots=()),
        )
        for call in calls:
            with self.subTest(call=call), self.assertRaisesRegex(ValueError, "source roots must not be empty"):
                call()

    def test_shared_default_root_selection_preserves_callers_and_aliases(self):
        root = Path("/synthetic/checkout")
        aliases = (Path("S:/"), Path("/owned/source"))
        self.assertEqual(tidyconfig.source_roots_or_default(None, project_root=root), (root,))
        self.assertIs(tidyconfig.source_roots_or_default(aliases, project_root=root), aliases)
        with mock.patch.object(analyze, "PROJECT_ROOT", root):
            self.assertEqual(
                analyze.project_header_filter(), analyze.project_header_filter(source_roots=(absolute_path(root),))
            )

    def test_source_root_option_does_not_depend_on_the_mapping_suffix(self):
        base = tidyconfig.CONFIG_BASE.rstrip()[:-1] + ", FormatStyle: none}"
        with mock.patch.object(tidyconfig, "CONFIG_BASE", base):
            config = tidyconfig.config_for("-*,aobus-*")
        self.assertEqual(self.roots_in(config), [tidyconfig.PROJECT_ROOT.as_posix()])
        self.assertTrue(config.endswith(", FormatStyle: none}"))
        self.assertNotIn("SOURCE_ROOTS_JSON", config)

    def test_missing_or_duplicate_root_placeholder_is_a_readable_error(self):
        for base in ("{}", tidyconfig.CONFIG_BASE + " SOURCE_ROOTS_JSON"):
            with self.subTest(base=base), mock.patch.object(tidyconfig, "CONFIG_BASE", base):
                with self.assertRaisesRegex(ValueError, "exactly one source-root option placeholder"):
                    tidyconfig.config_for("-*,aobus-*")


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


class AnalyzerChecksTest(unittest.TestCase):
    def test_single_check_overrides_groups(self):
        self.assertEqual(
            analyze.analyzer_checks(alpha=True, only="clang-analyzer-core.NullDereference"),
            "-*,clang-analyzer-core.NullDereference",
        )

    def test_alpha_appends_experimental_group(self):
        checks = analyze.analyzer_checks(alpha=True, only=None)
        self.assertIn("clang-analyzer-alpha.*", checks)
        self.assertNotIn("clang-analyzer-alpha.*", analyze.analyzer_checks(alpha=False, only=None))


if __name__ == "__main__":
    unittest.main()
