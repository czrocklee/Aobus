"""Shared Nix action wiring for the two concrete Linux compiler toolchains."""

load("@rules_cc//cc/toolchains:args.bzl", "cc_args")
load("@rules_cc//cc/toolchains:feature.bzl", "cc_feature")
load("@rules_cc//cc/toolchains:feature_constraint.bzl", "cc_feature_constraint")
load("@rules_cc//cc/toolchains:tool.bzl", "cc_tool")
load("@rules_cc//cc/toolchains:tool_map.bzl", "cc_tool_map")
load("@rules_cc//cc/toolchains:toolchain.bzl", "cc_toolchain")
load(":clang_linux_policy.bzl", CLANG_POLICY = "POLICY")
load(":gcc_linux_policy.bzl", "GCC16_PFR_COPTS", "GCC_RELEASE_LINK_WARNING_ARGS", "GCOV_SOURCE_MAP_ARG", "GCOV_SOURCE_ROOTS", GCC_POLICY = "POLICY")

def _hardening_environment(nix_cc_wrapper, optimized):
    # Fortify makes the Nix wrapper inject -O2 and requires optimized libc headers.
    # Preserve every other observed flag, including new names and an empty policy.
    flags = nix_cc_wrapper["hardeningEnable"]
    if not optimized:
        flags = [flag for flag in flags if flag not in ["fortify", "fortify3"]]
    return {"NIX_HARDENING_ENABLE_" + nix_cc_wrapper["suffixSalt"]: " ".join(flags)}

