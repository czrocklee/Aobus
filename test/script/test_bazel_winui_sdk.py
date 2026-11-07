"""Behavioral tests for the lazy WinUI NuGet SDK Bazel repository projection.

MSBuild is faked at the subprocess boundary and must discriminate the selected
executable, query, and imported project; no test claims native Windows build
behavior. NuGet archives are real minimal zips so the production
dependency_policy verifier and policy checks stay real. Library fixtures are
real minimal AMD64 COFF archives so the shared production classifier keeps
assigning the actual static/interface import roles; other payload bytes are
dummies that assert admission and projection only, and the Foundation bootstrap
runtime is asserted structurally from its locked provider. Projection stays
selective: queried include subtrees, package build metadata, the selected
generation tool roots, and the Full-CLR MSBuild host, selected VC targets, and
SDK metadata/MIDL families, never the whole installation.

The admitted native facts are pipelined through the actual checked-in Starlark
materialization and BUILD loader: repository_ctx is faked at its I/O boundary
and rule declarations are captured, so the tests observe the real projection
mechanism instead of a Python-rendered copy of it.
"""

import json
import os
import tempfile
import unittest
import xml.etree.ElementTree as ET
import zipfile
from pathlib import Path
from types import SimpleNamespace
from unittest import mock

from ao.core import bazel_native as native
from ao.core import bazel_provision, bazel_winui, dependency_policy

_BAZEL = Path(__file__).resolve().parents[2] / "bazel"


class _StarlarkFailure(Exception):
    """Sentinel for the checked-in Starlark helpers' fail() calls."""


def _materialize_winui(facts, subpackage="debug"):
    """Run the checked-in Starlark projection with repository I/O faked.

    The generated BUILD is executed under a load bridge so the actual
    winui_sdk_imports macro declares the public roles; artifact links and rule
    declarations are captured, never written to disk. The returned observer
    (files, links, rules, exports, targets) is owned by these tests; production
    carries only the admitted facts across this boundary.
    """
    links = {}
    files = {}
    rules = {}
    exports = []

    def fail(message):
        raise _StarlarkFailure(message)

    def symlink(target, destination):
        if destination in links:
            raise AssertionError("duplicate artifact link: " + destination)
        links[destination] = str(target)

    def write(name, content, *, executable=False):
        if executable:
            raise AssertionError("WinUI projection must not write executable programs")
        files[name] = content

    context = SimpleNamespace(path=lambda source: source, symlink=symlink, file=write)
    namespace = {"json": SimpleNamespace(encode=json.dumps), "fail": fail}
    source = (_BAZEL / "winui_repository.bzl").read_text(encoding="utf-8")
    exec(compile(source, "winui_repository.bzl", "exec"), namespace)
    targets = namespace["materialize_winui"](context, subpackage, facts)

    def rule(kind):
        def declare(**kwargs):
            if kwargs["name"] in rules:
                raise AssertionError("duplicate generated rule: " + kwargs["name"])
            rules[kwargs["name"]] = (kind, kwargs)

        return declare

    prefix = subpackage + "/"
    loader = {
        "fail": fail,
        "package": lambda **kwargs: None,
        "native": SimpleNamespace(exports_files=exports.extend, filegroup=rule("filegroup")),
    }

    def load(label, *symbols):
        if label == "@rules_cc//cc:defs.bzl":
            loader["cc_import"] = rule("cc_import")
            loader["cc_library"] = rule("cc_library")
            return
        if label == "@aobus//bazel:winui_sdk.bzl":
            loaded = (_BAZEL / "winui_sdk.bzl").read_text(encoding="utf-8")
        elif label == ":sdk_data.bzl":
            loaded = files[prefix + "sdk_data.bzl"]
        else:
            raise AssertionError("unexpected BUILD dependency: " + label)
        exec(compile(loaded, label, "exec"), loader)
        for symbol in symbols:
            if symbol not in loader:
                raise AssertionError("missing loaded symbol: " + symbol)

    loader["load"] = load
    exec(compile(files[prefix + "BUILD.bazel"], "BUILD.bazel", "exec"), loader)
    return {"files": files, "links": links, "rules": rules, "exports": exports, "targets": targets}


SDK = bazel_winui.WINDOWS_SDK_VERSION
VC_VERSION = "14.44.35207"
TOOLSET = "v143"
APPSDK_ID = "Microsoft.WindowsAppSDK"
APPSDK_VERSION = "1.6.250205002"
CPPWINRT_ID = "Microsoft.Windows.CppWinRT"
CPPWINRT_VERSION = "2.0.240113.1"
BUILDTOOLS_ID = "Microsoft.Windows.SDK.BuildTools"
BUILDTOOLS_VERSION = "10.0.26100.1742"
FOUNDATION_ID = "Microsoft.WindowsAppSDK.Foundation"
FOUNDATION_VERSION = "1.6.250205002"
FOUNDATION_BOOTSTRAP = "Microsoft.WindowsAppRuntime.Bootstrap.dll"
LOCKED = (
    (APPSDK_ID, APPSDK_VERSION),
    (CPPWINRT_ID, CPPWINRT_VERSION),
    (BUILDTOOLS_ID, BUILDTOOLS_VERSION),
    (FOUNDATION_ID, FOUNDATION_VERSION),
)


def _touch(path, content="fixture\n"):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(content, encoding="utf-8")
    return path


def _write_lock(path, entries):
    lines = ["<?xml version='1.0' encoding='utf-8'?>", "<packages>"]
    lines += [f'  <package id="{name}" version="{version}" />' for name, version in entries]
    lines.append("</packages>")
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text("\n".join(lines) + "\n", encoding="utf-8")


def _write_nupkg(directory, package_id, version, *, nuspec_id=None, nuspec_version=None):
    """Create one restored archive whose nuspec declares the given identity."""
    nuspec = (
        '<?xml version="1.0" encoding="utf-8"?>'
        '<package xmlns="urn:microsoft:nuspec:2013">'
        f"<metadata><id>{nuspec_id or package_id}</id>"
        f"<version>{nuspec_version or version}</version></metadata>"
        "</package>"
    )
    directory.mkdir(parents=True, exist_ok=True)
    archive = directory / f"{package_id}.nupkg"
    with zipfile.ZipFile(archive, "w") as bundle:
        bundle.writestr(f"{package_id}.nuspec", nuspec)
    return archive


def _archive_member(name: bytes, payload: bytes) -> bytes:
    """One well-formed COFF archive member; payload bytes are not a linkable image."""
    if len(payload) % 2:
        payload += b"\n"
    return (
        name.ljust(16, b" ")
        + b"0".ljust(12, b" ")
        + b"0".ljust(6, b" ")
        + b"0".ljust(6, b" ")
        + b"644".ljust(8, b" ")
        + str(len(payload)).encode("ascii").ljust(10, b" ")
        + b"`\n"
        + payload
    )


def _fixture_archive(*members: bytes) -> bytes:
    return b"!<arch>\n" + b"".join(members)


def _coff_object(section: bytes, payload: bytes) -> bytes:
    """A minimal real-shaped AMD64 COFF object: header, one named section, data."""
    return (
        (0x8664).to_bytes(2, "little")  # AMD64 machine
        + (1).to_bytes(2, "little")  # one section
        + (0).to_bytes(4, "little")  # timestamp
        + (0).to_bytes(4, "little")  # symbol table
        + (0).to_bytes(4, "little")  # symbol count
        + (0).to_bytes(2, "little")  # no optional header
        + (0).to_bytes(2, "little")  # characteristics
        + section.ljust(8, b"\x00")
        + (0).to_bytes(4, "little")  # virtual size
        + (0).to_bytes(4, "little")  # virtual address
        + len(payload).to_bytes(4, "little")  # raw data size
        + (60).to_bytes(4, "little")  # raw data pointer, after the section table
        + (0).to_bytes(4, "little")  # relocations
        + (0).to_bytes(4, "little")  # line numbers
        + (0).to_bytes(2, "little")
        + (0).to_bytes(2, "little")
        + (0x60000020).to_bytes(4, "little")  # executable code section
        + payload
    )


def _short_import_object() -> bytes:
    """A real-shaped COFF short import object (Version 0): a DLL import member."""
    return (
        (0).to_bytes(2, "little")  # anonymous object signature
        + (0xFFFF).to_bytes(2, "little")
        + (0).to_bytes(2, "little")  # Version: short import object
        + (0x8664).to_bytes(2, "little")  # AMD64 machine
        + (0).to_bytes(4, "little")  # timestamp
        + (12).to_bytes(4, "little")  # name size
        + (0).to_bytes(2, "little")  # hint/ordinal
        + (0).to_bytes(2, "little")  # import type: code
        + b"FIXTURE.dll\x00"
    )


STATIC_ARCHIVE = _fixture_archive(
    _archive_member(b"fixture.obj/", _coff_object(b".text", b"\x48\x89\x5c\x24\x08\x48\x8b\x05\xc3"))
)
IMPORT_ARCHIVE = _fixture_archive(_archive_member(b"fixture.obj/", _short_import_object()))


def _representative_contract():
    """Minimal valid contract covering the two governed NuGet dependencies."""
    return {
        "schemaVersion": 2,
        "leadResolver": "nix",
        "dependencies": {
            "winappsdk": {
                "platforms": ["windows"],
                "policy": {"kind": "exact", "version": APPSDK_VERSION},
                "nuget": {"package": APPSDK_ID, "lock": "app/windows-winui/packages.config"},
                "msbuild": {},
            },
            "cppwinrt": {
                "platforms": ["windows"],
                "policy": {"kind": "exact", "version": CPPWINRT_VERSION},
                "nuget": {"package": CPPWINRT_ID, "lock": "app/windows-winui/packages.config"},
                "msbuild": {},
            },
        },
        "exceptions": [],
    }


