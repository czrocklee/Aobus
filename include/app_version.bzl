"""Expand the existing application version template without a CMake configure."""

load("@aobus_native_tools//:app_version.bzl", "APP_VERSION")

def _app_version_impl(ctx):
    header = ctx.actions.declare_file("ao/AppVersion.h")
    ctx.actions.expand_template(
        template = ctx.file._template,
        output = header,
        substitutions = {"@PROJECT_VERSION@": APP_VERSION},
    )
    return [DefaultInfo(files = depset([header]))]

app_version = rule(
    implementation = _app_version_impl,
    attrs = {
        "_template": attr.label(default = Label("//:cmake/AppVersion.h.in"), allow_single_file = True),
    },
)
