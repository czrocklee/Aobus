"""Native sanitizer profiles over the existing compiler toolchains."""

_SanitizerFlagInfo = provider(doc = "Selected boolean sanitizer build-setting value for admission checks.", fields = ["enabled"])

def _flag_impl(ctx):
    return [_SanitizerFlagInfo(enabled = ctx.build_setting_value)]

sanitizer_flag = rule(
    implementation = _flag_impl,
    build_setting = config.bool(flag = True),
)

def _admission_impl(_target, ctx):
    asan = ctx.attr._asan[_SanitizerFlagInfo].enabled
    tsan = ctx.attr._tsan[_SanitizerFlagInfo].enabled
    if asan and tsan:
        fail("AddressSanitizer and ThreadSanitizer cannot be enabled together")
    if not asan and not tsan:
        return []
    if ctx.configuration.coverage_enabled:
        fail("Sanitizer and coverage instrumentation must use separate runs")
    if "aobus-lint-sdk-host" in getattr(ctx.rule.attr, "tags", []):
        fail("The lint checker uses its own uninstrumented LLVM ABI; select a normal Debug/Release lint profile")
    if tsan and ctx.target_platform_has_constraint(ctx.attr._windows[platform_common.ConstraintValueInfo]):
        fail("ThreadSanitizer is not supported by the selected MSVC Windows toolchain")
    if ctx.var.get("COMPILATION_MODE") != "dbg":
        fail("Aobus sanitizer profiles require -c dbg")
    return []

sanitizer_admission = aspect(
    implementation = _admission_impl,
    fragments = ["cpp"],
    attrs = {
        "_asan": attr.label(default = Label("//bazel:asan_enabled")),
        "_tsan": attr.label(default = Label("//bazel:tsan_enabled")),
        "_windows": attr.label(default = Label("@platforms//os:windows")),
    },
)

def sanitizer_platforms(host_constraints):
    """Keep target instrumentation separate from the normal execution platform.

    Args:
        host_constraints: Host constraint labels used to retain the native OS and CPU.
    """
    macos = "@platforms//os:macos" in host_constraints or "@platforms//os:osx" in host_constraints
    windows = "@platforms//os:windows" in host_constraints
    parents = ["@aobus_toolchain//:platform"] if macos else []
    constraints = [] if macos else host_constraints
    for profile in ["asan", "tsan"]:
        if windows:
            # The normal vcpkg SDK is not instrumented. Match its STL annotation
            # boundary for every target compilation, including external runfiles.
            flags = ["--copt=/fsanitize=address", "--copt=/D_DISABLE_STL_ANNOTATION"] if profile == "asan" else []
        else:
            flags = ["--features=asan", "--features=ubsan"] if profile == "asan" else ["--features=tsan"]
            flags.append("--features=-treat_warnings_as_errors")
        native.platform(
            name = profile + "_platform",
            # Bazel adds this tag during platform analysis. Declare it as well
            # so query and configured ownership observe the same metadata.
            tags = ["manual"],
            parents = parents,
            constraint_values = constraints,
            flags = flags,
        )