class LazyNonWindowsStubTest(unittest.TestCase):
    def test_stub_requires_no_msbuild_sdk_or_lock_inspection(self):
        with (
            mock.patch.object(dependency_policy, "read_json", side_effect=AssertionError("unexpected policy I/O")),
            mock.patch.object(bazel_winui.subprocess, "run", side_effect=AssertionError("msbuild")),
        ):
            with tempfile.TemporaryDirectory(prefix="aobus-winui-stub-") as temporary:
                tree = Path(temporary) / "tree"
                self.assertIsNone(bazel_winui.resolve_sdk(tree, {"host": "linux"}, {}))
                self.assertIsNone(bazel_winui.resolve_sdk(tree, {"host": "macos"}, {}))
                self.assertFalse(tree.exists())

    def test_stub_materializes_empty_loadable_interfaces_and_metadata(self):
        result = _materialize_winui(None)
        self.assertEqual(result["links"], {})
        self.assertEqual(json.loads(result["files"]["debug/sdk.json"]), {})
        self.assertEqual(json.loads(result["files"]["debug/sdk-diagnostics.json"]), {})
        # The stub keeps its export roles declared and its interfaces empty.
        self.assertEqual(result["exports"], ["sdk.json", "sdk-diagnostics.json"])
        self.assertEqual(
            {name: kind for name, (kind, _) in result["rules"].items()},
            {"headers": "cc_library", "bootstrap": "cc_library", "runtime": "filegroup", "tools": "filegroup"},
        )
        _, headers = result["rules"]["headers"]
        self.assertEqual((headers["hdrs"], headers["includes"], headers["defines"]), ([], [], []))
        _, bootstrap = result["rules"]["bootstrap"]
        self.assertEqual((bootstrap["srcs"], bootstrap["deps"], bootstrap["linkopts"]), ([], [], []))
        self.assertIs(bootstrap["alwayslink"], False)
        for role in ("runtime", "tools"):
            self.assertEqual(result["rules"][role][1]["srcs"], [])

    def test_release_subpackage_prefixes_artifacts_without_changing_interfaces(self):
        result = _materialize_winui(None, subpackage="release")
        self.assertEqual(
            set(result["files"]),
            {"release/BUILD.bazel", "release/sdk.json", "release/sdk-diagnostics.json", "release/sdk_data.bzl"},
        )
        self.assertEqual(result["links"], {})
        self.assertEqual(json.loads(result["files"]["release/sdk.json"]), {})
        # The public roles and declarations stay identical; only the artifact
        # locations move under the configured subpackage.
        self.assertEqual(
            result["targets"], ["bootstrap", "headers", "runtime", "sdk-diagnostics.json", "sdk.json", "tools"]
        )
        self.assertEqual(result["exports"], ["sdk.json", "sdk-diagnostics.json"])
        self.assertEqual(
            {name: kind for name, (kind, _) in result["rules"].items()},
            {"headers": "cc_library", "bootstrap": "cc_library", "runtime": "filegroup", "tools": "filegroup"},
        )


class WinuiStarlarkProjectionTest(unittest.TestCase):
    """Fail-closed record validation in the checked-in Starlark materialization."""

    @staticmethod
    def facts(files=(), includes=()):
        """Minimal admitted-facts shape; only the probed records vary."""
        return {
            "sdk_files": list(files),
            "includes": list(includes),
            "defines": [],
            "headers": [],
            "sources": [],
            "libraries": [],
            "system_libraries": [],
            "runtime": [],
            "tools": [],
            "native_inputs": [],
            "execution": {},
            "diagnostics": {},
        }

    def test_projection_fails_closed_on_unsupported_subpackages(self):
        for subpackage in ("", "profile", "fastbuild", "Debug"):
            with (
                self.subTest(subpackage=subpackage),
                self.assertRaisesRegex(_StarlarkFailure, "unsupported WinUI SDK subpackage: " + subpackage),
            ):
                _materialize_winui(self.facts(), subpackage=subpackage)

    def test_root_sdk_projection_fails_before_artifact_writes(self):
        context = SimpleNamespace(file=mock.Mock(), symlink=mock.Mock())
        namespace = {"fail": self.fail}
        source = (_BAZEL / "winui_repository.bzl").read_text(encoding="utf-8")
        exec(compile(source, "winui_repository.bzl", "exec"), namespace)
        with self.assertRaisesRegex(AssertionError, "unsupported WinUI SDK subpackage:"):
            namespace["materialize_winui"](context, "", self.facts())
        context.file.assert_not_called()
        context.symlink.assert_not_called()

    def test_projection_fails_closed_on_unsafe_provider_records_and_collisions(self):
        record = {"source": "/provider/Pkg/a.h", "package": "Pkg", "path": "a.h"}
        for name, probed, message in (
            ("provider traversal", self.facts(files=[dict(record, package="../Escape")]), "provider name"),
            ("provider separator", self.facts(files=[dict(record, package="Pkg/Evil")]), "provider name"),
            ("path traversal", self.facts(files=[dict(record, path="../escape.h")]), "provider-relative path"),
            ("absolute path", self.facts(files=[dict(record, path="/abs/a.h")]), "provider-relative path"),
            (
                "destination collision",
                self.facts(files=[record, {"source": "/provider/Other/a.h", "package": "Pkg", "path": "a.h"}]),
                "projection collision",
            ),
            (
                "include traversal",
                self.facts(includes=[{"package": "Pkg", "path": "../escape"}]),
                "provider-relative path",
            ),
        ):
            with self.subTest(name=name), self.assertRaisesRegex(_StarlarkFailure, message):
                _materialize_winui(probed)


class LockedPackagesTest(unittest.TestCase):
    """The lock gate reuses the dependency-policy verifier and current policy."""

    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix="aobus-winui-lock-")
        self.addCleanup(temporary.cleanup)
        self.tmp = Path(temporary.name).resolve()
        self.project_root = self.tmp / "repo"
        self.lock = self.project_root / "app/windows-winui/packages.config"
        self.packages_root = self.tmp / "packages"
        self.contract = _representative_contract()
        self.enterContext(mock.patch.object(bazel_winui, "PROJECT_ROOT", self.project_root))
        read_json = dependency_policy.read_json
        self.enterContext(
            mock.patch.object(
                dependency_policy,
                "read_json",
                side_effect=lambda path: self.contract if path == dependency_policy.CONTRACT_FILE else read_json(path),
            )
        )

    def restore(self, entries, *, identities=None):
        _write_lock(self.lock, entries)
        for name, version in entries:
            nuspec_id, nuspec_version = (identities or {}).get(name, (None, None))
            _write_nupkg(
                self.packages_root / f"{name}.{version}",
                name,
                version,
                nuspec_id=nuspec_id,
                nuspec_version=nuspec_version,
            )
        self.packages_root.mkdir(parents=True, exist_ok=True)

    def locked(self):
        return bazel_winui._locked_packages(self.packages_root.resolve())

    def test_lock_admits_restored_packages_with_normalized_and_casefold_identity(self):
        # CppWinRT's nuspec spells its id in lower case and pads the version with
        # a trailing zero component; both are accepted by the shared verifier, and
        # extra locked entries (BuildTools, Foundation) stay admitted providers.
        self.restore(LOCKED, identities={CPPWINRT_ID: (CPPWINRT_ID.lower(), "2.0.240113.1.0")})
        self.assertEqual(
            self.locked(),
            [
                (APPSDK_ID, APPSDK_VERSION, (self.packages_root / f"{APPSDK_ID}.{APPSDK_VERSION}").resolve()),
                (CPPWINRT_ID, CPPWINRT_VERSION, (self.packages_root / f"{CPPWINRT_ID}.{CPPWINRT_VERSION}").resolve()),
                (
                    BUILDTOOLS_ID,
                    BUILDTOOLS_VERSION,
                    (self.packages_root / f"{BUILDTOOLS_ID}.{BUILDTOOLS_VERSION}").resolve(),
                ),
                (
                    FOUNDATION_ID,
                    FOUNDATION_VERSION,
                    (self.packages_root / f"{FOUNDATION_ID}.{FOUNDATION_VERSION}").resolve(),
                ),
            ],
        )

    def test_lock_rejects_packages_missing_from_the_governed_lock(self):
        self.restore(LOCKED[:1] + LOCKED[2:])
        with self.assertRaisesRegex(
            bazel_winui.BazelWorkspaceError, "does not satisfy the existing cppwinrt dependency policy"
        ):
            self.locked()

    def test_lock_rejects_versions_outside_the_current_contract_policy(self):
        self.restore(LOCKED)
        self.contract["dependencies"]["winappsdk"]["policy"]["version"] = "9.9.9"
        with self.assertRaisesRegex(
            bazel_winui.BazelWorkspaceError, "does not satisfy the existing winappsdk dependency policy"
        ):
            self.locked()

    def test_lock_rejects_foreign_restored_identity(self):
        for nuspec_id, nuspec_version in ((CPPWINRT_ID + ".Offline", None), (None, "3.0.0.0")):
            with self.subTest(identity=(nuspec_id, nuspec_version)):
                self.restore(
                    LOCKED, identities={CPPWINRT_ID: (nuspec_id or CPPWINRT_ID, nuspec_version or CPPWINRT_VERSION)}
                )
                with self.assertRaisesRegex(
                    (dependency_policy.DependencyPolicyError, bazel_winui.BazelWorkspaceError),
                    "restored NuGet identity does not match",
                ):
                    self.locked()

    def test_lock_rejects_packages_without_exactly_one_archive(self):
        self.restore(LOCKED)
        (self.packages_root / f"{CPPWINRT_ID}.{CPPWINRT_VERSION}" / f"{CPPWINRT_ID}.nupkg").unlink()
        with self.assertRaisesRegex(
            (dependency_policy.DependencyPolicyError, bazel_winui.BazelWorkspaceError), "lacks one package archive"
        ):
            self.locked()

    def test_lock_rejects_invalid_package_identities(self):
        self.restore((*LOCKED, ("../Escape", "1.0.0")))
        with self.assertRaisesRegex(bazel_winui.BazelWorkspaceError, "invalid WinUI NuGet package identity"):
            self.locked()

    def test_empty_lock_is_rejected(self):
        self.restore(())
        with self.assertRaisesRegex(bazel_winui.BazelWorkspaceError, "WinUI NuGet lock is empty"):
            self.locked()


class ImportOrderTest(unittest.TestCase):
    def test_props_import_forward_and_targets_reversed_with_native_preference(self):
        with tempfile.TemporaryDirectory(prefix="aobus-winui-imports-") as temporary:
            root = Path(temporary).resolve()
            native_props = _touch(root / "A/build/native/A.props")
            _touch(root / "A/build/A.props")  # generic fallback loses to the native import
            native_targets = _touch(root / "A/build/native/A.targets")
            generic_props = _touch(root / "B/build/B.props")
            generic_targets = _touch(root / "B/build/B.targets")
            (root / "C").mkdir()
            packages = [("A", "1", root / "A"), ("B", "2", root / "B"), ("C", "3", root / "C")]
            self.assertEqual(bazel_winui._imports(packages, "props"), [native_props, generic_props])
            self.assertEqual(bazel_winui._imports(packages, "targets"), [generic_targets, native_targets])


