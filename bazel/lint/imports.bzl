"""Declare the lint SDK repository graph from projected, data-only SDK facts."""

load("@rules_cc//cc:defs.bzl", "cc_import", "cc_library")

def _unavailable_impl(ctx):
    fail("Cannot use the lint SDK (not provisioned, no provisioning attempted): " + ctx.attr.diagnostic)

_lint_sdk_unavailable = rule(
    implementation = _unavailable_impl,
    attrs = {"diagnostic": attr.string(mandatory = True)},
    # metadata.json is this rule's declared output, never a fake repository
    # source file: selecting it analyzes the failing rule that would generate it.
    outputs = {"metadata": "metadata.json"},
)

def lint_sdk_imports(sdk):
    """Declare the fixed lint SDK targets from repository-relative SDK data.

    The repository rule owns the projected layout; native admission stays in
    the helper. The globs evaluate in the generated repository package.

    Args:
        sdk: Projected lint SDK paths and interface facts, or unavailable-state diagnostics.
    """
    if sdk["host"] not in ["linux", "macos", "windows"]:
        fail("Unsupported lint SDK host: %s" % sdk["host"])
    if sdk.get("state", "") == "unavailable":
        # Query-only projection: the repository graph is loadable, but every
        # actual lint target fails analysis with the retained native diagnostic.
        _lint_sdk_unavailable(name = "sdk", diagnostic = sdk["diagnostic"])
        native.alias(name = "clang_tidy", actual = ":sdk")
        native.alias(name = "tools", actual = ":sdk")
        native.exports_files(["flags.bzl"])
        return
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
