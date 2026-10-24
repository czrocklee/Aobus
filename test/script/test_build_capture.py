"""Focused tests for the private WinUI MSBuild capture option.

These checks exercise the portal's Python boundary only: parsing, destination
validation, snapshot manifests, and phase-record hygiene. Native-Windows
local authorities (drive classification, Windows destination syntax) are
mocked where the host cannot provide them; no test claims native MSBuild
logger transport, which the delegated Windows owner validates separately.
"""

import contextlib
import hashlib
import io
import json
import os
import shutil
import tempfile
import unittest
from collections import Counter
from pathlib import Path
from unittest import mock

from ao.__main__ import make_parser
from ao.command import build
from ao.core import builddir
from ao.core.paths import PROJECT_ROOT

# Every field a phase record may carry; capture data never appears here.
PHASE_ROW_FIELDS = {
    "schema_version",
    "kind",
    "session_id",
    "pid",
    "phase_id",
    "phase",
    "walltime_start_ns",
    "parent_id",
    "command",
    "tree",
    "mode",
    "suite",
    "jobs",
    "count",
    "source_id",
    "elapsed_ns",
    "status",
    "outcome",
}


class MsbuildCaptureParserTest(unittest.TestCase):
    def test_build_owns_the_capture_option(self):
        args = make_parser().parse_args(["build", "--target", "winui", "--msbuild-capture", r"C:\cap\first"])
        self.assertEqual(args.msbuild_capture, r"C:\cap\first")
        self.assertEqual(args.target, ["winui"])
        self.assertIsNone(make_parser().parse_args(["build"]).msbuild_capture)

    def test_shared_build_parsers_do_not_expose_the_capture_option(self):
        for arguments in (
            ["check", "--msbuild-capture", r"C:\cap\first"],
            ["run", "cli", "--msbuild-capture", r"C:\cap\first"],
            ["tidy", "--msbuild-capture", r"C:\cap\first"],
        ):
            with self.subTest(arguments=arguments):
                with contextlib.redirect_stderr(io.StringIO()), self.assertRaises(SystemExit):
                    make_parser().parse_args(arguments)

    def test_capture_option_requires_a_value(self):
        with contextlib.redirect_stderr(io.StringIO()), self.assertRaises(SystemExit):
            make_parser().parse_args(["build", "--msbuild-capture"])


class MsbuildCaptureDestinationTest(unittest.TestCase):
    def test_portable_windows_destination_syntax_is_accepted(self):
        for destination in (r"C:\aobus-capture\first", "c:\\aobus-capture\\first", "C:/aobus-capture/first"):
            with self.subTest(destination=destination):
                self.assertIsNone(build._capture_syntax_error(destination))

    def test_invalid_destination_syntax_is_rejected(self):
        cases = (
            ("", "requires"),
            ("cap\\first", "absolute"),
            (r"\\server\share\cap\first", "fixed local drive"),
            ("C:\\first", "parent"),
            ("C:\\", "parent"),
            (r"C:\cap\first second", "whitespace"),
            (r"C:\cap\first;extra", "delimiter"),
            ('C:\\cap\\"first', "quote"),
            ("C:\\cap\\first\n", "delimiter"),
            ("C:\\cap\\first\x00", "delimiter"),
        )
        for destination, fragment in cases:
            with self.subTest(destination=destination):
                message = build._capture_syntax_error(destination)
                self.assertIsNotNone(message)
                self.assertIn(fragment, message)

    def test_unverifiable_drive_classification_is_rejected(self):
        # A spelling without a drive cannot establish fixed-local ownership on
        # any host; Windows TEMP itself commonly resides on a valid fixed C:.
        self.assertFalse(build._capture_drive_is_fixed(Path("relative")))


class MsbuildCapturePrepareTest(unittest.TestCase):
    """Destination lifetime against the real local filesystem authority."""

    def setUp(self):
        self.root = Path(tempfile.mkdtemp())
        self.source = self.root / "source"
        self.tree = self.root / "tree"
        self.source.mkdir()
        self.tree.mkdir()

    def tearDown(self):
        shutil.rmtree(self.root, ignore_errors=True)

    def prepare(self, destination, *, tree=None, source=None):
        with mock.patch.object(build, "_capture_drive_is_fixed", return_value=True):
            return build._prepare_capture(
                str(destination), build_dir=tree or self.tree, source_dir=source or self.source
            )

    def reject(self, destination, fragment, *, tree=None, source=None):
        with (
            contextlib.redirect_stderr(io.StringIO()) as stderr,
            self.assertRaises(SystemExit) as raised,
        ):
            self.prepare(destination, tree=tree, source=source)
        self.assertEqual(raised.exception.code, 1)
        self.assertIn(fragment, stderr.getvalue())
        return Path(destination)

    def test_creates_exclusive_leaf_with_native_permission_semantics(self):
        parent = self.root / "capture"
        parent.mkdir()
        leaf = parent / "first"
        created = self.prepare(leaf)
        self.assertTrue(created.is_dir())
        if os.name != "nt":
            self.assertEqual(created.stat().st_mode & 0o077, 0)
        # Windows stat mode bits do not describe its ACL; privacy remains the
        # caller's documented responsibility, not a POSIX-mode certification.
        self.assertEqual(created, leaf)

    def test_existing_leaf_is_rejected_without_mutation(self):
        leaf = self.prepare(self.root / "capture-first")
        with contextlib.redirect_stderr(io.StringIO()), self.assertRaises(SystemExit):
            self.prepare(leaf)
        self.assertTrue(leaf.is_dir())
        self.assertEqual(list(leaf.iterdir()), [])

    def test_existing_file_destination_is_rejected(self):
        leaf = self.root / "capture-first"
        leaf.write_text("occupied", encoding="utf-8")
        self.reject(leaf, "already exists")
        self.assertEqual(leaf.read_text(encoding="utf-8"), "occupied")

    def test_missing_parent_is_rejected(self):
        self.reject(self.root / "missing" / "first", "parent directory does not exist")

    def test_capture_inside_the_build_tree_is_rejected(self):
        leaf = self.reject(self.tree / "inside", "disjoint")
        self.assertFalse(leaf.exists())

    def test_capture_inside_the_source_is_rejected(self):
        leaf = self.reject(self.source / "cap", "disjoint")
        self.assertFalse(leaf.exists())

    def test_lexically_local_destination_resolving_to_remote_storage_is_rejected(self):
        remote = self.root / "remote-target"
        remote.mkdir()
        link = self.root / "link-parent"
        link.mkdir()
        leaf = link / "first"
        resolve = Path.resolve

        def resolved(path, *args, **kwargs):
            # Model a reparse-point result without requiring Windows symlink
            # privileges or changing the guest's Developer Mode.
            return remote / "first" if path == leaf else resolve(path, *args, **kwargs)

        def fixed(destination):
            return destination == leaf  # the lexical spelling looks local; the resolution does not

        with (
            mock.patch.object(build, "_capture_drive_is_fixed", side_effect=fixed),
            mock.patch.object(Path, "resolve", autospec=True, side_effect=resolved),
            contextlib.redirect_stderr(io.StringIO()) as stderr,
            self.assertRaises(SystemExit) as raised,
        ):
            build._prepare_capture(str(leaf), build_dir=self.tree, source_dir=self.source)

        self.assertEqual(raised.exception.code, 1)
        self.assertIn("fixed local drive", stderr.getvalue())
        self.assertFalse((remote / "first").exists())

    def test_build_tree_inside_the_capture_is_rejected(self):
        # A fresh tree the portal would create under the capture destination.
        (self.root / "outer").mkdir()
        capture = self.root / "outer" / "cap"
        leaf = self.reject(capture, "disjoint", tree=capture / "tree")
        self.assertFalse(leaf.exists())


