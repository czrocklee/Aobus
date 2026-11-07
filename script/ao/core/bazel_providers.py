"""Admit governed Nix and installed vcpkg providers without a portal workspace."""

from __future__ import annotations

import os
import re
import subprocess
from pathlib import Path
from typing import Any

from . import bazel_native as native
from . import dependency_policy

POLICY = native.PROJECT_ROOT / "config/bazel/dependencies.json"


def load_policy() -> dict[str, Any]:
    policy = native.read_json(POLICY)
    if policy.get("schema") != 3:
        raise native.BazelWorkspaceError("unsupported Bazel dependency policy schema")
    pending: list[Any] = [policy]
    while pending:
        value = pending.pop()
        if isinstance(value, dict):
            if value.keys() & {
                "version",
                "versions",
                "provider_version",
                "status_version",
                "status_versions",
                "metadata_versions",
            }:
                raise native.BazelWorkspaceError(
                    "dependency version policy belongs to the existing contract/provider inputs"
                )
            pending.extend(value.values())
        elif isinstance(value, list):
            pending.extend(value)
    return policy


def nix_root(path: Path) -> Path:
    resolved = path.resolve()
    if not resolved.is_relative_to("/nix/store") or len(resolved.parts) < 4:
        raise native.BazelWorkspaceError(f"provider path is outside the pinned Nix store: {path}")
    return Path(*resolved.parts[:4])


def nix_dependency(report: dict[str, Any], source: str) -> tuple[Path, str, str, list[Path]]:
    """Trust report-owned output membership, not the local store DB's deriver records."""
    message = f"{source} output identity is unavailable in the Nix dependency report; re-enter through ./ao"
    dependencies = report.get("dependencies")
    entry = dependencies.get(source) if isinstance(dependencies, dict) else None
    if not isinstance(entry, dict):
        raise native.BazelWorkspaceError(message)

    def store_path(value: object, *, derivation: bool = False) -> Path:
        # Reject nested/normalized spellings before _nix_root can promote them.
        if not isinstance(value, str) or not re.fullmatch(r"/nix/store/[A-Za-z0-9+._?=-]+", value):
            raise native.BazelWorkspaceError(message)
        path = Path(value)
        if value.rsplit("/", 1)[1] in {".", ".."} or (
            derivation and (not value.endswith(".drv") or path.name == ".drv")
        ):
            raise native.BazelWorkspaceError(message)
        return path

    version = entry.get("version")
    if not isinstance(version, str) or not version or any(c in version for c in "\r\n\x00"):
        raise native.BazelWorkspaceError(message)
    selected = store_path(entry.get("storePath"))
    derivation = store_path(entry.get("drvPath"), derivation=True)
    reported = entry.get("outputs")
    if not isinstance(reported, dict) or not reported:
        raise native.BazelWorkspaceError(message)
    declared = []
    for name, value in reported.items():
        if not isinstance(name, str) or not re.fullmatch(r"[A-Za-z_][A-Za-z0-9_+-]*", name):
            raise native.BazelWorkspaceError(message)
        declared.append(store_path(value))
    # Optional outputs need not be installed. Validate every spelling first,
    # then retain the existing canonical-root and existing-directory behavior.
    outputs = sorted({nix_root(path) for path in declared if path.is_dir()})
    root = nix_root(selected)
    if root not in outputs:
        raise native.BazelWorkspaceError(message)
    return root, version, derivation.as_posix(), outputs


