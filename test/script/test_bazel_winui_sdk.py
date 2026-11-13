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
import tempfile
import unittest
import zipfile
from pathlib import Path
from types import SimpleNamespace
from unittest import mock

from ao.core import bazel_winui, dependency_policy

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
        return bazel_winui.resolve_sdk(self.tree, self.request, self.environment)

    def query_output(self):
        return "MSBuild preamble\n" + json.dumps(self.payload) + "\n"

    @staticmethod
    def _query_set(command, prefix):
        flag = next(argument for argument in command if argument.startswith(prefix))
        return set(flag[len(prefix) :].split(","))

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

    def test_import_evaluator_failure_retains_native_query_output(self):
        self.request["config"] = "debug"
        self.exit_code, self.stdout, self.stderr = 7, "evaluation failed", "bad import"
        with self.assertRaisesRegex(bazel_winui.BazelWorkspaceError, r"interface query failed \(7\)"):
            self.plan()
        self.assertEqual((self.tree / "query.stdout").read_text(encoding="utf-8"), "evaluation failed")
        self.assertEqual((self.tree / "query.stderr").read_text(encoding="utf-8"), "bad import")

    def test_unselected_same_named_assemblies_cannot_satisfy_host_inventory(self):
        original = self.payload["Items"]["AobusEvaluatorInput"]
        for name in ("Microsoft.Build.dll", "Microsoft.Build.Tasks.Core.dll"):
            with self.subTest(name=name):
                foreign = _touch(self.tmp / "unselected-msbuild" / name)
                self.payload["Items"]["AobusEvaluatorInput"] = [
                    {"Identity": str(foreign)} if record["Identity"] == str(self.msbuild.parent / name) else record
                    for record in original
                ]
                with self.assertRaisesRegex(bazel_winui.BazelWorkspaceError, "selected MSBuild assemblies/config"):
                    self.plan()
        self.payload["Items"]["AobusEvaluatorInput"] = original

    def test_admitted_facts_carry_no_bazel_program_or_destination_protocol(self):
        # The native boundary returns admitted observations only: the exact
        # record keys and absolute source identities asserted below. Bazel
        # program, destination and alias naming belong entirely to the
        # checked-in Starlark materialization.
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

    def test_observed_import_absent_from_msbuildallprojects_reaches_the_tools_filegroup(self):
        # A real condition-true import absent from the legacy MSBuildAllProjects
        # property: only the evaluator's import inventory can admit it, and a
        # refactor dropping observed imports silently starves generation actions.
        self.assertNotIn(str(self.evaluated_props), self.payload["Properties"]["MSBuildAllProjects"])
        facts = self.plan()
        links = {}
        files = {}
        rules = {}
        namespace = {
            "json": SimpleNamespace(encode=json.dumps),
            "fail": self.fail,
            "load": lambda *label: None,
            "package": lambda **kwargs: None,
            "native": SimpleNamespace(
                exports_files=lambda names: None,
                filegroup=lambda **kwargs: rules.__setitem__(kwargs["name"], kwargs),
            ),
            "cc_import": lambda **kwargs: None,
            "cc_library": lambda **kwargs: None,
        }
        context = SimpleNamespace(
            path=lambda source: source,
            symlink=lambda target, destination: links.__setitem__(destination, str(target)),
            file=lambda name, content, executable=False: files.__setitem__(name, content),
        )
        bazel = Path(__file__).resolve().parents[2] / "bazel"
        exec(
            compile((bazel / "winui_repository.bzl").read_text(encoding="utf-8"), "winui_repository.bzl", "exec"),
            namespace,
        )
        exec(compile((bazel / "winui_sdk.bzl").read_text(encoding="utf-8"), "winui_sdk.bzl", "exec"), namespace)
        prefix = self.request["config"] + "/"
        namespace["materialize_winui"](context, self.request["config"], facts)
        exec(compile(files[prefix + "sdk_data.bzl"], "sdk_data.bzl", "exec"), namespace)
        exec(compile(files[prefix + "BUILD.bazel"], "BUILD.bazel", "exec"), namespace)
        destination = next(d for d, origin in links.items() if origin == str(self.evaluated_props))
        self.assertTrue(destination.startswith(prefix + "native-tools/"), destination)
        self.assertIn(destination[len(prefix) :], rules["tools"]["srcs"])

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


if __name__ == "__main__":
    unittest.main()
