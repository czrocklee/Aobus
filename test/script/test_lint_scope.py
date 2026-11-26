"""Production lint scope against real Git checkouts."""

import contextlib
import io
import os
import unittest
from unittest import mock

from git_support import GitCheckoutTestCase

from ao.portal import git_files, lint_scope


class LintScopeTest(GitCheckoutTestCase):
    def setUp(self):
        super().setUp()
        for module in (git_files, lint_scope):
            self.enterContext(mock.patch.object(module, "PROJECT_ROOT", self.root))

    def write_file(self, name: str, content: str) -> None:
        path = self.root / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(content, encoding="utf-8", newline="\n")

    def advance_mtime(self, name: str, seconds: int = 5) -> None:
        path = self.root / name
        st = path.stat()
        os.utime(path, ns=(st.st_atime_ns, st.st_mtime_ns + seconds * 1_000_000_000))

    def commit_files(self, contents: dict[str, str]) -> str:
        for name, content in contents.items():
            self.write_file(name, content)
        return self.commit("update inputs")

    def test_pr_lint_uses_both_committed_endpoints_and_separates_fixture_and_production_inputs(self):
        base = self.commit_files({"README.md": "base"})
        self.commit_files(
            {
                "doc/guide.md": "doc",
                "test/script/test_tools.py": "",
                "script/ao/command/measure.py": "",
                "test/lint/fixture/case.cpp": "",
            }
        )
        self.assertEqual(lint_scope.plan(base).files, ())
        self.commit_files({"lib/a.cc": "", "include/a.hxx": ""})
        self.commit_files({"app/b.mm": "", "include/detail.def": "", "test/t.cpp": ""})
        self.assertEqual(
            lint_scope.plan(base).files,
            ("app/b.mm", "include/a.hxx", "include/detail.def", "lib/a.cc", "test/t.cpp"),
        )

    @unittest.skipIf(os.name == "nt", "Windows forbids newlines in filenames")
    def test_pr_lint_preserves_newlines_and_spaces_in_git_paths(self):
        base = self.commit_files({"README.md": "base"})
        self.commit_files({"lib/one\ntwo space.cpp": ""})
        self.assertEqual(lint_scope.plan(base).files, ("lib/one\ntwo space.cpp",))

    def test_pr_lint_empty_comparison_does_not_establish_a_narrow_scope(self):
        base = self.commit_files({"README.md": "base"})
        self.assertIsNone(lint_scope.plan(base).files)
        self.git("commit", "--allow-empty", "-m", "unchanged tree")
        self.assertIsNone(lint_scope.plan(base).files)
        self.assertEqual(lint_scope.plan(None).files, ())

    def test_pr_lint_requires_the_base_commit_to_reach_the_checked_out_merge(self):
        self.commit_files({"README.md": "base"})
        self.git("checkout", "-b", "topic")
        self.commit_files({"lib/input.cpp": ""})
        self.git("checkout", "main")
        base = self.commit_files({"README.md": "advanced main"})
        self.git("checkout", "topic")
        self.assertIsNone(lint_scope.plan(base).files)
        self.git("merge", "--no-ff", "main", "-m", "merge the base into topic")
        self.assertEqual(lint_scope.plan(base).files, ("lib/input.cpp",))

    def test_pr_lint_rejects_invalid_bases_and_dirty_checkouts(self):
        base = self.commit_files({"README.md": "base"})
        self.git("tag", "-a", "annotated", "-m", "annotated")
        self.commit_files({"lib/input.cpp": ""})
        self.assertEqual(lint_scope.plan(base).files, ("lib/input.cpp",))
        for invalid in (base[:12], "--all", "HEAD", "0" * 40, self.git("rev-parse", "annotated")):
            with self.subTest(invalid=invalid):
                self.assertIsNone(lint_scope.plan(invalid).files)

        untracked = self.root / "new.hpp"
        untracked.touch()
        self.assertIsNone(lint_scope.plan(base).files)
        untracked.unlink()
        self.write_file("README.md", "dirty")
        self.assertIsNone(lint_scope.plan(base).files)
        self.git("add", "README.md")
        self.assertIsNone(lint_scope.plan(base).files)

    def test_lint_rejects_malformed_git_bindings(self):
        base = self.commit_files({"README.md": "base"})
        self.commit_files({"lib/Input.cpp": "change"})
        metadata = str(self.root / ".git")
        worktree = str(self.root)
        for binding, diagnosis in (
            ({"AOBUS_GIT_DIR": metadata}, r"both|pair|together"),
            ({"AOBUS_GIT_DIR": ".git", "AOBUS_GIT_WORK_TREE": worktree}, r"absolute"),
            ({"AOBUS_GIT_DIR": metadata, "AOBUS_GIT_WORK_TREE": "checkout"}, r"absolute"),
        ):
            for comparison in (base, None):
                with (
                    self.subTest(binding=binding, comparison=comparison),
                    mock.patch.dict(os.environ, binding),
                    contextlib.redirect_stderr(io.StringIO()),
                ):
                    with self.assertRaises(ValueError) as error:
                        lint_scope.plan(comparison)
                    self.assertIn("AOBUS_GIT", str(error.exception))
                    self.assertRegex(str(error.exception).lower(), diagnosis)

    def test_lint_rejects_missing_effective_metadata_but_ignores_another_checkout_binding(self):
        base = self.commit_files({"README.md": "base"})
        self.commit_files({"lib/Input.cpp": "change"})
        other = self.root.parent / "other-checkout"
        other.mkdir()
        metadata = other / "missing-metadata.git"
        binding = {"AOBUS_GIT_DIR": str(metadata), "AOBUS_GIT_WORK_TREE": str(other)}
        for comparison in (base, None):
            with self.subTest(comparison=comparison):
                with mock.patch.dict(os.environ, binding):
                    self.assertEqual(lint_scope.plan(comparison).files, ("lib/Input.cpp",))
                with (
                    mock.patch.dict(os.environ, {**binding, "AOBUS_GIT_WORK_TREE": str(self.root)}),
                    self.assertRaises(ValueError) as error,
                ):
                    lint_scope.plan(comparison)
                self.assertIn(metadata.name, str(error.exception))

    def test_lint_does_not_narrow_a_clean_sparse_checkout(self):
        base = self.commit_files({"README.md": "base", "test/linux-gtk/HiddenTest.cpp": ""})
        self.commit_files({"lib/input.cpp": ""})
        self.git("sparse-checkout", "set", "--cone", "lib")
        self.assertFalse((self.root / "test/linux-gtk/HiddenTest.cpp").exists())
        for comparison in (base, None):
            with self.subTest(comparison=comparison):
                self.assertIsNone(lint_scope.plan(comparison).files)

    def test_lint_treats_ignored_glob_inputs_as_dirty_but_not_bytecode_or_root_cache(self):
        base = self.commit_files({".gitignore": "build-*/\n__pycache__/\n/.cache/\n", "README.md": "base"})
        self.commit_files({"lib/input.cpp": ""})
        self.write_file("test/script/__pycache__/test_lint_scope.cpython-314.pyc", "")
        self.write_file(".cache/checkout/lib/Other.cpp", "")
        for comparison in (base, None):
            with self.subTest(comparison=comparison):
                self.assertEqual(lint_scope.plan(comparison).files, ("lib/input.cpp",))
        self.write_file("test/linux-gtk/build-preview/ExtraTest.cpp", "")
        for comparison in (base, None):
            with self.subTest(comparison=comparison):
                self.assertIsNone(lint_scope.plan(comparison).files)

    def test_pr_lint_cannot_bound_unknown_inputs_or_controls_even_mixed_with_native_edits(self):
        base = self.commit_files({"lib/product.cpp": "base"})
        for name in (
            "new-component/unknown.dat",
            "tool/lint/Check.cpp",
            "tool/catalog/build_catalog.py",
            "lib/utility/Path.cpp",
            "app/i18n/catalog/root.txt",
            "script/ao/graph/bazel_graph_lint.py",
            "script/ao/policy/tidy_config.py",
            "script/ao/toolchain.json",
            "script/ao/portal/lint_scope.py",
            ".bazelrc",
            "bazel/lint/lint.bzl",
            ".github/workflows/ci.yml",
        ):
            with self.subTest(name=name):
                head = self.commit_files({name: "changed", "lib/product.cpp": name})
                self.assertIsNone(lint_scope.plan(base).files)
                base = head

    def test_pr_lint_classifies_deleted_side_of_renames(self):
        base = self.commit_files({"lib/old.cpp": "source", "app/BUILD": "# build"})
        (self.root / "doc").mkdir()
        (self.root / "lib/old.cpp").rename(self.root / "doc/new.md")
        head = self.commit("move out of production")
        self.assertIsNone(lint_scope.plan(base).files)
        (self.root / "app/BUILD").rename(self.root / "app/BUILD.bazel")
        self.commit("rename package control")
        self.assertIsNone(lint_scope.plan(head).files)

    def test_pr_lint_build_trivia_preserves_strings_and_logical_statement_boundaries(self):
        name = "lib/BUILD.bazel"
        base = self.commit_files({name: 'custom(name="a # b", deps=select({":x": [":b"]}))\n'})
        formatted = (
            '# explain\ncustom(\n    name = "a # b", # not part of the name\n    deps = select({":x": [":b"]})\n)\n'
        )
        head = self.commit_files({name: formatted})
        self.assertEqual(lint_scope.plan(base).files, ())
        self.commit_files({name: formatted.replace("a # b", "a # c"), "app/cli/Other.cpp": ""})
        self.assertIsNone(lint_scope.plan(head).files)
        base = self.commit_files({name: "x = f\n(y)\n"})
        self.commit_files({name: "x = f(y)\n"})
        self.assertIsNone(lint_scope.plan(base).files)

    def test_pr_lint_global_control_and_new_package_boundary_require_the_full_graph(self):
        alias = 'alias(name="check", actual="//lib:one")'
        base = self.commit_files({"BUILD.bazel": alias, "tool/lint/BUILD": alias, "lib/pkg/Sub.cpp": ""})
        head = self.commit_files({"BUILD.bazel": alias + " # explanation", "tool/lint/BUILD": "# note\n" + alias})
        self.assertEqual(lint_scope.plan(base).files, ())
        head = self.commit_files({"BUILD.bazel": alias.replace(":one", ":two")})
        self.assertIsNone(lint_scope.plan(base).files)
        self.commit_files({"lib/pkg/BUILD.bazel": "# new boundary"})
        self.assertIsNone(lint_scope.plan(head).files)

    def test_winui_xaml_header_changes_require_full_lint_but_cpp_changes_stay_scoped(self):
        header = "app/windows-winui/MainWindow.xaml.h"
        source = "app/windows-winui/MainWindow.xaml.cpp"
        base = self.commit_files({header: "old declaration", source: "old implementation"})
        self.git("checkout", "-b", "topic")
        self.write_file(source, "new implementation")
        self.assertEqual(lint_scope.plan(None).files, (source,))
        implementation = self.commit("edit XAML implementation")
        self.assertEqual(lint_scope.plan(base).files, (source,))

        self.write_file(header, "new declaration")
        with self.subTest(mode="local"):
            self.assertIsNone(lint_scope.plan(None).files)
        self.commit("edit XAML header")
        with self.subTest(mode="explicit"):
            self.assertIsNone(lint_scope.plan(implementation).files)

    def test_local_lint_uses_previous_commit_on_main(self):
        self.commit_files({"README.md": "initial commit"})
        previous = self.commit_files({"lib/Earlier.cpp": "earlier commit"})
        self.commit_files({"lib/MainChange.cpp": "last commit"})
        plan = lint_scope.plan(None)
        self.assertEqual(plan.files, ("lib/MainChange.cpp",))
        self.assertIn(previous, plan.reason)

    def test_local_lint_uses_only_the_actual_main_branch_as_topic_base(self):
        base = self.commit_files({"README.md": "initial commit"})
        self.git("checkout", "-b", "topic")
        self.commit_files({"lib/Earlier.cpp": "earlier topic commit"})
        tip = self.commit_files({"lib/Latest.cpp": "latest topic commit"})
        self.git("tag", "main", tip)

        expected = ("lib/Earlier.cpp", "lib/Latest.cpp")
        with self.subTest(main="branch and tag"):
            plan = lint_scope.plan(None)
            self.assertEqual(plan.files, expected)
            self.assertIn(base, plan.reason)

        self.git("branch", "-D", "main")
        with self.subTest(main="tag only"):
            plan = lint_scope.plan(None)
            self.assertIsNone(plan.files)
            self.assertIn("main", plan.reason)
        self.git("tag", "-d", "main")
        with self.subTest(main="no matching refs"):
            plan = lint_scope.plan(None)
            self.assertIsNone(plan.files)
            self.assertIn("main", plan.reason)
            self.assertEqual(lint_scope.plan(base).files, expected)

    def test_local_lint_explains_missing_base_history_without_git_diagnostics(self):
        self.commit_files({"README.md": "initial commit"})
        binding = {"AOBUS_GIT_DIR": str(self.root / ".git"), "AOBUS_GIT_WORK_TREE": str(self.root)}
        missing_parent = r"\b(?:base|parent|previous|history)\b"
        for history, environment, diagnosis in (
            ("single commit", {}, missing_parent),
            ("single commit with bound Git", binding, missing_parent),
            ("unrelated branch", {}, r"\b(?:base|ancestor|history)\b"),
        ):
            with self.subTest(history=history):
                if history == "unrelated branch":
                    self.git("checkout", "--orphan", "unrelated")
                    self.git("rm", "-rf", "--", ".")
                    self.commit_files({"lib/Input.cpp": "unrelated root"})
                with mock.patch.dict(os.environ, environment), contextlib.redirect_stderr(io.StringIO()) as stderr:
                    plan = lint_scope.plan(None)
                self.assertIsNone(plan.files)
                self.assertEqual(stderr.getvalue(), "")
                self.assertEqual(plan.reason.splitlines(), [plan.reason])
                self.assertRegex(plan.reason.lower(), diagnosis)
                self.assertNotRegex(plan.reason, r"Command \[|returned non-zero|--no-replace-objects|fatal:|--commit")

    def test_local_lint_combines_topic_commit_and_dirty_paths_without_main_only_changes(self):
        self.commit_files({"include/Unstaged.hpp": "base"})
        self.git("checkout", "-b", "topic")
        self.commit_files({"lib/Committed.cpp": "topic commit"})
        self.git("checkout", "main")
        self.commit_files({"MODULE.bazel": "main changed a global input"})
        self.git("checkout", "topic")
        self.write_file("include/Unstaged.hpp", "working tree")
        self.write_file("lib/Staged.cpp", "new source")
        self.git("add", "lib/Staged.cpp")
        self.write_file("app/windows-winui/new source.cpp", "")

        index = self.root / ".git" / "index"
        before = index.read_bytes()
        self.assertEqual(
            lint_scope.plan(None).files,
            ("app/windows-winui/new source.cpp", "include/Unstaged.hpp", "lib/Committed.cpp", "lib/Staged.cpp"),
        )
        self.assertEqual(index.read_bytes(), before)

    def test_local_lint_ignores_replace_refs_when_selecting_the_base(self):
        os.environ.pop("GIT_NO_REPLACE_OBJECTS", None)
        self.commit_files({"lib/Input.cpp": "base", "lib/MainOnly.cpp": "base"})
        self.git("branch", "topic")
        replacement = self.commit_files({"lib/MainOnly.cpp": "main-only edit"})
        main = self.commit_files({"lib/Input.cpp": "shared edit", "lib/MainOnly.cpp": "base"})
        self.git("checkout", "topic")
        topic = self.commit_files({"lib/Input.cpp": "shared edit"})

        # The real main tree matches topic. The replacement main tree instead
        # differs at both paths, and the graft makes it look like an ancestor.
        self.git("replace", "--graft", topic, main)
        self.git("replace", main, replacement)
        self.assertEqual(git_files.diff_base(), main)

        self.assertEqual(lint_scope.plan(None).files, ("lib/Input.cpp",))

    def test_local_lint_distinguishes_stat_only_changes_from_real_worktree_inputs(self):
        control, native = "MODULE.bazel", "lib/Unchanged.cpp"
        base = self.commit_files({".gitattributes": "* text=auto eol=lf\n", control: "abcd\n", native: "// base\n"})
        self.git("checkout", "-b", "topic")
        self.commit_files({"lib/Topic.cpp": "// topic\n"})
        for name in (control, native):
            self.advance_mtime(name)

        index = self.root / ".git" / "index"
        before = index.read_bytes(), index.stat().st_mtime_ns
        for comparison in (None, base):
            self.assertEqual(lint_scope.plan(comparison).files, ("lib/Topic.cpp",))
        self.assertEqual((index.read_bytes(), index.stat().st_mtime_ns), before)

        # Both edits can yield the same all-zero worktree OID as a touch.
        self.write_file(control, "abce\n")
        self.advance_mtime(control, 10)
        self.assertIsNone(lint_scope.plan(None).files)
        self.git("add", control)
        self.write_file(control, "abcd\n")
        self.advance_mtime(control, 15)
        self.assertEqual(lint_scope.plan(None).files, ("lib/Topic.cpp",))
        self.assertIsNone(lint_scope.plan(base).files)  # explicit comparison still requires a clean index

    def test_local_lint_compares_bytes_after_checkout_eol_conversion(self):
        name = "launch.bat"
        self.commit_files({".gitattributes": "*.bat text eol=crlf\n", name: "@echo off\n"})
        self.git("checkout", "-b", "topic")
        self.commit_files({"lib/Topic.cpp": "// topic\n"})
        path = self.root / name
        path.unlink()
        self.git("checkout-index", "--", name)
        self.assertEqual(path.read_bytes(), b"@echo off\r\n")
        self.advance_mtime(name)
        self.assertEqual(lint_scope.plan(None).files, ("lib/Topic.cpp",))
        path.write_bytes(b"@echo off\n")  # equals the stored blob, not the expected checkout bytes
        # On Windows, a rewrite in the index's timestamp tick can be invisible to Git itself.
        self.advance_mtime(name, 10)
        self.assertIsNone(lint_scope.plan(None).files)

    def test_local_lint_keeps_uncertain_inputs_with_a_custom_filter(self):
        self.commit_files({".gitattributes": "MODULE.bazel filter=unspecified\n", "MODULE.bazel": "# base\n"})
        self.git("checkout", "-b", "topic")
        self.advance_mtime("MODULE.bazel")
        self.assertIsNone(lint_scope.plan(None).files)

    @unittest.skipIf(os.name == "nt", "Windows discovery deliberately ignores executable-bit changes")
    def test_local_lint_keeps_mode_only_control_change(self):
        self.commit_files({"MODULE.bazel": "# control\n"})
        self.git("checkout", "-b", "topic")
        self.git("config", "core.filemode", "true")
        (self.root / "MODULE.bazel").chmod(0o755)
        self.assertIsNone(lint_scope.plan(None).files)

    def test_local_lint_uses_working_tree_for_build_trivia_even_with_staged_semantic_edit(self):
        name = "lib/BUILD.bazel"
        alias = 'alias(name="foo", actual=":bar")\n'
        self.commit_files({name: alias})
        self.git("checkout", "-b", "topic")
        self.write_file(name, alias.replace(":bar", ":baz"))
        self.git("add", name)
        self.assertIsNone(lint_scope.plan(None).files)
        self.write_file(name, "# note\n" + alias)
        self.assertEqual(lint_scope.plan(None).files, ())

    def test_local_lint_checks_deleted_side_of_staged_and_unstaged_renames(self):
        self.commit_files({"lib/old.cpp": "source"})
        self.git("checkout", "-b", "topic")
        (self.root / "doc").mkdir()
        (self.root / "lib/old.cpp").rename(self.root / "doc/new.md")
        self.assertIsNone(lint_scope.plan(None).files)
        self.git("add", "--all")
        self.assertIsNone(lint_scope.plan(None).files)

    @unittest.skipIf(os.name == "nt", "native symlink creation needs separate Windows privileges")
    def test_local_lint_rejects_tracked_type_changes_and_untracked_links(self):
        self.commit_files({"README.md": "text", "doc/guide.md": "target"})
        self.git("checkout", "-b", "topic")
        linked = self.root / "doc/linked.md"
        linked.symlink_to("guide.md")
        self.assertIsNone(lint_scope.plan(None).files)
        linked.unlink()
        readme = self.root / "README.md"
        readme.unlink()
        readme.symlink_to("doc/guide.md")
        self.assertIsNone(lint_scope.plan(None).files)

    def test_local_lint_falls_back_when_index_flags_hide_graph_changes(self):
        self.commit_files({"BUILD.bazel": "# empty graph"})
        self.git("checkout", "-b", "topic")
        self.git("update-index", "--assume-unchanged", "BUILD.bazel")
        self.write_file("BUILD.bazel", 'alias(name="new", actual="//lib:target")')
        self.assertIsNone(lint_scope.plan(None).files)


if __name__ == "__main__":
    unittest.main()