def package_groups(policy: dict[str, Any], *, host: str | None = None) -> dict[str, list[dict[str, Any]]]:
    groups = policy["packages"]
    if set(groups) != {"native", "test"} or any(not isinstance(value, list) for value in groups.values()):
        raise native.BazelWorkspaceError("unsupported Bazel dependency groups")
    result = {group: value for group, value in groups.items()}
    names = [package["name"] for packages in result.values() for package in packages]
    if len(names) != len(set(names)) or set(names) - {
        "icu",
        "icu_uc",
        "cli11",
        "spdlog",
        "ftxui",
        "mimalloc",
        "stb",
        "date_tz",
        "giomm",
        "gtkmm",
        "boost_process",
        "lmdb",
        "rapidyaml",
        "boost",
        "gsl_lite",
        "fast_float",
        "xxhash",
        "lexy",
        "catch2",
        "flac",
        "alac",
        "fdk_aac",
        "mpg123",
        "opus",
        "alsa",
        "pipewire",
        "udev",
        "fakeit",
        "xtest",
    }:
        raise native.BazelWorkspaceError("unknown or duplicate Bazel dependency package")
    host = native.native_platform() if host is None else host
    if host not in {"linux", "macos", "windows"}:
        raise native.BazelWorkspaceError(f"unsupported dependency platform: {host}")
    # These filters follow actual OS-specific audio, GTK, MPRIS and time-zone consumers.
    # No arbitrary per-package platform policy is supported by this projection.
    for group, packages in result.items():
        for package in packages:
            if package["name"] == "giomm":
                if package.get("nix") != "glibmm" or package.get("nix_inputs") != ["glib", "libsigcxx"]:
                    raise native.BazelWorkspaceError("giomm requires exactly the glibmm/glib/libsigcxx Nix inputs")
            elif package["name"] == "gtkmm":
                if (
                    package.get("nix") != "gtkmm"
                    or package.get("modules") != ["gtkmm-4.0", "cairo-ft", "cairo-fc"]
                    or package.get("nix_inputs")
                    != [
                        "gtk4",
                        "glibmm",
                        "glib",
                        "libsigcxx",
                        "cairo",
                        "cairomm",
                        "pango",
                        "pangomm",
                        "gdk-pixbuf",
                        "graphene",
                        "harfbuzz",
                        "freetype",
                        "fontconfig",
                        "vulkan-headers",
                        "vulkan-loader",
                    ]
                ):
                    raise native.BazelWorkspaceError("gtkmm requires its closed GTK/glibmm rendering SDK Nix inputs")
            elif package["name"] == "xtest":
                if (
                    package.get("nix") != "libXtst"
                    or package.get("modules") != ["xtst", "x11", "xi"]
                    or package.get("nix_inputs") != ["libX11", "libXi", "xorgproto"]
                ):
                    raise native.BazelWorkspaceError(
                        "xtest requires exactly the libXtst/libX11/libXi/xorgproto Linux test interface"
                    )
            elif "nix_inputs" in package:
                raise native.BazelWorkspaceError(f"unsupported additional Nix dependency inputs: {package['name']}")
            if package["name"] == "boost_process" and (
                package.get("nix") != "boost"
                or package.get("modules") != ["boost_process"]
                or package.get("vcpkg") != ["boost-process", "boost-filesystem"]
            ):
                raise native.BazelWorkspaceError("boost_process requires the selected Boost Process SDK")
            platforms = package.get("platforms")
            if package["name"] in {
                "alsa",
                "pipewire",
                "udev",
                "giomm",
                "gtkmm",
                "boost_process",
                "date_tz",
                "xtest",
            }:
                required = {
                    "date_tz": ["macos"],
                    "boost_process": ["linux", "macos", "windows"],
                    "xtest": ["linux"],
                }.get(package["name"], ["linux"])
                expected_group = "test" if package["name"] == "xtest" else "native"
                if group != expected_group or platforms != required:
                    raise native.BazelWorkspaceError(
                        f"{package['name']} dependency platform contract must be {required}"
                    )
            elif platforms is not None:
                raise native.BazelWorkspaceError(f"unsupported package platform filter: {package['name']}")
        result[group] = [package for package in packages if host in package.get("platforms", [host])]
    return result


