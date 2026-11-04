"""Compile the AppKit boundary with the governed native C++ toolchain."""

load("@rules_cc//cc:find_cc_toolchain.bzl", "find_cc_toolchain", "use_cc_toolchain")
load("@rules_cc//cc/common:cc_common.bzl", "cc_common")
load("@rules_cc//cc/common:cc_info.bzl", "CcInfo")

def _appkit_objc_library_impl(ctx):
    toolchain = find_cc_toolchain(ctx)
    features = cc_common.configure_features(
        ctx = ctx,
        cc_toolchain = toolchain,
        language = "objc++",
        requested_features = ctx.features,
        unsupported_features = ctx.disabled_features,
    )
    if not cc_common.action_is_enabled(
        feature_configuration = features,
        action_name = "objc++-compile",
    ):
        fail("AppKit requires the governed Objective-C++ compile action")
    contexts = [dependency[CcInfo] for dependency in ctx.attr.deps]
    compilation_context, outputs = cc_common.compile(
        actions = ctx.actions,
        name = ctx.label.name,
        cc_toolchain = toolchain,
        feature_configuration = features,
        language = "objc++",
        srcs = ctx.files.srcs,
        public_hdrs = ctx.files.hdrs,
        includes = [ctx.label.package],
        compilation_contexts = [info.compilation_context for info in contexts],
    )
    linking_context, linking_outputs = cc_common.create_linking_context_from_compilation_outputs(
        actions = ctx.actions,
        name = ctx.label.name,
        cc_toolchain = toolchain,
        feature_configuration = features,
        compilation_outputs = outputs,
        linking_contexts = [info.linking_context for info in contexts],
        user_link_flags = ctx.attr.linkopts,
        # CMake's OBJECT owner links every production translation unit into the
        # product. Preserve Objective-C class/category registration as well.
        alwayslink = True,
        disallow_dynamic_library = True,
    )
    library = linking_outputs.library_to_link
    archives = [file for file in [library.static_library, library.pic_static_library] if file != None]
    return [
        DefaultInfo(files = depset(archives)),
        CcInfo(
            compilation_context = compilation_context,
            linking_context = linking_context,
        ),
    ]

# rules_cc's cc_library does not admit .mm sources. This package-local rule
# uses only public CcInfo/cc_common APIs; compiler arguments remain toolchain
# policy rather than a second shell compiler launcher or Apple auto-toolchain.
appkit_objc_library = rule(
    implementation = _appkit_objc_library_impl,
    attrs = {
        "srcs": attr.label_list(allow_files = [".mm"], mandatory = True),
        "hdrs": attr.label_list(allow_files = [".h"]),
        "deps": attr.label_list(providers = [CcInfo]),
        "linkopts": attr.string_list(),
    },
    fragments = ["cpp"],
    toolchains = use_cc_toolchain(),
    provides = [CcInfo],
)
