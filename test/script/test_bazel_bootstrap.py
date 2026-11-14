"""Native environment boundaries of the Bazel route and required CI gates."""

import json
import os
import platform
import shlex
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

from ao import __main__ as portal

ROOT = Path(__file__).resolve().parents[2]


@unittest.skipUnless(shutil.which("bash"), "POSIX bootstrap requires Bash")
class MacosBazelEnvironmentTests(unittest.TestCase):
    def _run(self, *, version: str = "22.1.8", architecture: str = "x86_64") -> subprocess.CompletedProcess[str]:
        with tempfile.TemporaryDirectory(prefix="aobus-bazel-bootstrap-") as temporary:
            root = Path(temporary)
            llvm = root / "llvm"
            (llvm / "bin").mkdir(parents=True)
            compiler = llvm / "bin/clang++"
            bash = str(shutil.which("bash"))
            # Git Bash can live under "Program Files"; a shebang cannot quote
            # that native path. This fixture uses only portable shell builtins.
            compiler.write_text(f"#!/bin/sh\nprintf '%s\\n' {shlex.quote(version)}\n", encoding="utf-8", newline="\n")
            compiler.chmod(0o755)
            calls = root / "calls"
            script = f"""
set -eu
source {shlex.quote(str(ROOT / "script/ao/macos-vcpkg-bootstrap.sh"))}
aobus_macos_load_toolchain_lock() {{
    export AOBUS_MACOS_LLVM_FORMULA=llvm@22
    export AOBUS_MACOS_LLVM_MAJOR=22
    export AOBUS_MACOS_DEPLOYMENT_TARGET=15.0
    export AOBUS_HOMEBREW=/unused/brew
}}
aobus_macos_check_triplet_deployment() {{
    :
}}
aobus_macos_formula_prefix() {{
    printf '%s\\n' "$2" >> {shlex.quote(str(calls))}
    case "$2" in
        llvm@22) printf '%s\\n' {shlex.quote(str(llvm))} ;;
        pkgconf) printf '%s\\n' {shlex.quote(str(root / "pkgconf"))} ;;
        *) return 91 ;;
    esac
}}
aobus_macos_prepare_expected_shim() {{
    printf 'shim=%s|%s|%s\\n' "$1" "$2" "$3"
}}
uname() {{ printf '%s\\n' {shlex.quote(architecture)}; }}
aobus_macos_prepare_bazel_environment {shlex.quote(str(ROOT))}
printf 'triplet=%s\\ncompiler=%s\\ndeployment=%s\\n' "$AOBUS_VCPKG_TRIPLET" "$CXX" "$MACOSX_DEPLOYMENT_TARGET"
"""
            environment = dict(os.environ, AOBUS_STATE_ROOT=str(root / "state"))
            result = subprocess.run([bash, "-c", script], env=environment, capture_output=True, text=True, check=False)
            queried = calls.read_text(encoding="utf-8").splitlines()
            self.assertEqual(queried, ["llvm@22", "pkgconf"])
            return result

    def test_existing_provider_path_needs_no_cmake_ninja_or_vcpkg_bootstrap(self) -> None:
        result = self._run()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("triplet=x64-aobus-osx", result.stdout)
        self.assertIn("deployment=15.0", result.stdout)
        self.assertIn("|22.1.8|", result.stdout)

    def test_arm64_uses_the_native_project_triplet(self) -> None:
        result = self._run(architecture="arm64")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("triplet=arm64-aobus-osx", result.stdout)


