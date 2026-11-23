"""Source selection and false-green regressions for the folded native gate."""

import contextlib
import io
import json
import os
import subprocess
import tempfile
import unittest
from pathlib import Path
from unittest import mock

from ao.portal import bazel_validation as validation


def _document(*, platform="linux", declared=(), executed=(), built=(), wrappers=(), gtk=True, system_media=True):
    return {
        "platform": platform,
        "gtk_enabled": gtk,
        "system_media_enabled": system_media,
        "records": [{"declared": list(declared)}],
        "executed": list(executed),
        "built": list(built),
        "test_wrappers": list(wrappers),
    }


def _executable(label, kind, source, *, is_tool=False):
    return {"label": label, "kind": kind, "is_tool": is_tool, "sources": [source]}


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

    def facts(self, name, *, mode="fastbuild", **fields):
        document = {
            **_document(**fields),
            "schema": 2,
            "mode": mode,
            "profile": None,
            "builtin_include_roots": [],
            "records": [],
        }
        for role in ("executed", "built"):
            document[role] = [json.dumps(entry) for entry in document[role]]
        return self.write(name, json.dumps(document))

    def test_building_a_cc_test_does_not_execute_its_registration(self):
        test = self.registration("test/unit/EngineTest.cpp")
        bench = self.registration("test/unit/EngineBenchmark.cpp")
        orphan = self.registration("test/unit/OrphanTest.cpp")
        test_owner = _executable("//test:engine_test", "cc_test", test)
        bench_owner = _executable("//test:engine_bench", "cc_binary", bench)
        document = _document(built=[test_owner, bench_owner])
        errors, deferred, required = validation.registration_audit([document], self.root)
        self.assertEqual(len(errors), 2, errors)
        self.assertTrue(any("//test:engine_test" in error and "executed" in error for error in errors), errors)
        self.assertTrue(any(orphan in error for error in errors), errors)
        self.assertFalse(deferred or required)

        # A test can also supply build evidence for a binary it requires.
        (self.root / orphan).unlink()
        reached = _document(executed=[test_owner, bench_owner])
        self.assertEqual(validation.registration_audit([reached], self.root), ([], {}, ["//test:engine_test"]))
        errors, _, required = validation.registration_audit([document, reached], self.root)
        self.assertEqual(len(errors), 1, errors)
        self.assertIn("//test:engine_test", errors[0])
        self.assertFalse(required)

    def test_a_tool_configuration_test_does_not_own_a_target_registration(self):
        source = self.registration("test/unit/EngineTest.cpp")
        tool = _executable("//test:engine_test", "cc_test", source, is_tool=True)
        errors, deferred, required = validation.registration_audit([_document(executed=[tool])], self.root)
        self.assertEqual(len(errors), 1, errors)
        self.assertIn(source, errors[0])
        self.assertFalse(deferred or required)

    def test_conflicting_public_wrappers_fail_the_registration_audit(self):
        source = self.registration("test/unit/EngineTest.cpp")
        owner = _executable("//test:engine_impl", "cc_test", source)
        document = _document(
            executed=[owner],
            wrappers=[("//test:engine_impl", "//test:engine_one"), ("//test:engine_impl", "//test:engine_two")],
        )
        errors, deferred, required = validation.registration_audit([document], self.root)
        self.assertEqual(len(errors), 1, errors)
        self.assertIn(owner["label"], errors[0])
        self.assertFalse(deferred or required)

    def test_a_frontend_disabled_in_only_one_configuration_cannot_excuse_a_missing_registration(self):
        gtk = self.registration("test/linux-gtk/WindowTest.cpp")
        media = self.registration("test/systemmedia/linux/BackendTest.cpp")
        disabled = _document(gtk=False, system_media=False)
        errors, deferred, _ = validation.registration_audit([disabled], self.root)
        self.assertFalse(errors)
        self.assertEqual(set(deferred), {gtk, media})

        gtk_enabled = _document(system_media=False)
        errors, deferred, _ = validation.registration_audit([disabled, gtk_enabled], self.root)
        self.assertEqual(len(errors), 1, errors)
        self.assertIn(gtk, errors[0])
        self.assertEqual(set(deferred), {media})

    def test_linux_regression_requires_an_owner_even_without_frontends(self):
        source = self.registration("test/linux/library/DictionaryPublicationTest.cpp")
        for platform in ("linux", "macos", "windows"):
            with self.subTest(platform=platform):
                document = _document(platform=platform, gtk=False, system_media=False)
                errors, deferred, required = validation.registration_audit([document], self.root)
                self.assertFalse(required)
                orphans, skipped = validation.authored_cpp_audit([document], [source])
                if platform == "linux":
                    self.assertEqual(len(errors), 1, errors)
                    self.assertIn(source, errors[0])
                    self.assertEqual(orphans, [source])
                    self.assertFalse(deferred or skipped)
                else:
                    self.assertFalse(errors or orphans)
                    self.assertEqual(set(deferred), {source})
                    self.assertEqual(set(skipped), {source})

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

    def test_an_untracked_orphan_fails_the_gate_without_publishing_test_labels(self):
        # The fixture owns its repository even when the caller selected a different worktree.
        self.enterContext(mock.patch.dict(os.environ))
        for name in tuple(os.environ):
            if name.startswith(("GIT_", "AOBUS_GIT_")):
                del os.environ[name]
        os.environ.update(HOME=str(self.root), XDG_CONFIG_HOME=str(self.root), GIT_CONFIG_NOSYSTEM="1")
        subprocess.run(["git", "init", "--quiet"], cwd=self.root, check=True)
        orphan = "app/core/Orphan.cpp"
        self.write(orphan, "int orphan;\n")
        facts = self.facts("facts.json")
        outputs = self.root / "outputs"

        with (
            mock.patch.object(validation, "project_root", return_value=self.root),
            mock.patch.object(validation, "validate_native_policy", return_value=[]),
            contextlib.redirect_stderr(io.StringIO()) as diagnostics,
        ):
            self.assertEqual(validation.run([str(facts)], {"TEST_UNDECLARED_OUTPUTS_DIR": str(outputs)}), 1)
        self.assertIn(orphan, diagnostics.getvalue())
        self.assertFalse((outputs / "registered-test-labels.json").exists())

    def test_success_reports_only_the_invocations_public_registered_tests(self):
        direct = self.registration("test/unit/DirectTest.cpp")
        wrapped = self.registration("test/unit/WrappedTest.cpp")
        debug = self.registration("test/unit/DebugTest.cpp")
        bench = self.registration("test/unit/EngineBenchmark.cpp")
        unregistered = "test/unit/Probe.cpp"
        self.write(unregistered, "int main() {}\n")
        owner = _executable("@@//test:fixture_impl", "cc_test", wrapped)
        owner["sources"].append(self.registration("test/unit/AlsoWrappedTest.cpp"))
        facts = self.facts(
            "primary.json",
            executed=[
                _executable("//test:z_direct.native", "cc_test", direct),
                owner,
                _executable("//test:probe", "cc_test", unregistered),
            ],
            built=[_executable("//test:benchmark", "cc_binary", bench)],
            wrappers=[("@@//test:fixture_impl", "@@//test:a_wrapper")],
        )
        debug_facts = self.facts(
            "debug.json", mode="dbg", executed=[_executable("//test:debug_test", "cc_test", debug)]
        )
        outputs = self.root / "outputs"
        arguments = [str(facts), str(debug_facts)]
        with (
            mock.patch.object(validation, "project_root", return_value=self.root),
            mock.patch.object(validation, "validate_native_policy", return_value=[]),
            mock.patch.object(validation, "_authored_cpp_manifest", return_value=[]),
            contextlib.redirect_stdout(io.StringIO()),
        ):
            with contextlib.redirect_stderr(io.StringIO()) as diagnostics:
                self.assertEqual(validation.run(arguments, {}), 0)
            self.assertIn("TEST_UNDECLARED_OUTPUTS_DIR", diagnostics.getvalue())
            self.assertIn("manifest not written", diagnostics.getvalue())
            self.assertFalse(outputs.exists())
            with contextlib.redirect_stderr(io.StringIO()) as diagnostics:
                self.assertEqual(validation.run(arguments, {"TEST_UNDECLARED_OUTPUTS_DIR": str(outputs)}), 0)
            self.assertNotIn("manifest not written", diagnostics.getvalue())
        self.assertEqual(
            json.loads((outputs / "registered-test-labels.json").read_text(encoding="utf-8")),
            ["//test:a_wrapper", "//test:z_direct.native"],
        )


class FoldedActionSelectionTest(unittest.TestCase):
    def test_generated_c_is_exempt_but_explicit_cpp_compiled_from_dot_c_is_not(self):
        c_source = "bazel-out/k8-fastbuild/bin/gtk/resources.c"
        c_argv = ["clang++", "-c", c_source]
        captured = [c_argv, ["clang++", "-x", "c++", "-c", c_source], ["clang++", "-c", "lib/Engine.cpp"]]
        record = {
            "label": "//app:resources",
            "is_tool": False,
            "quote_includes": [],
            "winui_projections": [],
            "compiles": [{"argv": argv, "env": {}, "spa_inputs": []} for argv in captured],
        }
        self.assertEqual(
            [action.argv for action in validation._compile_actions([record])], list(map(tuple, captured[1:]))
        )


if __name__ == "__main__":
    unittest.main()
