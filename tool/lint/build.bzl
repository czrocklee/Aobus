"""Build the existing native checker host forms against their own LLVM ABI."""

load("@aobus_lint_sdk//:flags.bzl", "LINT_SDK_COPTS", "LINT_SDK_HOST")
load("@rules_cc//cc:defs.bzl", "cc_binary")
load("//bazel:compiler_policy.bzl", "UNIX_LLVM_HOST_COPTS")
load("//bazel:msvc_policy.bzl", "MSVC_LLVM_HOST_COPTS")
load("//bazel:windows_utf8_manifest.bzl", "WINDOWS_UTF8_LINKER_INPUTS", "WINDOWS_UTF8_LINKOPTS")

def _sdk_release_impl(settings, _attr):
    if settings["//bazel:asan_enabled"] or settings["//bazel:tsan_enabled"]:
        fail("The lint checker uses its own uninstrumented LLVM ABI; select a normal Debug/Release lint profile")
    return {"//command_line_option:compilation_mode": "opt"}

_sdk_release = transition(
    implementation = _sdk_release_impl,
    inputs = ["//bazel:asan_enabled", "//bazel:tsan_enabled"],
    outputs = ["//command_line_option:compilation_mode"],
)

def _release_checker_impl(ctx):
    # Forward files, not the executable provider: the binary belongs to the
    # transitioned rule. Omit CcInfo so generic PE validation stays out of lint.
    return [DefaultInfo(files = ctx.attr.lint_checker_binary[0][DefaultInfo].files)]

_release_checker = rule(
    implementation = _release_checker_impl,
    attrs = {
        "lint_checker_binary": attr.label(cfg = _sdk_release, mandatory = True),
        "_allowlist_function_transition": attr.label(default = "@bazel_tools//tools/allowlists/function_transition_allowlist"),
    },
)

def lint_checker(srcs, hdrs):
    """Expose checker/host/plugin/tools without changing the product CRT or mode.

    Args:
        srcs: Checker implementation sources shared by the Windows host and Unix plugin.
        hdrs: Checker headers included in the native binary's declared source inputs.
    """
    if LINT_SDK_HOST == "windows":
        cc_binary(
            name = "AobusClangTidy",
            srcs = srcs + hdrs + ["AobusClangTidyMain.cpp"],
            deps = ["@aobus_lint_sdk//:sdk"],
            # The compiling MSVC toolchain owns the language standard; the SDK
            # metadata's own std/EH/RTTI spellings never reach these copts.
            copts = LINT_SDK_COPTS + ["/I" + native.package_name()] + MSVC_LLVM_HOST_COPTS,
            # Keep all ordinary defaults except /EHsc: the SDK's /EHs-c- must
            # select the no-exception ABI without contradictory driver flags.
            features = ["static_link_msvcrt", "-dynamic_link_msvcrt", "-default_exception_handling"],
            additional_linker_inputs = WINDOWS_UTF8_LINKER_INPUTS,
            linkopts = WINDOWS_UTF8_LINKOPTS,
            tags = ["manual", "aobus-lint-sdk-host"],
            visibility = ["//visibility:private"],
        )
        _release_checker(name = "checker", lint_checker_binary = ":AobusClangTidy")
        native.filegroup(name = "host", srcs = [":checker"])
        native.filegroup(name = "plugin")
    else:
        output = "libAobusLintPlugin.dylib" if LINT_SDK_HOST == "macos" else "libAobusLintPlugin.so"
        compatibility_headers = ["@aobus_lint_sdk//:GccLlvmNonnullCompatibility.h"] if LINT_SDK_HOST == "linux" else []
        compatibility_copts = [
            "-include",
            "$(execpath %s)" % compatibility_headers[0],
        ] if compatibility_headers else []
        cc_binary(
            name = output,
            srcs = srcs + hdrs + compatibility_headers,
            deps = ["@aobus_lint_sdk//:sdk"],
            # The compiling Unix toolchain owns the language standard; the SDK
            # host keeps its own unsanitized LLVM ABI for compile and link.
            # The forced include scopes GCC 16 / LLVM 22's nonnull workaround
            # to the SDK definitions, never the checker implementation.
            copts = LINT_SDK_COPTS + ["-iquote", native.package_name()] + UNIX_LLVM_HOST_COPTS + compatibility_copts,
            linkopts = UNIX_LLVM_HOST_COPTS,
            linkshared = True,
            linkstatic = True,
            tags = ["manual", "aobus-lint-sdk-host"],
        )
        native.alias(name = "checker", actual = ":" + output)
        native.filegroup(name = "host", srcs = ["@aobus_lint_sdk//:clang_tidy"])
        native.filegroup(name = "plugin", srcs = [":checker"], tags = ["manual", "aobus-lint-sdk-host"])
    native.filegroup(name = "tools", srcs = [":host", ":plugin", "@aobus_lint_sdk//:tools"], tags = ["manual", "aobus-lint-sdk-host"])