@unittest.skipUnless(shutil.which("bash"), "POSIX bootstrap requires Bash")
# Git Bash rewrites the inherited PATH and keeps native separators, so the
# exported POSIX path contract of this macOS-only route is not observable there.
@unittest.skipIf(os.name == "nt", "the macOS admission path contract is POSIX-only")
class MacosNativeEnvironmentAdmissionTests(unittest.TestCase):
    """Admission and provisioning boundaries of the shared native bootstrap."""

    def _fixture(self, deployments: dict[str, list[str]]) -> tuple[Path, dict[str, str]]:
        """A project root with the real lock and explicit consumed triplets."""
        temporary = Path(tempfile.mkdtemp(prefix="aobus-native-admission-"))
        self.addCleanup(shutil.rmtree, temporary, ignore_errors=True)
        project = temporary / "project"
        lock_file = project / "script/ao/macos-toolchain.json"
        lock_file.parent.mkdir(parents=True)
        shutil.copy2(ROOT / "script/ao/macos-toolchain.json", lock_file)
        lock = json.loads(lock_file.read_text(encoding="utf-8"))
        for name, targets in deployments.items():
            triplet = project / "cmake/vcpkg-triplets" / name
            triplet.parent.mkdir(parents=True, exist_ok=True)
            architecture = "x86_64" if name.startswith("x64") else "arm64"
            target_architecture = "x64" if architecture == "x86_64" else "arm64"
            triplet.write_text(
                "\n".join(
                    (
                        f"set(VCPKG_TARGET_ARCHITECTURE {target_architecture})",
                        "set(VCPKG_CRT_LINKAGE dynamic)",
                        "set(VCPKG_LIBRARY_LINKAGE static)",
                        "",
                        "set(VCPKG_CMAKE_SYSTEM_NAME Darwin)",
                        f"set(VCPKG_OSX_ARCHITECTURES {architecture})",
                        *(f"set(VCPKG_OSX_DEPLOYMENT_TARGET {value})" for value in targets),
                    )
                )
                + "\n",
                encoding="utf-8",
            )
        return project, lock

    def _harness(self, project: Path) -> tuple[Path, Path]:
        """Fake Homebrew prefixes with one LLVM keg matching the lock."""
        temporary = Path(tempfile.mkdtemp(prefix="aobus-native-harness-"))
        self.addCleanup(shutil.rmtree, temporary, ignore_errors=True)
        lock = json.loads((project / "script/ao/macos-toolchain.json").read_text(encoding="utf-8"))
        llvm = temporary / "llvm"
        (llvm / "bin").mkdir(parents=True)
        compiler = llvm / "bin/clang++"
        version = lock["homebrew"]["llvmMajorVersion"] + ".1.8"
        compiler.write_text(f"#!/bin/sh\nprintf '%s\\n' {shlex.quote(version)}\n", encoding="utf-8", newline="\n")
        compiler.chmod(0o755)
        return temporary, llvm

    def _run_route(
        self,
        project: Path,
        route: str,
        *,
        harness: Path,
        llvm: Path,
        lock: dict[str, str],
        state_root: Path | None = None,
        fail_formula: str | None = None,
        environment: dict[str, str] | None = None,
    ) -> tuple[subprocess.CompletedProcess[str], list[str]]:
        """Drive one real prepare route over stubbed Homebrew and provisioning."""
        state_root = harness / "state" if state_root is None else state_root
        calls = harness / f"calls-{route}"
        formula = lock["homebrew"]["llvmFormula"]
        tools: dict[str, Path] = {
            formula: llvm,
            "cmake": harness / "cmake",
            "ninja": harness / "ninja",
            "pkgconf": harness / "pkgconf",
            "autoconf": harness / "autoconf",
            "autoconf-archive": harness / "autoconf-archive",
            "automake": harness / "automake",
            "libtool": harness / "libtool",
        }
        branches = []
        for name, path in tools.items():
            action = "return 91" if name == fail_formula else f"printf '%s\\n' {shlex.quote(str(path))}"
            branches.append(f"        {shlex.quote(name)}) {action} ;;")
        joined_branches = "\n".join(branches)
        script = f"""
set -eu
source {shlex.quote(str(ROOT / "script/ao/macos-vcpkg-bootstrap.sh"))}
aobus_macos_formula_prefix() {{
    printf '%s\\n' "$2" >> {shlex.quote(str(calls))}
    case "$2" in
{joined_branches}
        *) return 91 ;;
    esac
}}
aobus_macos_install_pinned_vcpkg() {{
    printf 'install:%s\\n' "$*"
    mkdir -p "$1/scripts/buildsystems"
    printf '#!/bin/sh\\n' > "$1/vcpkg"
    chmod +x "$1/vcpkg"
    printf 'fixture\\n' > "$1/scripts/buildsystems/vcpkg.cmake"
}}
aobus_macos_prepare_expected_shim() {{
    printf 'shim:%s|%s|%s\\n' "$1" "$2" "$3"
}}
uname() {{ printf '%s\\n' x86_64; }}
aobus_macos_prepare_{route}_environment {shlex.quote(str(project))}
printf 'triplet=%s\\ncc=%s\\ncxx=%s\\ndeployment=%s\\nstate=%s\\nllvm=%s\\n' \\
    "$AOBUS_VCPKG_TRIPLET" "$CC" "$CXX" "$MACOSX_DEPLOYMENT_TARGET" "$AOBUS_STATE_ROOT" "$AOBUS_LLVM_ROOT"
printf 'path=%s\\n' "$PATH"
"""
        if route == "build":
            script += """printf 'vcpkg=%s\\ndownloads=%s\\nbinary=%s\\nmetrics=%s\\n' \\
    "$VCPKG_ROOT" "$VCPKG_DOWNLOADS" "$VCPKG_DEFAULT_BINARY_CACHE" "$VCPKG_DISABLE_METRICS"
"""
        child = dict(
            os.environ, AOBUS_PYTHON=sys.executable, AOBUS_HOMEBREW="/unused/brew", AOBUS_STATE_ROOT=str(state_root)
        )
        child.update(environment or {})
        result = subprocess.run(
            [str(shutil.which("bash")), "-c", script], env=child, capture_output=True, text=True, check=False
        )
        queried = calls.read_text(encoding="utf-8").splitlines() if calls.exists() else []
        return result, queried

    def _admitted_deployments(self) -> dict[str, list[str]]:
        deployment = json.loads((ROOT / "script/ao/macos-toolchain.json").read_text(encoding="utf-8"))[
            "deploymentTarget"
        ]
        return {"x64-aobus-osx.cmake": [deployment], "arm64-aobus-osx.cmake": [deployment]}

    def test_checked_in_triplets_admit_the_real_toolchain_lock_before_brew(self):
        lock = json.loads((ROOT / "script/ao/macos-toolchain.json").read_text(encoding="utf-8"))
        harness, llvm = self._harness(ROOT)
        result, queried = self._run_route(ROOT, "bazel", harness=harness, llvm=llvm, lock=lock)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn(f"deployment={lock['deploymentTarget']}", result.stdout)
        self.assertIn(f"shim:{llvm}|", result.stdout)
        self.assertEqual(queried, [lock["homebrew"]["llvmFormula"], "pkgconf"])

    def test_extra_deployment_spellings_cannot_hide_an_override_from_admission(self):
        for override in (
            "set(VCPKG_OSX_DEPLOYMENT_TARGET 14.0) # override\n",
            "SET(VCPKG_OSX_DEPLOYMENT_TARGET 14.0)\n",
            "set(\n    VCPKG_OSX_DEPLOYMENT_TARGET 14.0\n)\n",
        ):
            with self.subTest(override=override):
                project, lock = self._fixture(self._admitted_deployments())
                triplet = project / "cmake/vcpkg-triplets/x64-aobus-osx.cmake"
                with triplet.open("a", encoding="utf-8") as output:
                    output.write(override)
                harness, llvm = self._harness(project)
                result, queried = self._run_route(project, "bazel", harness=harness, llvm=llvm, lock=lock)
                self.assertNotEqual(result.returncode, 0)
                self.assertIn("exactly one VCPKG_OSX_DEPLOYMENT_TARGET assignment", result.stderr)
                self.assertEqual(queried, [])
                self.assertNotIn("shim:", result.stdout)

    def test_both_routes_publish_the_admitted_shared_exports_and_only_the_build_route_provisions(self):
        project, lock = self._fixture(self._admitted_deployments())
        harness, llvm = self._harness(project)
        state = harness / "state"
        results: dict[str, subprocess.CompletedProcess[str]] = {}
        exports: dict[str, dict[str, str]] = {}
        queried_by_route: dict[str, list[str]] = {}
        for route in ("bazel", "build"):
            result, queried = self._run_route(project, route, harness=harness, llvm=llvm, lock=lock, state_root=state)
            self.assertEqual(result.returncode, 0, result.stderr)
            results[route] = result
            queried_by_route[route] = queried
            exports[route] = dict(line.split("=", 1) for line in result.stdout.splitlines() if "=" in line)
        shared = {
            "triplet": "x64-aobus-osx",
            "cc": f"{llvm}/bin/clang",
            "cxx": f"{llvm}/bin/clang++",
            "deployment": lock["deploymentTarget"],
            "state": str(state),
            "llvm": str(llvm),
        }
        for route, values in exports.items():
            with self.subTest(route=route):
                for key, value in shared.items():
                    self.assertEqual(values[key], value)
                self.assertIn(f"shim:{llvm}|{lock['homebrew']['llvmMajorVersion']}.1.8|{state}", results[route].stdout)
        # The ready-only Bazel route keeps its two-entry tool PATH and never
        # provisions; the build route keeps the full ordering and bootstraps.
        self.assertEqual(queried_by_route["bazel"], [lock["homebrew"]["llvmFormula"], "pkgconf"])
        self.assertEqual(
            sorted(queried_by_route["build"]),
            sorted(
                [
                    lock["homebrew"]["llvmFormula"],
                    "pkgconf",
                    "cmake",
                    "ninja",
                    "autoconf",
                    "autoconf-archive",
                    "automake",
                    "libtool",
                ]
            ),
        )
        self.assertEqual(exports["bazel"]["path"], f"{llvm}/bin:{harness}/pkgconf/bin:{os.environ['PATH']}")
        self.assertNotIn("install:", results["bazel"].stdout)
        self.assertEqual(
            exports["build"]["path"],
            ":".join(
                (
                    f"{llvm}/bin",
                    f"{harness}/cmake/bin",
                    f"{harness}/ninja/bin",
                    f"{harness}/pkgconf/bin",
                    f"{harness}/autoconf/bin",
                    f"{harness}/automake/bin",
                    f"{harness}/libtool/libexec/gnubin",
                    os.environ["PATH"],
                )
            ),
        )
        revision = lock["vcpkg"]["revision"]
        self.assertIn(
            "install:"
            + " ".join(
                (
                    str(state / "tools/vcpkg" / revision),
                    lock["vcpkg"]["archiveUrl"],
                    lock["vcpkg"]["archiveSha256"],
                    str(state),
                    revision,
                )
            ),
            results["build"].stdout,
        )
        self.assertEqual(exports["build"]["vcpkg"], str(state / "tools/vcpkg" / revision))
        self.assertEqual(exports["build"]["downloads"], str(state / "cache/vcpkg/downloads"))
        self.assertEqual(exports["build"]["binary"], str(state / "cache/vcpkg/binaries"))
        self.assertEqual(exports["build"]["metrics"], "1")
        self.assertTrue((state / "cache/vcpkg/downloads").is_dir())
        self.assertTrue((state / "cache/vcpkg/binaries").is_dir())

    def test_the_state_root_honors_the_override_and_the_home_default(self):
        project, lock = self._fixture(self._admitted_deployments())
        harness, llvm = self._harness(project)
        home = harness / "home"
        overridden, _ = self._run_route(
            project, "bazel", harness=harness, llvm=llvm, lock=lock, state_root=harness / "chosen"
        )
        self.assertEqual(overridden.returncode, 0, overridden.stderr)
        self.assertIn(f"state={harness}/chosen", overridden.stdout)
        defaulted, _ = self._run_route(
            project,
            "bazel",
            harness=harness,
            llvm=llvm,
            lock=lock,
            state_root=home,
            environment={"AOBUS_STATE_ROOT": "", "HOME": str(home)},
        )
        self.assertEqual(defaulted.returncode, 0, defaulted.stderr)
        self.assertIn(f"state={home}/Library/Caches/Aobus", defaulted.stdout)
        rejected, _ = self._run_route(
            project,
            "bazel",
            harness=harness,
            llvm=llvm,
            lock=lock,
            state_root=home,
            environment={"AOBUS_STATE_ROOT": "", "HOME": ""},
        )
        self.assertNotEqual(rejected.returncode, 0)
        self.assertIn("HOME is unset", rejected.stderr)


class BazelPortalEnvironmentTests(unittest.TestCase):
    def test_native_dispatch_skips_build_env_discovery_and_platform_inventory(self) -> None:
        # Every flat command binds the same dispatch boundary; one representative
        # native command witnesses that no implicit build-env or platform
        # inventory sneaks in before the command's own run.
        with (
            patch.object(portal.buildenv, "requires_parsed_build_env", side_effect=AssertionError("CMake lookup")),
            patch.object(platform, "system", side_effect=AssertionError("no OS inventory")),
            patch.object(platform, "uname", side_effect=AssertionError("no OS inventory")),
            patch.object(platform, "machine", side_effect=AssertionError("no implicit ISA inventory")),
            patch("ao.command.build.run_command", return_value=0) as run,
        ):
            self.assertEqual(portal.main(["build"]), 0)
        run.assert_called_once()


if __name__ == "__main__":
    unittest.main()
