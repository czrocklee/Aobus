"""Source selection and false-green regressions for the folded native gate."""

import contextlib
import io
import json
import tempfile
import unittest
from pathlib import Path
from unittest import mock

from ao.portal import bazel_validation as validation


def _document(*, platform="linux", declared=(), executed=(), built=(), gtk=True, system_media=True):
    return {
        "platform": platform,
        "gtk_enabled": gtk,
        "system_media_enabled": system_media,
        "records": [{"declared": list(declared)}],
        "executed": list(executed),
        "built": list(built),
    }


def _executable(label, kind, source):
    return {"label": label, "kind": kind, "is_tool": False, "sources": [source]}


class NativeSourceAuditTest(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix="ao-native-source-audit-")
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)

    def write(self, relative, source):
        path = self.root / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(source, encoding="utf-8")
        return path

    def registration(self, path):
        self.write(path, 'TEST_CASE("Example - exercises the owner") {}\n')
        return path

    def test_building_a_cc_test_does_not_execute_its_registration(self):
        test = self.registration("test/unit/EngineTest.cpp")
        bench = self.registration("test/unit/EngineBenchmark.cpp")
        orphan = self.registration("test/unit/OrphanTest.cpp")
        test_owner = _executable("//test:engine_test", "cc_test", test)
        bench_owner = _executable("//test:engine_bench", "cc_binary", bench)
        document = _document(built=[test_owner, bench_owner])
        errors, deferred = validation.registration_audit([document], self.root)
        self.assertEqual(len(errors), 2, errors)
        self.assertTrue(any("//test:engine_test" in error and "executed" in error for error in errors), errors)
        self.assertTrue(any(orphan in error for error in errors), errors)
        self.assertFalse(deferred)

        # A test can also supply build evidence for a binary it requires.
        (self.root / orphan).unlink()
        document = _document(executed=[test_owner, bench_owner])
        self.assertEqual(validation.registration_audit([document], self.root), ([], {}))

    def test_a_frontend_disabled_in_only_one_configuration_cannot_excuse_a_missing_registration(self):
        gtk = self.registration("test/linux-gtk/WindowTest.cpp")
        media = self.registration("test/systemmedia/linux/BackendTest.cpp")
        disabled = _document(gtk=False, system_media=False)
        errors, deferred = validation.registration_audit([disabled], self.root)
        self.assertFalse(errors)
        self.assertEqual(set(deferred), {gtk, media})

        gtk_enabled = _document(system_media=False)
        errors, deferred = validation.registration_audit([disabled, gtk_enabled], self.root)
        self.assertEqual(len(errors), 1, errors)
        self.assertIn(gtk, errors[0])
        self.assertEqual(set(deferred), {media})

    def test_orphans_follow_the_configured_universe_not_the_running_host(self):
        winui = "app/windows-winui/App.cpp"
        gtk = "app/linux-gtk/Shell.cpp"
        declared = "app/core/Engine.cpp"
        orphan = "app/core/Orphan.cpp"
        paths = [winui, gtk, declared, orphan]
        linux = _document(gtk=False, declared=[declared])
        windows = _document(platform="windows", declared=[declared])
        for document, orphans, deferred in ((linux, [orphan], {gtk, winui}), (windows, [winui, orphan], {gtk})):
            with self.subTest(platform=document["platform"]):
                errors, skipped = validation.authored_cpp_audit([document], paths)
                self.assertCountEqual(errors, orphans)
                self.assertEqual(set(skipped), deferred)

    def test_inventory_keeps_authored_paths_with_spaces_but_not_tooling_or_deleted_files(self):
        authored = "app/core/With spaces.cpp"
        fixture = "test/lint/fixture/Fixture.cpp"
        self.write(authored, "int value;\n")
        self.write(fixture, "int fixture;\n")
        manifest = f"{authored}\0{fixture}\0app/core/Deleted.cpp\0".encode()
        self.assertEqual(validation._authored_cpp_inventory(manifest, self.root), [authored])

    def test_only_configured_authored_sources_are_checked_for_stale_suppressions(self):
        source = "app/core/Engine.cpp"
        deferred = "app/linux-gtk/Shell.cpp"
        check = "bugprone-easily-swappable-parameters"
        self.write(deferred, f"int f(); // NOLINT({check})\n")
        self.write(source, f"\nint f(); // NOLINT({check})\nint g(); // NOLINT(aobus-license-header)\n")
        document = _document(declared=[source], gtk=False)
        self.assertEqual(
            validation.stale_suppression_audit([document], self.root, [source, deferred]),
            [f"{source}:2: {check}"],
        )

    def test_an_orphan_in_the_checkout_fails_the_gate_even_with_passing_action_policy(self):
        orphan = "app/core/Orphan.cpp"
        self.write(orphan, "int orphan;\n")
        document = {
            **_document(),
            "schema": 1,
            "mode": "dbg",
            "profile": None,
            "builtin_include_roots": [],
            "records": [],
        }
        facts = self.write("facts.json", json.dumps(document))

        def list_untracked(command, *, cwd, **options):
            self.assertTrue(Path(command[0]).stem.startswith("git"), command)
            self.assertIn("ls-files", command)
            self.assertIn("-z", command)
            self.assertEqual(cwd, self.root)
            return mock.Mock(stdout=f"{orphan}\0".encode() if "--others" in command else b"")

        with (
            mock.patch.object(validation, "project_root", return_value=self.root),
            mock.patch.object(validation, "validate_native_policy", return_value=[]),
            mock.patch.object(validation.subprocess, "run", side_effect=list_untracked),
            contextlib.redirect_stderr(io.StringIO()) as diagnostics,
        ):
            self.assertEqual(validation.run([str(facts)], {}), 1)
        self.assertIn(orphan, diagnostics.getvalue())


class FoldedActionSelectionTest(unittest.TestCase):
    def test_generated_c_is_exempt_but_explicit_cpp_compiled_from_dot_c_is_not(self):
        c_source = "bazel-out/k8-fastbuild/bin/gtk/resources.c"
        c_argv = ["clang++", "-c", c_source]
        captured = [c_argv, ["clang++", "-x", "c++", "-c", c_source], ["clang++", "-c", "lib/Engine.cpp"]]
        record = {
            "label": "//app:resources",
            "is_tool": False,
            "quote_includes": [],
            "compiles": [{"argv": argv, "env": {}, "spa_inputs": []} for argv in captured],
        }
        self.assertEqual(
            [action.argv for action in validation._compile_actions([record])], list(map(tuple, captured[1:]))
        )


if __name__ == "__main__":
    unittest.main()