class MsbuildCaptureSnapshotSelectionTest(unittest.TestCase):
    def setUp(self):
        self.root = Path(tempfile.mkdtemp())
        self.tree = self.root / "tree"
        self.leaf = self.root / "capture"
        self.tree.mkdir()
        self.leaf.mkdir()

    def tearDown(self):
        shutil.rmtree(self.root, ignore_errors=True)

    def put(self, relative, payload):
        path = self.tree / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(payload)
        return path

    def snapshot(self, stage):
        build._write_capture_snapshot(self.leaf, stage, self.tree)
        return json.loads((self.leaf / stage / "manifest.json").read_text(encoding="utf-8"))

    def test_project_state_preserves_bytes_zero_byte_presence_and_stage_absence(self):
        payloads = {
            "app/windows-winui/obj/Debug/Aobus.tlog/Aobus.LASTBUILDSTATE": b"state\r\n\x00\xff",
            "app/windows-winui/obj/Debug/Aobus.tlog/UnSuccessfulBuild": b"",
            "lib/obj/Release/core.tlog/core.unsuccessfulbuild": b"project-specific marker",
        }
        for relative, payload in payloads.items():
            self.put(relative, payload)
        tlog = "app/windows-winui/obj/Debug/Aobus.tlog/CL.read.1.TLOG"
        self.put(tlog, b"raw tracking\x00\xff")
        self.put("app/windows-winui/obj/Debug/unrelated.state", b"not selected")

        before = self.snapshot("before-configure")
        configured = self.snapshot("before-build")
        marker = "app/windows-winui/obj/Debug/Aobus.tlog/UnSuccessfulBuild"
        (self.tree / marker).unlink()
        after = self.snapshot("after-build")

        for stage, manifest in (("before-configure", before), ("before-build", configured), ("after-build", after)):
            expected = set(payloads) - ({marker} if stage == "after-build" else set())
            entries = {entry["path"]: entry for entry in manifest["project_state"]}
            self.assertEqual(set(entries), expected)
            for relative in expected:
                copy = self.leaf / stage / "project-state" / relative
                self.assertEqual(copy.read_bytes(), payloads[relative])
                self.assertEqual(entries[relative]["sha256"], hashlib.sha256(payloads[relative]).hexdigest())
                self.assertEqual(entries[relative]["size"], len(payloads[relative]))
            self.assertEqual([entry["path"] for entry in manifest["tlogs"]], [tlog])
            self.assertEqual((self.leaf / stage / "tlogs" / tlog).read_bytes(), b"raw tracking\x00\xff")
        self.assertEqual(
            {entry["path"] for entry in configured["project_state"]}
            - {entry["path"] for entry in after["project_state"]},
            {marker},
        )
        self.assertFalse((self.leaf / "after-build" / "project-state" / marker).exists())

    def test_dependency_roots_are_pruned_without_excluding_nested_first_party_names(self):
        included = {
            "app/windows-winui/Debug/Aobus.exe": b"product",
            "lib/Debug/core.lib": b"first-party library",
            "app/windows-winui/nested/third-party_deps/identity.dll": b"first-party name",
            "tool/vcpkg_installed/generator/tool.DLL": b"tool identity",
        }
        excluded = {
            "vcpkg_installed/x64-windows/lib/dependency.lib": b"dependency",
            "vcpkg_installed/nested/third-party_deps/tool.exe": b"nested dependency",
            "third-party_deps/nested/build/tool.dll": b"third-party tool",
        }
        for relative, payload in {**included, **excluded}.items():
            self.put(relative, payload)
        tlog = "app/windows-winui/obj/Debug/Aobus.tlog/CL.read.1.tlog"
        self.put(tlog, b"native project tracking")
        self.put("vcpkg_installed/obj/dependency.tlog", b"dependency tracking")
        obj = self.put("app/windows-winui/obj/Debug/Aobus.obj", b"object never opened")
        visited = []
        real_walk, real_open = build.os.walk, Path.open

        def observed_walk(top, **kwargs):
            for entry in real_walk(top, **kwargs):
                visited.append(Path(entry[0]).relative_to(self.tree).as_posix())
                yield entry

        def guarded_open(path, *args, **kwargs):
            self.assertNotEqual(path, obj)
            self.assertNotIn(path.relative_to(self.tree).as_posix() if path.is_relative_to(self.tree) else "", excluded)
            return real_open(path, *args, **kwargs)

        with (
            mock.patch.object(build.os, "walk", side_effect=observed_walk),
            mock.patch.object(Path, "open", autospec=True, side_effect=guarded_open),
            mock.patch.object(build, "_capture_digest", wraps=build._capture_digest) as digest,
        ):
            for stage in ("before-configure", "before-build", "after-build"):
                manifest = self.snapshot(stage)
                self.assertEqual({entry["path"] for entry in manifest["outputs"]}, set(included))
                self.assertEqual([entry["path"] for entry in manifest["tlogs"]], [tlog])
                for entry in manifest["outputs"]:
                    self.assertEqual(entry["sha256"], hashlib.sha256(included[entry["path"]]).hexdigest())
                self.assertFalse(
                    [
                        path
                        for path in (self.leaf / stage).rglob("*")
                        if path.suffix.lower() in {".exe", ".dll", ".lib", ".obj"}
                    ]
                )
        counts = Counter(call.args[0].relative_to(self.tree).as_posix() for call in digest.call_args_list)
        self.assertEqual(counts, {relative: 3 for relative in (*included, tlog)})
        self.assertFalse(
            any(
                path == "vcpkg_installed"
                or path.startswith("vcpkg_installed/")
                or path == "third-party_deps"
                or path.startswith("third-party_deps/")
                for path in visited
            )
        )
        self.assertIn("app/windows-winui/nested/third-party_deps", visited)
        self.assertIn("tool/vcpkg_installed/generator", visited)

    def test_selected_compiler_and_wrapper_still_hash_actual_tools_each_time(self):
        compiler = self.put("vcpkg_installed/tools/cl.exe", b"compiler")
        wrapper = self.put("third-party_deps/tools/cl.exe", b"wrapper1")
        cache = {"entries": {"CMAKE_CXX_COMPILER": str(compiler)}}
        env = {"AOBUS_MSBUILD_CL_TOOL_EXE": str(wrapper)}
        with mock.patch.object(build, "_capture_digest", wraps=build._capture_digest) as digest:
            before = build._capture_build_metadata(env, cache)
            stat = wrapper.stat()
            wrapper.write_bytes(b"wrapper2")
            os.utime(wrapper, ns=(stat.st_atime_ns, stat.st_mtime_ns))
            after = build._capture_build_metadata(env, cache)
        self.assertEqual(before["compiler"]["sha256"], hashlib.sha256(b"compiler").hexdigest())
        self.assertEqual(after["compiler"], before["compiler"])
        self.assertEqual(before["wrapper"]["sha256"], hashlib.sha256(b"wrapper1").hexdigest())
        self.assertEqual(after["wrapper"]["sha256"], hashlib.sha256(b"wrapper2").hexdigest())
        self.assertEqual(Counter(call.args[0] for call in digest.call_args_list), {compiler: 2, wrapper: 2})


