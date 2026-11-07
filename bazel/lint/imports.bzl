"""Declare the lint SDK repository graph from projected, data-only SDK facts."""

load("@rules_cc//cc:defs.bzl", "cc_import", "cc_library")

def lint_sdk_imports(sdk):
    """Declare the fixed lint SDK targets from repository-relative SDK data.

    The repository rule owns the projected layout; native admission stays in
    the helper. The globs evaluate in the generated repository package.
    """
    if sdk["host"] not in ["linux", "macos", "windows"]:
        fail("Unsupported lint SDK host: %s" % sdk["host"])
    native.exports_files(["metadata.json", "flags.bzl"])
    dependencies = []
    for index, library in enumerate(sdk["libraries"]):
        name = "library_%d" % index
        if library.endswith(".a") or library.endswith(".lib"):
            cc_import(name = name, static_library = library)
        else:
            cc_import(name = name, shared_library = library)
        dependencies.append(":" + name)
    cc_library(
        name = "sdk",
        hdrs = native.glob(["include/**"]),
        includes = sdk["includes"],
        deps = dependencies,
        linkopts = sdk["linkopts"],
    )
    native.filegroup(name = "clang_tidy", srcs = [sdk["clang_tidy"]])
    native.filegroup(
        name = "tools",
        srcs = [":clang_tidy", "metadata.json"] + native.glob(["resource/**"]) + [sdk["clang"]] + sdk["runtime"],
    )
