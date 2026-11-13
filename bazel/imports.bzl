"""Declare native SDK imports from projected, repository-relative graph data."""

load("@rules_cc//cc:defs.bzl", "cc_import", "cc_library")
load(":import_repository.bzl", "alias_targets", "import_targets")

def _prepared(config, value):
    mode = "dbg" if config == "debug" else "opt"
    return select({
        ":_prepared_mode": value,
    }, no_match_error = "Prepared %s dependencies require -c %s" % (config.capitalize(), mode))

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
            hdrs = [":" + header["name"] for header in package["headers"]],
            includes = package["includes"],
            defines = package["defines"],
            linkopts = package["linkopts"],
            deps = _prepared(config, deps),
        )

def configured_aliases(targets):
    """Select SDK packages in the consumer configuration, never fastbuild.

    Args:
        targets: Public SDK target names shared by the debug and release subpackages.
    """
    targets = alias_targets(targets)
    native.config_setting(name = "_debug", values = {"compilation_mode": "dbg"})
    native.config_setting(name = "_release", values = {"compilation_mode": "opt"})
    for target in targets:
        native.alias(
            name = target,
            actual = select({
                ":_debug": "//debug:" + target,
                ":_release": "//release:" + target,
            }, no_match_error = "Native SDK imports support dbg or opt; fastbuild is not supported"),
        )