class NativeToolsTest(unittest.TestCase):
    def fixture(self, root, *, install_dir=None, version=VC_VERSION, defaults=(VC_VERSION,), sdk_version=SDK):
        visual_studio = root / "visual-studio"
        msbuild = _touch(visual_studio / "MSBuild/Current/Bin/amd64/MSBuild.exe")
        auxiliary = visual_studio / "VC/Auxiliary/Build"
        auxiliary.mkdir(parents=True, exist_ok=True)
        for index, content in enumerate(defaults):
            (auxiliary / f"Microsoft.VCToolsVersion.v{143 + index}.default.txt").write_text(
                content, encoding="utf-8-sig"
            )
        return (
            {
                "VCToolsInstallDir": str(
                    visual_studio / "VC/Tools/MSVC" / version if install_dir is None else install_dir
                ),
                "VCToolsVersion": version,
                "WindowsSdkDir": str(root / "windows-sdk"),
                "WindowsSDKVersion": sdk_version,
            },
            msbuild,
            visual_studio,
        )

    def test_selected_msbuild_is_bound_to_the_installed_platform_toolset(self):
        with tempfile.TemporaryDirectory(prefix="aobus-winui-tools-") as temporary:
            root = Path(temporary).resolve()
            compiler, msbuild, visual_studio = self.fixture(root)
            self.assertEqual(bazel_winui._native_tools(compiler), (msbuild, TOOLSET, visual_studio))

    def test_unselected_or_relative_vc_layout_fails_closed(self):
        with tempfile.TemporaryDirectory(prefix="aobus-winui-tools-") as temporary:
            root = Path(temporary).resolve()
            for install_dir in (str(root / "toolchain" / VC_VERSION), "VC/Tools/MSVC/" + VC_VERSION):
                with self.subTest(install_dir=install_dir):
                    compiler, _, _ = self.fixture(root, install_dir=install_dir)
                    with self.assertRaisesRegex(
                        bazel_winui.BazelWorkspaceError,
                        "requires the selected VCToolsInstallDir/VCToolsVersion",
                    ):
                        bazel_winui._native_tools(compiler)

    def test_mismatched_vc_version_name_fails_closed(self):
        with tempfile.TemporaryDirectory(prefix="aobus-winui-tools-") as temporary:
            root = Path(temporary).resolve()
            compiler, _, _ = self.fixture(root)
            # Only the version field mismatches; the install directory keeps the
            # selected layout so the version-name check is what rejects the pair.
            compiler["VCToolsVersion"] = "14.44.99999"
            with self.assertRaisesRegex(
                bazel_winui.BazelWorkspaceError, "requires the selected VCToolsInstallDir/VCToolsVersion"
            ):
                bazel_winui._native_tools(compiler)

    def test_platform_toolset_binding_must_be_unique(self):
        for defaults in ((), (VC_VERSION, VC_VERSION)):
            with self.subTest(defaults=defaults), tempfile.TemporaryDirectory(prefix="aobus-winui-tools-") as temporary:
                root = Path(temporary).resolve()
                compiler, _, _ = self.fixture(root, defaults=defaults)
                with self.assertRaisesRegex(bazel_winui.BazelWorkspaceError, "cannot uniquely bind"):
                    bazel_winui._native_tools(compiler)

    def test_windows_sdk_selection_is_governed(self):
        with tempfile.TemporaryDirectory(prefix="aobus-winui-tools-") as temporary:
            root = Path(temporary).resolve()
            compiler, _, _ = self.fixture(root, sdk_version="10.0.19041.0")
            with self.assertRaisesRegex(bazel_winui.BazelWorkspaceError, "governed Windows SDK selection"):
                bazel_winui._native_tools(compiler)


class ProjectionBoundaryTest(unittest.TestCase):
    def test_owned_file_must_exist_inside_its_root(self):
        with tempfile.TemporaryDirectory(prefix="aobus-winui-owned-") as temporary:
            root = Path(temporary).resolve()
            package = root / "Pkg.1"
            inside = _touch(package / "include/a.h")
            self.assertEqual(bazel_winui._owned(inside, package), inside)
            with self.assertRaisesRegex(bazel_winui.BazelWorkspaceError, "missing or escapes"):
                bazel_winui._owned(package / "include/missing.h", package)

    @unittest.skipIf(os.name == "nt", "No Windows symlink privilege is required or requested")
    def test_owned_file_rejects_a_symlink_outside_its_selected_provider(self):
        with tempfile.TemporaryDirectory(prefix="aobus-winui-owned-link-") as temporary:
            root = Path(temporary).resolve()
            package = root / "Pkg.1"
            (package / "include").mkdir(parents=True)
            outside = _touch(root / "outside/b.h")
            (package / "include/alias.h").symlink_to(outside)
            with self.assertRaisesRegex(bazel_winui.BazelWorkspaceError, "missing or escapes"):
                bazel_winui._owned(package / "include/alias.h", package)

    def test_provider_paths_must_stay_inside_selected_packages(self):
        with tempfile.TemporaryDirectory(prefix="aobus-winui-provider-") as temporary:
            root = Path(temporary).resolve()
            packages = [("Pkg", "1", root / "Pkg.1"), ("Other", "2", root / "Other.2")]
            for _, _, directory in packages:
                directory.mkdir()
            owned = _touch(root / "Pkg.1/include/a.h")
            self.assertEqual(bazel_winui._provider_path(owned, packages), ("Pkg", Path("include/a.h")))
            rogue = _touch(root / "Rogue.3/include/a.h")
            with self.assertRaisesRegex(bazel_winui.BazelWorkspaceError, "points outside its locked NuGet providers"):
                bazel_winui._provider_path(rogue, packages)


