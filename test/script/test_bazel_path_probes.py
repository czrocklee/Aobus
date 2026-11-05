"""Executable selection and lexical PATH input inventory without running tools."""

import os
import sys
import tempfile
import unittest
from contextlib import chdir
from pathlib import Path
from types import SimpleNamespace
from unittest.mock import Mock, patch

from ao.core import bazel_native as native


class ExecutablePathProbeTest(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix="aobus-executable-probes-")
        self.addCleanup(temporary.cleanup)
        # macOS getcwd resolves /var through /private/var; normalize only the
        # fixture base, leaving deliberate lexical directory aliases intact.
        self.root = Path(temporary.name).resolve()
        self.before = self.root / "before"
        self.selected = self.root / "selected"
        self.after = self.root / "after"
        self.cwd = self.root / "cwd"
        for directory in (self.before, self.selected, self.after, self.cwd):
            directory.mkdir()
        # Tooling unittest cases run in an isolated process, not concurrent threads.
        # chdir restores cwd even when a selection or assertion raises.
        self.enterContext(chdir(self.cwd))
        self.enterContext(
            patch.dict(
                os.environ,
                {
                    "PATH": os.pathsep.join(map(str, (self.before, self.selected, self.after))),
                    "PATHEXT": ".COM;.EXE;.CMD",
                    "NoDefaultCurrentDirectoryInExePath": "1",
                },
            )
        )
        self.query = "aobus-path-probe"
        self.filename = self.query + (".EXE" if os.name == "nt" else "")
        self.names = (
            (self.query, self.query + ".COM", self.query + ".EXE", self.query + ".CMD")
            if os.name == "nt"
            else (self.query,)
        )
        self.cwd_probes = self.group(self.cwd) if os.name == "nt" else set()
        self.probes = set()

    def fixture(self, directory, name=None, *, executable=True):
        path = directory / (self.filename if name is None else name)
        path.parent.mkdir(parents=True, exist_ok=True)
        # which checks permissions/extensions, not PE content; never execute this.
        path.write_bytes(b"tiny executable lookup fixture\n")
        if os.name != "nt":
            path.chmod(0o755 if executable else 0o644)
        return path

    def group(self, directory, names=None):
        return {str(directory / name) for name in (self.names if names is None else names)}

    def select(self, name=None, *, nix=False):
        self.probes = set()
        with native.capture_probes(lambda path: self.probes.add(str(path))):
            return native.executable(self.query if name is None else name, nix=nix)

    def test_prefix_negatives_and_winner_remain_but_after_hit_changes_do_not_matter(self):
        winner = self.fixture(self.selected)
        prefix = self.group(self.before) | self.group(self.selected) | self.cwd_probes
        self.assertEqual(self.select(), str(winner))
        self.assertEqual(self.probes, prefix)

        later = self.fixture(self.after)
        for payload in (b"later executable appeared", b"later executable changed"):
            with self.subTest(payload=payload):
                later.write_bytes(payload)
                self.assertEqual(self.select(), str(winner))
                self.assertEqual(self.probes, prefix)

        earlier = self.fixture(self.before)
        self.assertEqual(self.select(), str(earlier))
        self.assertEqual(self.probes, self.group(self.before) | self.cwd_probes)

    def test_removing_winner_selects_fallback_and_retains_preceding_negatives(self):
        winner = self.fixture(self.selected)
        fallback = self.fixture(self.after)
        self.assertEqual(self.select(), str(winner))
        self.assertEqual(self.probes, self.group(self.before) | self.group(self.selected) | self.cwd_probes)

        winner.unlink()
        self.assertEqual(self.select(), str(fallback))
        self.assertEqual(
            self.probes, self.group(self.before) | self.group(self.selected) | self.group(self.after) | self.cwd_probes
        )

    def test_all_misses_keep_every_negative_candidate(self):
        with self.assertRaisesRegex(native.BazelWorkspaceError, "unavailable"):
            self.select()
        self.assertEqual(
            self.probes, self.group(self.before) | self.group(self.selected) | self.group(self.after) | self.cwd_probes
        )

    def test_direct_and_slashed_queries_watch_only_their_local_directory(self):
        direct = self.cwd / "subdir"
        winner = self.fixture(direct)
        self.fixture(self.before)
        queries = [str(direct / self.query), os.path.join("subdir", self.query)]
        if os.name == "nt":
            queries.extend(("subdir/" + self.query, os.path.splitdrive(str(direct))[0] + "subdir\\" + self.query))
        for query in queries:
            with self.subTest(query=query):
                self.assertEqual(self.select(query), str(winner))
                self.assertEqual(self.probes, self.group(direct))

    def test_unmatched_which_spelling_keeps_all_groups_and_explicit_winner(self):
        winner = self.fixture(self.root / "unknown-layout")
        selector = Mock(return_value=str(winner))
        # Replace only the native namespace, never the shared shutil function.
        with patch.object(native, "shutil", SimpleNamespace(which=selector)):
            self.assertEqual(self.select(), str(winner))
        selector.assert_called_once_with(self.query)
        self.assertEqual(
            self.probes,
            self.group(self.before)
            | self.group(self.selected)
            | self.group(self.after)
            | self.cwd_probes
            | {str(winner)},
        )

    @unittest.skipIf(os.name == "nt", "POSIX execute permission selection")
    def test_nonexecutable_prefix_file_becomes_winner_after_chmod(self):
        earlier = self.fixture(self.before, executable=False)
        winner = self.fixture(self.selected)
        self.assertEqual(self.select(), str(winner))
        self.assertEqual(self.probes, self.group(self.before) | self.group(self.selected))

        earlier.chmod(0o755)
        self.assertEqual(self.select(), str(earlier))
        self.assertEqual(self.probes, self.group(self.before))

    @unittest.skipIf(os.name == "nt", "POSIX lexical directory aliases")
    def test_lexical_alias_negative_is_retained_without_watching_later_real_directory(self):
        common = self.root / "common"
        common.mkdir()
        alias = self.root / "alias"
        alias.symlink_to(common, target_is_directory=True)
        os.environ["PATH"] = os.pathsep.join(map(str, (alias, self.selected, common)))
        winner = self.fixture(self.selected)
        self.assertEqual(self.select(), str(winner))
        self.assertEqual(self.probes, self.group(alias) | self.group(self.selected))

    @unittest.skipIf(os.name == "nt", "POSIX lexical wrapper basename and Nix rejection")
    def test_wrapper_basename_is_preserved_and_non_nix_selection_is_rejected(self):
        payload = self.fixture(self.selected, "clang")
        wrapper = self.selected / "clang++"
        wrapper.symlink_to(payload)
        expected = {str(self.before / "clang++"), str(wrapper)}
        self.assertEqual(self.select("clang++"), str(wrapper))
        self.assertEqual(self.probes, expected)
        with self.assertRaisesRegex(native.BazelWorkspaceError, "unavailable"):
            self.select("clang++", nix=True)
        self.assertEqual(self.probes, expected)

    @unittest.skipIf(os.name == "nt", "POSIX empty PATH components")
    def test_empty_path_does_not_select_cwd_but_empty_component_does(self):
        winner = self.fixture(self.cwd)
        os.environ["PATH"] = ""
        with self.assertRaisesRegex(native.BazelWorkspaceError, "unavailable"):
            self.select()

        os.environ["PATH"] = os.pathsep + str(self.selected)
        self.assertEqual(self.select(), str(winner))
        self.assertEqual(self.probes, self.group(self.cwd))

    @unittest.skipIf(os.name == "nt", "POSIX confstr default PATH parser")
    def test_unset_path_uses_confstr_and_its_documented_defpath_fallbacks(self):
        winner = self.fixture(self.selected)
        search_path = os.pathsep.join(map(str, (self.before, self.selected, self.after)))
        for failure in (None, AttributeError("no confstr"), ValueError("no CS_PATH")):
            with self.subTest(failure=failure):
                environment = dict(os.environ)
                environment.pop("PATH", None)
                local_os = SimpleNamespace(**vars(os))
                local_os.environ = environment
                local_os.confstr = Mock(return_value=search_path, side_effect=failure)
                local_os.defpath = search_path if failure else str(self.root / "wrong-default")
                local_os.get_exec_path = Mock(return_value=local_os.defpath.split(os.pathsep))
                selector = Mock(return_value=str(winner))
                with (
                    patch.object(native, "os", local_os),
                    patch.object(native, "shutil", SimpleNamespace(which=selector)),
                ):
                    self.assertEqual(self.select(), str(winner))
                selector.assert_called_once_with(self.query)
                self.assertEqual(self.probes, self.group(self.before) | self.group(self.selected))

    @unittest.skipUnless(sys.platform == "win32", "native Windows PATHEXT selection")
    def test_pathext_keeps_entire_winner_group_and_earlier_com_preempts_exe(self):
        os.environ["PATHEXT"] = ".COM;.eXe;.CMD"
        names = (self.query, self.query + ".COM", self.query + ".eXe", self.query + ".CMD")
        winner = self.fixture(self.selected, self.query + ".eXe")
        cwd = self.group(self.cwd, names)
        self.assertEqual(self.select(), str(winner))
        self.assertEqual(self.probes, self.group(self.before, names) | self.group(self.selected, names) | cwd)

        earlier = self.fixture(self.before, self.query + ".COM")
        self.assertEqual(self.select(), str(earlier))
        self.assertEqual(self.probes, self.group(self.before, names) | cwd)

    @unittest.skipUnless(sys.platform == "win32", "native Windows suffixed query selection")
    def test_doubled_extensions_can_preempt_selected_bare_exe(self):
        os.environ["PATHEXT"] = ".EXE;.CMD"
        query = self.query + ".EXE"
        names = (query, query + ".EXE", query + ".CMD")
        winner = self.fixture(self.selected, query)
        cwd = self.group(self.cwd, names)
        self.assertEqual(self.select(query), str(winner))
        self.assertEqual(self.probes, self.group(self.before, names) | self.group(self.selected, names) | cwd)

        earlier = self.fixture(self.before, query + ".CMD")
        self.assertEqual(self.select(query), str(earlier))
        self.assertEqual(self.probes, self.group(self.before, names) | cwd)

    @unittest.skipUnless(sys.platform == "win32", "native Windows default and trailing-dot PATHEXT")
    def test_unset_empty_and_trailing_dot_pathext_keep_all_extension_negatives(self):
        winner = self.fixture(self.selected)
        defaults = ("", ".COM", ".EXE", ".BAT", ".CMD", ".VBS", ".JS", ".WS", ".MSC")
        for value in (None, "", ".COM.;.EXE.;.CMD."):
            with self.subTest(pathext=value), patch.dict(os.environ):
                if value is None:
                    os.environ.pop("PATHEXT", None)
                else:
                    os.environ["PATHEXT"] = value
                names = tuple(self.query + suffix for suffix in defaults) if not value else self.names
                self.assertEqual(self.select(), str(winner))
                self.assertEqual(
                    self.probes,
                    self.group(self.before, names) | self.group(self.selected, names) | self.group(self.cwd, names),
                )

    @unittest.skipUnless(sys.platform == "win32", "native Windows explicit cwd PATH entry")
    def test_no_default_cwd_does_not_cut_off_path_before_explicit_dot(self):
        os.environ["NoDefaultCurrentDirectoryInExePath"] = "1"
        os.environ["PATH"] = os.pathsep.join((str(self.before), ".", str(self.after)))
        winner = self.fixture(self.cwd)
        self.assertEqual(self.select(), str(winner))
        self.assertEqual(self.probes, self.group(self.before) | self.group(self.cwd))

        earlier = self.fixture(self.before)
        self.assertEqual(self.select(), str(earlier))
        self.assertEqual(self.probes, self.group(self.before) | self.group(self.cwd))

    @unittest.skipUnless(sys.platform == "win32", "native Windows implicit cwd selection")
    def test_implicit_cwd_can_appear_and_conservatively_retain_full_path(self):
        os.environ.pop("NoDefaultCurrentDirectoryInExePath", None)
        winner = self.fixture(self.selected)
        self.assertEqual(self.select(), str(winner))
        self.assertEqual(self.probes, self.group(self.before) | self.group(self.selected) | self.cwd_probes)

        cwd_winner = self.fixture(self.cwd)
        self.assertEqual(self.select(), str(cwd_winner))
        # The synthetic cwd group cannot prove a cutoff in the actual PATH.
        self.assertEqual(
            self.probes, self.group(self.before) | self.group(self.selected) | self.group(self.after) | self.cwd_probes
        )

    @unittest.skipUnless(sys.platform == "win32", "native Windows non-basename PATHEXT")
    def test_pathext_with_directory_component_disables_path_cutoff(self):
        os.environ["PATHEXT"] = ".EXE;.nested\\variant"
        names = (self.query, self.query + ".EXE", self.query + ".nested\\variant")
        winner = self.fixture(self.selected)
        self.assertEqual(self.select(), str(winner))
        self.assertEqual(
            self.probes,
            self.group(self.before, names)
            | self.group(self.selected, names)
            | self.group(self.after, names)
            | self.group(self.cwd, names),
        )


if __name__ == "__main__":
    unittest.main()
