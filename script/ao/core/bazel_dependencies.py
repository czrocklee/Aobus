"""Resolve and validate target-library interfaces from admitted native providers.

Reviewed metadata repairs and ABI checks stay beside the sole adapter consumer.
The result is observations, never a generated Bazel program or launcher state.
"""

from __future__ import annotations

import hashlib
import io
import re
import shlex
import subprocess
from pathlib import Path
from typing import Any

from . import bazel_deps, dependency_policy
from . import bazel_native as native
from . import bazel_providers as providers


def _patch(
    ident: str,
    spec: dict[str, Any],
    package: str,
    *,
    add: dict[str, Any],
    remove: dict[str, Any] | None = None,
    present: dict[str, Any] | None = None,
    missing: dict[str, Any] | None = None,
    requirements: tuple[str, ...] = (),
) -> dict[str, Any]:
    """Pair repair effects with their exact guards and satisfied requirements."""
    return {
        "id": ident,
        "when": {
            "package": package,
            "platform": spec["platform"],
            "config": spec["config"],
            "provider": spec["provider"]["name"],
            "provider_version": spec["provider"]["version"],
        },
        "expect": {
            "present": present if present is not None else remove if remove is not None else {},
            "missing": missing if missing is not None else add,
            "unmet_requirements": list(requirements),
        },
        "add": add,
        "satisfy": list(requirements),
        **({"remove": remove} if remove is not None else {}),
    }


def _windows_static_packages(policy: dict[str, Any]) -> set[str]:
    # Pinned producer exceptions, not permission for an absent shared SDK to fall back.
    expected = {"cli11", "ftxui", "alac", "rapidyaml", "catch2"}
    reviewed = policy.get("windows_static_only")
    if (
        not isinstance(reviewed, dict)
        or set(reviewed) != expected
        or any(not isinstance(reason, str) or not reason.strip() for reason in reviewed.values())
    ):
        raise native.BazelWorkspaceError("Windows static-only exceptions require the exact pinned producer contracts")
    return expected


def _windows_library_names(name: str, config: str, policy: dict[str, Any]) -> list[str]:
    suffix = "d" if config == "debug" else ""
    if name in {"icu", "icu_uc"}:
        return [value.format(debug_suffix=suffix) for value in policy["windows_icu"]["libraries"][name]]
    if name in {"flac", "fdk_aac", "mpg123", "opus"}:
        audio_libraries: list[str] = policy["windows_audio"][name]["libraries"]
        return audio_libraries
    if name == "catch2":
        return ["Catch2Main" + suffix, "Catch2" + suffix]
    libraries: list[str] = policy["library_fallbacks"][name]["windows"]["libraries"]
    if name == "spdlog":
        return ["spdlog" + suffix]
    if name == "mimalloc":
        return ["mimalloc" + ("-debug" if suffix else "")]
    return libraries


def _metadata_flags(spec: dict[str, Any], option: str, *, package: str | None = None) -> list[str]:
    environment = bazel_deps.pkg_config_environment(spec["pkg_config"]["pc_dirs"])
    selected = next((p for p in spec["packages"] if p["name"] == package), spec["packages"][0])
    static = ["--static"] if selected.get("linkage", spec["linkage"]) == "static" else []
    return shlex.split(
        subprocess.run(
            [spec["pkg_config"]["executable"], *static, option, *selected["modules"]],
            env=environment,
            capture_output=True,
            text=True,
            check=True,
        ).stdout
    )


def _windows_static_link_patch(
    spec: dict[str, Any], name: str, libraries: list[str], system_libraries: list[str]
) -> dict[str, Any] | None:
    """Translate only reviewed trailing Windows SDK imports, retaining archive order."""
    if (
        name not in {"icu", "icu_uc", "mimalloc", "flac", "fdk_aac", "mpg123", "opus"}
        or spec["platform"] != "windows"
        or spec["config"] not in {"Debug", "Release"}
        or spec["provider"]["name"] != "vcpkg"
        or spec["linkage"] != "static"
        or not set(system_libraries) <= set(spec["system_libraries"])
    ):
        raise native.BazelWorkspaceError(f"unsupported Windows static metadata interface: {name}")
    flags = _metadata_flags(spec, "--libs", package=name)
    actual = [flag.removeprefix("-l") for flag in flags if flag.startswith("-l")]
    if actual != [*libraries, *system_libraries]:
        raise native.BazelWorkspaceError(f"unreviewed Windows static library sequence/interleaving: {name}: {actual}")
    if not system_libraries:
        return None
    # The typed adapter deliberately rejects mixed static/system imports. The
    # reviewed metadata puts these SDK libraries after every provider archive,
    # so named linkopts lose no archive/system interleaving. Do not generalize
    # this translation to unknown metadata or remove the adapter's order guard.
    present = [{"name": library, "kind": "static"} for library in libraries]
    removed = [{"name": library, "kind": "system"} for library in system_libraries]
    added = {"linkopts": [library + ".lib" for library in system_libraries]}
    return _patch(
        f"windows-{name}-system-links",
        spec,
        name,
        present={"libraries": [*present, *removed]},
        remove={"libraries": removed},
        add=added,
    )


def _macos_icu_runtime_patch(
    spec: dict[str, Any], policy: dict[str, Any], *, package: str = "icu"
) -> dict[str, Any] | None:
    flags = _metadata_flags(spec, "--libs", package=package)
    implicit = {f"-l{name}" for name in policy["libraries"]}
    saw_implicit = False
    for flag in flags:
        if flag in implicit:
            saw_implicit = True
        elif saw_implicit and flag.startswith("-l"):
            raise native.BazelWorkspaceError("Darwin ICU system/provider library interleaving is unsupported")
    removed = [{"name": name, "kind": "system"} for name in policy["libraries"] if f"-l{name}" in flags]
    if not removed:
        return None
    return _patch(
        f"macos-{package}-implicit-runtime",
        spec,
        package,
        remove={"libraries": removed},
        add={},
    )


def _macos_audio_runtime_patch(spec: dict[str, Any], name: str, policy: dict[str, Any]) -> dict[str, Any]:
    if (
        name not in {"flac", "fdk_aac", "mpg123", "opus"}
        or spec["platform"] != "macos"
        or spec["config"] not in {"Debug", "Release"}
        or spec["provider"]["name"] != "vcpkg"
        or spec["linkage"] != "static"
    ):
        raise native.BazelWorkspaceError(f"unsupported macOS codec spec: {name}")
    flags = _metadata_flags(spec, "--libs", package=name)
    libraries = [flag.removeprefix("-l") for flag in flags if flag.startswith("-l")]
    if libraries != policy["libraries"]:
        raise native.BazelWorkspaceError(f"unreviewed macOS codec metadata library sequence: {name}: {libraries}")
    # Apple-targeting clang++ supplies libc++; libSystem supplies math. FLAC's
    # reviewed -lm is interleaved, so retain the relative order of its archives.
    present = [
        {"name": library, "kind": "system" if library in policy["implicit"] else "static"} for library in libraries
    ]
    removed = [library for library in present if library["name"] in policy["implicit"]]
    return _patch(
        f"macos-{name}-implicit-runtime",
        spec,
        name,
        present={"libraries": present},
        remove={"libraries": removed},
        add={},
    )


def _catch_fallback(spec: dict[str, Any], policy: dict[str, Any], prefix: Path) -> dict[str, Any]:
    values = {"debug_suffix": "d" if spec["config"] == "Debug" else ""}
    libraries = []
    native.native_path(Path(spec["library_dirs"][0]))
    for index, template in enumerate(policy[spec["platform"]]):
        file = native.native_path(Path(spec["library_dirs"][0]) / template.format(**values))
        if not file.is_file():
            raise native.BazelWorkspaceError(f"Catch2 metadata and fallback archive are missing: {file}")
        libraries.append(
            {
                "name": file.stem if spec["platform"] == "windows" else f"catch2_{index}",
                "kind": "static",
                "file": str(file),
            }
        )
    return _patch(
        "catch2-static-fallback",
        spec,
        "catch2",
        missing={},
        requirements=("manual_interface",),
        add={
            "header_dirs": [str(native.native_path(prefix / "include"))],
            "libraries": libraries,
        },
    )


def _has_metadata(spec: dict[str, Any], modules: list[str]) -> bool:
    present = []
    for module in modules:
        matches = {
            path.resolve()
            for directory in spec["pkg_config"]["pc_dirs"]
            if native.probe(path := Path(directory) / f"{module}.pc").is_file()
        }
        if len(matches) > 1:
            raise native.BazelWorkspaceError(f"ambiguous pkg-config metadata: {module}")
        present.append(bool(matches))
    if any(present) and not all(present):
        raise native.BazelWorkspaceError(f"incomplete pkg-config module group: {modules}")
    return bool(present) and all(present)


def _rapidyaml_version_header(include: Path) -> Path:
    candidates = [include / "c4/yml/version.hpp", include / "ryml/c4/yml/version.hpp"]
    markers = [path for path in candidates if native.probe(path).is_file()]
    if len(markers) != 1:
        raise native.BazelWorkspaceError("RapidYaml version header is missing/ambiguous")
    return markers[0]


def _compile_interface_patch(spec: dict[str, Any], name: str, interface: dict[str, Any]) -> dict[str, Any] | None:
    if not interface:
        return None
    flags = _metadata_flags(spec, "--cflags", package=name)
    # Normalize the split forms accepted by the artifact adapter as well.
    for option in ("-I", "-isystem", "-D"):
        flags = [
            token + flags[index + 1] if token == option and index + 1 < len(flags) else token
            for index, token in enumerate(flags)
        ]
    # The adapter canonicalizes include paths before checking patch predicates.
    # Compare the same identities here, including pkg-config's ../ spellings.
    headers = {
        Path(token[len(option) :]).resolve()
        for token in flags
        for option in ("-I", "-isystem")
        if token.startswith(option) and len(token) > len(option)
    }
    missing = {}
    for field, values in interface.items():
        if field == "header_dirs":
            absent = [value for value in values if Path(value).resolve() not in headers]
        elif field == "defines":
            absent = [value for value in values if "-D" + value not in flags]
        elif field == "copts":
            absent = [value for value in values if value not in flags]
        else:
            continue
        if absent:
            missing[field] = absent
    if not missing:
        return None
    return _patch(
        f"{name}-compile-interface",
        spec,
        name,
        add=missing,
    )


def _manual_patch(spec: dict[str, Any], name: str, interface: dict[str, Any]) -> dict[str, Any]:
    return _patch(
        f"{name}-complete-interface",
        spec,
        name,
        add=interface,
        requirements=("manual_interface",),
    )


