"""Installed MSVC toolchain policy instantiated in the generated repository."""

# Narrow, pinned-private MSVC configuration dependency. The documented
# migration bridge handles data-free warnings and external-header selection;
# installed tools, PDBs, CRT, response files and runtime copying stay upstream.
load("@rules_cc//cc/private/toolchain:windows_cc_toolchain_config.bzl", "cc_toolchain_config")
load("@rules_cc//cc/toolchains:args.bzl", "cc_args")
load("@rules_cc//cc/toolchains:cc_toolchain.bzl", "cc_toolchain")
load("@rules_cc//cc/toolchains:feature.bzl", "cc_feature")

def windows_toolchain(tools, tool_inputs, builtin_include_directories, msvc_environment):
    """Declare installed execution paths separately from caller-relative inputs."""
    native.exports_files(["identity.json", "native_paths.bzl"])

    native.filegroup(
        name = "metadata",
        srcs = ["identity.json", "native_paths.bzl"],
    )

    native.filegroup(
        name = "compiler_inputs",
        srcs = [":metadata", tool_inputs["cl"]],
    )

    native.filegroup(
        name = "assembler_inputs",
        srcs = [":metadata", tool_inputs["ml64"]],
    )

    native.filegroup(
        name = "archiver_inputs",
        srcs = [":metadata", tool_inputs["lib"]],
    )

    native.filegroup(
        name = "linker_inputs",
        srcs = [":metadata", tool_inputs["link"]],
    )

    native.filegroup(
        name = "all_inputs",
        srcs = [":compiler_inputs", ":assembler_inputs", ":archiver_inputs", ":linker_inputs"],
    )

    native.filegroup(name = "empty")

    cc_args(
        name = "warning_args",
        actions = ["@rules_cc//cc/toolchains/actions:cpp_compile_actions"],
        args = ["/W4", "/WX"],
    )

    # Initially disabled: only owned targets request this through REPO.bazel.
    # The legacy bridge appends after user flags; use feature opt-out, not /WX-.
    cc_feature(
        name = "aobus_warnings",
        feature_name = "aobus_warnings",
        args = [":warning_args"],
    )

    # Reference the existing legacy feature by name; do not register it again.
    cc_feature(
        name = "msvc_external_include_paths",
        feature_name = "external_include_paths",
    )

    # Toolchain default for target and exec consumers, independent of warnings.
    cc_feature(
        name = "aobus_external_headers",
        feature_name = "aobus_external_headers",
        implies = [":msvc_external_include_paths"],
    )

    cc_toolchain_config(
        name = "msvc_config",
        cpu = "x64_windows",
        compiler = "msvc-cl",
        host_system_name = "local",
        target_system_name = "local",
        target_libc = "msvcrt",
        abi_version = "local",
        abi_libc_version = "local",
        toolchain_identifier = "aobus_msvc_x64",
        # Execute installed tools in their original DLL layout. Bazel-owned links
        # above declare their bytes, not an independently executable SDK package.
        msvc_cl_path = tools["cl"],
        msvc_link_path = tools["link"],
        msvc_lib_path = tools["lib"],
        msvc_ml_path = tools["ml64"],
        msvc_env_include = msvc_environment["INCLUDE"],
        msvc_env_lib = msvc_environment["LIB"],
        msvc_env_path = msvc_environment["PATH"],
        msvc_env_tmp = msvc_environment["TMP"],
        cxx_builtin_include_directories = builtin_include_directories,
        tool_paths = {
            "ar": tools["lib"],
            "cpp": tools["cl"],
            "gcc": tools["cl"],
            "ld": tools["link"],
            "ml": tools["ml64"],
            # Required legacy roles, not supported Unix tools. Missing paths fail
            # closed if requested; no NOP, gcov alias or undeclared module scanner.
            "nm": "unsupported/nm",
            "objdump": "unsupported/objdump",
            "strip": "unsupported/strip",
        },
        archiver_flags = ["/MACHINE:X64"],
        default_link_flags = ["/MACHINE:X64"],
        # Retain admitted native debug-linker and English showIncludes capabilities.
        dbg_mode_debug_flag = "/DEBUG:FULL",
        fastbuild_mode_debug_flag = "/DEBUG:FASTLINK",
        supports_parse_showincludes = True,
        shorten_virtual_includes = True,
        # Pinned default_cpp_std still emits c++17 first (known D9025 baseline).
        # NOMINMAX and _CRT_SECURE_NO_WARNINGS already belong to default flags.
        # Dynamic CRT, NDEBUG, debug/PDB and optimization follow the actual Bazel
        # compilation_mode, independently for target and exec consumers.
        cxx_flags = [
            "/std:c++latest",
            "/external:W0",
            "/utf-8",
            "/permissive-",
            "/Zc:__cplusplus",
            "/Zc:preprocessor",
            "/DWIN32_LEAN_AND_MEAN",
            "/DUNICODE",
            "/D_UNICODE",
            "/DWINVER=0x0A00",
        ],
        win32_winnt_flag = "/D_WIN32_WINNT=0x0A00",
        extra_enabled_features = [":aobus_external_headers"],
        extra_known_features = [":aobus_warnings"],
    )

    cc_toolchain(
        name = "msvc",
        toolchain_identifier = "aobus_msvc_x64",
        toolchain_config = ":msvc_config",
        all_files = ":all_inputs",
        compiler_files = ":compiler_inputs",
        as_files = ":assembler_inputs",
        ar_files = ":archiver_inputs",
        linker_files = ":linker_inputs",
        dwp_files = ":empty",
        objcopy_files = ":empty",
        strip_files = ":empty",
        supports_param_files = 1,
    )

    native.toolchain(
        name = "registered_msvc",
        exec_compatible_with = ["@platforms//cpu:x86_64", "@platforms//os:windows"],
        target_compatible_with = ["@platforms//cpu:x86_64", "@platforms//os:windows"],
        toolchain = ":msvc",
        toolchain_type = "@bazel_tools//tools/cpp:toolchain_type",
    )
