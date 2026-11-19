"""Declare native SDK imports from projected, repository-relative graph data."""

load("@rules_cc//cc:defs.bzl", "cc_import", "cc_library")
load(":import_repository.bzl", "alias_targets", "import_targets")

def _prepared(config, value):
    # Debug artifacts keep their dbg-only ABI guard. Release artifacts also
    # serve fastbuild consumers: the pinned toolchains give fastbuild the
    # Release CRT and dependency ABI on every native platform.
    if config == "debug":
        return select({
            ":_prepared_mode": value,
        }, no_match_error = "Prepared Debug dependencies require -c dbg")
    return select({
        ":_prepared_mode": value,
        ":_prepared_fastbuild": value,
    }, no_match_error = "Prepared Release dependencies require -c opt or -c fastbuild")

def sdk_imports(config, packages, platform):
    """Declare fixed native import rules from projected, data-only SDK values.

    Args:
        config: Prepared SDK configuration, either debug or release.
        packages: Projected package interfaces with repository-relative header and library inputs.
        platform: Admitted SDK platform. Indirect DLL imports are generated only for windows.
    """
    if config not in ["debug", "release"]:
        fail("unsupported SDK configuration: %s" % config)
    if platform not in ["linux", "macos", "windows"]:
        fail("unsupported SDK platform: %s" % platform)
    import_targets(packages)
    native.exports_files(["manifest.json"])
    native.config_setting(
        name = "_prepared_mode",
        values = {"compilation_mode": "dbg" if config == "debug" else "opt"},
    )
    if config == "release":
        native.config_setting(
            name = "_prepared_fastbuild",
            values = {"compilation_mode": "fastbuild"},
        )

    # The admitted platform, not the host loading this macro. A non-Windows
    # toolchain would link a shared_library-only import, so it is not generated there.
    windows = platform == "windows"
    imports = {}
    indirect = {}
    for package in packages:
        for header in package["headers"]:
            native.filegroup(
                name = header["name"],
                srcs = header["files"],
            )
        hdrs = [":" + header["name"] for header in package["headers"]]
        includes = package["includes"]
        header_deps = []
        header_package = package.get("header_package")
        if header_package != None:
            # Only paths are shared; defines, libraries, runtime, and mode guards
            # stay on the original configured package. Preserve fastbuild include spellings.
            header_target = "_shared_headers_" + package["name"]
            if config == "release":
                cc_library(
                    name = header_target,
                    hdrs = hdrs,
                    includes = includes,
                    visibility = ["//" + header_package.removesuffix("release") + "debug:__pkg__"],
                )
            header_deps = ["//" + header_package + ":" + header_target]
            hdrs = []
            includes = []
        deps = []
        for library in package["libraries"]:
            name = library["name"]
            deps.append(":" + name)
            if name in imports:
                continue
            imports[name] = True
            if library["kind"] == "static":
                cc_import(
                    name = name,
                    static_library = _prepared(config, library["file"]),
                    visibility = ["//visibility:private"],
                )
            elif library["runtime"] != None:
                cc_import(
                    name = name,
                    interface_library = _prepared(config, library["file"]),
                    shared_library = library["runtime"],
                    visibility = ["//visibility:private"],
                )
            else:
                cc_import(
                    name = name,
                    shared_library = _prepared(config, library["file"]),
                    visibility = ["//visibility:private"],
                )
        if windows:
            for item in package["runtime"]:
                name = item["name"]
                deps.append(":" + name)
                if name in indirect:
                    continue
                indirect[name] = True
                cc_import(
                    name = name,
                    shared_library = _prepared(config, item["file"]),
                    visibility = ["//visibility:private"],
                )
        cc_library(
            name = package["name"],
            hdrs = hdrs,
            includes = includes,
            defines = package["defines"],
            linkopts = package["linkopts"],
            deps = header_deps + _prepared(config, deps),
        )

def _unavailable_dependency_impl(ctx):
    fail("Native SDK dependency %s is not supported on this host" % ctx.label)

_unavailable_dependency = rule(implementation = _unavailable_dependency_impl)

def configured_aliases(targets, unavailable = []):
    """Select admitted SDK packages and retain unsupported names for query.

    Args:
        targets: Public SDK target names shared by the debug and release subpackages.
        unavailable: Host-excluded package names that must fail if selected for analysis.
    """
    alias_targets(targets + unavailable)
    targets = sorted(targets)
    native.config_setting(name = "_debug", values = {"compilation_mode": "dbg"})
    native.config_setting(name = "_fastbuild", values = {"compilation_mode": "fastbuild"})
    native.config_setting(name = "_release", values = {"compilation_mode": "opt"})
    for target in targets:
        # fastbuild consumes the Release SDK; there is no third dependency
        # configuration, so it shares the Release branch instead of adding one.
        native.alias(
            name = target,
            actual = select({
                ":_debug": "//debug:" + target,
                ":_fastbuild": "//release:" + target,
                ":_release": "//release:" + target,
            }, no_match_error = "Native SDK imports require dbg, fastbuild or opt"),
        )

    # Query follows every select branch. Keep those labels loadable without
    # inventing an SDK or letting a selected consumer silently become incompatible.
    for target in sorted(unavailable):
        _unavailable_dependency(name = target)