def _metadata_versions(name: str, version: str, modules: list[str], host: str) -> dict[str, str]:
    """Compare exact producer spellings, derived from the selected provider release."""
    if name == "mimalloc":
        match = re.fullmatch(r"(\d+\.\d+)\.\d+", version)
        module = "mimalloc"
    elif host == "linux" and name == "alac":
        match = re.fullmatch(r"(\d+\.\d+\.\d+)(?:-unstable-\d{4}-\d{2}-\d{2})?", version)
        module = "alac"
    elif host == "linux" and name == "udev":
        match = re.fullmatch(r"(\d+)(?:\.\d+)*", version)
        module = "libudev"
    else:
        return {module: version for module in modules}
    if match is None or modules != [module]:
        raise native.BazelWorkspaceError(f"unsupported metadata version format/module: {name}: {host}: {version}")
    return {module: match[1]}


def _library_interface(
    package: dict[str, Any],
    roots: list[Path],
    config: str,
    host: str,
    policy: dict[str, Any],
) -> dict[str, Any]:
    """Project a complete structural interface; absent metadata never invokes CMake."""
    name = package["name"]
    reviewed = policy["library_fallbacks"].get(name, {}).get(host)
    if reviewed is None:
        raise native.BazelWorkspaceError(f"no reviewed {host} artifact interface for {name}")
    marker = {
        "lmdb": "lmdb.h",
        "xxhash": "xxhash.h",
        "rapidyaml": "ryml.hpp",
        "alac": "alac/ALACDecoder.h",
        "ftxui": "ftxui/component/component.hpp",
        "spdlog": "spdlog/spdlog.h",
        "mimalloc": "mimalloc.h",
        "date_tz": "date/tz.h",
        "cli11": "CLI/Version.hpp",
    }[name]
    if name == "rapidyaml" and not any(native.probe(root / "include/ryml.hpp").is_file() for root in roots):
        marker = "ryml/ryml.hpp"
    include = native.header_root(roots, marker)
    if host != "linux":
        native.native_path(include)
    header_dirs = [str(include)]
    if marker.startswith("ryml/"):
        header_dirs.append(str(include / "ryml"))
    kind = reviewed["linkage"]
    if kind != "static" and not (host == "windows" and kind == "shared"):
        raise native.BazelWorkspaceError(f"unsupported reviewed linkage: {name}: {host}: {kind}")
    if host == "windows" and name == "mimalloc":
        header = native.native_path(include / "mimalloc.h")
        if not re.search(
            r"#if 0[^\n]*\n\s*#define mi_decl_export\s*\n\s*#elif defined\(MI_SHARED_LIB_EXPORT\)",
            header.read_text(encoding="utf-8"),
        ):
            raise native.BazelWorkspaceError(f"shared mimalloc requires its importing installed header: {header}")
    if host == "windows" and name == "spdlog":
        tweak = native.native_path(include / "spdlog/tweakme.h").read_text(encoding="utf-8")
        common = native.native_path(include / "spdlog/common.h").read_text(encoding="utf-8")
        if (
            not re.search(r"^\s*#\s*define SPDLOG_USE_STD_FORMAT\b", tweak, re.MULTILINE)
            or re.search(r"^\s*#\s*define SPDLOG_FMT_EXTERNAL\b", tweak, re.MULTILINE)
            or not re.search(r"#\s*define SPDLOG_API __declspec\(dllimport\)", common)
        ):
            raise native.BazelWorkspaceError("Windows spdlog installed headers require the shared std::format ABI")
    if host == "windows" and name == "xxhash":
        xxhash_header = native.native_path(include / "xxhash.h").read_text(encoding="utf-8")
        if not re.search(r"#\s*elif XXH_IMPORT\s*\n#\s*define XXH_PUBLIC_API __declspec\(dllimport\)", xxhash_header):
            raise native.BazelWorkspaceError("Windows xxhash requires its installed consumer import branch")
    libraries = []
    for library in reviewed["libraries"]:
        # Debug spellings are bounded provider layouts, not a search path.
        # Reject old shared layouts rather than interpreting them as static.
        windows_static = host == "windows" and kind == "static"
        if name in {"cli11", "spdlog", "ftxui", "mimalloc", "date_tz"}:
            suffix = ("d" if name == "spdlog" else "-debug" if name == "mimalloc" else "") if config == "debug" else ""
            spellings = [library + suffix]
        else:
            spellings = [library, library + "d"] if config == "debug" and host == "macos" else [library]
        candidates: set[Path] = set()
        for root in roots:
            selected = root / "debug" if config == "debug" and host != "linux" else root
            if windows_static:
                debug_spelling = library + ("-debug" if name == "mimalloc" else "d")
                alternate = library if config == "debug" and name in {"spdlog", "mimalloc"} else debug_spelling
                unexpected = [selected / "lib" / (alternate + ".lib")]
                for spelling in (library, debug_spelling):
                    unexpected.extend(
                        [selected / "lib" / (spelling + ".dll.lib"), selected / "bin" / (spelling + ".dll")]
                    )
                if name == "mimalloc":
                    unexpected.append(selected / "bin/mimalloc-redirect.dll")
                if any(native.probe(path).exists() or path.is_symlink() for path in unexpected):
                    raise native.BazelWorkspaceError(
                        f"unsupported static provider shape: {name}: {selected}: {library}"
                    )
            for spelling in spellings:
                extension = ".dll.lib" if host == "windows" and name == "mimalloc" else ".lib"
                file = selected / "lib" / (spelling + extension if host == "windows" else "lib" + spelling + ".a")
                if native.probe(file).is_file():
                    candidates.add(native.native_path(file) if host != "linux" else file.resolve())
        if len(candidates) != 1:
            raise native.BazelWorkspaceError(
                f"missing/ambiguous reviewed {host} {config} {library} artifact: {sorted(map(str, candidates))}"
            )
        file = candidates.pop()
        if not any(file.is_relative_to(root.resolve()) for root in roots):
            raise native.BazelWorkspaceError(f"artifact escapes selected provider: {file}")
        logical_name = spellings[0] if name in {"spdlog", "mimalloc"} else library
        libraries.append({"name": logical_name, "kind": kind, "file": str(file)})
    result: dict[str, Any] = {
        "header_dirs": header_dirs,
        "defines": reviewed.get("defines", []),
        "libraries": libraries,
    }
    if reviewed.get("linkopts"):
        result["linkopts"] = reviewed["linkopts"]
    if reviewed.get("runtime"):
        raise native.BazelWorkspaceError(f"unsupported extra frontend runtime interface: {name}: {host}")
    return result


def _linux_boost_process_interface(spec: dict[str, Any], roots: list[Path], version: str) -> dict[str, Any]:
    """The detached launcher consumes compiled Process v2 with Boost.Filesystem, never std::filesystem."""
    if spec["platform"] != "linux" or spec["provider"]["name"] != "nix" or spec["linkage"] != "shared":
        raise native.BazelWorkspaceError("Boost Process adapter requires the Linux shared Nix SDK")
    include = native.header_root(roots, "boost/version.hpp")

    def owned(path: Path) -> Path:
        resolved = native.probe(path).resolve()
        if not path.is_file() or not any(resolved.is_relative_to(root) for root in roots):
            raise native.BazelWorkspaceError(f"Boost Process input escapes/missing from selected Boost outputs: {path}")
        return resolved

    native.verify_header_version(owned(include / "boost/version.hpp"), version, ["BOOST_VERSION"], encoded=True)
    for marker in ("default_launcher.hpp", "environment.hpp", "process.hpp", "stdio.hpp"):
        owned(include / "boost/process/v2" / marker)
    configuration = owned(include / "boost/process/v2/detail/config.hpp").read_text(encoding="utf-8")
    if any(
        marker not in configuration
        for marker in ("BOOST_PROCESS_V2_DECL", "BOOST_PROCESS_DYN_LINK", "namespace filesystem = boost::filesystem;")
    ):
        raise native.BazelWorkspaceError("Boost Process requires the installed compiled v2 Boost.Filesystem API")
    owned(include / "boost/filesystem/path.hpp")
    libraries = []
    for name, symbol in (
        ("boost_process", "boost::process::v2::process_environment::do_setup("),
        ("boost_filesystem", "boost::filesystem::path::"),
    ):
        candidates = {
            owned(path) for root in roots if native.probe(path := root / "lib" / ("lib" + name + ".so")).is_file()
        }
        if len(candidates) != 1:
            raise native.BazelWorkspaceError(f"missing/ambiguous shared Boost Process artifact: {name}")
        file = candidates.pop()
        result = subprocess.run(
            [spec["elf"]["readelf"], "--dynamic", "--dyn-syms", "--wide", "--demangle", str(file)],
            env=native.child_environment(),
            capture_output=True,
            text=True,
            check=False,
        )
        sonames = re.findall(r"\(SONAME\).*?\[([^\]]+)\]", result.stdout)
        exported = re.search(r"\b(?:GLOBAL|WEAK)\s+DEFAULT\s+\d+\s+" + re.escape(symbol), result.stdout)
        if result.returncode or sonames != [f"lib{name}.so.{version}"] or exported is None:
            raise native.BazelWorkspaceError(f"Boost Process shared library ABI/version mismatch: {file}")
        libraries.append({"name": name, "kind": "shared", "file": str(file)})
    return {
        "header_dirs": [str(include)],
        "defines": [
            "BOOST_PROCESS_NO_LIB",
            "BOOST_PROCESS_DYN_LINK",
            "BOOST_FILESYSTEM_NO_LIB",
            "BOOST_FILESYSTEM_DYN_LINK",
        ],
        "libraries": libraries,
    }


def _static_boost_process_headers(prefix: Path, version: str) -> Path:
    """Shared static Process v2 headers over compiled Boost.Filesystem, never std::filesystem."""
    include = native.native_path(native.header_root([prefix], "boost/version.hpp"))

    def owned(path: Path) -> Path:
        resolved = native.probe(path).resolve()
        if not path.is_file() or not resolved.is_relative_to(include):
            raise native.BazelWorkspaceError(
                f"Boost Process input escapes/missing from the selected vcpkg prefix: {path}"
            )
        return resolved

    native.verify_header_version(owned(include / "boost/version.hpp"), version, ["BOOST_VERSION"], encoded=True)
    for marker in ("default_launcher.hpp", "environment.hpp", "process.hpp", "stdio.hpp"):
        owned(include / "boost/process/v2" / marker)
    configuration = owned(include / "boost/process/v2/detail/config.hpp").read_text(encoding="utf-8")
    if any(
        marker not in configuration
        for marker in ("BOOST_PROCESS_V2_DECL", "BOOST_PROCESS_DYN_LINK", "namespace filesystem = boost::filesystem;")
    ):
        raise native.BazelWorkspaceError("Boost Process requires the installed compiled v2 Boost.Filesystem API")
    owned(include / "boost/filesystem/path.hpp")
    filesystem_configuration = owned(include / "boost/filesystem/config.hpp")
    # Narrow match of the installed config's static default branch: the same
    # branch the compiled archive and the installed package's static export consume.
    if (
        re.search(
            r"#if !defined\(BOOST_FILESYSTEM_DYN_LINK\) && !defined\(BOOST_FILESYSTEM_STATIC_LINK\) && "
            r"!defined\(BOOST_ALL_DYN_LINK\) && !defined\(BOOST_ALL_STATIC_LINK\)\s*\n"
            r"#define BOOST_FILESYSTEM_STATIC_LINK\b",
            filesystem_configuration.read_text(encoding="utf-8"),
        )
        is None
    ):
        raise native.BazelWorkspaceError(
            "static Boost Process requires the installed Boost.Filesystem static-link config: "
            f"{filesystem_configuration}"
        )
    return include


