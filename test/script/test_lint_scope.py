"""Production lint scope against real Git checkouts."""

import contextlib
import io
import os
import subprocess
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

    def advance_mtime(self, name: str) -> None:
        path = self.root / name
        st = path.stat()
        os.utime(path, ns=(st.st_atime_ns, st.st_mtime_ns + 5_000_000_000))

    def commit_files(self, contents: dict[str, str]) -> str:
        for name, content in contents.items():
            self.write_file(name, content)
        return self.commit("update inputs")

    def assert_scope(self, base: str, expected: tuple[str, ...] | None) -> None:
        for comparison in (None, base):
            with self.subTest(comparison=comparison):
                self.assertEqual(lint_scope.plan(comparison).files, expected)

    def test_verified_comparisons_select_changed_native_files_but_not_lint_fixtures(self):
        base = self.commit_files({"lib/Unchanged.cpp": "// base\n"})
        self.git("checkout", "-b", "topic")
        self.commit_files({"lib/Input.cc": "// input\n", "include/Api.hxx": "// declaration\n"})
        self.commit_files(
            {
                "app/macos-appkit/Window.mm": "// implementation\n",
                "include/Detail.def": "// fragment\n",
                "shared/Native.hpp": "// header outside the traditional source roots\n",
                "test/uimodel/WindowTest.cpp": "// test\n",
                "tool/lint/AobusLintModule.cpp": "// checker module\n",
                "test/lint/fixture/case.cpp": "// negative fixture\n",
                "test/lint/Infrastructure.h": "// fixture infrastructure\n",
                "lib/Legacy.C": "// no report for this extension in the Bazel lint aspect\n",
            }
        )
        self.assert_scope(
            base,
            (
                "app/macos-appkit/Window.mm",
                "include/Api.hxx",
                "include/Detail.def",
                "lib/Input.cc",
                "shared/Native.hpp",
                "test/uimodel/WindowTest.cpp",
                "tool/lint/AobusLintModule.cpp",
            ),
        )

    @unittest.skipIf(os.name == "nt", "Windows forbids newlines in filenames")
    def test_pr_lint_preserves_newlines_and_spaces_in_git_paths(self):
        base = self.commit_files({"README.md": "base"})
        self.commit_files({"lib/one\ntwo space.cpp": ""})
        self.assertEqual(lint_scope.plan(base).files, ("lib/one\ntwo space.cpp",))

    def test_verified_empty_comparison_skips_production_lint(self):
        base = self.commit_files({"lib/Input.cpp": "// unchanged\n"})
        self.assertEqual(lint_scope.plan(base).files, ())
        self.git("commit", "--allow-empty", "-m", "unchanged tree")
        self.assert_scope(base, ())

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

        untracked = self.root / "BUILD.bazel"
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
        self.assert_scope(base, None)

    def test_ignored_files_and_local_untracked_controls_do_not_expand_scope(self):
        base = self.commit_files({".gitignore": "build-*/\n", "README.md": "base\n"})
        self.commit_files({"lib/Input.cpp": "// changed\n"})
        self.write_file("lib/build-preview/Generated.cpp", "// ignored native source\n")
        self.write_file("lib/build-preview/BUILD.bazel", "# ignored control\n")
        self.assert_scope(base, ("lib/Input.cpp",))

        self.write_file("lib/BUILD.bazel", "# untracked package\n")
        self.assertEqual(lint_scope.plan(None).files, ("lib/Input.cpp",))

    def test_verified_scope_ignores_controls_with_or_without_added_source(self):
        alias = 'alias(name="check", actual="//lib:default")\n'
        base = self.commit_files({"BUILD.bazel": alias, "lib/Unchanged.cpp": "// unchanged\n"})
        controls = self.commit_files(
            {
                "BUILD.bazel": alias.replace(":default", ":native"),
                ".bazelrc": "build --keep_going\n",
                ".github/workflows/ci.yml": "name: check\n",
                "app/i18n/catalog/root.txt": "root { greeting { hello } }\n",
                "script/ao/policy/tidy_config.py": "CHECKS = 'changed'\n",
                "tool/catalog/build_catalog.py": "# catalog build tool\n",
                "new-component/unknown.dat": "new inputs\n",
            }
        )
        self.assert_scope(base, ())

        self.commit_files(
            {
                "lib/BUILD.bazel": 'cc_library(name="new", srcs=["New.cpp"])\n',
                "lib/New.cpp": "// new implementation\n",
            }
        )
        self.assert_scope(controls, ("lib/New.cpp",))

    def test_verified_deletions_do_not_select_missing_files_or_the_full_graph(self):
        base = self.commit_files({"lib/Deleted.cpp": "// removed\n", "lib/BUILD.bazel": "# removed package\n"})
        self.git("rm", "lib/Deleted.cpp", "lib/BUILD.bazel")
        self.commit("delete native input and package")
        self.assert_scope(base, ())

    def test_verified_comparisons_include_existing_xaml_and_catalog_tool_headers(self):
        headers = (
            "app/i18n/MessageIds.h",
            "app/windows-winui/MainWindow.xaml.h",
            "include/ao/Contract.h",
            "include/ao/utility/Path.h",
        )
        base = self.commit_files(dict.fromkeys(headers, "// old declaration\n"))
        self.commit_files(dict.fromkeys(headers, "// new declaration\n"))
        self.assert_scope(base, headers)

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
        self.commit_files({"include/Unstaged.hpp": "base", "lib/MainOnly.cpp": "common ancestor"})
        self.git("checkout", "-b", "topic")
        self.commit_files({"lib/Committed.cpp": "topic commit"})
        self.git("checkout", "main")
        self.commit_files({"lib/MainOnly.cpp": "main-only edit"})
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

    def test_local_lint_drops_a_touch_but_keeps_a_same_size_edit(self):
        name = "lib/Input.cpp"
        original = "// original\n"
        base = self.commit_files({".gitattributes": "*.cpp text eol=lf\n", name: original})
        self.git("checkout", "-b", "topic")
        self.commit_files({"lib/Topic.cpp": "// topic\n"})
        self.advance_mtime(name)

        index = self.root / ".git" / "index"
        before = index.read_bytes(), index.stat().st_mtime_ns
        self.assert_scope(base, ("lib/Topic.cpp",))
        self.assertEqual((index.read_bytes(), index.stat().st_mtime_ns), before)

        # A touch and a same-size edit can both yield an all-zero worktree OID.
        self.write_file(name, original.replace("original", "modified"))
        self.advance_mtime(name)
        self.assertEqual(lint_scope.plan(None).files, (name, "lib/Topic.cpp"))

        self.git("add", name)
        self.write_file(name, original)
        self.advance_mtime(name)
        self.assertEqual(lint_scope.plan(None).files, ("lib/Topic.cpp",))
        self.assertIsNone(lint_scope.plan(base).files)  # explicit comparison still requires a clean index

    def test_local_lint_compares_native_bytes_after_checkout_eol_conversion(self):
        name = "lib/Input.cpp"
        self.commit_files({".gitattributes": "*.cpp text eol=crlf\n", name: "// original\n"})
        self.git("checkout", "-b", "topic")
        path = self.root / name
        path.unlink()
        self.git("checkout-index", "--", name)
        self.assertEqual(path.read_bytes(), b"// original\r\n")
        self.advance_mtime(name)
        self.assertEqual(lint_scope.plan(None).files, ())

        path.write_bytes(b"// original\n")  # equals the stored blob, not the expected checkout bytes
        # On Windows, a rewrite in the index's timestamp tick can be invisible to Git itself.
        self.advance_mtime(name)
        self.assertEqual(lint_scope.plan(None).files, (name,))

    def test_local_lint_keeps_uncertain_native_inputs_with_a_custom_filter(self):
        name = "lib/Input.cpp"
        self.commit_files({".gitattributes": "*.cpp filter=unspecified\n", name: "// original\n"})
        self.git("checkout", "-b", "topic")
        self.advance_mtime(name)
        self.assertEqual(lint_scope.plan(None).files, (name,))

    @unittest.skipIf(os.name == "nt", "Windows discovery deliberately ignores executable-bit changes")
    def test_local_lint_keeps_mode_only_native_change(self):
        name = "lib/Input.cpp"
        self.commit_files({name: "// original\n"})
        self.git("checkout", "-b", "topic")
        self.git("config", "core.filemode", "true")
        (self.root / name).chmod(0o755)
        self.assertEqual(lint_scope.plan(None).files, (name,))

    def test_renames_select_only_surviving_eligible_destinations(self):
        base = self.commit_files(
            {"lib/Retired.cpp": "// retired\n", "lib/Old.cpp": "// renamed\n", "doc/sample.md": "// promoted\n"}
        )
        self.git("checkout", "-b", "topic")
        (self.root / "lib/Retired.cpp").rename(self.root / "doc/retired.md")
        (self.root / "lib/Old.cpp").rename(self.root / "lib/New.cpp")
        (self.root / "doc/sample.md").rename(self.root / "lib/Promoted.h")
        expected = ("lib/New.cpp", "lib/Promoted.h")
        self.assertEqual(lint_scope.plan(None).files, expected)
        self.git("add", "--all")
        self.assertEqual(lint_scope.plan(None).files, expected)
        self.commit("rename native and non-native paths")
        self.assert_scope(base, expected)

    @unittest.skipIf(os.name == "nt", "native symlink creation needs separate Windows privileges")
    def test_only_regular_sides_of_native_symlink_transitions_are_selected(self):
        base = self.commit_files({"README.md": "link target\n", "lib/ToLink.cpp": "// original\n"})
        (self.root / "lib/ToRegular.hpp").symlink_to("../README.md")
        links = self.commit("add native-named symlink")
        self.assert_scope(base, ())
        self.git("checkout", "-b", "topic")

        untracked = self.root / "lib/Untracked.cpp"
        untracked.symlink_to("../README.md")
        self.assertEqual(lint_scope.plan(None).files, ())
        untracked.unlink()

        to_link = self.root / "lib/ToLink.cpp"
        to_link.unlink()
        to_link.symlink_to("../README.md")
        to_regular = "lib/ToRegular.hpp"
        (self.root / to_regular).unlink()
        self.write_file(to_regular, "// declaration\n")
        self.assertEqual(lint_scope.plan(None).files, (to_regular,))
        self.commit("swap native regular and symlink modes")
        self.assert_scope(links, (to_regular,))

    def test_verified_scope_skips_gitlinks_and_symlinks_materialized_as_text(self):
        base = self.commit_files({"README.md": "link target\n"})
        self.git("config", "core.symlinks", "false")
        symlink = "lib/Link.cpp"
        payload = self.git("hash-object", "-w", "README.md")
        self.git("update-index", "--add", "--cacheinfo", f"120000,{payload},{symlink}")
        self.git("checkout-index", "--", symlink)
        self.assertTrue((self.root / symlink).is_file())
        self.assertFalse((self.root / symlink).is_symlink())

        submodule = "lib/Nested.cpp"
        self.git("update-index", "--add", "--cacheinfo", f"160000,{base},{submodule}")
        (self.root / submodule).mkdir()
        self.write_file("lib/Input.cpp", "// native source\n")
        self.git("add", "lib/Input.cpp")
        self.git("commit", "-m", "record non-regular Git modes alongside a source")
        self.assert_scope(base, ("lib/Input.cpp",))

    def test_lint_falls_back_when_index_flags_can_hide_changes(self):
        name = "lib/Input.cpp"
        base = self.commit_files({name: "// original\n"})
        self.commit_files({"include/Api.h": "// declaration\n"})
        for flag in ("assume-unchanged", "skip-worktree"):
            with self.subTest(flag=flag):
                self.git("update-index", f"--{flag}", name)
                self.assert_scope(base, None)
                self.git("update-index", f"--no-{flag}", name)

    def test_local_lint_falls_back_for_an_unmerged_index_even_on_a_control_path(self):
        name = "BUILD.bazel"
        self.commit_files({name: "# base\n"})
        self.git("checkout", "-b", "topic")
        self.commit_files({name: "# topic\n", "lib/Input.cpp": "// new source\n"})
        self.git("checkout", "main")
        self.commit_files({name: "# main\n"})
        self.git("checkout", "topic")
        with self.assertRaises(subprocess.CalledProcessError):
            self.git("merge", "--no-edit", "main")
        self.assertTrue(self.git("ls-files", "--unmerged", "--", name))
        self.assertIsNone(lint_scope.plan(None).files)


if __name__ == "__main__":
    unittest.main()
