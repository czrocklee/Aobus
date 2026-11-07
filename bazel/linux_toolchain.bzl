"""Nix toolchain policy instantiated in the generated repository."""

load("@rules_cc//cc/toolchains:args.bzl", "cc_args")
load("@rules_cc//cc/toolchains:feature.bzl", "cc_feature")
load("@rules_cc//cc/toolchains:feature_constraint.bzl", "cc_feature_constraint")
load("@rules_cc//cc/toolchains:tool.bzl", "cc_tool")
load("@rules_cc//cc/toolchains:tool_map.bzl", "cc_tool_map")
load("@rules_cc//cc/toolchains:toolchain.bzl", "cc_toolchain")
load(":compiler_policy.bzl", "UNIX_WARNINGS")

def _hardening_environment(nix_cc_wrapper, optimized):
    # Fortify makes the Nix wrapper inject -O2 and requires optimized libc headers.
    # Preserve every other observed flag, including new names and an empty policy.
    flags = nix_cc_wrapper["hardeningEnable"]
    if not optimized:
        flags = [flag for flag in flags if flag not in ["fortify", "fortify3"]]
    return {"NIX_HARDENING_ENABLE_" + nix_cc_wrapper["suffixSalt"]: " ".join(flags)}

def linux_toolchain(tools, builtin_include_directories, nix_cc_wrapper, constraint_cpu):
    """Declare the fixed native graph using caller-relative tool/input labels."""
    native.exports_files([
        "identity.json",
        "native_paths.bzl",
    ] + tools.values())

    native.filegroup(
        name = "identity",
        srcs = [
            "identity.json",
            "native_paths.bzl",
        ],
    )

    # Keep the lexical source basename: Nix and LLVM wrappers dispatch on argv[0].
    # The repository supplies local source labels, not resolved compiler executables.
    cc_tool(
        name = "clang",
        src = tools["clang"],
        data = [":identity"],
    )

    cc_tool(
        name = "clang++",
        src = tools["clang++"],
        data = [":identity"],
    )

    cc_tool(
        name = "llvm-ar",
        src = tools["ar"],
        data = [":identity"],
    )

    cc_tool(
        name = "llvm-strip",
        src = tools["strip"],
        data = [":identity"],
    )

    cc_tool(
        name = "llvm-objcopy",
        src = tools["objcopy"],
        data = [":identity"],
    )

    cc_tool_map(
        name = "tools",
        tools = {
            "@rules_cc//cc/toolchains/actions:assembly_actions": ":clang",
            "@rules_cc//cc/toolchains/actions:c_compile": ":clang",
            "@rules_cc//cc/toolchains/actions:cc_flags_make_variable": ":clang++",
            "@rules_cc//cc/toolchains/actions:cpp_compile_actions": ":clang++",
            # The Nix compiler wrapper owns linker selection and runtime policy.
            "@rules_cc//cc/toolchains/actions:link_actions": ":clang++",
            "@rules_cc//cc/toolchains/actions:ar_actions": ":llvm-ar",
            "@rules_cc//cc/toolchains/actions:objcopy_embed_data": ":llvm-objcopy",
            "@rules_cc//cc/toolchains/actions:strip": ":llvm-strip",
        },
    )

    cc_args(
        name = "compile_policy",
        actions = ["@rules_cc//cc/toolchains/actions:compile_actions"],
        allowlist_absolute_include_directories = builtin_include_directories,
        args = [
            "-fno-omit-frame-pointer",
            "-fcolor-diagnostics",
            "-no-canonical-prefixes",
            "-Wno-builtin-macro-redefined",
            "-D__DATE__=\"redacted\"",
            "-D__TIME__=\"redacted\"",
            "-D__TIMESTAMP__=\"redacted\"",
        ],
        data = [":identity"],
    )

    cc_args(
        name = "cpp_standard",
        actions = ["@rules_cc//cc/toolchains/actions:cpp_compile_actions"],
        args = ["-std=c++26"],
    )

    # Repository defaults select warnings for the compiling target, including
    # generated translation units, without enabling them for external repositories.
    cc_feature(
        name = "aobus_warnings",
        feature_name = "aobus_warnings",
    )

    cc_args(
        name = "warning_policy",
        actions = ["@rules_cc//cc/toolchains/actions:cpp_compile_actions"],
        # Retain the pinned Linux diagnostics before the shared Clang policy.
        args = [
            "-Wthread-safety",
            "-Wself-assign",
            "-Wunused-but-set-parameter",
            "-Wno-free-nonheap-object",
        ] + UNIX_WARNINGS,
        requires_any_of = [":aobus_warnings"],
    )

    # Builtin external declarations are not emitted as actual feature definitions.
    # Materialize the mode names without default-enabling them: Bazel's -c mode
    # request alone selects the matching compile arguments and hardening environment.
    cc_feature(
        name = "dbg",
        overrides = "@rules_cc//cc/toolchains/features:dbg",
    )

    cc_feature(
        name = "opt",
        overrides = "@rules_cc//cc/toolchains/features:opt",
    )

    cc_feature(
        name = "fastbuild",
        overrides = "@rules_cc//cc/toolchains/features:fastbuild",
    )

    cc_feature_constraint(
        name = "is_dbg",
        all_of = [":dbg"],
    )

    cc_feature_constraint(
        name = "is_opt",
        all_of = [":opt"],
    )

    cc_feature_constraint(
        name = "is_fastbuild",
        all_of = [":fastbuild"],
    )

    cc_args(
        name = "dbg_compile",
        actions = ["@rules_cc//cc/toolchains/actions:compile_actions"],
        args = ["-O0", "-g"],
        requires_any_of = [":is_dbg"],
    )

    cc_args(
        name = "opt_compile",
        actions = ["@rules_cc//cc/toolchains/actions:compile_actions"],
        args = [
            "-O2",
            "-DNDEBUG",
            "-ffunction-sections",
            "-fdata-sections",
            "-g0",
        ],
        requires_any_of = [":is_opt"],
    )

    # Retain the optimized link policy paired with function/data sections.
    cc_args(
        name = "opt_link",
        actions = ["@rules_cc//cc/toolchains/actions:link_actions"],
        args = ["-Wl,--gc-sections"],
        requires_any_of = [":is_opt"],
    )

    cc_args(
        name = "fastbuild_compile",
        actions = ["@rules_cc//cc/toolchains/actions:compile_actions"],
        args = ["-O0", "-g0"],
        requires_any_of = [":is_fastbuild"],
    )

    # Nix owns hardening. These explicit, salted environment entries participate in
    # action keys without importing ambient NIX_CFLAGS_COMPILE or NIX_LDFLAGS.
    # Keep them separate from compile arguments: link-only actions need the policy,
    # not compiler-only flags such as -DNDEBUG.
    cc_args(
        name = "dbg_hardening",
        actions = [
            "@rules_cc//cc/toolchains/actions:compile_actions",
            "@rules_cc//cc/toolchains/actions:link_actions",
        ],
        data = [":identity"],
        env = _hardening_environment(nix_cc_wrapper, optimized = False),
        requires_any_of = [":is_dbg"],
    )

    cc_args(
        name = "opt_hardening",
        actions = [
            "@rules_cc//cc/toolchains/actions:compile_actions",
            "@rules_cc//cc/toolchains/actions:link_actions",
        ],
        data = [":identity"],
        env = _hardening_environment(nix_cc_wrapper, optimized = True),
        requires_any_of = [":is_opt"],
    )

    cc_args(
        name = "fastbuild_hardening",
        actions = [
            "@rules_cc//cc/toolchains/actions:compile_actions",
            "@rules_cc//cc/toolchains/actions:link_actions",
        ],
        data = [":identity"],
        env = _hardening_environment(nix_cc_wrapper, optimized = False),
        requires_any_of = [":is_fastbuild"],
    )

    # Prevent implicit generic defaults from competing with Nix hardening or the
    # compilation-mode policy above. The other legacy behaviors are reused below.
    cc_feature(
        name = "default_compile_flags",
        overrides = "@rules_cc//cc/toolchains/features/legacy:default_compile_flags",
    )

    cc_feature(
        name = "supports_pic",
        feature_name = "supports_pic",
    )

    cc_feature(
        name = "supports_dynamic_linker",
        feature_name = "supports_dynamic_linker",
    )

    cc_feature(
        name = "external_include_paths",
        args = ["@rules_cc//cc/toolchains/args/external_include_paths"],
        feature_name = "external_include_paths",
    )

    cc_toolchain(
        name = "nix_clang",
        args = [
            ":compile_policy",
            ":cpp_standard",
            # Action args precede feature args, including ordinary user copts.
            ":warning_policy",
            ":dbg_compile",
            ":opt_compile",
            ":opt_link",
            ":fastbuild_compile",
            ":dbg_hardening",
            ":opt_hardening",
            ":fastbuild_hardening",
        ],
        # Leave SONAME unforced, as in the standard Linux toolchain: Bazel's solib
        # alias selected at link time supplies the runtime dependency name.
        compiler = "clang",
        # Explicit action configs do not imply the old argument features. Reuse
        # their public replacements individually, not the experimental umbrella.
        enabled_features = [
            ":supports_pic",
            ":supports_dynamic_linker",
            ":external_include_paths",
            "@rules_cc//cc/toolchains/args/dependency_file:feature",
            "@rules_cc//cc/toolchains/args/pic_flags:feature",
            "@rules_cc//cc/toolchains/args/preprocessor_defines:feature",
            "@rules_cc//cc/toolchains/args/include_flags:feature",
            "@rules_cc//cc/toolchains/args/random_seed:feature",
            "@rules_cc//cc/toolchains/args/compiler_input_flags:feature",
            "@rules_cc//cc/toolchains/args/compiler_output_flags:feature",
            "@rules_cc//cc/toolchains/args/shared_flag:feature",
            "@rules_cc//cc/toolchains/args/linkstamp_flags:feature",
            "@rules_cc//cc/toolchains/args/link_flags:feature",
            "@rules_cc//cc/toolchains/args/runtime_library_search_directories:feature",
            "@rules_cc//cc/toolchains/args/library_search_directories:feature",
            "@rules_cc//cc/toolchains/args/archiver_flags:feature",
            "@rules_cc//cc/toolchains/args/libraries_to_link:feature",
            "@rules_cc//cc/toolchains/args/strip_debug_symbols:feature",
            "@rules_cc//cc/toolchains/args/strip_flags:feature",
            "@rules_cc//cc/toolchains/args/linker_param_file:feature",
            # Keep user copts after toolchain defaults so ordinary Bazel overrides
            # retain their standard precedence.
            "@rules_cc//cc/toolchains/args/compile_flags:feature",
        ],
        known_features = [
            ":aobus_warnings",
            ":default_compile_flags",
            ":dbg",
            ":opt",
            ":fastbuild",
        ],
        supports_param_files = False,
        tool_map = ":tools",
    )

    native.toolchain(
        name = "registered_nix_clang",
        exec_compatible_with = [
            "@platforms//os:linux",
            "@platforms//cpu:" + constraint_cpu,
        ],
        target_compatible_with = [
            "@platforms//os:linux",
            "@platforms//cpu:" + constraint_cpu,
        ],
        toolchain = ":nix_clang",
        toolchain_type = "@bazel_tools//tools/cpp:toolchain_type",
    )