def _macos_boost_process_interface(
    spec: dict[str, Any], prefix: Path, config: str, version: str, policy: dict[str, Any]
) -> dict[str, Any]:
    """AppKit consumes compiled Process v2 as static archives over Boost.Filesystem, never dylibs."""
    if spec["platform"] != "macos" or spec["provider"]["name"] != "vcpkg" or spec["linkage"] != "static":
        raise native.BazelWorkspaceError("macOS Boost Process adapter requires the reviewed static vcpkg SDK")
    reviewed = policy["library_fallbacks"].get("boost_process", {}).get("macos")
    if (
        not isinstance(reviewed, dict)
        or reviewed.get("linkage") != "static"
        or reviewed.get("libraries") != ["boost_process", "boost_filesystem"]
        or not isinstance(reviewed.get("defines"), list)
        or not reviewed["defines"]
    ):
        raise native.BazelWorkspaceError("macOS Boost Process requires its reviewed two-archive static interface")
    # The static archive was compiled against Boost.Filesystem with its default
    # STATIC_LINK branch; DYN_LINK, USE_STD_FS, standalone or a filesystem
    # version pin would compile consumers to a different ABI/semantics.
    forbidden = {
        "BOOST_PROCESS_DYN_LINK",
        "BOOST_FILESYSTEM_DYN_LINK",
        "BOOST_PROCESS_USE_STD_FS",
        "BOOST_PROCESS_V2_STANDALONE",
        "BOOST_FILESYSTEM_VERSION",
    }
    if forbidden & {define.split("=", 1)[0] for define in reviewed["defines"]}:
        raise native.BazelWorkspaceError(
            "macOS Boost Process consumer definitions must remain static without a filesystem version pin"
        )
    include = _static_boost_process_headers(prefix, version)
    library_dir, _ = providers.native_config_directories(prefix, config)
    libraries = []
    for name in reviewed["libraries"]:
        # Exact configuration archive spelling; no dylib, CMake export or ambient route.
        file = native.native_path(library_dir / ("lib" + name + ".a"))
        if not file.is_file():
            raise native.BazelWorkspaceError(f"missing/ambiguous reviewed macos {config} {name} artifact: {file}")
        # The .a suffix alone is not provenance: require the ar magic so a
        # same-configuration dylib alias or non-archive cannot pass as static.
        with file.open("rb") as archive:
            if archive.read(8) != b"!<arch>\n":
                raise native.BazelWorkspaceError(
                    f"reviewed macos {config} {name} artifact is not a static ar archive: {file}"
                )
        libraries.append({"name": name, "kind": "static", "file": str(file)})
    return {"header_dirs": [str(include)], "defines": list(reviewed["defines"]), "libraries": libraries}


def _windows_coff_object_member(archive: Any, file: Path, name: str, config: str, member: int) -> str:
    """Classify one member; its Version separates BigObj objects from short imports."""
    if member < 20:
        raise native.BazelWorkspaceError(f"truncated reviewed windows {config} {name} object member: {file}")
    header = archive.read(20)
    if len(header) != 20:
        raise native.BazelWorkspaceError(f"truncated reviewed windows {config} {name} object member: {file}")
    if header[0:4] == b"\x00\x00\xff\xff":
        # Short imports and BigObj share the signature. BigObj additionally
        # requires its full 56-byte header, Version >= 2 and the COFF UUID.
        version = int.from_bytes(header[4:6], "little")
        if version == 0:
            # A valid AMD64 short import object is an import-library member;
            # unknown machines stay rejected, never classified.
            if int.from_bytes(header[6:8], "little") != 0x8664:
                raise native.BazelWorkspaceError(
                    f"unsupported reviewed windows {config} {name} import member machine: {file}"
                )
            return "interface"
        if version < 2:
            raise native.BazelWorkspaceError(
                f"unsupported reviewed windows {config} {name} anonymous COFF member: {file}"
            )
        if member < 56:
            raise native.BazelWorkspaceError(f"truncated reviewed windows {config} {name} BigObj header: {file}")
        header += archive.read(36)
        if (
            len(header) != 56
            or int.from_bytes(header[6:8], "little") != 0x8664
            or header[12:28] != bytes.fromhex("c7a1bad1eebaa94baf20faf66aa4dcb8")
        ):
            raise native.BazelWorkspaceError(f"malformed reviewed windows {config} {name} BigObj header: {file}")
        sections = int.from_bytes(header[44:48], "little")
        header_size = 56
    else:
        if int.from_bytes(header[0:2], "little") != 0x8664:
            raise native.BazelWorkspaceError(
                f"reviewed windows {config} {name} archive member is not an x64 object: {file}"
            )
        sections = int.from_bytes(header[2:4], "little")
        header_size = 20
        if int.from_bytes(header[16:18], "little") != 0:
            raise native.BazelWorkspaceError(f"malformed reviewed windows {config} {name} object member: {file}")
    if sections < 1 or header_size + 40 * sections > member:
        raise native.BazelWorkspaceError(f"malformed reviewed windows {config} {name} object member: {file}")
    for _ in range(sections):
        section = archive.read(40)
        if len(section) != 40:
            raise native.BazelWorkspaceError(f"truncated reviewed windows {config} {name} object sections: {file}")
        section_name = section[0:8].rstrip(b"\x00")
        if section_name == b".idata" or section_name.startswith(b".idata$"):
            # Full import-descriptor objects are import-library payloads too.
            return "interface"
    return "static"


def coff_archive_kind(file: Path, name: str, config: str, *, data: bytes | None = None) -> str:
    """Classify a reviewed .lib archive as 'static' or 'interface' by validating every payload member.

    Archive magic and the .lib suffix are shared by both shapes; only the bounded
    member inventory classifies. 'static' holds native object members only;
    'interface' holds import-library members, possibly mixed with objects. This
    classifies payload shape, not full ABI/build provenance; the installed port
    status, owned filenames and the actual native link remain required evidence.
    """
    captured = file.read_bytes() if data is None else data
    with io.BytesIO(captured) as archive:
        if archive.read(8) != b"!<arch>\n":
            raise native.BazelWorkspaceError(
                f"reviewed windows {config} {name} artifact is not a static COFF archive: {file}"
            )
        size = len(captured)
        offset, classified, imported = 8, 0, False
        while offset < size:
            archive.seek(offset)
            header = archive.read(60)
            if len(header) != 60 or header[58:60] != b"`\n":
                raise native.BazelWorkspaceError(f"malformed reviewed windows {config} {name} archive member: {file}")
            digits = header[48:58].decode("ascii", "replace").strip()
            if not digits.isdecimal():
                raise native.BazelWorkspaceError(
                    f"malformed reviewed windows {config} {name} archive member size: {file}"
                )
            member = int(digits)
            end = offset + 60 + member
            if end > size or end + (member & 1) > size:
                raise native.BazelWorkspaceError(f"truncated reviewed windows {config} {name} archive member: {file}")
            # Linker symbol and long-name index members carry no object payload.
            if header[0:16].decode("ascii", "replace").rstrip(" ") not in {"/", "//", "/SYM64/"}:
                archive.seek(offset + 60)
                if _windows_coff_object_member(archive, file, name, config, member) == "interface":
                    imported = True
                classified += 1
            offset = end + (member & 1)
        if not classified:
            raise native.BazelWorkspaceError(
                f"no native object members in reviewed windows {config} {name} archive: {file}"
            )
        return "interface" if imported else "static"


def _windows_binary_inventory(prefix: Path, ports: dict[str, str]) -> tuple[dict[str, Path], list[Path]]:
    """Select owned binaries without walking the SDK or accepting stale port inventories."""
    info = prefix.parent / "vcpkg/info"
    native.native_path(info)
    inventory: dict[str, Path] = {}
    manifests = []
    names = [path.name for path in info.iterdir()]
    for port, version in sorted(ports.items()):
        expected = f"{port}_{version.split('#', 1)[0]}_{prefix.name}.list"
        matches = sorted(
            name
            for name in names
            if name.casefold().startswith(port + "_") and name.casefold().endswith(f"_{prefix.name}.list")
        )
        if matches != [expected]:
            raise native.BazelWorkspaceError(f"missing/stale/ambiguous binary ownership for {port}: {matches}")
        manifest = native.native_path(info / expected)
        manifests.append(manifest)
        for line in manifest.read_text(encoding="utf-8").splitlines():
            # Each entry is "<triplet>/<path below the installed prefix>"; directories end in "/".
            if line.endswith("/") or Path(line).suffix.lower() not in {".lib", ".dll", ".exe"}:
                continue
            path = prefix.joinpath(*line.split("/")[1:])
            inventory[path.relative_to(prefix).as_posix().casefold()] = path
    return inventory, manifests