def linux_toolchain(
        compiler,
        compiler_version,
        coverage_source_root,
        lto_make,
        archive_search_directory,
        tools,
        builtin_include_directories,
        compiler_resource_directory,
        sanitizer_include_directory,
        nix_cc_wrapper):
    """Declare the fixed native graph using caller-relative tool/input labels.

    Args:
        compiler: Selected concrete Linux compiler, gcc or clang.
        compiler_version: Admitted compiler version, for bounded compatibility inputs.
        coverage_source_root: Canonical checkout root used only for GCC coverage note paths.
        lto_make: Declared GNU make helper label for GCC Release IPO, or None for Clang.
        archive_search_directory: Original Nix wrapper directory for GCC's ar lookup, or None for Clang.
        tools: Compiler and binutils roles mapped to projected lexical tool labels.
        builtin_include_directories: Observed compiler include directories admitted for dependency checks.
        compiler_resource_directory: Observed Clang resource root whose share files are admitted inputs.
        sanitizer_include_directory: Compiler SDK include directory providing the TSan interface.
        nix_cc_wrapper: Observed wrapper hardening flags and salt used for action environments.
    """
    if compiler not in ["gcc", "clang"]:
        fail("Unsupported native Linux compiler: " + compiler)
    policy = GCC_POLICY if compiler == "gcc" else CLANG_POLICY
    gcc_major = int(compiler_version.split(".")[0]) if compiler == "gcc" else 0

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

    # The src basename survives projection: Nix wrappers dispatch on argv[0].
    for role, tool in tools.items():
        cc_tool(name = role, src = tool, data = [":identity"])

    cc_tool_map(
        name = "tools",
        tools = {
            "@rules_cc//cc/toolchains/actions:assembly_actions": ":cc",
            "@rules_cc//cc/toolchains/actions:c_compile": ":cc",
            "@rules_cc//cc/toolchains/actions:cc_flags_make_variable": ":cxx",
            "@rules_cc//cc/toolchains/actions:cpp_compile_actions": ":cxx",
            # The Nix compiler wrapper owns linker selection and runtime policy.
            "@rules_cc//cc/toolchains/actions:link_actions": ":cxx",
            "@rules_cc//cc/toolchains/actions:ar_actions": ":ar",
            "@rules_cc//cc/toolchains/actions:objcopy_embed_data": ":objcopy",
            "@rules_cc//cc/toolchains/actions:strip": ":strip",
            "@rules_cc//cc/toolchains/actions:gcov": ":gcov",
        },
    )

    cc_args(
        name = "compile_policy",
        actions = ["@rules_cc//cc/toolchains/actions:compile_actions"],
        # Clang adds its own sanitizer resource files to dependency output.
        # They belong to the observed compiler SDK, not application includes.
        allowlist_absolute_include_directories = builtin_include_directories + ([compiler_resource_directory + "/share"] if compiler == "clang" else []),
        args = policy.base_compile,
        data = [":identity"],
    )

    # gcc-ar locates its plugin-aware GNU ar through GCC's -B search prefix.
    # Preserve the restricted action PATH; use the admitted Nix wrapper directory.
    cc_args(
        name = "archive_tool_search",
        actions = ["@rules_cc//cc/toolchains/actions:ar_actions"],
        args = ["-B" + archive_search_directory + "/"] if compiler == "gcc" else [],
        data = [":identity"],
    )

    cc_args(
        name = "cpp_standard",
        actions = ["@rules_cc//cc/toolchains/actions:cpp_compile_actions"],
        args = policy.cpp_standard + (GCC16_PFR_COPTS if gcc_major == 16 else []),
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
        args = policy.warnings,
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
        args = policy.debug_compile,
        requires_any_of = [":is_dbg"],
    )

    cc_args(
        name = "opt_compile",
        actions = ["@rules_cc//cc/toolchains/actions:compile_actions"],
        args = policy.optimized_compile,
        requires_any_of = [":is_opt"],
    )

    # Retain the optimized link policy paired with function/data sections.
    cc_args(
        name = "opt_link",
        actions = ["@rules_cc//cc/toolchains/actions:link_actions"],
        args = policy.optimized_link,
        requires_any_of = [":is_opt"],
    )

    # Each driver owns its IPO: GCC LTO or Clang ThinLTO, without Bazel's
    # separate index/backend pipeline. Archives never carry compiler LTO flags.
    cc_args(
        name = "release_ipo_compile",
        actions = [
            "@rules_cc//cc/toolchains/actions:cpp_compile",
            "@rules_cc//cc/toolchains/actions:linkstamp_compile",
        ],
        args = policy.release_compile,
        requires_any_of = [":is_opt"],
    )

    cc_args(
        name = "release_ipo_link",
        actions = ["@rules_cc//cc/toolchains/actions:link_actions"],
        args = policy.release_link,
        requires_any_of = [":is_opt"],
    )

    if compiler == "gcc":
        cc_feature_constraint(
            name = "is_warned_opt",
            all_of = [":opt", ":aobus_warnings"],
        )
        cc_args(
            name = "release_ipo_warnings",
            actions = ["@rules_cc//cc/toolchains/actions:link_actions"],
            args = GCC_RELEASE_LINK_WARNING_ARGS,
            requires_any_of = [":is_warned_opt"],
        )

        # cc_args.format accepts a single-file target rather than a raw source
        # prerequisite. Keep this linker helper separate from compiler inputs.
        native.filegroup(name = "lto_make", srcs = [lto_make])

        # GCC uses make to dispatch LTRANS. Bind the declared helper without
        # importing the shell's PATH or Make jobserver environment. Existing
        # -flto=auto still chooses the link's own parallelism, not Bazel --jobs.
        cc_args(
            name = "release_ipo_runtime",
            actions = ["@rules_cc//cc/toolchains/actions:link_actions"],
            data = [":lto_make"],
            env = {"MAKE": "{lto_make}"},
            format = {"lto_make": ":lto_make"},
            requires_any_of = [":is_opt"],
        )

    cc_feature(
        name = "aobus_release_ipo",
        feature_name = "aobus_release_ipo",
        args = [":release_ipo_compile", ":release_ipo_link"] + ([":release_ipo_warnings", ":release_ipo_runtime"] if compiler == "gcc" else []),
    )

    # Sampling keeps the Release object policy but adds debug information and
    # frame pointers without any IPO. Feature arguments follow the mode
    # arguments above, so -g is effective over the opt -g0; user copts still
    # come later through the compile_flags feature.
    cc_args(
        name = "sampling_compile",
        actions = [
            "@rules_cc//cc/toolchains/actions:cpp_compile",
            "@rules_cc//cc/toolchains/actions:linkstamp_compile",
        ],
        args = policy.sampling_compile,
        requires_any_of = [":is_opt"],
    )

    cc_args(
        name = "sampling_link",
        actions = ["@rules_cc//cc/toolchains/actions:link_actions"],
        args = policy.sampling_link,
        requires_any_of = [":is_opt"],
    )

    cc_feature(
        name = "aobus_sampling",
        feature_name = "aobus_sampling",
        args = [":sampling_compile", ":sampling_link"],
    )

    cc_args(
        name = "fastbuild_compile",
        actions = ["@rules_cc//cc/toolchains/actions:compile_actions"],
        args = policy.fastbuild_compile,
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

    # libstdc++ uses this compiler-rt interface to annotate its shared-pointer
    # atomic locks. Clearing ambient Nix flags must not remove that SDK input.
    cc_args(
        name = "tsan_interface",
        actions = ["@rules_cc//cc/toolchains/actions:compile_actions"],
        args = ["-isystem", sanitizer_include_directory],
        allowlist_absolute_include_directories = [sanitizer_include_directory],
        data = [":identity"],
    )

    for sanitizer, flags in policy.sanitizers.items():
        cc_args(
            name = sanitizer + "_flags",
            actions = [
                "@rules_cc//cc/toolchains/actions:compile_actions",
                "@rules_cc//cc/toolchains/actions:link_actions",
            ],
            args = flags,
        )
        cc_feature(
            name = sanitizer,
            feature_name = sanitizer,
            args = [":" + sanitizer + "_flags"] + ([":tsan_interface"] if sanitizer == "tsan" else []),
        )

    # Global coverage also applies to uninstrumented targets and execution tools.
    # For C/C++ object actions, rules_cc creates gcov_gcno_file only when this
    # per-target feature is requested. Its public variable type is not optional,
    # so requires_not_none cannot guard it. Exclude header/module/assembly actions
    # and leave the legacy GCOV compile/link/runtime features unchanged.
    cc_feature(
        name = "coverage_instrumented",
        feature_name = "coverage_instrumented",
    )

    cc_feature_constraint(
        name = "is_gcov_coverage",
        all_of = [
            "@rules_cc//cc/toolchains/features/legacy:coverage",
            "@rules_cc//cc/toolchains/features/legacy:gcc_coverage_map_format",
            ":coverage_instrumented",
        ],
        none_of = [":asan", ":ubsan", ":tsan"],
    )

    cc_args(
        name = "gcov_compile_policy",
        actions = [
            "@rules_cc//cc/toolchains/actions:c_compile",
            "@rules_cc//cc/toolchains/actions:cpp_compile",
        ],
        args = policy.coverage_compile + ([
            GCOV_SOURCE_MAP_ARG % (coverage_source_root, prefix, prefix)
            for prefix in GCOV_SOURCE_ROOTS
        ] if compiler == "gcc" else []),
        requires_any_of = [":is_gcov_coverage"],
    )

    cc_feature(
        name = "external_include_paths",
        args = ["@rules_cc//cc/toolchains/args/external_include_paths"],
        feature_name = "external_include_paths",
    )

    test_warnings = policy.test_warnings if gcc_major >= 15 else []
    cc_args(
        name = "test_warning_policy",
        actions = ["@rules_cc//cc/toolchains/actions:cpp_compile_actions"],
        args = test_warnings,
    )
    cc_feature(
        name = "aobus_test_warnings",
        feature_name = "aobus_test_warnings",
        args = [":test_warning_policy"] if test_warnings else [],
    )
    cc_toolchain(
        name = "nix_" + compiler,
        args = [
            ":compile_policy",
            ":archive_tool_search",
            ":cpp_standard",
            # Action args precede feature args, including ordinary user copts.
            ":warning_policy",
            ":dbg_compile",
            ":opt_compile",
            ":opt_link",
            ":fastbuild_compile",
            ":gcov_compile_policy",
            ":dbg_hardening",
            ":opt_hardening",
            ":fastbuild_hardening",
        ],
        # Leave SONAME unforced, as in the standard Linux toolchain: Bazel's solib
        # alias selected at link time supplies the runtime dependency name.
        compiler = compiler,
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
            ":asan",
            ":ubsan",
            ":tsan",
            ":aobus_warnings",
            ":aobus_test_warnings",
            ":aobus_release_ipo",
            ":aobus_sampling",
            ":default_compile_flags",
            ":dbg",
            ":opt",
            ":fastbuild",
            ":coverage_instrumented",
        ],
        supports_param_files = False,
        tool_map = ":tools",
    )

def register_linux_toolchains(constraint_cpu):
    """Bind native --compiler selection to two real Nix compiler toolchains.

    Args:
        constraint_cpu: Native CPU constraint suffix.
    """
    constraints = ["@platforms//os:linux", "@platforms//cpu:" + constraint_cpu]
    for compiler in ["gcc", "clang"]:
        native.config_setting(name = compiler + "_selected", values = {"compiler": compiler})
        native.toolchain(
            name = "registered_nix_" + compiler,
            exec_compatible_with = constraints,
            target_compatible_with = constraints,
            target_settings = [":" + compiler + "_selected"],
            toolchain = "//" + compiler + ":nix_" + compiler,
            toolchain_type = "@bazel_tools//tools/cpp:toolchain_type",
        )