def linux_provider(
    policy: dict[str, Any], *, identity_path: Path
) -> tuple[dict[str, Any], dict[str, str], dict[str, list[Path]]]:
    report_path = Path(os.environ.get("AOBUS_NIX_DEPENDENCY_REPORT", ""))
    if not report_path.is_file():
        raise native.BazelWorkspaceError("AOBUS_NIX_DEPENDENCY_REPORT is missing; enter the pinned shell through ./ao")
    report = native.read_json(report_path)
    pin = native.read_json(native.PROJECT_ROOT / "nixpkgs.json")
    if report.get("schemaVersion") != 1 or report.get("nixpkgsRevision") != pin["rev"]:
        raise native.BazelWorkspaceError("Nix dependency report does not match nixpkgs.json; re-enter through ./ao")
    packages = [package for group in package_groups(policy, host="linux").values() for package in group]
    versions, expected_roots, derivers = {}, {}, {}
    outputs: dict[str, list[Path]] = {}
    package_outputs: dict[str, list[Path]] = {}
    input_identities: dict[str, dict[str, Any]] = {}
    for package in packages:
        name, source = package["name"], package["nix"]
        root, version, derivation, selected = nix_dependency(report, source)
        versions[name] = version
        expected_roots[name] = root
        derivers[name] = derivation
        package_outputs[name] = selected
        outputs[name] = selected
        extra = package.get("nix_inputs", [])
        for input_name in extra:
            input_root, input_version, input_deriver, input_outputs = nix_dependency(report, input_name)
            input_identities[input_name] = {
                "storePath": str(input_root),
                "version": input_version,
                "deriver": input_deriver,
                "outputs": [str(path) for path in input_outputs],
            }
            outputs[name] = sorted(set(outputs[name]) | set(input_outputs))
    roots = {root for selected in outputs.values() for root in selected}
    pc_dirs = {
        str(root / directory)
        for root in roots
        for directory in ("lib/pkgconfig", "share/pkgconfig")
        if (root / directory).is_dir()
    }
    library_dirs = {str(root / "lib") for root in roots if (root / "lib").is_dir()}
    pkg_config = native.executable("pkg-config", nix=True)
    # Detect stale shell metadata before isolating the generator's lookup roots.
    for package in packages:
        for module in package["modules"]:
            result = subprocess.run(
                [pkg_config, "--variable=pcfiledir", module], capture_output=True, text=True, check=False
            )
            if result.returncode:
                continue  # Only the bounded complete fallbacks below may handle absent metadata.
            path = Path(result.stdout.strip())
            if not path.is_absolute() or nix_root(path) not in outputs[package["name"]]:
                raise native.BazelWorkspaceError(
                    f"{module} metadata does not match the Nix report output {expected_roots[package['name']]}"
                )
            pc_dirs.add(str(path))
    native.write_json(
        identity_path,
        {
            "schema": 1,
            "resolver": "nix",
            "nixpkgsRevision": pin["rev"],
            "inputs": input_identities,
            "packages": {
                name: {
                    "storePath": str(root),
                    "outputs": [str(p) for p in package_outputs[name]],
                    "deriver": derivers[name],
                    "version": versions[name],
                }
                for name, root in expected_roots.items()
            },
        },
    )
    evidence_paths = [report_path, identity_path, native.PROJECT_ROOT / "nixpkgs.json"]
    return (
        {
            "provider": {"name": "nix", "version": pin["rev"], "evidence": native.evidence(evidence_paths)},
            "allowed_roots": sorted(str(root) for root in roots) + [str(p.resolve()) for p in evidence_paths],
            "library_dirs": sorted(library_dirs),
            "pkg_config": {"executable": pkg_config, "pc_dirs": sorted(pc_dirs)},
            "elf": {"readelf": native.executable("readelf", nix=True)},
        },
        versions,
        outputs,
    )


def verify_linux_resolution(manifest: dict[str, Any], expected: dict[str, list[Path]]) -> None:
    for package in manifest["packages"]:
        name = package["name"]
        paths = [*package["interface"]["header_dirs"]]
        paths.extend(library["file"] for library in package["resolved_libraries"] if library["kind"] != "system")
        for value in paths:
            if nix_root(Path(value)) not in expected[name]:
                raise native.BazelWorkspaceError(
                    f"resolved library/header {value} does not match Nix report outputs {expected[name]}"
                )
    roots = {root for outputs in expected.values() for root in outputs}
    for module in manifest["modules"].values():
        metadata = Path(module["metadata"])
        if nix_root(metadata) not in roots:
            raise native.BazelWorkspaceError(f"resolved metadata {metadata} does not match the Nix report")


def vcpkg_triplets(*, host: str | None = None) -> tuple[str, str]:
    """Return the native (target, host) vcpkg triplets, matching the native CMake presets."""
    host = native.native_platform() if host is None else host
    if host == "windows":
        return "x64-windows-static-md", "x64-windows"
    if host == "macos":
        # The macOS launcher selects the project triplet from the machine architecture.
        if triplet := os.environ.get("AOBUS_VCPKG_TRIPLET"):
            return triplet, triplet
        raise native.BazelWorkspaceError("macOS vcpkg selection requires AOBUS_VCPKG_TRIPLET from the ./ao launcher")
    raise native.BazelWorkspaceError("Linux dependencies come from the pinned Nix shell, not vcpkg")