class MsbuildCaptureWinuiFlowTest(unittest.TestCase):
    """do_build flows with mocked native-Windows authorities."""

    def setUp(self):
        self.root = Path(tempfile.mkdtemp())
        self.leaf = self.root / "capture" / "first"
        self.leaf.parent.mkdir()
        self.tree = self.root / "tree"

    def tearDown(self):
        shutil.rmtree(self.root, ignore_errors=True)

    def args(self, capture=None, *, target="winui", tree=None):
        arguments = ["build", "--target", target, "-p", str(tree or self.tree)]
        if capture is not None:
            arguments += ["--msbuild-capture", capture]
        return make_parser().parse_args(arguments)

    def native_run(
        self, calls, *, configure_status=0, build_status=0, cache_text=None, tlog_after=b"after", binlog=True
    ):
        cache_text = (
            cache_text
            if cache_text is not None
            else (
                "CMAKE_BUILD_TYPE:STRING=Debug\n"
                "CMAKE_CXX_COMPILER:FILEPATH=C:/VS/VC/Tools/MSVC/cl.exe\n"
                "CMAKE_GENERATOR:INTERNAL=Visual Studio 18 2026\n"
                "CMAKE_GENERATOR_INSTANCE:INTERNAL=C:/Program Files/Microsoft Visual Studio/18/BuildTools\n"
                "CMAKE_GENERATOR_PLATFORM:INTERNAL=x64\n"
                "CMAKE_VS_PLATFORM_TOOLSET:INTERNAL=v145\n"
                "CMAKE_VS_PLATFORM_TOOLSET_VERSION:INTERNAL=14.50\n"
                "CMAKE_VS_WINDOWS_TARGET_PLATFORM_VERSION:STRING=10.0.26100.0\n"
                "SECRET_COMMAND_TOKEN:STRING=do-not-leak\n"
            )
        )

        def implementation(argv, *, cwd=PROJECT_ROOT, env=None, log=None, append=False):
            calls.append({"argv": list(argv), "env": env})
            if argv[1] == "-S":
                self.tree.mkdir(parents=True, exist_ok=True)
                (self.tree / "CMakeCache.txt").write_text(cache_text, encoding="utf-8")
                return configure_status
            if argv[1] == "--build":
                native = self.tree / "aobus-winui.dir" / "CL.read.1.tlog"
                native.parent.mkdir(parents=True, exist_ok=True)
                native.write_bytes(tlog_after)
                executable = self.tree / "app" / "Aobus.exe"
                executable.parent.mkdir(parents=True, exist_ok=True)
                executable.write_bytes(b"exe payload")
                if binlog and self.leaf.is_dir():
                    # The mocked native logger still produces its evidence file.
                    (self.leaf / "build.binlog").write_bytes(b"binlog events")
                return build_status
            raise AssertionError(f"unexpected native argv: {argv}")

        return implementation

    @contextlib.contextmanager
    def native_windows(
        self,
        run_implementation,
        *,
        msbuild_env=None,
        validate_destination=True,
        validate_tree=True,
        prepare_workspace=True,
    ):
        workspace = mock.Mock(source_dir=PROJECT_ROOT, compiler_build_dir=None, cmake_arguments=[])
        patches = [
            mock.patch.object(builddir, "platform_profile", return_value=builddir.WINDOWS_PROFILE),
            mock.patch.object(build.winui, "require_build_host"),
            mock.patch.object(
                build.compiler_cache,
                "msbuild_environment",
                return_value=dict(msbuild_env or {"AOBUS_MSBUILD_CACHE_TRACKING": "0"}),
            ),
            mock.patch.object(build, "run", side_effect=run_implementation),
        ]
        if validate_tree:
            patches.append(mock.patch.object(build, "validate_build_tree", return_value="msvc"))
        if prepare_workspace:
            patches.append(mock.patch.object(build, "_workspace_cache", return_value=workspace))
        if validate_destination:
            # Portable tests cannot honor Windows destination spelling; the
            # syntax validator has its own direct coverage above.
            patches.append(mock.patch.object(build, "_capture_syntax_error", return_value=None))
        patches.append(mock.patch.object(build, "_capture_drive_is_fixed", return_value=True))
        with contextlib.ExitStack() as stack:
            for patch in patches:
                stack.enter_context(patch)
            yield

    def manifest(self, stage):
        return json.loads((self.leaf / stage / "manifest.json").read_text(encoding="utf-8"))

    def assert_classified_and_nonreusable(self, *, stage, build_status="not-run", outcome="failed"):
        receipt = json.loads((self.leaf / "capture.json").read_text(encoding="utf-8"))
        self.assertEqual(receipt, {"stage": stage, "outcome": outcome, "build_status": build_status})
        contents = {path.relative_to(self.leaf): path.read_bytes() for path in self.leaf.rglob("*") if path.is_file()}
        with (
            mock.patch.object(build, "_capture_drive_is_fixed", return_value=True),
            contextlib.redirect_stderr(io.StringIO()) as stderr,
            self.assertRaises(SystemExit) as raised,
        ):
            build._prepare_capture(str(self.leaf), build_dir=self.tree, source_dir=PROJECT_ROOT)
        self.assertEqual(raised.exception.code, 1)
        self.assertIn("already exists", stderr.getvalue())
        self.assertEqual(
            contents,
            {path.relative_to(self.leaf): path.read_bytes() for path in self.leaf.rglob("*") if path.is_file()},
        )

    def test_real_file_as_build_tree_is_classified_before_native_work(self):
        self.tree.write_bytes(b"not a directory")
        calls = []
        with self.native_windows(self.native_run(calls)):
            with self.assertRaises(FileExistsError) as raised:
                build.do_build(self.args(str(self.leaf)), ["winui"])
        self.assertEqual(Path(raised.exception.filename), self.tree)
        self.assertEqual(calls, [])
        self.assert_classified_and_nonreusable(stage="setup")
        self.assertFalse(list(self.leaf.glob("*/manifest.json")))
        self.assertEqual(self.tree.read_bytes(), b"not a directory")

    def test_clean_permission_error_is_preserved_and_classified(self):
        self.tree.mkdir()
        args = self.args(str(self.leaf))
        args.clean = True
        denial = PermissionError("synthetic sharing denial")
        calls = []
        with (
            self.native_windows(self.native_run(calls)),
            mock.patch.object(build, "_remove_build_directory", side_effect=denial),
            self.assertRaises(PermissionError) as raised,
        ):
            build.do_build(args, ["winui"])
        self.assertIs(raised.exception, denial)
        self.assertEqual(calls, [])
        self.assert_classified_and_nonreusable(stage="setup")
        self.assertFalse((self.leaf / "before-build").exists())

    def test_actual_cached_compiler_validator_rejection_is_classified(self):
        self.tree.mkdir()
        (self.tree / "CMakeCache.txt").write_text(
            "CMAKE_CACHE_MAJOR_VERSION:INTERNAL=4\nCMAKE_CACHE_MINOR_VERSION:INTERNAL=3\nCMAKE_CACHE_PATCH_VERSION:INTERNAL=1\n",
            encoding="utf-8",
        )
        compiler_dir = self.tree / "CMakeFiles" / "4.3.1"
        compiler_dir.mkdir(parents=True)
        for language in ("C", "CXX"):
            (compiler_dir / f"CMake{language}Compiler.cmake").write_text(
                f'set(CMAKE_{language}_COMPILER_ID "GNU")\n', encoding="utf-8"
            )
        calls = []
        with (
            self.native_windows(self.native_run(calls), validate_tree=False),
            mock.patch.dict(build.os.environ, {build.compiler_cache.SHARED_WORKSPACES_EFFECTIVE: "0"}),
            contextlib.redirect_stderr(io.StringIO()) as stderr,
            self.assertRaises(SystemExit) as raised,
        ):
            build.do_build(self.args(str(self.leaf)), ["winui"])
        self.assertEqual(raised.exception.code, 1)
        self.assertIn("Compiler mismatch", stderr.getvalue())
        self.assertIn("configured C compiler is GNU", stderr.getvalue())
        self.assertEqual(calls, [])
        self.assert_classified_and_nonreusable(stage="setup")
        self.assertFalse((self.leaf / "before-build").exists())

    def test_actual_workspace_error_wrapper_is_preserved_and_classified(self):
        error = build.workspace_cache.WorkspaceCacheError("synthetic workspace failure")
        calls = []
        with (
            self.native_windows(self.native_run(calls), prepare_workspace=False),
            mock.patch.object(build.workspace_cache, "prepare", side_effect=error),
            contextlib.redirect_stderr(io.StringIO()) as stderr,
            self.assertRaises(SystemExit) as raised,
        ):
            build.do_build(self.args(str(self.leaf)), ["winui"])
        self.assertEqual(raised.exception.code, 1)
        self.assertIs(raised.exception.__cause__, error)
        self.assertIn("synthetic workspace failure", stderr.getvalue())
        self.assertEqual(calls, [])
        self.assert_classified_and_nonreusable(stage="setup")
        self.assertFalse((self.leaf / "before-build").exists())

    def test_arbitrary_native_exceptions_survive_secondary_snapshot_io_errors(self):
        for stage in ("configure", "build"):
            with self.subTest(stage=stage):
                self.leaf = self.leaf.parent / stage
                calls = []
                original = RuntimeError(f"synthetic {stage} exception")
                native = self.native_run(calls)
                real_snapshot = build._write_capture_snapshot

                def failing_run(argv, *, stage=stage, calls=calls, original=original, native=native, **kwargs):
                    if argv[1] == ("-S" if stage == "configure" else "--build"):
                        calls.append({"argv": list(argv)})
                        raise original
                    return native(argv, **kwargs)

                def failing_snapshot(capture_dir, snapshot_stage, build_tree, *, real_snapshot=real_snapshot, **kwargs):
                    if snapshot_stage == "after-build":
                        raise PermissionError("secondary snapshot denial")
                    return real_snapshot(capture_dir, snapshot_stage, build_tree, **kwargs)

                with (
                    self.native_windows(failing_run),
                    mock.patch.object(build, "_write_capture_snapshot", side_effect=failing_snapshot),
                    contextlib.redirect_stderr(io.StringIO()) as stderr,
                    self.assertRaises(RuntimeError) as raised,
                ):
                    build.do_build(self.args(str(self.leaf)), ["winui"])
                self.assertIs(raised.exception, original)
                self.assertEqual(len(calls), 1 if stage == "configure" else 2)
                self.assertIn("evidence is incomplete", stderr.getvalue())
                self.assert_classified_and_nonreusable(
                    stage=stage, build_status="not-run" if stage == "configure" else "failed"
                )
                self.assertEqual((self.leaf / "before-build").exists(), stage == "build")

    def test_secondary_receipt_write_failure_preserves_native_exit_and_initial_classification(self):
        original = SystemExit(37)
        real_receipt = build._write_capture_lifecycle
        receipts = []

        def failing_receipt(capture_dir, **kwargs):
            receipts.append(kwargs)
            if len(receipts) > 1:
                raise PermissionError("secondary receipt denial")
            return real_receipt(capture_dir, **kwargs)

        with (
            self.native_windows(mock.Mock(side_effect=original)),
            mock.patch.object(build, "_write_capture_lifecycle", side_effect=failing_receipt),
            contextlib.redirect_stderr(io.StringIO()) as stderr,
            self.assertRaises(SystemExit) as raised,
        ):
            build.do_build(self.args(str(self.leaf)), ["winui"])
        self.assertIs(raised.exception, original)
        self.assertEqual(raised.exception.code, 37)
        self.assertIn("evidence is incomplete", stderr.getvalue())
        self.assert_classified_and_nonreusable(stage="setup", outcome="aborted")

    def test_interruption_is_classified_as_aborted_without_replacing_it(self):
        original = KeyboardInterrupt()
        with (
            self.native_windows(mock.Mock(side_effect=original)),
            self.assertRaises(KeyboardInterrupt) as raised,
        ):
            build.do_build(self.args(str(self.leaf)), ["winui"])
        self.assertIs(raised.exception, original)
        self.assert_classified_and_nonreusable(stage="configure", outcome="aborted")
        self.assertFalse((self.leaf / "before-build").exists())

    def test_atomic_receipt_failure_keeps_prior_classification(self):
        self.leaf.mkdir()
        build._write_capture_lifecycle(self.leaf, stage="setup", outcome="aborted", build_status="not-run")
        before = (self.leaf / "capture.json").read_bytes()
        with (
            mock.patch.object(Path, "replace", side_effect=PermissionError("replace denied")),
            self.assertRaises(PermissionError),
        ):
            build._write_capture_lifecycle(self.leaf, stage="after-build", outcome="ok", build_status="ok")
        self.assertEqual((self.leaf / "capture.json").read_bytes(), before)

    def test_capture_off_and_on_differ_only_by_the_fixed_logger_tail(self):
        calls_off, calls_on = [], []
        with self.native_windows(self.native_run(calls_off)):
            build.do_build(self.args(), ["winui"])
        with self.native_windows(self.native_run(calls_on)):
            result = build.do_build(self.args(str(self.leaf)), ["winui"])

        self.assertEqual(calls_off[0]["argv"], calls_on[0]["argv"])
        self.assertEqual(calls_off[0]["env"], calls_on[0]["env"])
        self.assertEqual(calls_off[1]["env"], calls_on[1]["env"])
        tail = calls_on[1]["argv"][len(calls_off[1]["argv"]) :]
        self.assertEqual(tail, ["--", f"/bl:{self.leaf / 'build.binlog'};ProjectImports=None"])
        self.assertEqual(result.preset, builddir.WINDOWS_WINUI_PRESET)
        self.assert_classified_and_nonreusable(stage="after-build", outcome="ok", build_status="ok")

    def test_three_snapshots_capture_tlogs_cache_and_tool_identity(self):
        before_tlog = self.tree / "aobus-winui.dir" / "CL.read.1.tlog"
        before_tlog.parent.mkdir(parents=True)
        before_tlog.write_bytes(b"before")
        wrapper = self.root / "wrapper" / "cl.exe"
        wrapper.parent.mkdir()
        wrapper.write_bytes(b"wrapper payload")
        calls = []
        with self.native_windows(
            self.native_run(calls),
            msbuild_env={"AOBUS_MSBUILD_CACHE_TRACKING": "1", "AOBUS_MSBUILD_CL_TOOL_EXE": str(wrapper)},
        ):
            build.do_build(self.args(str(self.leaf)), ["winui"])

        stages = {stage: self.manifest(stage) for stage in ("before-configure", "before-build", "after-build")}
        # before-configure: the untouched tree, no cache yet
        self.assertEqual(
            [entry["path"] for entry in stages["before-configure"]["tlogs"]],
            ["aobus-winui.dir/CL.read.1.tlog"],
        )
        self.assertIsNone(stages["before-configure"]["cmake_cache"])
        self.assertIsNone(stages["before-configure"]["build_status"])
        copy = self.leaf / "before-configure" / "tlogs" / "aobus-winui.dir" / "CL.read.1.tlog"
        self.assertEqual(copy.read_bytes(), b"before")
        # before-build: same tlog identity, cache digest plus allowlisted entries only
        self.assertEqual(stages["before-build"]["tlogs"][0]["sha256"], hashlib.sha256(b"before").hexdigest())
        self.assertIsInstance(stages["before-build"]["tlogs"][0]["mtime_ns"], int)
        self.assertEqual(
            stages["before-build"]["cmake_cache"]["sha256"],
            hashlib.sha256((self.tree / "CMakeCache.txt").read_bytes()).hexdigest(),
        )
        self.assertEqual(
            stages["before-build"]["cmake_cache"]["entries"],
            {
                "CMAKE_BUILD_TYPE": "Debug",
                "CMAKE_CXX_COMPILER": "C:/VS/VC/Tools/MSVC/cl.exe",
                "CMAKE_GENERATOR": "Visual Studio 18 2026",
                "CMAKE_GENERATOR_INSTANCE": "C:/Program Files/Microsoft Visual Studio/18/BuildTools",
                "CMAKE_GENERATOR_PLATFORM": "x64",
                "CMAKE_VS_PLATFORM_TOOLSET": "v145",
                "CMAKE_VS_PLATFORM_TOOLSET_VERSION": "14.50",
                "CMAKE_VS_WINDOWS_TARGET_PLATFORM_VERSION": "10.0.26100.0",
            },
        )
        metadata = stages["before-build"]["build"]
        self.assertEqual(metadata["tracking_mode"], "1")
        self.assertEqual(metadata["wrapper"]["sha256"], hashlib.sha256(b"wrapper payload").hexdigest())
        self.assertEqual(metadata["compiler"]["path"], "C:/VS/VC/Tools/MSVC/cl.exe")
        self.assertEqual(self.leaf.joinpath("build.binlog").read_bytes(), b"binlog events")
        # after-build: rewritten tlog, shipping executable identity, ok status
        after = stages["after-build"]
        self.assertEqual(after["build_status"], "ok")
        self.assertTrue(after["complete"])
        self.assertEqual(after["build"]["tracking_mode"], "1")
        self.assertEqual(after["tlogs"][0]["sha256"], hashlib.sha256(b"after").hexdigest())
        outputs = {entry["path"]: entry for entry in after["outputs"]}
        self.assertEqual(outputs["app/Aobus.exe"]["sha256"], hashlib.sha256(b"exe payload").hexdigest())
        self.assertEqual(outputs["app/Aobus.exe"]["size"], len(b"exe payload"))
        # Raw evidence is private; unrelated cache values never enter any manifest.
        for stage in stages:
            self.assertNotIn("do-not-leak", (self.leaf / stage / "manifest.json").read_text(encoding="utf-8"))

    def test_before_configure_snapshot_of_a_missing_tree_is_valid_and_empty(self):
        calls = []
        with self.native_windows(self.native_run(calls)):
            build.do_build(self.args(str(self.leaf)), ["winui"])

        manifest = self.manifest("before-configure")
        self.assertEqual(manifest["tlogs"], [])
        self.assertEqual(manifest["outputs"], [])
        self.assertIsNone(manifest["cmake_cache"])
        self.assertTrue(manifest["complete"])

    def test_native_build_failure_keeps_its_error_and_records_the_failed_capture(self):
        calls = []
        with self.native_windows(self.native_run(calls, build_status=1)):
            with (
                contextlib.redirect_stderr(io.StringIO()) as stderr,
                self.assertRaises(SystemExit) as raised,
            ):
                build.do_build(self.args(str(self.leaf)), ["winui"])

        self.assertEqual(raised.exception.code, 1)
        self.assertIn("build failed.", stderr.getvalue())
        self.assertTrue((self.leaf / "before-build" / "manifest.json").is_file())
        self.assertEqual(self.manifest("after-build")["build_status"], "failed")
        self.assert_classified_and_nonreusable(stage="build", build_status="failed")

    def test_configure_failure_leaves_no_fabricated_build_stage(self):
        calls = []
        with self.native_windows(self.native_run(calls, configure_status=1)):
            with (
                contextlib.redirect_stderr(io.StringIO()) as stderr,
                self.assertRaises(SystemExit) as raised,
            ):
                build.do_build(self.args(str(self.leaf)), ["winui"])

        self.assertEqual(raised.exception.code, 1)
        self.assertIn("configure failed.", stderr.getvalue())
        self.assertFalse((self.leaf / "before-build").exists())
        self.assertEqual(self.manifest("after-build")["build_status"], "failed")
        self.assert_classified_and_nonreusable(stage="configure")

    def test_successful_capture_inside_a_caller_exception_handler_is_ok(self):
        calls = []
        with self.native_windows(self.native_run(calls)):
            try:
                raise RuntimeError("already handled by caller")
            except RuntimeError:
                result = build.do_build(self.args(str(self.leaf)), ["winui"])
        self.assertEqual(result.build_dir, self.tree)
        self.assertEqual(len(calls), 2)
        self.assert_classified_and_nonreusable(stage="after-build", outcome="ok", build_status="ok")

    def test_snapshot_failure_inside_a_caller_exception_handler_still_fails_capture(self):
        calls = []
        real_snapshot = build._write_capture_snapshot

        def failing(capture_dir, stage, build_tree, **kwargs):
            if stage == "after-build":
                raise OSError("evidence sink unavailable")
            return real_snapshot(capture_dir, stage, build_tree, **kwargs)

        with (
            self.native_windows(self.native_run(calls)),
            mock.patch.object(build, "_write_capture_snapshot", side_effect=failing),
            contextlib.redirect_stderr(io.StringIO()) as stderr,
            self.assertRaises(SystemExit) as raised,
        ):
            try:
                raise RuntimeError("already handled by caller")
            except RuntimeError:
                build.do_build(self.args(str(self.leaf)), ["winui"])
        self.assertEqual(raised.exception.code, 1)
        self.assertIn("capture is incomplete", stderr.getvalue())
        self.assertEqual(len(calls), 2)
        self.assert_classified_and_nonreusable(stage="after-build", build_status="ok")

    def test_snapshot_failure_after_a_successful_build_fails_the_capture(self):
        calls = []
        real_snapshot = build._write_capture_snapshot

        def failing(capture_dir, stage, build_tree, **kwargs):
            if stage == "after-build":
                raise OSError("evidence sink unavailable")
            return real_snapshot(capture_dir, stage, build_tree, **kwargs)

        with self.native_windows(self.native_run(calls)):
            with mock.patch.object(build, "_write_capture_snapshot", side_effect=failing):
                with (
                    contextlib.redirect_stderr(io.StringIO()) as stderr,
                    self.assertRaises(SystemExit) as raised,
                ):
                    build.do_build(self.args(str(self.leaf)), ["winui"])

        self.assertEqual(raised.exception.code, 1)
        self.assertIn("capture is incomplete", stderr.getvalue())
        self.assertEqual(len(calls), 2)  # native configure and build both completed
        self.assertTrue((self.leaf / "before-configure" / "manifest.json").is_file())
        self.assertFalse((self.leaf / "after-build").exists())
        self.assert_classified_and_nonreusable(stage="after-build", build_status="ok")

    def test_snapshot_failure_never_masks_a_native_failure(self):
        calls = []
        real_snapshot = build._write_capture_snapshot

        def failing(capture_dir, stage, build_tree, **kwargs):
            if stage == "after-build":
                raise OSError("evidence sink unavailable")
            return real_snapshot(capture_dir, stage, build_tree, **kwargs)

        with self.native_windows(self.native_run(calls, build_status=1)):
            with mock.patch.object(build, "_write_capture_snapshot", side_effect=failing):
                with (
                    contextlib.redirect_stderr(io.StringIO()) as stderr,
                    self.assertRaises(SystemExit) as raised,
                ):
                    build.do_build(self.args(str(self.leaf)), ["winui"])

        self.assertEqual(raised.exception.code, 1)
        self.assertIn("build failed.", stderr.getvalue())
        self.assertIn("evidence is incomplete", stderr.getvalue())
        self.assert_classified_and_nonreusable(stage="build", build_status="failed")

    def test_pre_build_snapshot_failure_stops_before_the_native_build(self):
        calls = []
        real_snapshot = build._write_capture_snapshot

        def failing(capture_dir, stage, build_tree, **kwargs):
            if stage == "before-build":
                raise OSError("cannot snapshot")
            return real_snapshot(capture_dir, stage, build_tree, **kwargs)

        with self.native_windows(self.native_run(calls)):
            with mock.patch.object(build, "_write_capture_snapshot", side_effect=failing):
                with (
                    contextlib.redirect_stderr(io.StringIO()) as stderr,
                    self.assertRaises(SystemExit) as raised,
                ):
                    build.do_build(self.args(str(self.leaf)), ["winui"])

        self.assertEqual(raised.exception.code, 1)
        self.assertIn("before-build capture snapshot", stderr.getvalue())
        self.assertEqual(len(calls), 1)  # the native build never ran
        self.assertEqual(self.manifest("after-build")["build_status"], "failed")
        self.assert_classified_and_nonreusable(stage="before-build")

    def test_missing_binlog_after_a_successful_build_fails_the_capture(self):
        calls = []
        with self.native_windows(self.native_run(calls, binlog=False)):
            with (
                contextlib.redirect_stderr(io.StringIO()) as stderr,
                self.assertRaises(SystemExit) as raised,
            ):
                build.do_build(self.args(str(self.leaf)), ["winui"])

        self.assertEqual(raised.exception.code, 1)
        self.assertIn("capture is incomplete", stderr.getvalue())
        self.assertIn("build.binlog", stderr.getvalue())
        self.assertEqual(len(calls), 2)  # the native configure and build both completed
        self.assertEqual(self.manifest("after-build")["build_status"], "failed")
        self.assert_classified_and_nonreusable(stage="after-build", build_status="ok")

    def test_missing_binlog_never_masks_a_native_failure(self):
        calls = []
        with self.native_windows(self.native_run(calls, build_status=1, binlog=False)):
            with (
                contextlib.redirect_stderr(io.StringIO()) as stderr,
                self.assertRaises(SystemExit) as raised,
            ):
                build.do_build(self.args(str(self.leaf)), ["winui"])

        self.assertEqual(raised.exception.code, 1)
        self.assertIn("build failed.", stderr.getvalue())
        self.assertNotIn("build.binlog", stderr.getvalue())
        self.assertEqual(self.manifest("after-build")["build_status"], "failed")

    def test_wrapper_identity_falls_back_to_the_inherited_environment(self):
        wrapper = self.root / "inherited" / "cl.exe"
        wrapper.parent.mkdir()
        wrapper.write_bytes(b"inherited wrapper")
        calls = []
        with (
            self.native_windows(self.native_run(calls)),
            mock.patch.dict(build.os.environ, {"AOBUS_MSBUILD_CL_TOOL_EXE": str(wrapper)}),
        ):
            with contextlib.redirect_stdout(io.StringIO()):
                build.do_build(self.args(str(self.leaf)), ["winui"])

        expected = hashlib.sha256(b"inherited wrapper").hexdigest()
        for stage in ("before-build", "after-build"):
            metadata = self.manifest(stage)["build"]
            self.assertEqual(metadata["wrapper"]["path"], str(wrapper))
            self.assertEqual(metadata["wrapper"]["sha256"], expected)

    def test_missing_compiler_entry_records_the_missing_boundary(self):
        cache_text = "CMAKE_GENERATOR:INTERNAL=Visual Studio 18 2026\n"
        calls = []
        with self.native_windows(self.native_run(calls, cache_text=cache_text)):
            with contextlib.redirect_stdout(io.StringIO()):
                build.do_build(self.args(str(self.leaf)), ["winui"])

        manifest = self.manifest("before-build")
        self.assertEqual(manifest["cmake_cache"]["entries"], {"CMAKE_GENERATOR": "Visual Studio 18 2026"})
        self.assertIsNone(manifest["build"]["compiler"])

    def test_denied_subdirectory_raises_instead_of_completing_the_snapshot(self):
        self.leaf.mkdir()
        self.tree.mkdir()
        denial = PermissionError("subdir denied")

        def denying(top, **kwargs):
            kwargs["onerror"](denial)

        with mock.patch.object(build.os, "walk", side_effect=denying):
            with self.assertRaises(PermissionError) as raised:
                build._write_capture_snapshot(self.leaf, "before-build", self.tree)

        self.assertIs(raised.exception, denial)
        self.assertFalse((self.leaf / "before-build" / "manifest.json").exists())

    def test_walk_denial_before_configure_fails_the_capture_before_native_work(self):
        calls = []

        def denying(top, **kwargs):
            kwargs["onerror"](PermissionError("subdir denied"))

        with self.native_windows(self.native_run(calls)):
            with mock.patch.object(build.os, "walk", side_effect=denying):
                with (
                    contextlib.redirect_stderr(io.StringIO()) as stderr,
                    self.assertRaises(SystemExit) as raised,
                ):
                    build.do_build(self.args(str(self.leaf)), ["winui"])

        self.assertEqual(raised.exception.code, 1)
        self.assertIn("cannot write the before-configure capture snapshot", stderr.getvalue())
        self.assertEqual(calls, [])  # the native configure never ran
        self.assertFalse(list(self.leaf.glob("*/manifest.json")))
        self.assert_classified_and_nonreusable(stage="before-configure")

    def test_walk_denial_after_a_native_failure_preserves_the_original_error(self):
        calls = []
        real_walk = build.os.walk
        visited = {"walks": 0}

        def denying(top, **kwargs):
            visited["walks"] += 1
            if visited["walks"] >= 3:  # before-configure and before-build succeeded already
                kwargs["onerror"](PermissionError("subdir denied"))
            return real_walk(top, **kwargs)

        with self.native_windows(self.native_run(calls, build_status=1)):
            with mock.patch.object(build.os, "walk", side_effect=denying):
                with (
                    contextlib.redirect_stderr(io.StringIO()) as stderr,
                    self.assertRaises(SystemExit) as raised,
                ):
                    build.do_build(self.args(str(self.leaf)), ["winui"])

        self.assertEqual(raised.exception.code, 1)
        self.assertIn("build failed.", stderr.getvalue())
        self.assertIn("evidence is incomplete", stderr.getvalue())
        self.assertFalse((self.leaf / "after-build" / "manifest.json").exists())

    def test_capture_requires_a_winui_target(self):
        calls = []
        with self.native_windows(self.native_run(calls)):
            with (
                contextlib.redirect_stderr(io.StringIO()) as stderr,
                self.assertRaises(SystemExit) as raised,
            ):
                build.do_build(self.args(str(self.leaf), target="aobus-tui"), ["aobus-tui"])

        self.assertEqual(raised.exception.code, 1)
        self.assertIn("--msbuild-capture is available only", stderr.getvalue())
        self.assertEqual(calls, [])
        self.assertFalse(self.leaf.exists())

    def test_capture_rejects_non_windows_profiles_before_any_run(self):
        calls = []
        with mock.patch.object(builddir, "platform_profile", return_value=builddir.LINUX_PROFILE):
            with (
                contextlib.redirect_stderr(io.StringIO()) as stderr,
                self.assertRaises(SystemExit) as raised,
            ):
                build.do_build(self.args(str(self.leaf), target="aobus-tui"), ["aobus-tui"])

        self.assertEqual(raised.exception.code, 1)
        self.assertIn("--msbuild-capture is available only", stderr.getvalue())
        self.assertEqual(calls, [])
        self.assertFalse(self.leaf.exists())

    def test_invalid_destination_syntax_fails_before_any_mutation(self):
        calls = []
        with self.native_windows(self.native_run(calls), validate_destination=False):
            with (
                contextlib.redirect_stderr(io.StringIO()) as stderr,
                self.assertRaises(SystemExit) as raised,
            ):
                build.do_build(self.args("cap\\first"), ["winui"])

        self.assertEqual(raised.exception.code, 1)
        self.assertIn("fixed local drive", stderr.getvalue())
        self.assertEqual(calls, [])
        self.assertFalse(self.leaf.exists())

    def test_capture_prints_one_static_privacy_warning(self):
        calls = []
        with self.native_windows(self.native_run(calls)):
            with contextlib.redirect_stdout(io.StringIO()) as stdout:
                build.do_build(self.args(str(self.leaf)), ["winui"])

        lines = [line for line in stdout.getvalue().splitlines() if "sensitive data" in line]
        self.assertEqual(lines, [build._CAPTURE_WARNING])

    def test_phase_records_carry_fixed_dimensions_and_no_capture_data(self):
        wrapper = self.root / "wrapper" / "cl.exe"
        wrapper.parent.mkdir()
        wrapper.write_bytes(b"wrapper payload")
        phase_dir = self.root / "phases"
        calls = []
        environment = {
            "AOBUS_PHASE_LOG_DIR": str(phase_dir),
            "CMAKE_BUILD_PARALLEL_LEVEL": "4",
        }
        with (
            self.native_windows(
                self.native_run(calls),
                msbuild_env={"AOBUS_MSBUILD_CACHE_TRACKING": "1", "AOBUS_MSBUILD_CL_TOOL_EXE": str(wrapper)},
            ),
            mock.patch.dict(build.os.environ, environment),
        ):
            with contextlib.redirect_stdout(io.StringIO()):
                build.do_build(self.args(str(self.leaf)), ["winui"])

        rows = []
        for path in sorted(phase_dir.glob("*.jsonl")):
            rows.extend(json.loads(line) for line in path.read_text(encoding="utf-8").splitlines())
        self.assertTrue(rows)
        begins = {row["phase"]: row for row in rows if row["kind"] == "BEGIN"}
        self.assertEqual(begins["configure"]["tree"], "winui")
        self.assertEqual(begins["build"]["tree"], "winui")
        self.assertEqual(begins["build"]["jobs"], 4)
        for row in rows:
            self.assertLessEqual(set(row), PHASE_ROW_FIELDS)
        serialized = "\n".join(json.dumps(row, sort_keys=True) for row in rows)
        for secret in (str(self.leaf), str(self.tree), str(wrapper), "/bl:", "msbuild-capture", "do-not-leak"):
            self.assertNotIn(secret, serialized)

    def test_phase_logging_is_inert_without_the_environment_variable(self):
        calls = []
        phase_dir = self.root / "phases"
        with (
            self.native_windows(self.native_run(calls)),
            mock.patch.dict(build.os.environ, {}, clear=True),
        ):
            with contextlib.redirect_stdout(io.StringIO()):
                build.do_build(self.args(), ["winui"])

        self.assertFalse(phase_dir.exists())


if __name__ == "__main__":
    unittest.main()
