"""Native diagnostic classification, path spelling and checker policy."""

import contextlib
import json
import os
import re
import tempfile
import unittest
from pathlib import Path
from unittest import mock

from ao.core.paths import PROJECT_ROOT, absolute_path
from ao.graph import bazel_lint_action
from ao.policy import tidy_config, tidy_policy
from ao.portal import source_analyzer, source_lint, source_scope, tidy_engine
from ao.portal.source_scope import SourceScope


class SourceClassificationTest(unittest.TestCase):
    def test_only_checkout_test_prefix_relaxes_policy(self):
        for relative, expected in (
            ("lib/audio/Foo.cpp", "STRICT"),
            ("app/macos-appkit/Application.mm", "STRICT"),
            ("tool/lint/Check.cpp", "STRICT"),
            ("test/audio/FooTest.cpp", "RELAXED"),
            ("app/x/test/Foo.cpp", "STRICT"),
            ("lib/test/lint/fixture/Policy.cpp", "STRICT"),
            ("app/test/lint/Helper.cpp", "STRICT"),
            ("te\u017ft/Policy.cpp", "STRICT"),
            ("TEST/Policy.cpp", "RELAXED" if os.name == "nt" else "STRICT"),
        ):
            with self.subTest(relative=relative):
                self.assertEqual(tidy_policy.classify_relative(relative, explicit=False), expected)

    def test_lint_fixtures_are_strict_only_when_individually_selected(self):
        fixture = "test/lint/fixture/aobus-check/case.cpp"
        self.assertEqual(tidy_policy.classify_relative(fixture, True), "STRICT")
        self.assertEqual(tidy_policy.classify_relative(fixture, False), "IGNORE")
        self.assertEqual(tidy_policy.classify_relative("test/lint/fixture_helper.cpp", True), "IGNORE")

    def test_native_spellings_deduplicate_before_lint_and_analyzer_selection(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = absolute_path(temporary)
            physical = root / "test/Policy.cpp"
            physical.parent.mkdir()
            physical.touch()
            alternate = physical.parent / "../test/Policy.cpp"
            with contextlib.ExitStack() as stack:
                for module in (source_lint, source_scope, source_analyzer, tidy_engine):
                    stack.enter_context(mock.patch.object(module, "PROJECT_ROOT", root))
                for files in ((str(alternate), str(physical)), (str(physical), str(alternate))):
                    self.assertEqual(source_lint.split_existing(files), (["test/Policy.cpp"], []))
                    self.assertEqual(
                        tidy_engine.classify_existing(list(files), True), {"STRICT": [], "RELAXED": [physical]}
                    )
                    self.assertEqual(
                        source_analyzer.analyzable_scope(SourceScope(files, files)),
                        SourceScope((str(physical),), (str(physical),)),
                    )

    def test_hardlink_outside_checkout_does_not_gain_fixture_exemptions(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary) / "checkout"
            physical = root / "test/lint/fixture/aobus-check/Policy.cpp"
            physical.parent.mkdir(parents=True)
            physical.touch()
            alias = root.parent / "outside.cpp"
            alias.hardlink_to(physical)
            with (
                mock.patch.object(tidy_engine, "PROJECT_ROOT", root),
                mock.patch.object(source_analyzer, "PROJECT_ROOT", root),
            ):
                self.assertEqual(tidy_engine.classify_existing([str(physical)], False), {"STRICT": [], "RELAXED": []})
                self.assertEqual(tidy_engine.classify_existing([str(alias)], False), {"STRICT": [alias], "RELAXED": []})
                self.assertFalse(source_analyzer.classify(physical))
                self.assertTrue(source_analyzer.classify(alias))
                self.assertTrue(source_analyzer.classify(root / "app/test/lint/fixture/Policy.cpp"))

    def test_foreign_python_file_keeps_its_absolute_spelling(self):
        with tempfile.NamedTemporaryFile(suffix=".py") as outside:
            self.assertEqual(source_lint.split_existing([outside.name]), ([], [absolute_path(outside.name).as_posix()]))


class TidyChecksTest(unittest.TestCase):
    def test_default_modes_and_selected_check_preserve_disables(self):
        self.assertEqual(tidy_config.checks_for("STRICT", None), tidy_config.STRICT_CHECKS)
        self.assertEqual(tidy_config.checks_for("RELAXED", None), tidy_config.RELAXED_CHECKS)
        check = "readability-magic-numbers"
        relaxed = tidy_config.checks_for("RELAXED", check).split(",")
        self.assertEqual(relaxed[:2], ["-*", check])
        self.assertEqual(relaxed.count(f"-{check}"), 1)
        strict = tidy_config.checks_for("STRICT", check).split(",")
        self.assertEqual(strict[:2], ["-*", check])
        self.assertNotIn(f"-{check}", strict)
        self.assertNotIn("-*", tidy_config.mode_disabled_checks("STRICT"))

    def test_stale_nolint_audit_uses_file_mode_and_reports_begin_only_once(self):
        for mode, comment, expected in (
            (
                "STRICT",
                "// NOLINTNEXTLINE(misc-multiple-inheritance,readability-qualified-auto)\n",
                "misc-multiple-inheritance",
            ),
            (
                "RELAXED",
                "// NOLINT(readability-function-cognitive-complexity)\n",
                "readability-function-cognitive-complexity",
            ),
            (
                "STRICT",
                "// NOLINTBEGIN(cppcoreguidelines-pro-bounds-constant-array-index)\n"
                "int value;\n"
                "// NOLINTEND(cppcoreguidelines-pro-bounds-constant-array-index)\n",
                "cppcoreguidelines-pro-bounds-constant-array-index",
            ),
        ):
            with self.subTest(mode=mode, check=expected), tempfile.TemporaryDirectory() as temporary:
                path = Path(temporary) / "Example.cpp"
                path.write_text(comment, encoding="utf-8")
                buckets = {"STRICT": [], "RELAXED": []}
                buckets[mode].append(path)
                issues = tidy_policy.find_stale_nolint_suppressions(buckets)
                self.assertEqual([(issue.line, issue.check) for issue in issues], [(1, expected)])


class TidySourceRootOptionsTest(unittest.TestCase):
    def roots_in(self, config):
        encoded = config.split("key: 'AobusSourceRoots', value: ", 1)[1]
        roots_json, _end = json.JSONDecoder().raw_decode(encoded)
        return json.loads(roots_json)

    def test_default_and_explicit_source_roots_preserve_lexical_spelling(self):
        self.assertEqual(self.roots_in(tidy_config.config_for("-*,aobus-*")), [tidy_config.PROJECT_ROOT.as_posix()])
        roots = (Path('/ci/a  b/"quoted";café'), Path(r"C:\source with spaces\checkout"), Path("S:/"))
        self.assertEqual(
            self.roots_in(tidy_config.config_for("-*,aobus-*", source_roots=roots)), [root.as_posix() for root in roots]
        )
        self.assertIs(tidy_config.source_roots_or_default(roots, project_root=Path("/other")), roots)
        self.assertEqual(
            tidy_config.source_roots_or_default(None, project_root=Path("/synthetic")), (Path("/synthetic"),)
        )

    def test_analyzer_filter_admits_checkout_spellings_without_foreign_prefixes(self):
        for platform in ("linux", "windows"):
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
                self.assertIsNotNone(re.search(expression, spelling))
            self.assertEqual(re.search(expression, absolute.replace("/", "\\")) is not None, platform == "windows")
            self.assertIsNone(
                re.search(expression, absolute_path(PROJECT_ROOT).as_posix() + "-foreign/lib/audio/Foo.h")
            )

    def test_source_root_encoding_does_not_depend_on_mapping_suffix(self):
        base = tidy_config.CONFIG_BASE.rstrip()[:-1] + ", FormatStyle: none}"
        with mock.patch.object(tidy_config, "CONFIG_BASE", base):
            config = tidy_config.config_for("-*,aobus-*")
        self.assertEqual(self.roots_in(config), [tidy_config.PROJECT_ROOT.as_posix()])
        self.assertTrue(config.endswith(", FormatStyle: none}"))


if __name__ == "__main__":
    unittest.main()