def vcpkg_prefix(explicit: str | None, tree: Path | None) -> Path:
    """Validate an explicit triplet prefix, or select the one provisioned in the Bazel tree."""
    if explicit:
        prefix = Path(explicit).resolve()
    elif tree is None:
        raise native.BazelWorkspaceError("native dependency resolution requires the prepared vcpkg prefix")
    else:
        prefix = tree / "vcpkg_installed" / vcpkg_triplets()[0]
        if not (prefix.parent / "vcpkg/status").is_file():
            raise native.BazelWorkspaceError(
                f"no vcpkg provider in {prefix.parent}; run `ao bazel provision` with the same --config/--path, "
                "or pass --vcpkg-prefix <installed/triplet>"
            )
    if not (prefix / "include").is_dir() or not (prefix.parent / "vcpkg/status").is_file():
        raise native.BazelWorkspaceError(f"not an installed vcpkg triplet prefix: {prefix}")
    return prefix.resolve()


def native_config_directories(prefix: Path, config: str) -> tuple[Path, Path]:
    selected = prefix.resolve() / "debug" if config == "debug" else prefix.resolve()
    # Do not resolve the intended roots: a Debug directory alias to Release
    # must fail before the adapter canonicalizes its allowed-root inventory.
    library_dir, runtime_dir = selected / "lib", selected / "bin"
    return native.native_path(library_dir, library_dir), native.native_path(runtime_dir, runtime_dir)


