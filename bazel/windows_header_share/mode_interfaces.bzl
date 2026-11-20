"""Verify the header interface of a real vcpkg import across Bazel modes."""

load("@rules_cc//cc:find_cc_toolchain.bzl", "use_cc_toolchain")
load("@rules_cc//cc/common:cc_info.bzl", "CcInfo")

_RELEASE_HEADER = "/release/artifacts/_headers/rapidyaml_0/ryml/ryml.hpp"
_HELPER = Label("@aobus_native_deps//release:_shared_headers_rapidyaml")

# An unnecessary helper may leave exactly the same CcInfo and compiler inputs.
_HelperEdgeInfo = provider(doc = "Whether the import directly depends on the Release header helper.", fields = ["present"])

def _helper_edge_impl(_target, ctx):
    deps = getattr(ctx.rule.attr, "deps", []) + getattr(ctx.rule.attr, "implementation_deps", [])
    return [_HelperEdgeInfo(present = any([dep.label == _HELPER for dep in deps]))]

_helper_edge = aspect(implementation = _helper_edge_impl)

def _mode(mode):
    return attr.label(
        aspects = [_helper_edge],
        cfg = analysis_test_transition(settings = {
            "//command_line_option:compilation_mode": mode,
            # Keep the Windows native toolchain, but not the Release platform's forced opt.
            "//command_line_option:platforms": ["@aobus_environment//:native_platform"],
        }),
        providers = [CcInfo],
    )

def _has_release_header(headers):
    return any([header.short_path.endswith(_RELEASE_HEADER) for header in headers])

def _mode_interfaces_impl(ctx):
    errors = []
    for mode, target, forwards in [
        ("Debug", ctx.attr.debug[0], True),
        ("Release", ctx.attr.release[0], False),
        ("fastbuild", ctx.attr.fastbuild[0], False),
    ]:
        context = target[CcInfo].compilation_context
        if not _has_release_header(context.headers.to_list()):
            errors.append(mode + " does not export the sparse Release header")
        if forwards and context.direct_public_headers:
            errors.append(mode + " should forward rather than own public headers")
        if not forwards and not _has_release_header(context.direct_public_headers):
            errors.append(mode + " does not directly own the sparse Release header")
        if target[_HelperEdgeInfo].present != forwards:
            errors.append(mode + (" is missing the Release helper edge" if forwards else " unexpectedly depends on the Release helper"))
    return [AnalysisTestResultInfo(success = not errors, message = "\n".join(errors))]

windows_header_mode_test = rule(
    implementation = _mode_interfaces_impl,
    analysis_test = True,
    test = True,
    attrs = {
        "debug": _mode("dbg"),
        "fastbuild": _mode("fastbuild"),
        "release": _mode("opt"),
    },
    # The named Release platform's profile constraint is target-only.
    exec_groups = {"test": exec_group(toolchains = use_cc_toolchain())},
)
