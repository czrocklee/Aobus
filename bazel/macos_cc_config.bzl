"""Compose upstream Apple rules with the admitted native LLVM SDK."""

load("@apple_support//rules:apple_genrule.bzl", "apple_genrule")
load("@apple_support//toolchain:cc_toolchain.bzl", apple_cc_toolchain = "cc_toolchain")
load("@rules_cc//cc/toolchains:args.bzl", "cc_args")
load("@rules_cc//cc/toolchains:feature.bzl", "cc_feature")
load("@rules_cc//cc/toolchains:feature_set.bzl", "cc_feature_set")
load("@rules_cc//cc/toolchains:tool.bzl", "cc_tool")
load("@rules_cc//cc/toolchains:tool_map.bzl", "cc_tool_map")
load(":compiler_policy.bzl", "UNIX_WARNINGS")

_COMPILE = [
    "@rules_cc//cc/toolchains/actions:cpp_compile",
    "@rules_cc//cc/toolchains/actions:cpp_header_parsing",
    "@rules_cc//cc/toolchains/actions:objcpp_compile",
]
_LINK = [
    "@rules_cc//cc/toolchains/actions:cpp_link_executable",
    "@rules_cc//cc/toolchains/actions:cpp_link_dynamic_library",
    "@rules_cc//cc/toolchains/actions:cpp_link_nodeps_dynamic_library",
    "@rules_cc//cc/toolchains/actions:objc_executable",
]

def _shell_argument(value):
    # Preserve data through Bazel make-variable expansion and the POSIX shell.
    return "'" + value.replace("$", "$$").replace("'", "'\"'\"'") + "'"