def native_provider(
    prefix: Path, config: str, host: str, *, identity_path: Path, policy: dict[str, Any] | None = None
) -> tuple[dict[str, Any], dict[str, str]]:
    prefix = prefix.resolve()
    if host == "windows" and prefix.name != "x64-windows-static-md":
        raise native.BazelWorkspaceError("Windows target dependencies require the x64-windows-static-md vcpkg triplet")
    include = native.native_path(prefix / "include", prefix / "include")
    library_dir, runtime_dir = native_config_directories(prefix, config)
    if not include.is_dir() or not library_dir.is_dir():
        raise native.BazelWorkspaceError(f"native provider headers/library directory is missing: {prefix}: {config}")
    pc_dir = native.native_path(library_dir / "pkgconfig", library_dir)
    status = prefix.parent / "vcpkg/status"
    # A status database can contain host-tool triplets alongside target ports.
    paragraphs = status.read_text(encoding="utf-8").replace("\r\n", "\n").split("\n\n")
    selected = "\n\n".join(p for p in paragraphs if f"Architecture: {prefix.name}" in p.splitlines())
    packages = dependency_policy.parse_vcpkg_status(selected)
    policy = load_policy() if policy is None else policy
    groups = package_groups(policy, host=host)
    manifest = native.read_json(native.PROJECT_ROOT / "vcpkg.json")
    requests = [*manifest["dependencies"], *manifest["features"]["tests"]["dependencies"]]
    boost_ports = {request if isinstance(request, str) else request["name"] for request in requests}
    boost_ports = {name for name in boost_ports if name.startswith("boost-")}
    versions: dict[str, str] = {}
    selected_ports: set[str] = set()
    for package in (package for group in groups.values() for package in group):
        ports = package["vcpkg"]
        if package["name"] == "boost":
            ports = sorted(set(ports) | boost_ports | {name for name in packages if name.startswith("boost-")})
        if package["name"] == "boost_process" and host == "windows":
            # The static Windows Process closure links the producer's compiled
            # dependency archives; the shared descriptor keeps its two-port
            # header contract while each archive's port owns status and headers.
            reviewed = (
                policy["library_fallbacks"].get("boost_process", {}).get("windows", {})
                if isinstance(policy.get("library_fallbacks"), dict)
                else {}
            )
            closure = reviewed.get("libraries") if isinstance(reviewed, dict) else None
            if (
                not isinstance(closure, list)
                or not closure
                or any(
                    not isinstance(library, str) or not re.fullmatch(r"boost_[a-z0-9_]+", library)
                    for library in closure
                )
            ):
                raise native.BazelWorkspaceError(
                    "Windows Boost Process requires its reviewed compiled closure libraries"
                )
            ports = [
                *ports,
                *(port for port in (library.replace("_", "-") for library in closure) if port not in ports),
            ]
        for name in ports:
            if name not in packages:
                raise native.BazelWorkspaceError(f"{name} is not installed for {prefix.name}: {status}")
            selected_ports.add(name)
        versions[package["name"]] = packages[ports[0]].upstream_version
        if package["name"] == "boost" and any(packages[name].upstream_version != versions["boost"] for name in ports):
            raise native.BazelWorkspaceError("installed Boost header ports have inconsistent upstream versions")
        if package["name"] == "boost_process" and any(
            packages[port].upstream_version != versions["boost_process"] for port in ports
        ):
            raise native.BazelWorkspaceError("installed Boost Process ports have inconsistent upstream versions")
    for request in requests:
        if not isinstance(request, dict) or request["name"] not in selected_ports:
            continue
        installed = packages[request["name"]]
        required = {"core", *request.get("features", [])}
        actual = set(installed.features)
        if not required.issubset(actual) or (request.get("default-features") is False and actual != required):
            raise native.BazelWorkspaceError(
                f"{request['name']} features {sorted(actual)} do not match vcpkg.json request {sorted(required)}"
            )
    native.write_json(
        identity_path,
        {
            "schema": 1,
            "prefix": str(prefix.resolve()),
            "triplet": prefix.name,
            "provenance": "Installed status/current manifest checked; original registry/build provenance unverified",
            "status_sha256": native.sha256(status),
            "packages": {
                name: {"version": packages[name].version, "features": list(packages[name].features)}
                for name in sorted(selected_ports)
            },
        },
    )
    identity_files = [
        status,
        identity_path,
        native.PROJECT_ROOT / "vcpkg.json",
        native.PROJECT_ROOT / "vcpkg-configuration.json",
    ]
    if host == "windows":
        host_prefix = prefix.parent / "x64-windows"
        native.native_path(host_prefix, host_prefix)
        host_status = "\n\n".join(p for p in paragraphs if "Architecture: x64-windows" in p.splitlines())
        pkgconf_package = dependency_policy.parse_vcpkg_status(host_status).get("pkgconf")
        if pkgconf_package is None:
            raise native.BazelWorkspaceError("Windows metadata queries require installed x64-windows pkgconf")
        info = prefix.parent / "vcpkg/info"
        ownership = native.native_path(
            info / f"pkgconf_{pkgconf_package.upstream_version}_{pkgconf_package.triplet}.list", info
        )
        if not ownership.is_file():
            raise native.BazelWorkspaceError(f"pkgconf host ownership manifest is missing: {ownership}")
        owned = ownership.read_text(encoding="utf-8").splitlines()
        directory = host_prefix / "tools/pkgconf"
        native.native_path(directory, directory)
        tool_directory = "x64-windows/tools/pkgconf"
        runtime = sorted(
            {
                Path(value).name
                for value in owned
                if Path(value).parent.as_posix() == tool_directory and Path(value).suffix.lower() == ".dll"
            }
            | {path.name for path in directory.iterdir() if path.suffix.lower() == ".dll"}
        )
        for name in ("pkgconf.exe", *runtime):
            path = native.native_path(directory / name, directory)
            if not path.is_file() or f"{tool_directory}/{name}" not in owned:
                raise native.BazelWorkspaceError(f"missing/unowned Windows pkgconf host input: {path}")
            identity_files.append(path)
        identity_files.append(ownership)
        pkgconf = str(directory / "pkgconf.exe")
    else:
        pkgconf = native.executable("pkgconf")
    if not Path(pkgconf).is_file():
        raise native.BazelWorkspaceError(f"native pkgconf is missing: {pkgconf}")
    return {
        "provider": {
            "name": "vcpkg",
            "version": f"{prefix.resolve()}:{native.sha256(status)}",
            "evidence": native.evidence(identity_files),
        },
        "allowed_roots": list(
            dict.fromkeys(
                [str(include), str(library_dir), str(Path(pkgconf).resolve())]
                + ([str(runtime_dir)] if runtime_dir.is_dir() else [])
                + [str(p.resolve()) for p in identity_files]
            )
        ),
        "library_roots": [str(library_dir)],
        "runtime_roots": [str(runtime_dir)] if runtime_dir.is_dir() else [],
        "library_dirs": [str(library_dir)],
        "pkg_config": {"executable": pkgconf, "pc_dirs": [str(pc_dir)]},
    }, versions