def _windows_coff_evidence(file: Path, name: str, config: str) -> dict[str, Any]:
    """Classify, parse and hash one immutable acquisition, never later file metadata."""
    captured = file.read_bytes()
    kind = coff_archive_kind(file, name, config, data=captured)
    dlls: set[str] = set()
    symbols: set[str | int] = set()
    directives = []
    with io.BytesIO(captured) as archive:
        offset, size = 8, len(captured)
        while offset < size:
            archive.seek(offset)
            header = archive.read(60)
            member = int(header[48:58].decode("ascii").strip())
            offset += 60 + member + (member & 1)
            if header[:16].decode("ascii").rstrip(" ") in {"/", "//", "/SYM64/"}:
                continue
            payload = archive.read(member)
            if payload[:6] == b"\x00\x00\xff\xff\x00\x00":
                length = int.from_bytes(payload[12:16], "little")
                fields = payload[20:].split(b"\x00")
                flags = int.from_bytes(payload[18:20], "little")
                mode = (flags >> 2) & 7
                if (
                    length != member - 20
                    or not fields
                    or fields[-1] != b""
                    or mode > 4
                    or flags >> 5
                    or (flags & 3) > 2
                ):
                    raise native.BazelWorkspaceError(f"malformed COFF short import payload: {file}")
                if len(fields) != (4 if mode == 4 else 3) or any(not value for value in fields[:-1]):
                    raise native.BazelWorkspaceError(f"malformed COFF import names: {file}")
                try:
                    symbol, dll = (value.decode("ascii") for value in fields[:2])
                except UnicodeError as exc:
                    raise native.BazelWorkspaceError(f"invalid COFF import encoding: {file}") from exc
                if not re.fullmatch(r"[A-Za-z0-9_+.-]+\.dll", dll, re.IGNORECASE):
                    raise native.BazelWorkspaceError(f"invalid COFF imported DLL name: {file}: {dll}")
                dlls.add(dll.casefold())
                if mode == 0:
                    symbols.add(int.from_bytes(payload[16:18], "little"))
                elif mode == 4:
                    symbols.add(fields[2].decode("ascii"))
                else:
                    if mode in {2, 3}:
                        symbol = symbol[1:] if symbol.startswith(("?", "@", "_")) else symbol
                    if mode == 3:
                        symbol = symbol.split("@", 1)[0]
                    symbols.add(symbol)
                continue
            big = payload[:4] == b"\x00\x00\xff\xff"
            start = 56 if big else 20
            count = int.from_bytes(payload[44:48] if big else payload[2:4], "little")
            for index in range(count):
                section = payload[start + 40 * index : start + 40 * (index + 1)]
                length = int.from_bytes(section[16:20], "little")
                pointer = int.from_bytes(section[20:24], "little")
                flags = int.from_bytes(section[36:40], "little")
                if flags & 0x80:  # IMAGE_SCN_CNT_UNINITIALIZED_DATA has allocation size, not file bytes.
                    if flags & 0x60 or pointer or any(section[24:36]):
                        raise native.BazelWorkspaceError(f"contradictory COFF uninitialized section: {file}")
                    continue
                if length and (pointer < start + 40 * count or pointer + length > member):
                    raise native.BazelWorkspaceError(f"COFF section payload escapes member: {file}")
                data = payload[pointer : pointer + length]
                section_name = section[:8].rstrip(b"\x00")
                if section_name == b".drectve":
                    directives.append(data.decode("ascii", "strict"))
                elif section_name == b".idata$6":
                    value = data.rstrip(b"\x00").decode("ascii", "strict")
                    if not re.fullmatch(r"[A-Za-z0-9_+.-]+\.dll", value, re.IGNORECASE):
                        raise native.BazelWorkspaceError(f"unreviewed full COFF import descriptor: {file}")
                    dlls.add(value.casefold())
    if kind == "interface" and (len(dlls) != 1 or not symbols):
        raise native.BazelWorkspaceError(f"missing/ambiguous COFF import DLL/symbol evidence: {file}: {sorted(dlls)}")
    text = " ".join(directives)
    crt = {value.casefold() for value in re.findall(r'/DEFAULTLIB:\s*"?([A-Za-z0-9_]+)', text, re.IGNORECASE)}
    runtime = set(re.findall(r'RuntimeLibrary=([^"\s\x00]+)', text))
    iterator_abi = set(re.findall(r"_ITERATOR_DEBUG_LEVEL=(\d+)\b", text))
    compiler_abi = set(re.findall(r"_MSC_VER=(\d+)\b", text))
    expected = "msvcrtd" if config.lower() == "debug" else "msvcrt"
    opposite = "msvcrt" if config.lower() == "debug" else "msvcrtd"
    if kind == "static" and (
        expected not in crt
        or opposite in crt
        or any(value.startswith(("libcmt", "libcpmt", "libvcruntime", "libucrt")) for value in crt)
        or ("msvcprt" if config.lower() == "debug" else "msvcprtd") in crt
        or (runtime and runtime != {"MDd_DynamicDebug" if config.lower() == "debug" else "MD_DynamicRelease"})
        or (iterator_abi and iterator_abi != {"2" if config.lower() == "debug" else "0"})
        or len(compiler_abi) > 1
    ):
        raise native.BazelWorkspaceError(
            f"Windows {config} static archive CRT mismatch: {file}: {sorted(crt)} {sorted(runtime)}"
        )
    return {
        "kind": kind,
        "sha256": hashlib.sha256(captured).hexdigest(),
        "dlls": sorted(dlls),
        "symbols": symbols,
        "crt": sorted(crt),
        "runtime_library": sorted(runtime),
        "iterator_debug_level": sorted(iterator_abi),
        "compiler_abi": sorted(compiler_abi),
    }


def _windows_pe(file: Path, config: str, *, dll: bool = True) -> dict[str, Any]:
    """Read only the AMD64 image headers, export names and direct import descriptors."""
    if not native.probe(file).is_file():
        raise native.BazelWorkspaceError(f"missing Windows PE image: {file}")
    data = file.read_bytes()

    def part(offset: int, size: int) -> bytes:
        if offset < 0 or size < 0 or offset + size > len(data):
            raise native.BazelWorkspaceError(f"truncated/out-of-bounds Windows PE image: {file}")
        return data[offset : offset + size]

    def number(offset: int, size: int = 4) -> int:
        return int.from_bytes(part(offset, size), "little")

    pe = number(60)
    if part(0, 2) != b"MZ" or part(pe, 4) != b"PE\x00\x00" or number(pe + 4, 2) != 0x8664:
        raise native.BazelWorkspaceError(f"Windows image is not AMD64 PE: {file}")
    if bool(number(pe + 22, 2) & 0x2000) != dll or not number(pe + 22, 2) & 2:
        raise native.BazelWorkspaceError(f"Windows image DLL/executable shape mismatch: {file}")
    optional, length, count = pe + 24, number(pe + 20, 2), number(pe + 6, 2)
    if length < 128 or number(optional, 2) != 0x20B or number(optional + 108) < 2 or count < 1:
        raise native.BazelWorkspaceError(f"unsupported Windows PE optional header: {file}")
    part(optional, length)
    if length >= 224 and number(optional + 108) >= 14 and (number(optional + 216) or number(optional + 220)):
        raise native.BazelWorkspaceError(f"unreviewed Windows delayed runtime imports: {file}")
    sections = [part(optional + length + 40 * index, 40) for index in range(count)]

    def rva(address: int, size: int) -> int:
        matches = []
        for section in sections:
            base = int.from_bytes(section[12:16], "little")
            raw = int.from_bytes(section[16:20], "little")
            pointer = int.from_bytes(section[20:24], "little")
            if base <= address and address + size <= base + raw:
                part(pointer + address - base, size)
                matches.append(pointer + address - base)
        if len(matches) != 1:
            raise native.BazelWorkspaceError(f"ambiguous/out-of-bounds Windows PE RVA: {file}: {address}")
        return matches[0]

    def string(address: int) -> str:
        offset = rva(address, 1)
        end = data.find(b"\x00", offset)
        if end < offset:
            raise native.BazelWorkspaceError(f"unterminated Windows PE string: {file}")
        rva(address, end - offset + 1)
        return part(offset, end - offset).decode("ascii", "strict")

    exports: set[str | int] = set()
    export_rva, export_size = number(optional + 112), number(optional + 116)
    if bool(export_rva) != bool(export_size):
        raise native.BazelWorkspaceError(f"malformed Windows PE export directory range: {file}")
    if export_rva:
        if export_size < 40 or export_rva + export_size > 0x100000000:
            raise native.BazelWorkspaceError(f"malformed Windows PE export directory range: {file}")
        directory = rva(export_rva, export_size)
        if dll and string(number(directory + 12)).casefold() != file.name.casefold():
            raise native.BazelWorkspaceError(f"Windows PE export DLL identity mismatch: {file}")
        base, functions, names = number(directory + 16), number(directory + 20), number(directory + 24)
        if not functions or names > functions or not export_size:
            raise native.BazelWorkspaceError(f"malformed Windows PE export table: {file}")
        addresses = rva(number(directory + 28), functions * 4)
        for index in range(functions):
            target = number(addresses + index * 4)
            if not target:
                continue  # Unused ordinal slot, not an export.
            if export_rva <= target < export_rva + export_size:
                raise native.BazelWorkspaceError(f"unreviewed Windows PE forwarded export: {file}")
            # Export targets are image addresses, not table bytes: zero-filled
            # local data is valid too, but an unmapped/overlapping RVA is not.
            mapped = [
                section
                for section in sections
                if int.from_bytes(section[12:16], "little")
                <= target
                < int.from_bytes(section[12:16], "little")
                + max(int.from_bytes(section[8:12], "little"), int.from_bytes(section[16:20], "little"))
            ]
            if len(mapped) != 1 or target >= number(optional + 56):
                raise native.BazelWorkspaceError(f"unmapped/out-of-bounds Windows PE export target: {file}: {target}")
            raw_size = int.from_bytes(mapped[0][16:20], "little")
            raw_pointer = int.from_bytes(mapped[0][20:24], "little")
            if raw_size and (raw_pointer < optional + length + 40 * count or raw_pointer + raw_size > len(data)):
                raise native.BazelWorkspaceError(f"malformed Windows PE export target backing: {file}: {target}")
            exports.add(base + index)
        if names:
            named = rva(number(directory + 32), names * 4)
            ordinals = rva(number(directory + 36), names * 2)
            for index in range(names):
                ordinal = number(ordinals + index * 2, 2)
                if ordinal >= functions or base + ordinal not in exports:
                    raise native.BazelWorkspaceError(f"malformed Windows PE export ordinal: {file}")
                exports.add(string(number(named + index * 4)))
    if dll and not exports:
        raise native.BazelWorkspaceError(f"Windows DLL has no export evidence: {file}")
    imports = []
    import_rva, import_size = number(optional + 120), number(optional + 124)
    if import_rva:
        terminated = False
        for offset in range(0, import_size - 19, 20):
            descriptor = rva(import_rva + offset, 20)
            if part(descriptor, 20) == bytes(20):
                terminated = True
                break
            name = string(number(descriptor + 12)).casefold()
            if not re.fullmatch(r"[A-Za-z0-9_+.-]+\.dll", name) or name in imports:
                raise native.BazelWorkspaceError(f"invalid/duplicate Windows PE import: {file}: {name}")
            imports.append(name)
        if not terminated:
            raise native.BazelWorkspaceError(f"unterminated Windows PE import table: {file}")
    crt = [
        name
        for name in imports
        if re.fullmatch(r"(?:msvcp|vcruntime)\d+(?:_\d+)?d?\.dll", name)
        or name in {"ucrtbase.dll", "ucrtbased.dll"}
        or name.startswith("api-ms-win-crt-")
    ]
    debug_crt = [
        name for name in crt if name == "ucrtbased.dll" or re.fullmatch(r"(?:msvcp|vcruntime)\d+(?:_\d+)?d\.dll", name)
    ]
    data_name = re.fullmatch(r"icudtd?(\d+)\.dll", file.name.casefold())
    named_exports = {value for value in exports if isinstance(value, str)}
    data_only = (
        data_name is not None
        and not imports
        and named_exports == {f"icudt{data_name[1]}_dat"}
        and number(optional + 4) == 0
        and not any(int.from_bytes(section[36:40], "little") & 0x20000000 for section in sections)
    )
    redirect = (
        file.name.casefold() == "mimalloc-redirect.dll"
        and imports == ["ntdll.dll"]
        and {
            "mi_allocator_done",
            "mi_allocator_init",
            "mi_redirect_disable",
            "mi_redirect_enable",
            "mi_redirect_enable_term",
        }
        <= named_exports
    )
    if not (data_only or redirect) and (
        not crt or (len(debug_crt) != len(crt) if config.lower() == "debug" else bool(debug_crt))
    ):
        raise native.BazelWorkspaceError(f"Windows {config} DLL/executable CRT mismatch: {file}: {crt}")
    return {
        "exports": exports,
        "imports": imports,
        "crt": crt,
        "data_only": data_only,
        "redirect": redirect,
        "sha256": hashlib.sha256(data).hexdigest(),
    }


