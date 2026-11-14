"""Expose declared authored inputs to the WinUI source-inspection contract."""

def _test_input_manifest_impl(ctx):
    manifest = ctx.actions.declare_file(ctx.attr.output_name)
    ctx.actions.write(manifest, "\n".join([file.short_path for file in ctx.files.srcs]) + "\n")
    return [DefaultInfo(
        files = depset([manifest]),
        runfiles = ctx.runfiles(files = ctx.files.srcs + [manifest]),
    )]

test_input_manifest = rule(
    implementation = _test_input_manifest_impl,
    attrs = {
        "srcs": attr.label_list(allow_files = True),
        "output_name": attr.string(mandatory = True),
    },
)
