"""Native Homebrew LLVM configuration; the portal supplies SDK/shim identity."""

load("@rules_cc//cc:cc_toolchain_config_lib.bzl", "action_config", "feature", "flag_group", "flag_set", "tool", "tool_path")
load("@rules_cc//cc/common:cc_common.bzl", "cc_common")
load("@rules_cc//cc/toolchains:cc_toolchain.bzl", "cc_toolchain")
load("@rules_cc//cc/toolchains:cc_toolchain_config_info.bzl", "CcToolchainConfigInfo")
load(":compiler_policy.bzl", "UNIX_WARNINGS")

_OBJCXX_COMPILE = "objc++-compile"
_COMPILE = ["c++-compile", "c++-header-parsing", _OBJCXX_COMPILE]
_LINK = ["c++-link-executable", "c++-link-dynamic-library", "c++-link-nodeps-dynamic-library"]

def _impl(ctx):
    return cc_common.create_cc_toolchain_config_info(
        ctx = ctx,
        toolchain_identifier = "aobus-homebrew-llvm22",
        host_system_name = "local",
        target_system_name = ctx.attr.cpu + "-apple-macosx",
        target_cpu = "darwin_" + ctx.attr.cpu,
        target_libc = "macosx",
        compiler = "clang",
        abi_version = "local",
        abi_libc_version = "local",
        cxx_builtin_include_directories = [ctx.attr.llvm, ctx.attr.llvm_real, ctx.attr.sdk, ctx.attr.shim],
        tool_paths = [
            tool_path(name = "gcc", path = ctx.attr.llvm + "/bin/clang++"),
            tool_path(name = "ld", path = ctx.attr.llvm + "/bin/clang++"),
            tool_path(name = "cpp", path = ctx.attr.llvm + "/bin/clang++"),
            tool_path(name = "ar", path = ctx.attr.llvm + "/bin/llvm-libtool-darwin"),
            tool_path(name = "nm", path = ctx.attr.llvm + "/bin/llvm-nm"),
            tool_path(name = "objdump", path = ctx.attr.llvm + "/bin/llvm-objdump"),
            tool_path(name = "strip", path = ctx.attr.llvm + "/bin/llvm-strip"),
            tool_path(name = "gcov", path = "/usr/bin/false"),
        ],
        # Objective-C++ stays on the same governed Homebrew compiler and SDK.
        # No Apple auto-configuration or second Xcode toolchain is selected.
        action_configs = [action_config(
            action_name = _OBJCXX_COMPILE,
            enabled = True,
            tools = [tool(path = ctx.attr.llvm + "/bin/clang++")],
        )],
        features = [
            feature(name = "supports_pic", enabled = True),
            # Selected by repository defaults, not enabled for external targets.
            # Legacy user_compile_flags are appended after supplied features,
            # preserving ordinary copts precedence over these warning defaults.
            feature(
                name = "aobus_warnings",
                flag_sets = [flag_set(
                    actions = _COMPILE,
                    flag_groups = [flag_group(flags = UNIX_WARNINGS)],
                )],
            ),
            # Compilation-mode features are requested by rules_cc, not inferred
            # from the dbg/opt output-directory names of a custom toolchain.
            feature(
                name = "dbg",
                flag_sets = [flag_set(
                    actions = _COMPILE,
                    flag_groups = [flag_group(flags = ["-O0", "-g"])],
                )],
            ),
            feature(
                name = "opt",
                flag_sets = [flag_set(
                    actions = _COMPILE,
                    flag_groups = [flag_group(flags = ["-O2", "-DNDEBUG"])],
                )],
            ),
            feature(
                name = "fastbuild",
                flag_sets = [flag_set(
                    actions = _COMPILE,
                    flag_groups = [flag_group(flags = ["-O0", "-g0"])],
                )],
            ),
            feature(
                name = "external_include_paths",
                enabled = True,
                flag_sets = [flag_set(
                    actions = _COMPILE,
                    flag_groups = [flag_group(
                        flags = ["-isystem", "%{external_include_paths}"],
                        iterate_over = "external_include_paths",
                        expand_if_available = "external_include_paths",
                    )],
                )],
            ),
            feature(
                name = "aobus_compile",
                enabled = True,
                flag_sets = [flag_set(
                    actions = _COMPILE,
                    flag_groups = [flag_group(flags = [
                        "--no-default-config",
                        "-std=c++26",
                        "-stdlib=libc++",
                        "-isysroot",
                        ctx.attr.sdk,
                        "-mmacosx-version-min=" + ctx.attr.deployment,
                        "-isystem",
                        ctx.attr.shim,
                    ])],
                )],
            ),
            # Legacy dependency-file, include-path and compiler input/output
            # features already cover Objective-C++; legacy defines and user
            # flags do not. Preserve those channels explicitly for this action.
            feature(
                name = "aobus_objcxx_compile",
                enabled = True,
                flag_sets = [flag_set(
                    actions = [_OBJCXX_COMPILE],
                    flag_groups = [
                        flag_group(flags = ["-fobjc-arc"]),
                        flag_group(
                            iterate_over = "preprocessor_defines",
                            flags = ["-D%{preprocessor_defines}"],
                        ),
                        flag_group(
                            iterate_over = "user_compile_flags",
                            flags = ["%{user_compile_flags}"],
                        ),
                    ],
                )],
            ),
            feature(
                name = "aobus_link",
                enabled = True,
                flag_sets = [flag_set(
                    actions = _LINK,
                    flag_groups = [flag_group(flags = [
                        "--no-default-config",
                        "--ld-path=" + ctx.attr.linker,
                        "-stdlib=libc++",
                        "-isysroot",
                        ctx.attr.sdk,
                        "-mmacosx-version-min=" + ctx.attr.deployment,
                    ])],
                )],
            ),
        ],
    )