def _windows_runtime_closure(
    initial: list[Path],
    runtime_dir: Path,
    owned: dict[str, Path],
    prefix: Path,
    config: str,
    *,
    paired_images: dict[Path, dict[str, Any]] | None = None,
) -> tuple[list[Path], dict[str, Any]]:
    """Only the selected producer ports may supply non-system runtime dependencies."""
    pending = list(initial)
    observed: dict[str, Any] = {}
    while pending:
        file = native.native_path(pending.pop(0))
        key = file.relative_to(prefix).as_posix().casefold()
        if key in observed:
            continue
        if key not in owned or not file.is_file():
            raise native.BazelWorkspaceError(f"missing/unowned Windows runtime: {file}")
        image = (
            paired_images[file] if paired_images is not None and file in paired_images else _windows_pe(file, config)
        )
        observed[key] = {
            "path": str(file),
            **{field: value for field, value in image.items() if field != "exports"},
            "export_count": len(image["exports"]),
        }
        for dependency in image["imports"]:
            target = runtime_dir / dependency
            dependency_key = target.relative_to(prefix).as_posix().casefold()
            if dependency_key in owned:
                pending.append(owned[dependency_key])
            elif dependency in image["crt"] or dependency in {
                "kernel32.dll",
                "advapi32.dll",
                "ntdll.dll",
                "shell32.dll",
                "user32.dll",
                "ws2_32.dll",
                "shlwapi.dll",
            }:
                continue
            else:
                raise native.BazelWorkspaceError(f"unreviewed/unowned Windows runtime dependency: {file}: {dependency}")
    initial_paths = {path.resolve() for path in initial}
    extra = [Path(value["path"]) for value in observed.values() if Path(value["path"]) not in initial_paths]
    return extra, observed


def _windows_bind_interface(
    prefix: Path, config: str, name: str, interface: Any, ports: dict[str, str], version: str
) -> tuple[dict[str, Any], list[Path], dict[str, Any]]:
    owned, manifests = _windows_binary_inventory(prefix, ports)
    library_dir, runtime_dir = providers.native_config_directories(prefix, config)
    libraries = []
    initial = []
    paired_images: dict[Path, dict[str, Any]] = {}
    observations = {}
    for library in interface.libraries:
        if library.kind == "system":
            raise native.BazelWorkspaceError(f"untranslated Windows system library: {name}: {library.name}")
        spelling = library.name + (".dll.lib" if name == "mimalloc" else ".lib")
        directory = (
            library_dir / "manual-link"
            if name == "catch2" and library.name in {"Catch2Main", "Catch2Maind"}
            else library_dir
        )
        file = native.native_path(Path(library.file) if library.file else directory / spelling)
        key = file.relative_to(prefix).as_posix().casefold()
        if key not in owned or not file.is_file():
            raise native.BazelWorkspaceError(f"missing/unowned Windows library: {file}")
        alternates = [library_dir / ("lib" + library.name + ".lib")]
        if library.kind == "shared":
            if name == "mimalloc":
                other = "mimalloc" if config == "debug" else "mimalloc-debug"
                alternates += [
                    library_dir / (library.name + ".lib"),
                    library_dir / (other + ".lib"),
                    library_dir / (other + ".dll.lib"),
                    runtime_dir / (other + ".dll"),
                ]
            else:
                alternates.append(library_dir / (library.name + ".dll.lib"))
                if name == "spdlog":
                    other = "spdlog" if config == "debug" else "spdlogd"
                    alternates += [library_dir / (other + ".lib"), runtime_dir / (other + ".dll")]
        if any((native.probe(path).exists() or path.is_symlink()) and path.resolve() != file for path in alternates):
            raise native.BazelWorkspaceError(f"ambiguous/stale Windows provider artifact shape: {file}")
        evidence = _windows_coff_evidence(file, library.name, config)
        expected = "static" if library.kind == "static" else "interface"
        if evidence["kind"] != expected:
            raise native.BazelWorkspaceError(
                f"Windows library linkage/payload mismatch: {file}: {evidence['kind']} != {expected}"
            )
        item = library.json()
        item["file"] = str(file)
        if library.kind == "shared":
            expected_dll = file.stem.removesuffix(".dll")
            if name in {"icu", "icu_uc"}:
                expected_dll += version.split(".", 1)[0]
            if evidence["dlls"] != [expected_dll.casefold() + ".dll"]:
                raise native.BazelWorkspaceError(
                    f"Windows configuration/provider DLL name mismatch: {file}: {evidence['dlls']}"
                )
            target = runtime_dir / evidence["dlls"][0]
            target_key = target.relative_to(prefix).as_posix().casefold()
            if target_key not in owned:
                raise native.BazelWorkspaceError(f"missing/unowned Windows import DLL pairing: {file}: {target}")
            target = native.native_path(owned[target_key])
            if target not in paired_images:
                paired_images[target] = _windows_pe(target, config)
            image = paired_images[target]
            if not evidence["symbols"] <= image["exports"]:
                raise native.BazelWorkspaceError(f"Windows import symbols/DLL exports mismatch: {file}: {target}")
            if library.runtime is not None and Path(library.runtime) != target:
                raise native.BazelWorkspaceError(f"Windows explicit import DLL pairing mismatch: {file}")
            item["runtime"] = str(target)
            initial.append(target)
        observations[key] = {
            "path": str(file),
            **{field: value for field, value in evidence.items() if field != "symbols"},
            "import_count": len(evidence["symbols"]),
        }
        libraries.append(item)
    extra, images = _windows_runtime_closure(initial, runtime_dir, owned, prefix, config, paired_images=paired_images)
    observations.update(images)
    return (
        {"libraries": libraries, **({"runtime": [str(path) for path in extra]} if extra else {})},
        manifests,
        observations,
    )


def _windows_static_coff_archive(file: Path, name: str, config: str) -> None:
    """The Boost Process guard: reviewed archives hold native objects, never import-library members."""
    if coff_archive_kind(file, name, config) == "interface":
        raise native.BazelWorkspaceError(f"reviewed windows {config} {name} artifact is an import library: {file}")


def _windows_boost_process_interface(
    spec: dict[str, Any], prefix: Path, config: str, version: str, policy: dict[str, Any]
) -> dict[str, Any]:
    """Windows consumes compiled Process v2 through the reviewed shared Boost.Filesystem ABI."""
    if spec["platform"] != "windows" or spec["provider"]["name"] != "vcpkg" or spec["linkage"] != "shared":
        raise native.BazelWorkspaceError("Windows Boost Process adapter requires the reviewed shared vcpkg SDK")
    reviewed = policy["library_fallbacks"].get("boost_process", {}).get("windows")
    required_defines = {
        "BOOST_PROCESS_DYN_LINK",
        "BOOST_PROCESS_NO_LIB",
        "BOOST_DATE_TIME_DYN_LINK",
        "BOOST_DATE_TIME_NO_LIB",
        "BOOST_CONTAINER_DYN_LINK",
        "BOOST_CONTAINER_NO_LIB",
        "BOOST_CONTEXT_DYN_LINK=",
        "BOOST_CONTEXT_NO_LIB=",
        "BOOST_CONTEXT_EXPORT=EXPORT",
        "BOOST_FILESYSTEM_DYN_LINK=1",
        "BOOST_FILESYSTEM_NO_LIB",
        "BOOST_ATOMIC_DYN_LINK",
        "BOOST_ATOMIC_NO_LIB",
    }
    if (
        not isinstance(reviewed, dict)
        or reviewed.get("linkage") != "shared"
        or not isinstance(reviewed.get("libraries"), list)
        or not reviewed["libraries"]
        or len(set(reviewed["libraries"])) != len(reviewed["libraries"])
        or reviewed["libraries"]
        != ["boost_process", "boost_date_time", "boost_container", "boost_context", "boost_filesystem", "boost_atomic"]
        or any(
            not isinstance(library, str) or not re.fullmatch(r"boost_[a-z0-9_]+", library)
            for library in reviewed["libraries"]
        )
        or not isinstance(reviewed.get("defines"), list)
        or not reviewed["defines"]
    ):
        raise native.BazelWorkspaceError("Windows Boost Process requires its reviewed shared import closure interface")
    if set(reviewed["defines"]) != required_defines:
        raise native.BazelWorkspaceError("Windows Boost Process requires the exact shared producer macro interface")
    # The compiled DLLs retain Process v2's Boost.Filesystem semantics. Static,
    # standalone and std::filesystem consumer branches would change that ABI.
    forbidden = {
        "BOOST_PROCESS_STATIC_LINK",
        "BOOST_FILESYSTEM_STATIC_LINK",
        "BOOST_PROCESS_USE_STD_FS",
        "BOOST_PROCESS_V2_STANDALONE",
        "BOOST_FILESYSTEM_VERSION",
    }
    if forbidden & {define.split("=", 1)[0] for define in reviewed["defines"]}:
        raise native.BazelWorkspaceError(
            "Windows Boost Process consumer definitions must remain shared without a filesystem version pin"
        )
    include = _static_boost_process_headers(prefix, version)
    components = version.split(".")
    if len(components) < 2 or not all(component.isdecimal() for component in components[:2]):
        raise native.BazelWorkspaceError(f"unsupported Windows Boost Process provider version: {version}")
    debug = "-gd" if config == "debug" else ""
    alternate = "" if debug else "-gd"
    library_dir, _ = providers.native_config_directories(prefix, config)
    # The installed producer toolset tag is observed per install and required
    # coherent across the closure; refresh has no independent selected-compiler
    # metadata, so native validation owns the compiler/toolset binding.
    toolset: str | None = None
    libraries = []
    for name in reviewed["libraries"]:
        # Discover the exact owned archive through the reviewed producer name
        # pattern (component, version from the shared status, mt[-gd], x64);
        # the vc tag is captured, never pinned, duplicated or guessed here.
        tag = f"-x64-{components[0]}_{components[1]}.lib"
        selected = re.compile(rf"{re.escape(name)}-vc(?P<toolset>[0-9]+)-mt{re.escape(debug)}{tag}")
        rejected = (
            re.compile(rf"{re.escape(name)}-vc[0-9]+-mt-s(?:gd)?{tag}"),
            re.compile(rf"{re.escape(name)}-vc[0-9]+-mt{re.escape(alternate)}{tag}"),
        )
        candidates: list[tuple[Path, str]] = []
        for entry in sorted(library_dir.iterdir()):
            if rejected[0].fullmatch(entry.name) or rejected[1].fullmatch(entry.name):
                raise native.BazelWorkspaceError(
                    f"unsupported static provider shape in reviewed windows {config} layout: {entry}"
                )
            match = selected.fullmatch(entry.name)
            if match is not None:
                candidates.append((native.probe(entry), match.group("toolset")))
        if len(candidates) != 1:
            raise native.BazelWorkspaceError(
                f"missing/ambiguous reviewed windows {config} {name} artifact: "
                f"{sorted(str(path) for path, _ in candidates)}"
            )
        file = native.native_path(candidates[0][0])
        if toolset is None:
            toolset = candidates[0][1]
        elif candidates[0][1] != toolset:
            raise native.BazelWorkspaceError(
                "incoherent installed producer toolset ABI across the reviewed Windows Process closure: "
                f"{toolset} != {candidates[0][1]}"
            )
        if coff_archive_kind(file, name, config) != "interface":
            raise native.BazelWorkspaceError(f"Windows Boost shared artifact is not an import library: {file}")
        libraries.append({"name": name, "kind": "shared", "file": str(file)})
    result: dict[str, Any] = {
        "header_dirs": [str(include)],
        "defines": list(reviewed["defines"]),
        "libraries": libraries,
    }
    if reviewed.get("linkopts"):
        result["linkopts"] = list(reviewed["linkopts"])
    return result


