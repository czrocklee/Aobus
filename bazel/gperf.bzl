"""Generate dispatch headers with the already resolved host tool."""

def _gperf_header_impl(ctx):
    arguments = ctx.actions.args()
    arguments.add("--enum")
    arguments.add("--output-file=" + ctx.outputs.out.path)
    arguments.add(ctx.file.src.path)
    ctx.actions.run(
        executable = ctx.executable._gperf,
        arguments = [arguments],
        inputs = [ctx.file.src],
        tools = depset(
            [ctx.executable._gperf],
            transitive = [ctx.attr._runtime[DefaultInfo].files],
        ),
        outputs = [ctx.outputs.out],
        env = {"LC_ALL": "C"},
        mnemonic = "AobusGperf",
        progress_message = "Generating " + ctx.outputs.out.short_path,
    )
    return [DefaultInfo(files = depset([ctx.outputs.out]))]

gperf_header = rule(
    implementation = _gperf_header_impl,
    attrs = {
        "src": attr.label(allow_single_file = [".gperf"], mandatory = True),
        "out": attr.output(mandatory = True),
        "_gperf": attr.label(
            default = Label("@aobus_native_tools//:gperf"),
            executable = True,
            allow_single_file = True,
            cfg = "exec",
        ),
        "_runtime": attr.label(
            default = Label("@aobus_native_tools//:gperf_runtime"),
            cfg = "exec",
        ),
    },
)
