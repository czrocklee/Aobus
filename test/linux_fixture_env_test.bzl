"""Check that a public native test rejects overrides of Linux fixture tools."""

LINUX = ["@platforms//os:linux"]

def _linux_fixture_env_test_impl(ctx):
    wrapper = ctx.attr.wrapper[0]
    expected = wrapper.label.same_package_label(wrapper.label.name + ".linux_fixture_inputs")
    causes = []
    if AnalysisFailureInfo in wrapper:
        causes = [cause.label for cause in wrapper[AnalysisFailureInfo].causes.to_list()]
    return [AnalysisTestResultInfo(
        success = causes == [expected],
        message = "Expected only %s to reject the caller's fixture tool override; failure labels: %s" % (expected, causes),
    )]

linux_fixture_env_test = rule(
    implementation = _linux_fixture_env_test_impl,
    analysis_test = True,
    test = True,
    attrs = {
        "wrapper": attr.label(
            mandatory = True,
            cfg = analysis_test_transition(settings = {"//command_line_option:allow_analysis_failures": "True"}),
        ),
    },
    exec_groups = {"test": exec_group(exec_compatible_with = LINUX)},
)