def _lexy_interface(roots: list[Path], host: str, version: str, evidence: Path) -> dict[str, Any]:
    """Complete header-only interface; provider metadata owns its version."""
    marker = "lexy/_detail/config.hpp"
    include = native.header_root(roots, marker)
    header = include / marker
    if host != "linux":
        native.native_path(include)
        native.native_path(header)
    if "#define LEXY_DETAIL_CONFIG_HPP_INCLUDED" not in header.read_text(encoding="utf-8"):
        raise native.BazelWorkspaceError(f"unrecognized lexy configuration header: {header}")
    native.write_json(
        evidence,
        {
            "package": "lexy",
            "provider_version": version,
            "header": native.evidence([header])[0],
            "version_evidence": "Provider report/status only; installed header has no upstream version macro",
            "interface": "Complete include root; header-only, no defines, compile flags or link inputs",
        },
    )
    return {"header_dirs": [str(include)]}


def _stb_interface(roots: list[Path], host: str, version: str) -> dict[str, Any]:
    marker = "stb/stb_image_resize2.h" if host == "linux" else "stb_image_resize2.h"
    include = native.header_root(roots, marker)
    header = include / marker
    if host != "linux":
        overlay = native.read_json(native.PROJECT_ROOT / "cmake/vcpkg-ports/stb/vcpkg.json")
        if version != overlay["version-string"]:
            raise native.BazelWorkspaceError(f"stb does not match the governed overlay: {version}")
        native.native_path(header)
    # The pinned Nix recipe/vcpkg overlay owns the fixed source and its hash.
    # Check the consumed API, not another source revision or release-banner pin.
    text = header.read_text(encoding="utf-8")
    if not text.startswith("/* stb_image_resize2 - ") or any(
        re.search(r"\b" + symbol + r"\b", text) is None for symbol in ("stbir_resize_uint8_srgb", "STBIR_RGBA")
    ):
        raise native.BazelWorkspaceError(f"stb resize2 interface is missing: {header}")
    return {"header_dirs": [str(header.parent)]}


