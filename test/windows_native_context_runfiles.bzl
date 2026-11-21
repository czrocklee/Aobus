"""Check that the real Windows wrapper's runfiles include its adjacent program."""

load("@rules_cc//cc:find_cc_toolchain.bzl", "use_cc_toolchain")

WINDOWS = ["@platforms//os:windows"]

def _windows_native_context_runfiles_test_impl(ctx):
    wrapper = ctx.attr.wrapper
    info = wrapper[DefaultInfo]
    executable = info.files_to_run.executable
    stem = executable.path.removesuffix(".exe")
    aliases = [
        file
        for file in info.files.to_list()
        if file.owner == wrapper.label and file != executable and file.path in (stem, stem + ".zip")
    ]
    return [AnalysisTestResultInfo(
        success = len(aliases) == 1 and aliases[0] in info.default_runfiles.files.to_list(),
        message = "%s must declare exactly one adjacent launcher program in its runfiles: %s or %s.zip" % (wrapper.label, stem, stem),
    )]

windows_native_context_runfiles_test = rule(
    implementation = _windows_native_context_runfiles_test_impl,
    analysis_test = True,
    test = True,
    attrs = {"wrapper": attr.label(mandatory = True)},
    exec_groups = {"test": exec_group(toolchains = use_cc_toolchain(), exec_compatible_with = WINDOWS)},
)
