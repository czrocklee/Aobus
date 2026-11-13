"""Expand the application version header from the governed product version."""

load("@aobus_native_tools//:app_version.bzl", "APP_VERSION")

def _app_version_impl(ctx):
    header = ctx.actions.declare_file("ao/AppVersion.h")
    ctx.actions.expand_template(
        template = ctx.file._template,
        output = header,
        substitutions = {"@AOBUS_VERSION@": APP_VERSION},
    )
    return [DefaultInfo(files = depset([header]))]

app_version = rule(
    implementation = _app_version_impl,
    attrs = {
        "_template": attr.label(default = Label("//include:AppVersion.h.in"), allow_single_file = True),
    },
)