def resolve_dependencies(tree: Path, config: str, *, group: str, prefix: str | None = None) -> dict[str, Any]:
    """Report validated SDK interfaces; Starlark owns declarations and materialization."""
    from .bazel_deps import _Adapter

    host = native.native_platform()
    policy = providers.load_policy()
    windows_static = _windows_static_packages(policy) if host == "windows" else set()
    groups = providers.package_groups(policy, host=host)
    if group == "catalog":
        # The real host catalog compiler needs ICU only, in its own compilation
        # mode. Keep the target repository's Debug/Release guards unchanged.
        packages = [package for package in groups["native"] if package["name"] == "icu"]
        if len(packages) != 1:
            raise native.BazelWorkspaceError("catalog compiler requires the governed ICU package")
        policy = {**policy, "packages": {"native": packages, "test": []}}
        groups = {"catalog": packages}
    elif group not in groups:
        raise native.BazelWorkspaceError(f"unsupported dependency group: {group}")
    provider_prefix = None if host == "linux" else providers.vcpkg_prefix(prefix, None)
    expected_nix_outputs: dict[str, list[Path]] = {}
    if provider_prefix is None:
        base, versions, expected_nix_outputs = providers.linux_provider(
            policy, identity_path=tree / "provider-identity.json"
        )
    else:
        base, versions = providers.native_provider(
            provider_prefix, config, host, identity_path=tree / "provider-identity.json", policy=policy
        )
    identity = native.read_json(tree / "provider-identity.json")
    contract = dependency_policy.load_contract()
    for package in (package for group in groups.values() for package in group):
        source = package["nix"]
        if source in contract["dependencies"] and not dependency_policy.effective_policy(
            contract, source, host
        ).accepts(versions[package["name"]]):
            raise native.BazelWorkspaceError(
                f"{source} {versions[package['name']]} does not satisfy dependency-contract.json"
            )
    common_evidence = native.evidence([providers.POLICY, native.PROJECT_ROOT / "dependency-contract.json"])
    base["provider"]["evidence"].extend(common_evidence)
    base["allowed_roots"].extend(item["path"] for item in common_evidence)
    for selected_group, packages in groups.items():
        if group != selected_group:
            continue
        spec = {
            **base,
            "schema": 1,
            "platform": host,
            "config": config.title(),
            "linkage": "static" if selected_group == "test" or host == "macos" else "shared",
            "system_libraries": {
                "linux": ["pthread", "m", "dl"],
                "macos": ["c++", "pthread", "m"],
                "windows": [
                    "Shell32",
                    "kernel32",
                    "user32",
                    "gdi32",
                    "winspool",
                    "shell32",
                    "ole32",
                    "oleaut32",
                    "comdlg32",
                    "advapi32",
                    "psapi",
                    "bcrypt",
                    "shlwapi",
                    "ntdll",
                    "ws2_32",
                    "Synchronization",
                    "mswsock",
                    "uuid",
                ]
                if selected_group != "test"
                else [],
            }[host],
            "packages": [],
        }
        patches = []
        for package in packages:
            name = package["name"]
            roots = (
                [provider_prefix]
                if provider_prefix is not None
                else [Path(value) for value in identity["packages"][name]["outputs"]]
            )
            selected: dict[str, Any] = {"name": name, "modules": package["modules"].copy()}
            if host == "windows" and name in windows_static:
                selected["linkage"] = "static"
            spec["packages"].append(selected)
            if name in {"icu", "icu_uc", "catch2"}:
                marker, macros = {
                    "icu": ("unicode/uvernum.h", ["U_ICU_VERSION_MAJOR_NUM", "U_ICU_VERSION_MINOR_NUM"]),
                    "icu_uc": ("unicode/uvernum.h", ["U_ICU_VERSION_MAJOR_NUM", "U_ICU_VERSION_MINOR_NUM"]),
                    "catch2": (
                        "catch2/catch_version_macros.hpp",
                        ["CATCH_VERSION_MAJOR", "CATCH_VERSION_MINOR", "CATCH_VERSION_PATCH"],
                    ),
                }[name]
                include = native.header_root(roots, marker)
                native.verify_header_version(include / marker, versions[name], macros)
                if host == "windows" and name in {"icu", "icu_uc"}:
                    header = native.native_path(include / "unicode/utypes.h")
                    static_api = (
                        r"#elif defined\(U_STATIC_IMPLEMENTATION\)[ \t]*\n"
                        + "".join(
                            r"#define " + macro + r"[ \t]*\n"
                            for macro in (
                                "U_DATA_API",
                                "U_COMMON_API",
                                "U_I18N_API",
                                "U_LAYOUT_API",
                                "U_LAYOUTEX_API",
                                "U_IO_API",
                                "U_TOOLUTIL_API",
                            )
                        )
                        + r"#elif defined\(U_COMMON_IMPLEMENTATION\)"
                    )
                    if not re.search(static_api, header.read_text(encoding="utf-8")):
                        raise native.BazelWorkspaceError(
                            f"shared ICU requires its unpatched import/export installed header: {header}"
                        )
                    platform_header = native.native_path(include / "unicode/platform.h")
                    if not re.search(
                        r"#\s*define U_IMPORT __declspec\(dllimport\)",
                        platform_header.read_text(encoding="utf-8"),
                    ):
                        raise native.BazelWorkspaceError(
                            f"shared ICU requires its installed DLL import header: {platform_header}"
                        )
                selected["expected_versions"] = {module: versions[name] for module in selected["modules"]}
                continue
            if name == "ftxui" or (name == "cli11" and host != "linux"):
                selected["linkage"] = "static"
            metadata = _has_metadata(spec, selected["modules"])
            if name == "stb":
                patches.append(_manual_patch(spec, name, _stb_interface(roots, host, versions[name])))
                continue
            if name in {"giomm", "gtkmm"}:
                if not metadata:
                    raise native.BazelWorkspaceError(f"{name} requires complete governed pkg-config metadata")
                if name == "gtkmm":
                    # Cairomm's public headers expose both Cairo font backends,
                    # which gtkmm metadata does not request on this SDK.
                    selected["expected_versions"] = {
                        module: identity["inputs"]["cairo"]["version"]
                        if module in {"cairo-ft", "cairo-fc"}
                        else versions[name]
                        for module in selected["modules"]
                    }
                else:
                    selected["expected_versions"] = {
                        module: identity["inputs"]["glib"]["version"] if module == "gio-unix-2.0" else versions[name]
                        for module in selected["modules"]
                    }
                continue
            if name == "xtest":
                if not metadata:
                    raise native.BazelWorkspaceError("xtest requires complete governed pkg-config metadata")
                # XTest is a shared X11 extension library; the test group's
                # otherwise-static default would look for archives Nix does not install.
                selected["linkage"] = "shared"
                selected["expected_versions"] = {
                    "xtst": versions[name],
                    "x11": identity["inputs"]["libX11"]["version"],
                    "xi": identity["inputs"]["libXi"]["version"],
                }
                continue
            if name == "boost_process":
                if host == "linux":
                    interface = _linux_boost_process_interface(spec, roots, versions[name])
                    if metadata:
                        selected["expected_versions"] = {module: versions[name] for module in selected["modules"]}
                    else:
                        selected["modules"] = []
                        patches.append(_manual_patch(spec, name, interface))
                    continue
                # The installed compiled ports ship no pkg-config metadata; a
                # discovered module is unreviewed for this static interface and
                # must fail instead of being consumed as ambient metadata.
                if metadata:
                    platform_name = "macOS" if spec["platform"] == "macos" else spec["platform"]
                    raise native.BazelWorkspaceError(f"unreviewed {platform_name} Boost Process pkg-config metadata")
                assert provider_prefix is not None
                interface = (
                    _macos_boost_process_interface(spec, provider_prefix, config, versions[name], policy)
                    if host == "macos"
                    else _windows_boost_process_interface(spec, provider_prefix, config, versions[name], policy)
                )
                selected["modules"] = []
                selected["linkage"] = "shared" if host == "windows" else "static"
                patches.append(_manual_patch(spec, name, interface))
                continue
            if name in {"spdlog", "ftxui", "mimalloc", "date_tz"}:
                if name == "spdlog":
                    include = native.header_root(roots, "spdlog/version.h")
                    native.verify_header_version(
                        include / "spdlog/version.h",
                        versions[name],
                        ["SPDLOG_VER_MAJOR", "SPDLOG_VER_MINOR", "SPDLOG_VER_PATCH"],
                    )
                elif name == "mimalloc":
                    include = native.header_root(roots, "mimalloc.h")
                    values = re.findall(
                        r"^\s*#\s*define\s+MI_MALLOC_VERSION\s+(\d+)\b",
                        (include / "mimalloc.h").read_text(encoding="utf-8"),
                        re.MULTILINE,
                    )
                    components = versions[name].split(".")
                    if (
                        len(values) != 1
                        or len(components) != 3
                        or not all(value.isdecimal() for value in components)
                        or int(values[0])
                        != sum(int(value) * scale for value, scale in zip(components, (10000, 100, 1), strict=True))
                    ):
                        raise native.BazelWorkspaceError("mimalloc header version does not match the selected provider")
                interface = {}
                if host != "linux":
                    interface = _library_interface(
                        package,
                        roots,
                        config,
                        host,
                        policy,
                    )
                    selected["linkage"] = interface["libraries"][0]["kind"]
                elif not metadata:
                    raise native.BazelWorkspaceError(f"{name} requires complete governed pkg-config metadata")
                if not metadata:
                    selected["modules"] = []
                    patches.append(_manual_patch(spec, name, interface))
                else:
                    selected["expected_versions"] = _metadata_versions(name, versions[name], selected["modules"], host)
                    patch = _compile_interface_patch(spec, name, interface)
                    if patch is not None:
                        patches.append(patch)
                    if host == "windows" and name == "spdlog":
                        reviewed = policy["windows_spdlog_threads_remove"]
                        removed = {"linkopts": reviewed["linkopts"]}
                        patches.append(
                            _patch(
                                "windows-spdlog-native-threads",
                                spec,
                                name,
                                remove=removed,
                                add={},
                            )
                        )
                    if host == "macos" and name == "mimalloc":
                        reviewed = policy["macos_mimalloc_runtime_remove"]
                        patch = _macos_icu_runtime_patch(spec, reviewed, package=name)
                        if patch is not None:
                            patches.append(patch)
                continue
            if name in {"lmdb", "xxhash"}:
                marker, macros = {
                    "lmdb": ("lmdb.h", ["MDB_VERSION_MAJOR", "MDB_VERSION_MINOR", "MDB_VERSION_PATCH"]),
                    "xxhash": ("xxhash.h", ["XXH_VERSION_MAJOR", "XXH_VERSION_MINOR", "XXH_VERSION_RELEASE"]),
                }[name]
                include = native.header_root(roots, marker)
                native.verify_header_version(include / marker, versions[name], macros)
            if name == "lexy":
                evidence_path = tree / "lexy-identity.json"
                lexy_interface = _lexy_interface(roots, host, versions[name], evidence_path)
                header_evidence = native.evidence(
                    [evidence_path, Path(lexy_interface["header_dirs"][0]) / package["header"]]
                )
                spec["provider"] = {
                    **spec["provider"],
                    "evidence": [*spec["provider"]["evidence"], *header_evidence],
                }
                spec["allowed_roots"] = [*spec["allowed_roots"], str(evidence_path.resolve())]
                patches.append(_manual_patch(spec, name, lexy_interface))
                continue
            if "header" in package:
                if name == "fakeit":
                    reviewed = policy["header_fallbacks"].get(name, {}).get(host)
                    if reviewed is None:
                        raise native.BazelWorkspaceError(f"unsupported FakeIt header interface: {host}")
                    include = native.header_root(roots, reviewed.get("header", package["header"]))
                    fakeit_interface = {"header_dirs": [str(include / reviewed["include"])]}
                    if metadata:
                        selected["expected_versions"] = {module: versions[name] for module in selected["modules"]}
                        patch = _compile_interface_patch(spec, name, fakeit_interface)
                        if patch is not None:
                            patches.append(patch)
                    else:
                        selected["modules"] = []
                        patches.append(_manual_patch(spec, name, fakeit_interface))
                    continue
                include = native.header_root(roots, package["header"])
                marker, macros = {
                    "boost": ("boost/version.hpp", ["BOOST_VERSION"]),
                    "gsl_lite": (package["header"], ["gsl_lite_MAJOR", "gsl_lite_MINOR", "gsl_lite_PATCH"]),
                    "cli11": ("CLI/Version.hpp", ["CLI11_VERSION_MAJOR", "CLI11_VERSION_MINOR", "CLI11_VERSION_PATCH"]),
                    "fast_float": (
                        "fast_float/float_common.h",
                        ["FASTFLOAT_VERSION_MAJOR", "FASTFLOAT_VERSION_MINOR", "FASTFLOAT_VERSION_PATCH"],
                    ),
                }[name]
                native.verify_header_version(include / marker, versions[name], macros, encoded=name == "boost")
                interface = {
                    "header_dirs": [str(include)],
                    **policy["header_fallbacks"].get(name, {}).get(host, {}),
                }
                if name == "cli11" and host != "linux":
                    interface = _library_interface(
                        package,
                        roots,
                        config,
                        host,
                        policy,
                    )
                if metadata:
                    selected["expected_versions"] = {module: versions[name] for module in selected["modules"]}
                    patch = _compile_interface_patch(spec, name, interface)
                    if patch is not None:
                        patches.append(patch)
                    if name == "cli11" and host == "windows":
                        # The reviewed compiled port omits CommandLineToArgvW's
                        # SDK import from pkg-config. Keep it on CLI11 itself.
                        links = {"linkopts": interface["linkopts"]}
                        patches.append(
                            _patch(
                                "cli11-windows-system-link",
                                spec,
                                name,
                                present={"libraries": [{"name": "CLI11", "kind": "static"}]},
                                add=links,
                            )
                        )
                else:
                    selected["modules"] = []
                    patches.append(_manual_patch(spec, name, interface))
                continue
            # These codec interfaces come from metadata, never a partial fallback.
            if name in {"flac", "fdk_aac", "mpg123", "opus"}:
                if not metadata:
                    raise native.BazelWorkspaceError(
                        f"{name} pkg-config metadata is missing; no reviewed fallback for {host}"
                    )
                interface = {}
                if name == "opus":
                    # Upstream metadata exposes <opus.h>; Aobus consumes <opus/opus.h>.
                    interface["header_dirs"] = [str(native.header_root(roots, "opus/opus.h"))]
                if host == "windows":
                    assert provider_prefix is not None
                    selected["linkage"] = "shared"
                    if name == "flac":
                        include = native.header_root(roots, "FLAC/export.h")
                        header = native.native_path(include / "FLAC/export.h")
                        if not re.search(
                            r"#if 0 && !\(defined\(DLL_EXPORT\)\)\s*\n\s*#define FLAC_API\s*\n",
                            header.read_text(encoding="utf-8"),
                        ):
                            raise native.BazelWorkspaceError(
                                f"shared FLAC requires its importing installed header: {header}"
                            )
                elif host == "macos":
                    patches.append(
                        _macos_audio_runtime_patch(
                            spec,
                            name,
                            policy["macos_audio_runtime_remove"][name],
                        )
                    )
            elif host != "linux" or name == "rapidyaml":
                if name == "alac" and not metadata and _has_metadata(spec, ["libalac"]):
                    raise native.BazelWorkspaceError(
                        "unreviewed ALAC metadata alias; complete fallback requires absence"
                    )
                # Linkage is reviewed policy; artifacts and runtimes must all exist.
                interface = _library_interface(
                    package,
                    roots,
                    config,
                    host,
                    policy,
                )
                selected["linkage"] = interface["libraries"][0]["kind"]
            else:
                interface = {}
            if metadata:
                selected["expected_versions"] = _metadata_versions(name, versions[name], selected["modules"], host)
                patch = _compile_interface_patch(spec, name, interface)
                if patch is not None:
                    patches.append(patch)
            else:
                if not interface.get("libraries"):
                    raise native.BazelWorkspaceError(
                        f"{name} pkg-config metadata is missing; no reviewed fallback for {host}"
                    )
                selected["modules"] = []
                patches.append(_manual_patch(spec, name, interface))
            if name == "rapidyaml":
                include = native.header_root(
                    roots,
                    "ryml.hpp"
                    if any(native.probe(r / "include/ryml.hpp").is_file() for r in roots)
                    else "ryml/ryml.hpp",
                )
                native.verify_header_version(
                    _rapidyaml_version_header(include),
                    versions[name],
                    ["RYML_VERSION_MAJOR", "RYML_VERSION_MINOR", "RYML_VERSION_PATCH"],
                )
                if provider_prefix is not None:
                    c4version = identity["packages"]["c4core"]["version"].split("#", maxsplit=1)[0]
                    native.verify_header_version(
                        include / "c4/version.hpp",
                        c4version,
                        ["C4CORE_VERSION_MAJOR", "C4CORE_VERSION_MINOR", "C4CORE_VERSION_PATCH"],
                    )
        if selected_group in {"native", "catalog"}:
            for package in packages:
                name = package["name"]
                if name not in {"icu", "icu_uc"}:
                    continue
                if host == "windows":
                    assert provider_prefix is not None
                    if not _has_metadata(spec, package["modules"]):
                        raise native.BazelWorkspaceError(f"{name} requires complete Windows shared metadata")
                elif host == "macos":
                    patch = _macos_icu_runtime_patch(spec, policy["macos_icu_runtime_remove"], package=name)
                    if patch is not None:
                        patches.append(patch)
        if selected_group == "test" and provider_prefix is not None:
            if host == "windows":
                bindir = provider_prefix / "debug/bin" if config == "debug" else provider_prefix / "bin"
                if list(bindir.glob("Catch2*.dll")):
                    raise native.BazelWorkspaceError(
                        "initial Bazel tests require static Catch2/Catch2Main, not Catch2 DLLs"
                    )
            pc = Path(base["pkg_config"]["pc_dirs"][0]) / "catch2-with-main.pc"
            if not native.probe(pc).is_file():
                spec["packages"][0] = {"name": "catch2", "modules": []}
                patches.append(_catch_fallback(spec, policy["catch2_fallback"], provider_prefix))
            elif host == "windows":
                removed = {}
                for field, option in (("linkopts", "--libs"), ("copts", "--cflags")):
                    flags = _metadata_flags(spec, option)
                    tokens = [token for token in policy["windows_catch2_remove"] if token in flags]
                    if tokens:
                        removed[field] = tokens
                if removed:
                    # Only reviewed translations are eligible, and generated guards assert exact presence.
                    patches.append(
                        _patch(
                            "windows-catch2-pthread",
                            spec,
                            "catch2",
                            remove=removed,
                            add={},
                        )
                    )
        from .bazel_headers import vcpkg_header_inventory

        if provider_prefix is not None:
            spec["allowed_roots"].append(str(provider_prefix.parent / "vcpkg/info"))
        if host == "windows":
            assert provider_prefix is not None
            preliminary = _Adapter(spec, {"schema": 1, "patches": patches})
            preliminary.prepare()
            binary_manifests: set[Path] = set()
            binary_observations = {}
            package_ports = {package["name"]: package["vcpkg"] for package in packages}
            for name, prepared_windows, _ in preliminary.packages:
                expected_linkage = "static" if name in windows_static else "shared"
                if not prepared_windows.libraries:
                    if prepared_windows.runtime:
                        raise native.BazelWorkspaceError(f"unexpected header-only Windows runtime: {name}")
                    continue
                if (
                    preliminary.package_linkages[name] != expected_linkage
                    or any(library.kind != expected_linkage for library in prepared_windows.libraries)
                    or [library.name for library in prepared_windows.libraries]
                    != _windows_library_names(name, config, policy)
                    or prepared_windows.runtime
                ):
                    raise native.BazelWorkspaceError(f"unreviewed Windows linkage/library sequence/runtime: {name}")
                ports = package_ports[name]
                if name == "boost_process":
                    ports = [port for port in identity["packages"] if port.startswith("boost-")]
                bound, manifests, observations = _windows_bind_interface(
                    provider_prefix,
                    config,
                    name,
                    prepared_windows,
                    {port: identity["packages"][port]["version"] for port in ports},
                    versions[name],
                )
                binary_manifests.update(manifests)
                binary_observations[name] = observations
                old = [library.json() for library in prepared_windows.libraries]
                additions = {key: value for key, value in bound.items() if value != prepared_windows.json()[key]}
                if additions:
                    removed = {"libraries": old} if "libraries" in additions else {}
                    patches.append(
                        _patch(
                            f"windows-{name}-binary-pairs",
                            spec,
                            name,
                            present={"libraries": old},
                            remove=removed,
                            add=additions,
                        )
                    )
            binary_identity = tree / "windows-binaries.json"
            native.write_json(
                binary_identity,
                {
                    "schema": 1,
                    "triplet": provider_prefix.name,
                    "configuration": config,
                    "crt": "/MDd" if config == "debug" else "/MD",
                    "packages": binary_observations,
                    "ownership": native.evidence(sorted(binary_manifests)),
                    "provenance": (
                        "Installed COFF/PE/status/ownership joins; original producer build provenance unverified"
                    ),
                },
            )
            spec["allowed_roots"].append(str(binary_identity.resolve()))
            spec["provider"]["evidence"].extend(native.evidence([binary_identity, *sorted(binary_manifests)]))
        adapter = _Adapter(spec, {"schema": 1, "patches": patches})
        adapter.prepare()
        for name, prepared_interface, _ in adapter.packages:
            if host == "windows":
                forbidden = {
                    "mimalloc": {"MI_SHARED_LIB_EXPORT", "MI_STATIC_LIB"},
                    "xxhash": {"XXH_EXPORT", "XXH_INLINE_ALL", "XXH_PRIVATE_API", "XXH_STATIC_LINKING_ONLY"},
                    "mpg123": {"BUILD_MPG123_DLL"},
                    "flac": {"DLL_EXPORT", "FLAC_API_EXPORTS", "FLAC__NO_DLL"},
                    "icu": {
                        "U_STATIC_IMPLEMENTATION",
                        "U_COMBINED_IMPLEMENTATION",
                        "U_COMMON_IMPLEMENTATION",
                        "U_I18N_IMPLEMENTATION",
                        "U_DATA_IMPLEMENTATION",
                    },
                    "icu_uc": {
                        "U_STATIC_IMPLEMENTATION",
                        "U_COMBINED_IMPLEMENTATION",
                        "U_COMMON_IMPLEMENTATION",
                        "U_I18N_IMPLEMENTATION",
                        "U_DATA_IMPLEMENTATION",
                    },
                }.get(name, set())
                consumer_macros = {define.split("=", 1)[0] for define in prepared_interface.defines}
                if consumer_macros & forbidden or (
                    name == "xxhash" and "XXH_IMPORT=1" not in prepared_interface.defines
                ):
                    raise native.BazelWorkspaceError(f"incompatible Windows shared consumer ABI definition: {name}")
                if prepared_interface.libraries:
                    expected_defines = policy["library_fallbacks"].get(name, {}).get("windows", {}).get("defines", [])
                    if set(prepared_interface.defines) != set(expected_defines):
                        raise native.BazelWorkspaceError(f"unreviewed Windows consumer compile definitions: {name}")
            if (
                name == "cli11"
                and host != "linux"
                and (
                    "CLI11_COMPILE" not in prepared_interface.defines
                    or adapter.package_linkages[name] != "static"
                    or [library.name for library in prepared_interface.libraries] != ["CLI11"]
                    or (
                        host == "windows"
                        and prepared_interface.linkopts != policy["library_fallbacks"]["cli11"][host]["linkopts"]
                    )
                )
            ):
                raise native.BazelWorkspaceError(
                    "native CLI11 requires its compiled static CLI11_COMPILE interface and reviewed system links"
                )
            if name == "boost_process" and host == "linux":
                required = {
                    "BOOST_PROCESS_NO_LIB",
                    "BOOST_PROCESS_DYN_LINK",
                    "BOOST_FILESYSTEM_NO_LIB",
                    "BOOST_FILESYSTEM_DYN_LINK",
                }
                if (
                    set(prepared_interface.defines) != required
                    or adapter.package_linkages[name] != "shared"
                    or [library.name for library in prepared_interface.libraries]
                    != ["boost_process", "boost_filesystem"]
                    or prepared_interface.copts
                    or prepared_interface.linkopts
                    or prepared_interface.runtime
                    or prepared_interface.header_dirs
                    != [str(native.header_root(expected_nix_outputs[name], "boost/version.hpp"))]
                ):
                    raise native.BazelWorkspaceError("Boost Process requires its shared Boost.Filesystem ABI interface")
            if name == "boost_process" and host == "macos":
                assert provider_prefix is not None
                # Static consumption mirrors the installed package's export interface: STATIC_LINK
                # plus NO_LIB, never DYN_LINK, USE_STD_FS, STANDALONE or a
                # BOOST_FILESYSTEM_VERSION that would skew consumer semantics.
                required = {
                    "BOOST_PROCESS_STATIC_LINK",
                    "BOOST_PROCESS_NO_LIB",
                    "BOOST_FILESYSTEM_STATIC_LINK",
                    "BOOST_FILESYSTEM_NO_LIB",
                }
                if (
                    set(prepared_interface.defines) != required
                    or adapter.package_linkages[name] != "static"
                    or [library.name for library in prepared_interface.libraries]
                    != ["boost_process", "boost_filesystem"]
                    or any(library.kind != "static" for library in prepared_interface.libraries)
                    or prepared_interface.copts
                    or prepared_interface.linkopts
                    or prepared_interface.runtime
                    or prepared_interface.header_dirs != [str((provider_prefix / "include").resolve())]
                ):
                    raise native.BazelWorkspaceError(
                        "macOS Boost Process requires its static Boost.Filesystem ABI interface"
                    )
            if name == "boost_process" and host == "windows":
                assert provider_prefix is not None
                # Shared consumption retains the complete reviewed producer closure,
                # import macros and SDK tail; binary pairs were independently checked.
                reviewed = policy["library_fallbacks"]["boost_process"]["windows"]
                if (
                    set(prepared_interface.defines) != set(reviewed["defines"])
                    or adapter.package_linkages[name] != "shared"
                    or [library.name for library in prepared_interface.libraries] != reviewed["libraries"]
                    or any(
                        library.kind != "shared" or library.runtime is None for library in prepared_interface.libraries
                    )
                    or prepared_interface.copts
                    or prepared_interface.linkopts != reviewed.get("linkopts", [])
                    or prepared_interface.runtime
                    or prepared_interface.header_dirs != [str((provider_prefix / "include").resolve())]
                ):
                    raise native.BazelWorkspaceError(
                        "Windows Boost Process requires its reviewed shared Boost.Filesystem ABI interface"
                    )
            if name == "spdlog":
                required = {"SPDLOG_COMPILED_LIB", "SPDLOG_USE_STD_FORMAT"}
                if adapter.package_linkages[name] == "shared":
                    required.add("SPDLOG_SHARED_LIB")
                forbidden = {"SPDLOG_FMT_EXTERNAL", "SPDLOG_HEADER_ONLY"}
                if host == "windows":
                    forbidden.add("spdlog_EXPORTS")
                    if adapter.package_linkages[name] == "static":
                        forbidden.add("SPDLOG_SHARED_LIB")
                if not required <= set(prepared_interface.defines) or any(
                    define.split("=", 1)[0] in forbidden for define in prepared_interface.defines
                ):
                    raise native.BazelWorkspaceError(
                        "spdlog must expose its compiled std::format ABI without external fmt"
                    )
            if name == "ftxui" and [library.name for library in prepared_interface.libraries] != [
                "ftxui-component",
                "ftxui-dom",
                "ftxui-screen",
            ]:
                raise native.BazelWorkspaceError("FTXUI requires the complete component/dom/screen archive interface")
        if provider_prefix is not None:
            package_ports = {package["name"]: package["vcpkg"] for package in packages}
            for name, resolved_interface, _ in adapter.packages:
                ports = package_ports[name]
                if name in {"boost", "boost_process"}:
                    # The Process v2 header closure and its Boost transitive
                    # headers come from the selected coherent Boost ports, never
                    # from an unowned whole-triplet include.
                    ports = [port for port in identity["packages"] if port.startswith("boost-")]
                inventory = vcpkg_header_inventory(
                    provider_prefix,
                    {port: identity["packages"][port]["version"].split("#", 1)[0] for port in ports},
                    [Path(path) for path in resolved_interface.header_dirs],
                )
                adapter.header_files[name] = list(inventory.files)
                for path in inventory.manifests:
                    adapter.record(path, "header_ownership")
        projection = adapter.resolve()
        manifest = projection["manifest"]
        if host == "windows":
            recorded = {entry["path"]: entry["sha256"] for entry in manifest["inputs"]}
            for observations in binary_observations.values():
                for observation in observations.values():
                    path = observation["path"]
                    if (
                        recorded.get(path) != observation["sha256"]
                        or native.sha256(Path(path)) != observation["sha256"]
                    ):
                        raise native.BazelWorkspaceError(
                            f"Windows binary changed during configuration/ABI admission: {path}"
                        )
        if host == "linux":
            providers.verify_linux_resolution(
                manifest, {package["name"]: expected_nix_outputs[package["name"]] for package in packages}
            )
        return {
            **projection,
            "watch_files": sorted(
                {
                    *[
                        entry["path"]
                        for entry in manifest["inputs"]
                        if set(entry["roles"]) - {"header", "library", "runtime"}
                    ],
                    *map(str, adapter.aliases),
                    *map(str, adapter.probes),
                }
            ),
            "watch_directories": list(map(str, [*adapter.libdirs, *adapter.pcdirs])),
        }
    raise native.BazelWorkspaceError(f"unsupported dependency group: {group}")