class WindowsProjectionTest(unittest.TestCase):
    """Admission on a fake Windows host, piped through the checked-in Starlark projection."""

    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix="aobus-winui-repository-")
        self.addCleanup(temporary.cleanup)
        self.tmp = Path(temporary.name).resolve()
        self.tree = self.tmp / "tree"
        self.project_root = self.tmp / "repo"
        self.lock = self.project_root / "app/windows-winui/packages.config"
        _write_lock(self.lock, LOCKED)
        self.contract = _representative_contract()
        self.enterContext(mock.patch.object(bazel_winui, "PROJECT_ROOT", self.project_root))
        read_json = dependency_policy.read_json
        self.enterContext(
            mock.patch.object(
                dependency_policy,
                "read_json",
                side_effect=lambda path: self.contract if path == dependency_policy.CONTRACT_FILE else read_json(path),
            )
        )
        self._restore_packages()
        self._install_visual_studio()
        self._install_windows_sdk()
        self.request = {
            "host": "windows",
            "config": "debug",
            "compiler_environment": {
                "VCToolsInstallDir": str(self.vc),
                "VCToolsVersion": VC_VERSION,
                "WindowsSdkDir": str(self.windows_sdk),
                "WindowsSDKVersion": SDK,
            },
        }
        self.environment = {
            "AOBUS_NUGET_PACKAGES_DIR": str(self.packages_root),
            "SYSTEMROOT": str(self.tmp / "systemroot"),
            "SYSTEMDRIVE": "/",
            "USERPROFILE": str(self.tmp / "userprofile"),
            "LOCALAPPDATA": str(self.tmp / "appdata-local"),
            "APPDATA": str(self.tmp / "appdata"),
            "AOBUS_SECRET_TOKEN": "secret-token-value",
        }
        self.exit_code = 0
        self.stdout = None
        self.stderr = "fixture diagnostics"
        self._payload()
        self.payload["Properties"]["MSBuildProjectFullPath"] = str(self.tree / "AobusSdk.vcxproj")
        self.payload["Items"]["AobusEvaluatedImport"] = [
            {"Identity": str(path)} for path in [*self.props, *self.targets, self.xaml_targets, self.evaluated_props]
        ]
        self.payload["Items"]["AobusEvaluatorInput"] = [
            {"Identity": str(path)}
            for path in (
                self.msbuild.parent / "Microsoft.Build.dll",
                self.msbuild.parent / "Microsoft.Build.Tasks.Core.dll",
                self.msbuild_config,
                self.redirected_dependency,
                self.loaded_dependency,
            )
        ]
        self.enterContext(mock.patch.object(bazel_winui.subprocess, "run", side_effect=self.query))

    def _restore_packages(self):
        self.packages_root = self.tmp / "nuget"
        self.package_a = self.packages_root / f"{APPSDK_ID}.{APPSDK_VERSION}"
        self.package_b = self.packages_root / f"{CPPWINRT_ID}.{CPPWINRT_VERSION}"
        self.package_buildtools = self.packages_root / f"{BUILDTOOLS_ID}.{BUILDTOOLS_VERSION}"
        self.package_foundation = self.packages_root / f"{FOUNDATION_ID}.{FOUNDATION_VERSION}"
        _write_nupkg(self.package_a, APPSDK_ID, APPSDK_VERSION)
        # The CppWinRT nuspec needs case folding and version normalization.
        _write_nupkg(
            self.package_b,
            CPPWINRT_ID,
            CPPWINRT_VERSION,
            nuspec_id=CPPWINRT_ID.lower(),
            nuspec_version="2.0.240113.1.0",
        )
        _write_nupkg(self.package_buildtools, BUILDTOOLS_ID, BUILDTOOLS_VERSION)
        _write_nupkg(self.package_foundation, FOUNDATION_ID, FOUNDATION_VERSION)
        self.props = [
            _touch(self.package_a / "build/native/Microsoft.WindowsAppSDK.props"),
            _touch(self.package_b / "build/Microsoft.Windows.CppWinRT.props"),
        ]
        _touch(self.package_a / "build/Microsoft.WindowsAppSDK.props")  # generic fallback loses to the native import
        self.targets = [
            _touch(self.package_b / "build/Microsoft.Windows.CppWinRT.targets"),
            _touch(self.package_a / "build/native/Microsoft.WindowsAppSDK.targets"),
        ]
        self.initializer = _touch(self.package_a / "build/native/Generated/AutoInitializer.cpp")
        # The linker payload libraries are real minimal COFF archives; the
        # shared classifier must read their actual member inventory.
        self.bootstrap_lib = self.package_a / "build/native/Aobus.Bootstrap.lib"
        self.bootstrap_lib.write_bytes(STATIC_ARCHIVE)
        self.runtime_lib = self.package_a / "lib/uap10.0/Microsoft.WindowsAppRuntime.lib"
        self.runtime_lib.parent.mkdir(parents=True, exist_ok=True)
        self.runtime_lib.write_bytes(IMPORT_ARCHIVE)
        _touch(self.package_a / "include/winappsdk.h")
        _touch(self.package_a / "include/winappsdk.idl")  # an MIDL header projects but is not an hdrs entry
        self.runtime_dll = _touch(self.package_a / "runtimes/win-x64/Aobus.RuntimeFixture.dll")
        self.xaml_compiler = _touch(self.package_a / "tools/net472/XamlCompiler.exe")
        self.winmd = _touch(self.package_a / "winmd/Microsoft.WindowsAppSDK.winmd")
        _touch(self.package_b / "include/winrt/winrt.h")
        # The CppWinRT tool root is its bin directory.
        self.cppwinrt_exe = _touch(self.package_b / "bin/cppwinrt.exe")
        self.runtime_pri = _touch(self.package_b / "runtimes/win-x64/B.BootstrapFixture.pri")
        self.metadata_less_pri = _touch(self.package_b / "runtimes/win-x64/B.MetadataLess.pri")
        self.makepri_exe = _touch(self.package_buildtools / "bin" / BUILDTOOLS_VERSION / "x64/makepri.exe")
        self.foundation_bootstrap_dll = _touch(
            self.package_foundation / "runtimes/win-x64/native" / FOUNDATION_BOOTSTRAP
        )

    def _install_visual_studio(self):
        self.visual_studio = self.tmp / "visual-studio"
        self.vc = self.visual_studio / "VC/Tools/MSVC" / VC_VERSION
        self.msbuild = _touch(self.visual_studio / "MSBuild/Current/Bin/amd64/MSBuild.exe")
        host = self.msbuild.parent
        self.msbuild_config = _touch(host / "MSBuild.exe.config", "selected native binding policy")
        _touch(host / "Microsoft.Build.dll")
        _touch(host / "Microsoft.Build.Tasks.Core.dll")
        self.redirected_dependency = _touch(host / "System.Collections.Immutable.dll", "selected redirected assembly")
        # Loaded framework dependencies need admission too, even outside the
        # selected MSBuild directory's pre-existing broad runtime family.
        self.loaded_dependency = _touch(self.tmp / "framework/System.Core.dll", "loaded framework assembly")
        self.task_compiler = _touch(host.parent / "Roslyn/csc.exe")
        self.task_compiler_config = _touch(host.parent / "Roslyn/csc.exe.config")
        self.xaml_targets = _touch(host / "Microsoft.Xaml.targets")
        # Outside every broad selected family and absent from MSBuildAllProjects:
        # only the native evaluator's actual import inventory can admit this.
        self.evaluated_props = _touch(self.visual_studio / "MSBuild/Current/Microsoft.Common.props", "<Project />")
        _touch(host / "en/Microsoft.Build.resources.dll")  # a satellite locale payload
        self.vc_targets = self.visual_studio / "MSBuild/Microsoft/VC/v180"
        _touch(self.vc_targets / "Microsoft.Cpp.props")
        _touch(self.vc_targets / "PlatformToolsets/v143/Microsoft.Cpp.v143.targets")
        self.preprocessor = _touch(self.vc / "bin/Hostx64/x64/cl.exe")
        self.compiler_runtime = _touch(self.preprocessor.parent / "c1.dll")
        self.crt_runtime = _touch(self.preprocessor.parent / "vcruntime140.dll")
        _touch(self.preprocessor.parent / "1033/clui.dll")
        self.vc_header = _touch(self.vc / "include/vcruntime.h")
        self.standard_header = _touch(self.vc / "include/vector")
        auxiliary = self.visual_studio / "VC/Auxiliary/Build"
        auxiliary.mkdir(parents=True, exist_ok=True)
        (auxiliary / f"Microsoft.VCToolsVersion.{TOOLSET}.default.txt").write_text(VC_VERSION, encoding="utf-8-sig")

    def _install_windows_sdk(self):
        self.windows_sdk = self.tmp / "windows-sdk"
        _touch(self.windows_sdk / "Lib" / SDK / "um/x64/AobusSystemFixture.lib")
        self.union_metadata = _touch(self.windows_sdk / "UnionMetadata" / SDK / "FixtureMetadata.winmd")
        _touch(self.windows_sdk / "References" / SDK / "Windows.Foundation.winmd")
        include = self.windows_sdk / "Include" / SDK
        self.ucrt_header = _touch(include / "ucrt/corecrt.h")
        _touch(include / "um/Windows.h")
        _touch(include / "shared/oaidl.acf")
        _touch(include / "winrt/Windows.Foundation.idl")
        _touch(self.windows_sdk / "bin" / SDK / "x64/midl.exe")

    def _payload(self):
        self.payload = {
            "Items": {
                "ClCompile": [
                    {
                        "Identity": "__aobus_sdk_interface.cpp",
                        "AdditionalIncludeDirectories": ";".join(
                            [
                                str(self.package_a / "include"),
                                "relative/generated",
                                str(self.tree / "int/Generated Files"),  # private project dir, action-owned
                                str(self.package_b / "include"),
                            ]
                        ),
                        # Mode macros stay toolchain-owned and are filtered away.
                        "PreprocessorDefinitions": "AOBUS_FIXTURE;_DEBUG;NDEBUG;WINUI_SDK_TEST=1.2.3",
                        "ForcedIncludeFiles": "",
                    },
                    {"Identity": "AutoInitializer.cpp", "FullPath": str(self.initializer)},
                ],
                "Link": [
                    {
                        "AdditionalLibraryDirectories": str(self.package_a / "lib/uap10.0"),
                        "AdditionalDependencies": ";".join(
                            [str(self.bootstrap_lib), "Microsoft.WindowsAppRuntime.lib", "AobusSystemFixture.lib"]
                        ),
                    }
                ],
                "Content": [
                    {"FullPath": str(self.winmd), "CopyToOutputDirectory": "PreserveNewest"},
                    {"FullPath": str(self.runtime_dll), "CopyToOutputDirectory": "Always"},
                ],
                # ReferenceCopyLocalPaths is itself the copy authority; the
                # second item carries no CopyLocal metadata at all.
                "ReferenceCopyLocalPaths": [
                    {"FullPath": str(self.runtime_pri), "CopyLocal": "true"},
                    {"FullPath": str(self.metadata_less_pri)},
                ],
            },
            "Properties": {
                "WindowsTargetPlatformVersion": SDK,
                "VCToolsInstallDir": str(self.vc),
                "MakePriExeFullPath": str(self.makepri_exe),
                # The native query may spell tool paths with normalization or
                # quotes; the fixture keeps the observed real shapes.
                "XamlCompilerExePath": str(self.package_a / "build/../tools/net472/../net472/XamlCompiler.exe"),
                "_WindowsAppSDKFoundationPlatform": "x64",
                "WindowsAppSdkBootstrapInitialize": "true",
                "WindowsAppSDKMLVerifyTransitiveDependencies": "true",
                "MSBuildRuntimeType": "Full",
                "VCTargetsPath": str(self.vc_targets),
                "CppWinRTPath": '"' + str(self.cppwinrt_exe.parent) + '"',
                # Deliberately incomplete conventionally maintained property:
                # Microsoft.Common.props is a real evaluated import but absent.
                "MSBuildAllProjects": ";".join(
                    [
                        str(self.tree / "AobusSdk.vcxproj"),
                        "",
                        str(self.targets[1]),
                        str(self.xaml_targets),
                    ]
                ),
            },
        }

    def plan(self):
        """Pipeline the native admission through the real checked-in Starlark projection."""
        return _materialize_winui(
            bazel_winui.resolve_sdk(self.tree, self.request, self.environment), subpackage=self.request["config"]
        )

    def query_output(self):
        return "MSBuild preamble\n" + json.dumps(self.payload) + "\n"

    @staticmethod
    def _query_set(command, prefix):
        flag = next(argument for argument in command if argument.startswith(prefix))
        return set(flag[len(prefix) :].split(","))

    def _destination_of(self, result, source):
        """Return a package-local rule path after checking the real repository destination."""
        destination = next(destination for destination, origin in result["links"].items() if origin == str(source))
        prefix = self.request["config"] + "/"
        self.assertTrue(destination.startswith(prefix), destination)
        return destination[len(prefix) :]

    def _cc_imports(self, result):
        """Map each projected library label to its cc_import rule (name, fields)."""
        imports = {}
        for name, (kind, fields) in result["rules"].items():
            if kind == "cc_import":
                imports[fields.get("static_library", fields.get("interface_library"))] = (name, fields)
        return imports

    def query(self, command, **kwargs):
        project = self.tree / "AobusSdk.vcxproj"
        # Discriminate the selected executable, project, and query; unrelated
        # flag spelling and ordering stay free.
        self.assertEqual(command[0], str(self.msbuild))
        self.assertEqual(command[1], str(project))
        self.assertIn("/t:AobusSdkInterface", command)
        self.assertIn("/nr:false", command)
        self.assertIn("/noautoresponse", command)
        # The query must request the properties the projection depends on;
        # unrelated property spelling and ordering stay free.
        self.assertLessEqual(
            {
                "VCToolsInstallDir",
                "WindowsSdkDir",
                "WindowsTargetPlatformVersion",
                "MakePriExeFullPath",
                "XamlCompilerExePath",
                "_WindowsAppSDKFoundationPlatform",
                "WindowsAppSdkBootstrapInitialize",
                "WindowsAppSDKMLVerifyTransitiveDependencies",
                "MSBuildRuntimeType",
                "VCTargetsPath",
                "CppWinRTPath",
                "MSBuildProjectFullPath",
            },
            self._query_set(command, "/getProperty:"),
        )
        self.assertNotIn("MSBuildAllProjects", self._query_set(command, "/getProperty:"))
        self.assertFalse(any(argument.lower().startswith("/p:") for argument in command))
        self.assertLessEqual(
            {"ClCompile", "Link", "Content", "ReferenceCopyLocalPaths", "AobusEvaluatedImport", "AobusEvaluatorInput"},
            self._query_set(command, "/getItem:"),
        )
        self.assertEqual(kwargs["cwd"], self.tree)
        self.assertTrue(kwargs["capture_output"])
        self.assertFalse(kwargs["check"])
        environment = kwargs["env"]
        self.assertEqual(environment["VCTOOLSINSTALLDIR"], str(self.vc))
        self.assertEqual(environment["VCTOOLSVERSION"], VC_VERSION)
        self.assertEqual(environment["WINDOWSSDKDIR"], str(self.windows_sdk))
        self.assertEqual(environment["SYSTEMROOT"], self.environment["SYSTEMROOT"])
        self.assertEqual(environment["TEMP"], str(self.tree / "tmp"))
        self.assertEqual(environment["TMP"], str(self.tree / "tmp"))
        self.assertEqual(environment["MSBUILDDISABLENODEREUSE"], "1")
        self.assertEqual(environment["MSBUILD_EXE_PATH"], str(self.msbuild))
        self.assertNotIn("AOBUS_NUGET_PACKAGES_DIR", environment)
        self.assertNotIn("AOBUS_SECRET_TOKEN", environment)
        # Observed Windows host architecture and shell runtime inputs pass
        # through verbatim, present or absent; the optional 32-bit alias is
        # never fabricated, and ambient compiler flags stay filtered.
        for key in ("PROCESSOR_ARCHITECTURE", "PROCESSOR_ARCHITEW6432", "WINDIR", "COMSPEC"):
            self.assertEqual(environment.get(key), self.environment.get(key))
        self.assertNotIn("CL", environment)
        # The SDK InitialTargets archive-presence check runs once from this
        # query, un-suppressed, against the real locked packages.config copied
        # into the private query directory before the tool is invoked.
        self.assertEqual((self.tree / "packages.config").read_bytes(), self.lock.read_bytes())
        # The generated project's own content belongs to the focused
        # interface-project test; the fake stays bounded to the external
        # subprocess protocol.
        stdout = self.stdout if self.stdout is not None else self.query_output()
        return SimpleNamespace(returncode=self.exit_code, stdout=stdout, stderr=self.stderr)

    def test_headers_rule_preserves_queried_roots_defines_and_header_classification(self):
        result = self.plan()
        kind, fields = result["rules"]["headers"]
        self.assertEqual(kind, "cc_library")
        # Queried include roots stay compiler search paths, and mode macros
        # stay toolchain-owned while the SDK's own defines survive.
        self.assertEqual(fields["includes"], [f"sdk/{APPSDK_ID}/include", f"sdk/{CPPWINRT_ID}/include"])
        self.assertEqual(fields["defines"], ["AOBUS_FIXTURE", "WINUI_SDK_TEST=1.2.3"])
        # Representative classification: .h headers are hdrs entries; .idl
        # projects as generation metadata but is not an hdrs entry.
        self.assertIn(f"sdk/{APPSDK_ID}/include/winappsdk.h", fields["hdrs"])
        self.assertIn(f"sdk/{CPPWINRT_ID}/include/winrt/winrt.h", fields["hdrs"])
        self.assertNotIn(f"sdk/{APPSDK_ID}/include/winappsdk.idl", fields["hdrs"])
        self.assertIn(f"sdk/{APPSDK_ID}/include/winappsdk.idl", result["rules"]["tools"][1]["srcs"])
        # The projected declarative surface never leaks absolute host paths.
        self.assertNotIn(str(self.tmp), result["files"]["debug/sdk_data.bzl"])

    def test_bootstrap_links_its_initializer_and_queried_libraries_by_coff_role(self):
        result = self.plan()
        kind, fields = result["rules"]["bootstrap"]
        self.assertEqual(kind, "cc_library")
        # The declared SDK autoinitializer stays the always-linked source and
        # the queried system libraries stay raw linker options.
        self.assertEqual(fields["srcs"], [self._destination_of(result, self.initializer)])
        self.assertTrue(fields["alwayslink"])
        self.assertEqual(fields["linkopts"], ["AobusSystemFixture.lib"])
        # The queried libraries reach the native linker through cc_import
        # roles read from their actual COFF payloads; the private rule
        # numbering and the dep graph shape are not contracts.
        imports = self._cc_imports(result)
        static_label = self._destination_of(result, self.bootstrap_lib)
        interface_label = self._destination_of(result, self.runtime_lib)
        self.assertEqual(imports[static_label][1]["static_library"], static_label)
        self.assertEqual(imports[interface_label][1]["interface_library"], interface_label)
        self.assertTrue(imports[interface_label][1]["system_provided"])
        self.assertIn(":headers", fields["deps"])
        self.assertIn(f":{imports[static_label][0]}", fields["deps"])
        self.assertIn(f":{imports[interface_label][0]}", fields["deps"])

    def test_runtime_copies_content_reference_paths_and_the_foundation_bootstrap(self):
        # Content Always and PreserveNewest, ReferenceCopyLocalPaths items
        # with and without CopyLocal metadata, and the structurally selected
        # Foundation bootstrap all materialize; a Never item stays excluded.
        never = _touch(self.package_a / "runtimes/win-x64/Skipped.dll")
        self.payload["Items"]["Content"].append({"FullPath": str(never), "CopyToOutputDirectory": "Never"})
        result = self.plan()
        kind, fields = result["rules"]["runtime"]
        self.assertEqual(kind, "filegroup")
        runtime = set(fields["srcs"])
        copied = (
            self.winmd,
            self.runtime_dll,
            self.runtime_pri,
            self.metadata_less_pri,
            self.foundation_bootstrap_dll,
        )
        for source in copied:
            self.assertIn(self._destination_of(result, source), runtime)
        self.assertNotIn(str(never), result["links"].values())
        self.assertNotIn(f"sdk/{APPSDK_ID}/runtimes/win-x64/Skipped.dll", runtime)

    def test_projection_stays_sparse_around_the_selected_families(self):
        # Negative witnesses live only here, next to the sparse contract.
        excluded = [
            _touch(self.package_a / "stray/stray.h"),  # outside every queried include subtree
            _touch(self.package_a / "lib/uap10.0/Aobus.Native.dll"),  # non-payload bytes outside build/
            _touch(self.package_a / "lib/uap10.0/Aobus.Native.pdb"),
            _touch(self.package_a / "runtimes/win-x86/UnselectedRuntime.dll"),  # other architecture
            _touch(self.package_a / "tools/net8/UnusedTool.dll"),  # unselected tool family
            _touch(self.package_a / "tools/net472/XamlCompiler.pdb"),  # excluded suffix inside a tool root
            _touch(self.package_a / "tools/net472/Payload.nupkg"),
            _touch(self.package_b / "bin/arm64/excluded.dll"),  # other architecture
            _touch(self.package_b / "bin/arm/legacy.dll"),
            _touch(self.msbuild.parent / "unrelated/unused.dll"),  # not a locale payload directory
            _touch(self.visual_studio / "MSBuild/Microsoft/NuGet/18.0/Microsoft.NuGet.targets"),  # unrelated subtree
            _touch(self.vc_targets / "PlatformToolsets/v142/Microsoft.Cpp.v142.targets"),  # unselected toolset
            _touch(self.vc / "bin/Hostx64/x64/link.exe"),  # not a preprocessing tool
            _touch(self.vc / "bin/Hostx86/x64/c1.dll"),  # another host toolchain
            _touch(self.vc / "bin/Hostx64/x64/onecore/vcruntime140.dll"),
        ]
        tool_bytes = _touch(self.package_b / "bin/x64/included.dll")  # selected tool-root bytes
        result = self.plan()
        sources = result["links"]
        # One selected witness per family: the queried generation tools and
        # their directory bytes, the queried libraries and runtime payloads,
        # the Full-CLR MSBuild host with its locale payloads, the selected VC
        # targets, the imported MSBuild projects, and the selected SDK
        # metadata/include/MIDL families.
        for source in (
            self.xaml_compiler,
            self.cppwinrt_exe,
            tool_bytes,
            self.makepri_exe,
            self.foundation_bootstrap_dll,
            self.bootstrap_lib,
            self.runtime_lib,
            self.msbuild,
            self.msbuild.parent / "en/Microsoft.Build.resources.dll",
            self.preprocessor,
            self.compiler_runtime,
            self.crt_runtime,
            self.preprocessor.parent / "1033/clui.dll",
            self.vc_header,
            self.standard_header,
            self.ucrt_header,
            self.vc_targets / "Microsoft.Cpp.props",
            self.xaml_targets,
            self.evaluated_props,
            self.windows_sdk / "References" / SDK / "Windows.Foundation.winmd",
            self.windows_sdk / "Include" / SDK / "um/Windows.h",
            self.windows_sdk / "bin" / SDK / "x64/midl.exe",
        ):
            self.assertIn(str(source), sources.values())
        for source in excluded:
            self.assertNotIn(str(source), sources.values())
        # The restored package archives themselves stay unprojected.
        for name, version in LOCKED:
            self.assertNotIn(str(self.packages_root / f"{name}.{version}" / f"{name}.nupkg"), sources.values())
        for destination, source in result["links"].items():
            self.assertFalse(Path(destination).is_absolute())
            self.assertNotIn("..", destination)
            self.assertTrue(Path(source).is_absolute())
            self.assertTrue(Path(source).is_file())
        # The selected tool witnesses reach the generation tool interface.
        kind, fields = result["rules"]["tools"]
        self.assertEqual(kind, "filegroup")
        tools = set(fields["srcs"])
        for source in (
            self.xaml_compiler,
            self.cppwinrt_exe,
            tool_bytes,
            self.makepri_exe,
            self.preprocessor,
            self.compiler_runtime,
            self.standard_header,
        ):
            self.assertIn(self._destination_of(result, source), tools)

    def test_actual_imports_absent_from_msbuildallprojects_are_observed_action_tools_in_both_modes(self):
        listed_only = _touch(self.visual_studio / "MSBuild/unused/ListedOnly.props", "<Project />")
        condition_false = _touch(self.visual_studio / "MSBuild/unused/ConditionFalse.props", "<Project />")
        self.payload["Properties"]["MSBuildAllProjects"] += ";" + str(listed_only)
        for config in ("debug", "release"):
            with self.subTest(config=config):
                self.request["config"] = config
                observed = []
                with native.capture_probes(observed.append):
                    result = self.plan()
                self.assertIn(self.evaluated_props, observed)
                self.assertIn(self.tree / "AobusSdk.vcxproj", observed)
                destination = self._destination_of(result, self.evaluated_props)
                self.assertIn(destination, result["rules"]["tools"][1]["srcs"])
                self.assertNotIn(str(listed_only), result["links"].values())
                self.assertNotIn(str(condition_false), result["links"].values())
                diagnostics = json.loads(result["files"][config + "/sdk-diagnostics.json"])
                evidence = diagnostics["import_evaluation"]
                self.assertEqual(evidence["global_properties"], {})
                self.assertEqual(evidence["project"]["path"], str(self.tree / "AobusSdk.vcxproj"))
                imported = {record["path"]: record["sha256"] for record in evidence["imports"]}
                self.assertEqual(imported[str(self.evaluated_props)], native.sha256(self.evaluated_props))
                self.assertNotIn(str(self.tree / "AobusSdk.vcxproj"), imported)
                self.assertIn("not all missing/wildcard", evidence["observation_boundary"])

    def test_mutating_an_actual_import_changes_evidence_without_changing_the_query_payload(self):
        for config in ("debug", "release"):
            with self.subTest(config=config):
                self.request["config"] = config
                self.evaluated_props.write_text("<Project /><!-- before -->", encoding="utf-8")
                before = self.plan()
                self.evaluated_props.write_text("<Project /><!-- after -->", encoding="utf-8")
                after = self.plan()
                self.assertNotEqual(
                    before["files"][config + "/sdk-diagnostics.json"],
                    after["files"][config + "/sdk-diagnostics.json"],
                )
                evidence = json.loads(after["files"][config + "/sdk-diagnostics.json"])["import_evaluation"]
                record = next(record for record in evidence["imports"] if record["path"] == str(self.evaluated_props))
                self.assertEqual(record["sha256"], native.sha256(self.evaluated_props))
                # The stable tool path carries the changed bytes to Bazel's
                # action digest, and its explicit probe invalidates repository
                # observations even when the SDK query result is identical.
                destination = self._destination_of(after, self.evaluated_props)
                self.assertIn(destination, after["rules"]["tools"][1]["srcs"])
                self.assertEqual(before["links"], after["links"])

    def test_import_evaluator_failures_record_native_query_output_in_both_modes(self):
        for config in ("debug", "release"):
            with self.subTest(config=config):
                self.request["config"] = config
                self.exit_code, self.stdout, self.stderr = 7, "evaluation failed", "bad import"
                with self.assertRaisesRegex(bazel_winui.BazelWorkspaceError, r"interface query failed \(7\)"):
                    self.plan()
                self.assertEqual((self.tree / "query.stdout").read_text(encoding="utf-8"), "evaluation failed")
                self.assertEqual((self.tree / "query.stderr").read_text(encoding="utf-8"), "bad import")

    def test_import_evaluator_requires_exact_project_and_selected_assemblies_and_config(self):
        original = self.payload["Items"]["AobusEvaluatorInput"]
        for name, values in (
            ("AobusEvaluatedImport", []),
            ("AobusEvaluatedImport", [str(self.tree / "AobusSdk.vcxproj")]),
            ("AobusEvaluatedImport", ["relative.props"]),
            ("AobusEvaluatedImport", [str(self.evaluated_props.parent / "Missing.props")]),
            ("AobusEvaluatorInput", []),
            ("AobusEvaluatorInput", [str(self.xaml_compiler)]),
            (
                "AobusEvaluatorInput",
                [record["Identity"] for record in original if record["Identity"] != str(self.msbuild_config)],
            ),
        ):
            with self.subTest(name=name, values=values):
                previous = self.payload["Items"][name]
                self.payload["Items"][name] = [{"Identity": value} for value in values]
                try:
                    with self.assertRaisesRegex(bazel_winui.BazelWorkspaceError, "invalid evidence"):
                        self.plan()
                finally:
                    self.payload["Items"][name] = previous
        self.payload["Properties"]["MSBuildProjectFullPath"] = str(self.tree / "Other.vcxproj")
        with self.assertRaisesRegex(bazel_winui.BazelWorkspaceError, "invalid evidence"):
            self.plan()

    def test_selected_host_binding_config_and_loaded_dependency_are_observed_action_tools(self):
        observed = []
        with native.capture_probes(observed.append):
            before = self.plan()
        for source in (
            self.msbuild_config,
            self.redirected_dependency,
            self.loaded_dependency,
            self.task_compiler,
            self.task_compiler_config,
        ):
            self.assertIn(self._destination_of(before, source), before["rules"]["tools"][1]["srcs"])
        self.assertIn(self.msbuild_config, observed)
        self.assertIn(self.loaded_dependency, observed)
        diagnostics = json.loads(before["files"]["debug/sdk-diagnostics.json"])
        evaluator = {record["path"]: record["sha256"] for record in diagnostics["import_evaluation"]["evaluator"]}
        self.assertEqual(evaluator[str(self.msbuild_config)], native.sha256(self.msbuild_config))
        self.assertEqual(evaluator[str(self.loaded_dependency)], native.sha256(self.loaded_dependency))
        self.msbuild_config.write_text("changed selected binding redirects", encoding="utf-8")
        after = self.plan()
        self.assertNotEqual(before["files"]["debug/sdk-diagnostics.json"], after["files"]["debug/sdk-diagnostics.json"])
        diagnostics = json.loads(after["files"]["debug/sdk-diagnostics.json"])
        evaluator = {record["path"]: record["sha256"] for record in diagnostics["import_evaluation"]["evaluator"]}
        self.assertEqual(evaluator[str(self.msbuild_config)], native.sha256(self.msbuild_config))

    def test_import_candidates_observe_missing_preferred_props_before_fallback(self):
        self.plan()
        missing_native = self.package_b / "build/native/Microsoft.Windows.CppWinRT.props"
        observed = []
        with native.capture_probes(observed.append):
            before = bazel_winui._imports([(CPPWINRT_ID, CPPWINRT_VERSION, self.package_b)], "props")
        self.assertIn(missing_native, observed)
        self.assertEqual(before, [self.props[1]])
        _touch(missing_native, "<Project />")
        self.assertEqual(
            bazel_winui._imports([(CPPWINRT_ID, CPPWINRT_VERSION, self.package_b)], "props"), [missing_native]
        )

    def test_optional_vc_include_roots_are_observed_before_later_installation(self):
        optional = (self.vc / "atlmfc/include", self.visual_studio / "VC/Auxiliary/VS/include")
        observed = []
        with native.capture_probes(observed.append):
            before = self.plan()
        self.assertLessEqual(set(optional), set(observed))
        before_sources = set(before["links"].values())
        for directory in optional:
            source = _touch(directory / "installed-later.h")
            self.assertNotIn(str(source), before_sources)
            after = self.plan()
            destination = self._destination_of(after, source)
            self.assertIn(destination, after["rules"]["tools"][1]["srcs"])

    @unittest.skipIf(os.name == "nt", "No Windows symlink privilege is required or requested")
    def test_vc_include_family_cannot_escape_the_selected_visual_studio(self):
        outside = self.tmp / "unselected-headers"
        _touch(outside / "escape.h")
        optional = self.vc / "atlmfc/include"
        optional.parent.mkdir(parents=True)
        optional.symlink_to(outside, target_is_directory=True)
        with self.assertRaisesRegex(bazel_winui.BazelWorkspaceError, "include directory escapes"):
            self.plan()

    def test_interface_project_stays_a_guarded_query_only_template(self):
        self.plan()
        text = (self.tree / "AobusSdk.vcxproj").read_text(encoding="utf-8")
        # Canonical MSBuild ordering: Globals are defined before Default.props.
        self.assertLess(text.index('Label="Globals"'), text.index("Microsoft.Cpp.Default.props"))
        # The SDK's own locked transitive-dependency check must run.
        self.assertIn(
            "<WindowsAppSDKMLVerifyTransitiveDependencies>true</WindowsAppSDKMLVerifyTransitiveDependencies>", text
        )
        # The query explicitly selects the installed VC tools version instead
        # of inheriting an ambient default.
        self.assertIn(f"<VCToolsVersion>{VC_VERSION}</VCToolsVersion>", text)
        # The sole query task re-evaluates imports under the real MSBuild host,
        # plus the no-product guard's Error before every compile/link pass. The
        # import sequence itself is owned by ImportOrderTest.
        namespace = "{http://schemas.microsoft.com/developer/msbuild/2003}"
        document = ET.fromstring(text)
        targets = list(document.iter(namespace + "Target"))
        self.assertEqual(
            [task.tag.rsplit("}", 1)[-1] for target in targets for task in target],
            ["AobusEvaluateSdkImports", "Error"],
        )
        evaluator = document.find(namespace + "UsingTask")
        self.assertEqual(evaluator.attrib["TaskFactory"], "RoslynCodeTaskFactory")
        self.assertEqual(evaluator.attrib["AssemblyFile"], "$(MSBuildToolsPath)\\Microsoft.Build.Tasks.Core.dll")
        references = evaluator.findall(namespace + "Task/" + namespace + "Reference")
        self.assertEqual(
            {reference.attrib["Include"] for reference in references},
            {"$(MSBuildToolsPath)\\Microsoft.Build.dll", "System.Xml"},
        )
        code = evaluator.find(namespace + "Task/" + namespace + "Code").text
        self.assertIn("new Project(ProjectPath, new Dictionary<string, string>(), null, collection)", code)
        self.assertIn("import.ImportedProject.FullPath", code)
        guard = next(target for target in targets if target.attrib["Name"] == "AobusNoProductBuild")
        self.assertIn("must not generate or compile product code", guard.find(namespace + "Error").attrib["Text"])
        for product_pass in ("ClCompile", "Link"):
            self.assertIn(product_pass, guard.attrib["BeforeTargets"])
        # The query receipts stay in the private query directory.
        self.assertEqual((self.tree / "query.stdout").read_text(encoding="utf-8"), self.query_output())
        self.assertEqual((self.tree / "query.stderr").read_text(encoding="utf-8"), self.stderr)
        self.assertTrue((self.tree / "tmp").is_dir())

    def test_metadata_receipts_report_selection_isolation_and_guard(self):
        result = self.plan()
        self.assertLessEqual(
            {"debug/BUILD.bazel", "debug/sdk.json", "debug/sdk-diagnostics.json", "debug/sdk_data.bzl"},
            set(result["files"]),
        )
        # The fixed BUILD loader wires the real macro to the projected data; the
        # exported metadata names are declared by the macro, not the loader text.
        build = result["files"]["debug/BUILD.bazel"]
        self.assertIn('load("@aobus//bazel:winui_sdk.bzl", "winui_sdk_imports")', build)
        self.assertIn('load(":sdk_data.bzl", "SDK")', build)
        self.assertEqual(result["exports"], ["sdk.json", "sdk-diagnostics.json"])
        metadata = json.loads(result["files"]["debug/sdk.json"])
        # The generator consumes this selected native toolchain and proof.
        self.assertEqual(metadata["msbuild"], str(self.msbuild))
        self.assertEqual(metadata["platform_toolset"], TOOLSET)
        self.assertEqual(metadata["windows_sdk_version"], SDK)
        self.assertIs(metadata["ml_transitive_dependencies_verified"], True)
        # The environment receipt keeps the compiler selection without the
        # private query directory or any secrets.
        reported = metadata["environment"]
        self.assertEqual(reported["VCTOOLSINSTALLDIR"], str(self.vc))
        self.assertEqual(reported["MSBUILDDISABLENODEREUSE"], "1")
        for excluded in ("TEMP", "TMP", "AOBUS_NUGET_PACKAGES_DIR", "AOBUS_SECRET_TOKEN"):
            self.assertNotIn(excluded, reported)

    def test_admitted_facts_carry_no_bazel_program_or_destination_protocol(self):
        # The native boundary returns admitted observations only: absolute
        # source identities and provider-relative paths. The Bazel program,
        # artifact links, and destination or alias naming belong entirely to
        # the checked-in Starlark materialization.
        facts = bazel_winui.resolve_sdk(self.tree, self.request, self.environment)
        self.assertEqual(
            set(facts),
            {
                "sdk_files",
                "includes",
                "defines",
                "headers",
                "sources",
                "libraries",
                "system_libraries",
                "runtime",
                "tools",
                "native_inputs",
                "execution",
                "diagnostics",
            },
        )
        for record in facts["sdk_files"]:
            self.assertEqual(set(record), {"source", "package", "path"})
            self.assertTrue(Path(record["source"]).is_absolute())
        for record in facts["includes"]:
            self.assertEqual(set(record), {"package", "path"})
        for record in facts["libraries"]:
            self.assertEqual(set(record), {"file", "kind"})
        for family in ("headers", "sources", "runtime", "tools", "native_inputs"):
            for source in facts[family]:
                self.assertTrue(Path(source).is_absolute(), family)
        serialized = json.dumps(facts)
        for forbidden in ("BUILD.bazel", "sdk_data", "native-tools", "destination", '"links"', "alias"):
            self.assertNotIn(forbidden, serialized)

    def test_diagnostic_only_query_change_leaves_execution_metadata_projection_and_links_unchanged(self):
        before = self.plan()
        before_diagnostics = json.loads(before["files"]["debug/sdk-diagnostics.json"])
        self.assertEqual(before_diagnostics["properties"]["VCToolsInstallDir"], str(self.vc))
        # The query echoes this selected VC install, but the projection consumes
        # the compiler environment. A cosmetic trailing separator is diagnostic
        # only and must not reach the Bazel action input or generated interface.
        self.payload["Properties"]["VCToolsInstallDir"] = str(self.vc) + os.sep
        after = self.plan()
        self.assertEqual(after["files"]["debug/sdk.json"], before["files"]["debug/sdk.json"])
        self.assertEqual(after["files"]["debug/sdk_data.bzl"], before["files"]["debug/sdk_data.bzl"])
        self.assertEqual(after["rules"], before["rules"])
        self.assertEqual(after["links"], before["links"])
        self.assertNotEqual(after["files"]["debug/sdk-diagnostics.json"], before["files"]["debug/sdk-diagnostics.json"])
        diagnostics = json.loads(after["files"]["debug/sdk-diagnostics.json"])
        self.assertEqual(diagnostics["properties"]["VCToolsInstallDir"], str(self.vc) + os.sep)

    def test_host_architecture_and_shell_runtime_inputs_survive_the_query_boundary(self):
        # The MSBuild host tool selection reads the observed Windows host
        # architecture and shell runtime inputs, so the query boundary must
        # preserve them from the passed environment into the generated
        # receipt; ambient compiler flags stay filtered, and the optional
        # 32-bit alias passes through only when the host actually sets it.
        self.environment.update(
            {
                "PROCESSOR_ARCHITECTURE": "AMD64",
                "PROCESSOR_ARCHITEW6432": "AMD64",
                "WINDIR": str(self.tmp / "windir"),
                "COMSPEC": str(self.tmp / "windir/System32/cmd.exe"),
                "CL": "/DAMBIENT=1",
            }
        )
        result = self.plan()
        reported = json.loads(result["files"]["debug/sdk.json"])["environment"]
        for key in ("PROCESSOR_ARCHITECTURE", "PROCESSOR_ARCHITEW6432", "WINDIR", "COMSPEC"):
            self.assertEqual(reported[key], self.environment[key])
        self.assertNotIn("CL", reported)
        # An absent optional alias is never fabricated in the receipt.
        del self.environment["PROCESSOR_ARCHITEW6432"]
        reported = json.loads(self.plan()["files"]["debug/sdk.json"])["environment"]
        self.assertNotIn("PROCESSOR_ARCHITEW6432", reported)
        self.assertNotIn("CL", reported)

    def test_request_configuration_selects_the_queried_msbuild_configuration(self):
        self.request["config"] = "release"
        self.plan()
        project = (self.tree / "AobusSdk.vcxproj").read_text(encoding="utf-8")
        self.assertIn("<Configuration>Release</Configuration>", project)
        self.assertIn("<UseDebugLibraries>false</UseDebugLibraries>", project)

    def test_missing_or_relative_restored_cache_is_rejected_without_provisioning(self):
        variants = (
            {},
            {"AOBUS_NUGET_PACKAGES_DIR": ""},
            {"AOBUS_NUGET_PACKAGES_DIR": "relative/cache"},
            {"AOBUS_NUGET_PACKAGES_DIR": str(self.lock)},
            {"AOBUS_NUGET_PACKAGES_DIR": str(self.tmp / "missing")},
        )
        for environment in variants:
            with (
                self.subTest(environment=environment),
                # The absent/empty selection lazily reads the machine's managed
                # ready cache; a clean machine has none, so keep that external
                # state hermetic instead of admitting this runner's real cache.
                mock.patch.object(bazel_provision, "restored_nuget_packages", return_value=None),
                self.assertRaisesRegex(
                    bazel_winui.BazelWorkspaceError, "requires a restored NuGet cache: run `ao.bat bazel provision`"
                ),
            ):
                bazel_winui.resolve_sdk(self.tree, self.request, environment)
            self.assertFalse(self.tree.exists())

    def test_failed_query_records_output_and_fails_closed(self):
        self.exit_code, self.stdout, self.stderr = 3, "native failure detail", "boom"
        with self.assertRaisesRegex(bazel_winui.BazelWorkspaceError, r"query failed \(3\)"):
            self.plan()
        self.assertEqual((self.tree / "query.stdout").read_text(encoding="utf-8"), "native failure detail")
        self.assertEqual((self.tree / "query.stderr").read_text(encoding="utf-8"), "boom")

    def test_query_without_declared_item_property_json_fails(self):
        self.stdout = "Microsoft (R) Build Tools\nno json payload"
        with self.assertRaisesRegex(bazel_winui.BazelWorkspaceError, "did not return its declared item/property JSON"):
            self.plan()

    def test_interface_requires_its_independent_compile_and_link_items(self):
        for compile_items, link_items in (
            ([{"Identity": "main.cpp"}], self.payload["Items"]["Link"]),
            (self.payload["Items"]["ClCompile"], self.payload["Items"]["Link"] * 2),
        ):
            with self.subTest(link_items=len(link_items)):
                original = (self.payload["Items"]["ClCompile"], self.payload["Items"]["Link"])
                self.payload["Items"]["ClCompile"] = compile_items
                self.payload["Items"]["Link"] = link_items
                try:
                    with self.assertRaisesRegex(
                        bazel_winui.BazelWorkspaceError, "lacks its independent compile/link interface items"
                    ):
                        self.plan()
                finally:
                    self.payload["Items"]["ClCompile"], self.payload["Items"]["Link"] = original

    def test_missing_autoinitializer_sources_fail(self):
        self.payload["Items"]["ClCompile"] = [self.payload["Items"]["ClCompile"][0]]
        with self.assertRaisesRegex(
            bazel_winui.BazelWorkspaceError, "did not supply its required native autoinitializer sources"
        ):
            self.plan()

    def test_forced_includes_and_freeform_definitions_are_rejected(self):
        exemplar = self.payload["Items"]["ClCompile"][0]
        for field, value in (("ForcedIncludeFiles", "pch.h"), ("PreprocessorDefinitions", "AOBUS_FIXTURE;BAD=Foo(1)")):
            with self.subTest(field=field):
                original = exemplar[field]
                exemplar[field] = value
                try:
                    with self.assertRaisesRegex(
                        bazel_winui.BazelWorkspaceError, "unreviewed WinUI SDK forced-include or compiler-definition"
                    ):
                        self.plan()
                finally:
                    exemplar[field] = original

    def test_unreviewed_linker_tokens_fail_closed(self):
        link = self.payload["Items"]["Link"][0]
        for token in ("$(OutDir)/evil.lib", "evil.dll"):
            with self.subTest(token=token):
                original = link["AdditionalDependencies"]
                link["AdditionalDependencies"] = token
                try:
                    with self.assertRaisesRegex(bazel_winui.BazelWorkspaceError, "unreviewed WinUI SDK linker token"):
                        self.plan()
                finally:
                    link["AdditionalDependencies"] = original

    def test_missing_or_ambiguous_library_candidates_fail(self):
        link = self.payload["Items"]["Link"][0]
        link["AdditionalDependencies"] = "Ghost.lib"
        with self.assertRaisesRegex(bazel_winui.BazelWorkspaceError, "missing or ambiguous WinUI SDK link input"):
            self.plan()
        alternate = _touch(self.package_a / "lib/alternate/Shared.lib")
        _touch(self.package_a / "lib/uap10.0/Shared.lib")
        link["AdditionalDependencies"] = "Shared.lib"
        link["AdditionalLibraryDirectories"] = ";".join([str(self.package_a / "lib/uap10.0"), str(alternate.parent)])
        with self.assertRaisesRegex(bazel_winui.BazelWorkspaceError, "missing or ambiguous WinUI SDK link input"):
            self.plan()

    def test_include_and_runtime_inputs_must_stay_inside_locked_providers(self):
        outside_include = self.tmp / "outside-include"
        outside_include.mkdir()
        self.payload["Items"]["ClCompile"][0]["AdditionalIncludeDirectories"] = str(outside_include)
        with self.assertRaisesRegex(bazel_winui.BazelWorkspaceError, "points outside its locked NuGet providers"):
            self.plan()
        self.payload["Items"]["ClCompile"][0]["AdditionalIncludeDirectories"] = str(self.package_a / "include")
        # Outside the restored cache the ownership check fails first; inside the
        # cache but in an unselected package the provider gate fires.
        for path, message in (
            (self.tmp / "outside-runtime.dll", "missing or escapes"),
            (self.packages_root / "Rogue.9/runtime.dll", "points outside its locked NuGet providers"),
        ):
            with self.subTest(path=path.name):
                _touch(path)
                self.payload["Items"]["Content"] = [{"FullPath": str(path), "CopyToOutputDirectory": "Always"}]
                with self.assertRaisesRegex(bazel_winui.BazelWorkspaceError, message):
                    self.plan()

    def test_absent_include_roots_stay_search_paths_until_their_headers_appear(self):
        # Microsoft advertises optional include subdirectories (for example
        # MachineLearning's include/winml/runtime) that may be absent from a
        # restored package. The compiler search interface survives without
        # fabricated bytes, and the sparse projection picks the headers up
        # once the watched directory becomes present.
        absent = self.package_a / "include/winml/runtime"
        exemplar = self.payload["Items"]["ClCompile"][0]
        exemplar["AdditionalIncludeDirectories"] += ";" + str(absent)
        result = self.plan()
        diagnostics = json.loads(result["files"]["debug/sdk-diagnostics.json"])
        self.assertIn(str(absent), diagnostics["includes"])
        self.assertIn(f"sdk/{APPSDK_ID}/include/winml/runtime", result["rules"]["headers"][1]["includes"])
        sources = result["links"]
        self.assertFalse(
            any(destination.startswith(f"debug/sdk/{APPSDK_ID}/include/winml/") for destination in sources)
        )
        header = _touch(absent / "WinmlFixture.h")
        result = self.plan()
        sources = result["links"]
        self.assertEqual(sources[f"debug/sdk/{APPSDK_ID}/include/winml/runtime/WinmlFixture.h"], str(header))
        self.assertIn(f"sdk/{APPSDK_ID}/include/winml/runtime/WinmlFixture.h", result["rules"]["headers"][1]["hdrs"])
        self.assertIn(f"sdk/{APPSDK_ID}/include/winml/runtime/WinmlFixture.h", result["rules"]["tools"][1]["srcs"])

    def test_absent_include_roots_must_stay_inside_the_locked_providers(self):
        # An absent include root still resolves lexically; missing directories
        # never waive provider containment.
        exemplar = self.payload["Items"]["ClCompile"][0]
        original = exemplar["AdditionalIncludeDirectories"]
        for absent in (
            str(self.package_a / "include/../../../outside-absent-include"),
            str(self.packages_root / "Rogue.9/include"),
        ):
            with self.subTest(absent=absent):
                exemplar["AdditionalIncludeDirectories"] = absent
                with self.assertRaisesRegex(
                    bazel_winui.BazelWorkspaceError, "points outside its locked NuGet providers"
                ):
                    self.plan()
        exemplar["AdditionalIncludeDirectories"] = original

    @unittest.skipIf(os.name == "nt", "No Windows symlink privilege is required or requested")
    def test_absent_include_roots_reject_existing_symlinks_outside_providers(self):
        outside = self.tmp / "outside-include-link"
        outside.mkdir()
        alias = self.package_a / "include/alias-include"
        alias.symlink_to(outside)
        exemplar = self.payload["Items"]["ClCompile"][0]
        exemplar["AdditionalIncludeDirectories"] = str(alias)
        with self.assertRaisesRegex(bazel_winui.BazelWorkspaceError, "points outside its locked NuGet providers"):
            self.plan()

    def test_bootstrap_requires_x64_framework_dependent_foundation_runtime(self):
        for name, value in (
            ("_WindowsAppSDKFoundationPlatform", "arm64"),
            ("WindowsAppSdkBootstrapInitialize", "false"),
        ):
            with self.subTest(property=name):
                original = self.payload["Properties"][name]
                self.payload["Properties"][name] = value
                try:
                    with self.assertRaisesRegex(
                        bazel_winui.BazelWorkspaceError, "x64 framework-dependent bootstrap contract"
                    ):
                        self.plan()
                finally:
                    self.payload["Properties"][name] = original
        _write_lock(self.lock, LOCKED[:3])  # Foundation absent from the restored lock
        with self.assertRaisesRegex(bazel_winui.BazelWorkspaceError, "x64 framework-dependent bootstrap contract"):
            self.plan()

    def test_projection_requires_the_querys_own_transitive_dependency_check(self):
        for value in ("false", None):
            with self.subTest(value=value):
                if value is None:
                    del self.payload["Properties"]["WindowsAppSDKMLVerifyTransitiveDependencies"]
                else:
                    self.payload["Properties"]["WindowsAppSDKMLVerifyTransitiveDependencies"] = value
                try:
                    with self.assertRaisesRegex(bazel_winui.BazelWorkspaceError, "locked transitive-dependency check"):
                        self.plan()
                finally:
                    self.payload["Properties"]["WindowsAppSDKMLVerifyTransitiveDependencies"] = "true"

    def test_missing_foundation_bootstrap_runtime_fails_closed(self):
        self.foundation_bootstrap_dll.unlink()
        with self.assertRaisesRegex(bazel_winui.BazelWorkspaceError, "missing or escapes"):
            self.plan()

    def test_autoinitializers_must_be_absolute_provider_owned_cpp_sources(self):
        outside = _touch(self.tmp / "outside-initializer.cpp")
        original = list(self.payload["Items"]["ClCompile"])
        for item in ({"Identity": "phantom.cpp"}, {"FullPath": "relative/Generated.cpp"}, {"FullPath": str(outside)}):
            with self.subTest(item=item):
                self.payload["Items"]["ClCompile"] = [*original, item]
                message = (
                    "points outside its locked NuGet providers"
                    if item.get("FullPath") == str(outside)
                    else "autoinitializers must be existing provider-owned"
                )
                with self.assertRaisesRegex(bazel_winui.BazelWorkspaceError, message):
                    self.plan()
        self.payload["Items"]["ClCompile"] = original

    def test_missing_runtime_copy_input_fails_closed(self):
        self.payload["Items"]["ReferenceCopyLocalPaths"] = [
            {"FullPath": str(self.package_b / "runtimes/win-x64/MissingRuntime.pri")}
        ]
        with self.assertRaisesRegex(bazel_winui.BazelWorkspaceError, "missing or escapes"):
            self.plan()

    def test_runtime_copy_inputs_must_be_provider_absolute(self):
        self.payload["Items"]["ReferenceCopyLocalPaths"] = [{"FullPath": "runtimes/win-x64/Relative.dll"}]
        with self.assertRaisesRegex(bazel_winui.BazelWorkspaceError, "runtime input must be provider-absolute"):
            self.plan()

    def test_ambiguous_runtime_copy_destinations_fail(self):
        first = _touch(self.package_a / "runtimes/win-x64/Dup.dll")
        second = _touch(self.package_b / "runtimes/win-x64/dup.dll")
        self.payload["Items"]["Content"] = [
            {"FullPath": str(first), "CopyToOutputDirectory": "Always"},
            {"FullPath": str(second), "CopyToOutputDirectory": "Always"},
        ]
        self.payload["Items"]["ReferenceCopyLocalPaths"] = []
        with self.assertRaisesRegex(bazel_winui.BazelWorkspaceError, "runtime copy destinations are ambiguous"):
            self.plan()

    def test_msbuild_host_must_report_its_native_full_clr_runtime(self):
        for value in ("Core", None):
            with self.subTest(value=value):
                original = self.payload["Properties"]["MSBuildRuntimeType"]
                if value is None:
                    del self.payload["Properties"]["MSBuildRuntimeType"]
                else:
                    self.payload["Properties"]["MSBuildRuntimeType"] = value
                try:
                    with self.assertRaisesRegex(bazel_winui.BazelWorkspaceError, "native Full-CLR MSBuild task host"):
                        self.plan()
                finally:
                    self.payload["Properties"]["MSBuildRuntimeType"] = original

    def test_vc_targets_must_stay_inside_the_selected_visual_studio(self):
        outside = self.tmp / "outside-vc"
        outside.mkdir()
        for value in ("", "relative/vc", str(outside), str(self.vc_targets / "missing")):
            with self.subTest(value=value):
                original = self.payload["Properties"]["VCTargetsPath"]
                self.payload["Properties"]["VCTargetsPath"] = value
                try:
                    with self.assertRaisesRegex(
                        bazel_winui.BazelWorkspaceError, "must select its installed VC targets directory"
                    ):
                        self.plan()
                finally:
                    self.payload["Properties"]["VCTargetsPath"] = original

    def test_generation_tools_must_be_selected_absolute_and_provider_owned(self):
        outside = _touch(self.tmp / "outside-tools/xamlcompiler.exe")
        for name, value, message in (
            ("XamlCompilerExePath", "", "omitted its selected generation tool"),
            ("XamlCompilerExePath", "relative/tools/XamlCompiler.exe", "omitted its selected generation tool"),
            ("XamlCompilerExePath", str(outside), "points outside its locked NuGet providers"),
            ("MakePriExeFullPath", "", "omitted its selected generation tool"),
            ("MakePriExeFullPath", str(outside), "points outside its locked NuGet providers"),
            ("CppWinRTPath", "", "omitted its selected generation tool"),
            # CppWinRTPath is the tool's own directory; an executable there is not.
            ("CppWinRTPath", str(self.cppwinrt_exe), "generation tool directory is missing"),
        ):
            with self.subTest(name=name, value=value):
                original = self.payload["Properties"][name]
                self.payload["Properties"][name] = value
                try:
                    with self.assertRaisesRegex(bazel_winui.BazelWorkspaceError, message):
                        self.plan()
                finally:
                    self.payload["Properties"][name] = original

    def test_library_roles_follow_the_real_coff_payload_classification(self):
        # Classification reads the actual archive members, never a mock: a
        # static payload in the import role flips the observed cc_import in
        # place, and an import payload flips the static role the same way.
        self.runtime_lib.write_bytes(STATIC_ARCHIVE)
        result = self.plan()
        imports = self._cc_imports(result)
        label = self._destination_of(result, self.runtime_lib)
        self.assertEqual(imports[label][1]["static_library"], label)
        self.runtime_lib.write_bytes(IMPORT_ARCHIVE)
        self.bootstrap_lib.write_bytes(IMPORT_ARCHIVE)
        result = self.plan()
        imports = self._cc_imports(result)
        for library in (self.runtime_lib, self.bootstrap_lib):
            label = self._destination_of(result, library)
            self.assertEqual(imports[label][1]["interface_library"], label)
            self.assertTrue(imports[label][1]["system_provided"])
        self.runtime_lib.write_bytes(b"not a COFF archive\n")
        with self.assertRaisesRegex(bazel_winui.BazelWorkspaceError, "is not a static COFF archive"):
            self.plan()


if __name__ == "__main__":
    unittest.main()
