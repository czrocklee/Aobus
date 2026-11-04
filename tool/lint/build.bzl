"""Build the existing native checker host forms against their own LLVM ABI."""

load("@aobus_lint_sdk//:flags.bzl", "LINT_SDK_COPTS", "LINT_SDK_HOST")
load("@rules_cc//cc:cc_binary.bzl", "cc_binary")

def _sdk_release_impl(_settings, _attr):
    return {"//command_line_option:compilation_mode": "opt"}

_sdk_release = transition(
    implementation = _sdk_release_impl,
    inputs = [],
    outputs = ["//command_line_option:compilation_mode"],
)

def _release_checker_impl(ctx):
    # Forward files, not the executable provider: the binary belongs to the
    # transitioned rule. Omit CcInfo so generic PE validation stays out of lint.
    return [DefaultInfo(files = ctx.attr.binary[0][DefaultInfo].files)]

_release_checker = rule(
    implementation = _release_checker_impl,
    attrs = {
        "binary": attr.label(cfg = _sdk_release, mandatory = True),
        "_allowlist_function_transition": attr.label(default = "@bazel_tools//tools/allowlists/function_transition_allowlist"),
    },
)

def lint_checker(srcs, hdrs):
    """Expose checker/host/plugin/tools without changing the product CRT or mode."""
    if LINT_SDK_HOST == "windows":
        cc_binary(
            name = "AobusClangTidy",
            srcs = srcs + hdrs + ["AobusClangTidyMain.cpp"],
            deps = ["@aobus_lint_sdk//:sdk"],
            copts = LINT_SDK_COPTS + [
                "/I" + native.package_name(),
                "/std:c++23preview",
                "/DNOMINMAX",
                "/D_CRT_SECURE_NO_WARNINGS",
                "/bigobj",
            ],
            # The MSVC action implies its default flags. SDK copts follow those
            # defaults and select the checker's exception/RTTI ABI explicitly.
            features = ["static_link_msvcrt", "-dynamic_link_msvcrt"],
            additional_linker_inputs = ["//app/windows:utf8-process.manifest"],
            linkopts = [
                "/MANIFEST:EMBED",
                "/MANIFESTINPUT:$(location //app/windows:utf8-process.manifest)",
            ],
            tags = ["manual"],
            visibility = ["//visibility:private"],
        )
        _release_checker(name = "checker", binary = ":AobusClangTidy")
        native.filegroup(name = "host", srcs = [":checker"])
        native.filegroup(name = "plugin")
    else:
        output = "libAobusLintPlugin.dylib" if LINT_SDK_HOST == "macos" else "libAobusLintPlugin.so"
        cc_binary(
            name = output,
            srcs = srcs + hdrs,
            deps = ["@aobus_lint_sdk//:sdk"],
            copts = LINT_SDK_COPTS + ["-iquote", native.package_name(), "-std=c++23", "-fno-sanitize=all"],
            linkopts = ["-fno-sanitize=all"],
            linkshared = True,
            linkstatic = True,
            tags = ["manual"],
        )
        native.alias(name = "checker", actual = ":" + output)
        native.filegroup(name = "host", srcs = ["@aobus_lint_sdk//:clang_tidy"])
        native.filegroup(name = "plugin", srcs = [":checker"])
    native.filegroup(name = "tools", srcs = [":host", ":plugin", "@aobus_lint_sdk//:tools"])