macos_cc_config = rule(
    implementation = _impl,
    attrs = {
        "llvm": attr.string(mandatory = True),
        "llvm_real": attr.string(mandatory = True),
        "sdk": attr.string(mandatory = True),
        "shim": attr.string(mandatory = True),
        "linker": attr.string(mandatory = True),
        "deployment": attr.string(mandatory = True),
        "cpu": attr.string(mandatory = True),
    },
    provides = [CcToolchainConfigInfo],
)

def macos_toolchain(llvm, llvm_real, sdk, shim, linker, deployment, cpu, constraint_cpu):
    """Declare the native graph in the generated repository using selected metadata."""
    native.exports_files([
        "native_paths.bzl",
        "identity.json",
    ])

    macos_cc_config(
        name = "config",
        llvm = llvm,
        llvm_real = llvm_real,
        sdk = sdk,
        shim = shim,
        linker = linker,
        deployment = deployment,
        cpu = cpu,
    )

    native.filegroup(
        name = "identity",
        srcs = ["identity.json"],
    )

    cc_toolchain(
        name = "llvm22",
        all_files = ":identity",
        ar_files = ":identity",
        as_files = ":identity",
        compiler_files = ":identity",
        dwp_files = ":identity",
        linker_files = ":identity",
        objcopy_files = ":identity",
        strip_files = ":identity",
        supports_param_files = 0,
        toolchain_config = ":config",
        toolchain_identifier = "aobus-homebrew-llvm22",
    )

    native.toolchain(
        name = "registered_llvm22",
        exec_compatible_with = [
            "@platforms//os:macos",
            "@platforms//cpu:" + constraint_cpu,
        ],
        target_compatible_with = [
            "@platforms//os:macos",
            "@platforms//cpu:" + constraint_cpu,
        ],
        toolchain = ":llvm22",
        toolchain_type = "@bazel_tools//tools/cpp:toolchain_type",
    )