def macos_toolchain(llvm, llvm_real, llvm_version, sdk, shim, linker, developer_dir, deployment, cpu, constraint_cpu, tools):
    """Keep native facts and project policy local; delegate Apple mechanics."""
    native.exports_files([
        "native_paths.bzl",
        "identity.json",
    ] + tools.values())
    native.filegroup(
        name = "identity",
        srcs = ["identity.json", "native_paths.bzl"],
    )
    constraints = ["@platforms//os:macos", "@platforms//cpu:" + constraint_cpu]
    native.platform(
        name = "platform",
        constraint_values = constraints,
        flags = ["--macos_minimum_os=" + deployment, "--host_macos_minimum_os=" + deployment],
    )
    native.config_setting(
        name = "admitted_deployment",
        values = {"macos_minimum_os": deployment, "host_macos_minimum_os": deployment},
    )

    # Bootstrap upstream source without resolving the C++ toolchain being built.
    # Metadata and installed tools remain inputs, not compiler source arguments.
    apple_genrule(
        name = "compiler_entry",
        srcs = ["@apple_support//crosstool:wrapped_clang.cc", ":identity"],
        tools = tools.values(),
        outs = ["entry/wrapped_clang", "entry/wrapped_clang_pp"],
        cmd = " ".join([
            "env",
            "DEVELOPER_DIR=" + _shell_argument(developer_dir),
            "SDKROOT=" + _shell_argument(sdk),
            _shell_argument(llvm + "/bin/clang++"),
            "--no-default-config -std=c++17 -stdlib=libc++ -O2",
            "-target " + _shell_argument(cpu + "-apple-macosx" + deployment),
            "-isysroot " + _shell_argument(sdk),
            _shell_argument("--ld-path=" + linker),
            "'$(execpath @apple_support//crosstool:wrapped_clang.cc)'",
            "-o '$(execpath :entry/wrapped_clang)'",
            "&& ln -s wrapped_clang '$(execpath :entry/wrapped_clang_pp)'",
        ]),
        exec_compatible_with = ["@platforms//os:macos"],
    )
    for name, source in [("clang", "entry/wrapped_clang"), ("clang++", "entry/wrapped_clang_pp")]:
        cc_tool(
            name = name,
            src = source,
            # Compiler entry can invoke installed LLVM, Apple ld, dsymutil and
            # strip. Projected bytes and the live SDK ledger bind that closure.
            data = [":identity"] + tools.values(),
            env = {"APPLE_SUPPORT_LLVM_ROOT": llvm},
        )
    cc_tool(name = "libtool", src = tools["ar"], data = [":identity"])
    cc_tool(name = "strip", src = tools["strip"], data = [":identity"])
    cc_tool_map(
        name = "tools",
        tools = {
            "@rules_cc//cc/toolchains/actions:assembly_actions": ":clang",
            "@rules_cc//cc/toolchains/actions:c_compile_actions": ":clang",
            "@rules_cc//cc/toolchains/actions:cc_flags_make_variable": ":clang++",
            "@rules_cc//cc/toolchains/actions:cpp_compile": ":clang++",
            "@rules_cc//cc/toolchains/actions:cpp_header_parsing": ":clang++",
            "@rules_cc//cc/toolchains/actions:linkstamp_compile": ":clang++",
            "@rules_cc//cc/toolchains/actions:objcpp_compile": ":clang++",
            "@rules_cc//cc/toolchains/actions:objc_compile": ":clang",
            "@rules_cc//cc/toolchains/actions:cpp_link_executable": ":clang++",
            "@rules_cc//cc/toolchains/actions:cpp_link_dynamic_library": ":clang++",
            "@rules_cc//cc/toolchains/actions:cpp_link_nodeps_dynamic_library": ":clang++",
            "@rules_cc//cc/toolchains/actions:objc_executable": ":clang++",
            "@rules_cc//cc/toolchains/actions:ar_actions": ":libtool",
            "@rules_cc//cc/toolchains/actions:strip": ":strip",
        },
    )

    cc_feature(name = "aobus_warnings", feature_name = "aobus_warnings")
    cc_args(
        name = "warning_policy",
        actions = _COMPILE,
        args = UNIX_WARNINGS,
        requires_any_of = [":aobus_warnings"],
    )
    cc_feature_set(name = "known_policy", all_of = [":aobus_warnings"])
    # Replace automatic SDK environment and broad include exemptions together.
    # This unconditional input also binds identity for every selected action.
    cc_args(
        name = "native_inputs",
        actions = ["@rules_cc//cc/toolchains/actions:all_actions"],
        allowlist_absolute_include_directories = [llvm, llvm_real, sdk, shim],
        env = {
            "DEVELOPER_DIR": developer_dir,
            "SDKROOT": sdk,
            "ZERO_AR_DATE": "1",
        },
        data = select(
            {":admitted_deployment": [":identity"]},
            no_match_error = "Select @aobus_toolchain//:platform with admitted macOS target/exec deployment " + deployment,
        ),
    )
    cc_args(
        name = "cpp_policy",
        actions = _COMPILE + ["@rules_cc//cc/toolchains/actions:linkstamp_compile"],
        args = ["-std=c++26", "-stdlib=libc++", "-isystem", shim],
    )
    cc_args(
        name = "selected_linker",
        actions = _LINK,
        args = ["--ld-path=" + linker, "-stdlib=libc++", "-isysroot", sdk],
        data = [":identity"],
    )
    cc_args(
        name = "sysroot_args",
        actions = ["@rules_cc//cc/toolchains/actions:compile_actions"],
        args = ["-isysroot", sdk],
        data = [":identity"],
    )
    cc_feature(name = "sysroot", feature_name = "sysroot", args = [":sysroot_args"])
    cc_feature(
        name = "native_policy",
        feature_name = "aobus_native_policy",
        # The upstream macro inserts extra enabled features after its defaults
        # and before user copts. Gate only warnings on the repository request.
        args = [":warning_policy", ":cpp_policy", ":selected_linker"],
    )
    cc_feature_set(
        name = "enabled_policy",
        all_of = [":native_policy", "@apple_support//toolchain:external_include_paths"],
    )

    apple_cc_toolchain(
        name = "llvm22",
        target = cpu + "-apple-macosx" + deployment,
        module_map = "@apple_support//crosstool:module.modulemap",
        sysroot_feature = ":sysroot",
        supports_header_parsing = True,
        tool_map = ":tools",
        flags_from_env = False,
        llvm_version = llvm_version,
        platform_args = ":native_inputs",
        extra_enabled_features = ":enabled_policy",
        extra_known_features = ":known_policy",
    )
    native.toolchain(
        name = "registered_llvm22",
        exec_compatible_with = constraints,
        target_compatible_with = constraints,
        toolchain = ":llvm22",
        toolchain_type = "@bazel_tools//tools/cpp:toolchain_type",
    )
