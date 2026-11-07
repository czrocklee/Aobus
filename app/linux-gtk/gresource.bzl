"""Compile a GResource bundle into a C source with the already resolved host tool."""

def _gresource_c_impl(ctx):
    arguments = ctx.actions.args()
    arguments.add("--target=" + ctx.outputs.out.path)
    # The bundle XML stays the single authority for membership and aliases. Its
    # <file> paths resolve against the package directory and the workspace root,
    # matching the two --sourcedir roots the CMake build passes.
    arguments.add("--sourcedir=" + ctx.file.xml.dirname)
    arguments.add("--sourcedir=.")
    arguments.add("--generate-source")
    arguments.add("--c-name=aobus")
    arguments.add(ctx.file.xml.path)
    ctx.actions.run(
        executable = ctx.executable._glib_compile_resources,
        arguments = [arguments],
        inputs = [ctx.file.xml] + ctx.files.srcs,
        tools = depset(
            [ctx.executable._glib_compile_resources],
            transitive = [ctx.attr._glib_compile_resources_runtime[DefaultInfo].files],
        ),
        outputs = [ctx.outputs.out],
        mnemonic = "AobusGResource",
        progress_message = "Compiling GResources for " + ctx.outputs.out.short_path,
    )
    return [DefaultInfo(files = depset([ctx.outputs.out]))]

gresource_c = rule(
    implementation = _gresource_c_impl,
    attrs = {
        "srcs": attr.label_list(allow_files = True, mandatory = True),
        "xml": attr.label(allow_single_file = [".gresource.xml"], mandatory = True),
        "out": attr.output(mandatory = True),
        "_glib_compile_resources": attr.label(
            default = Label("@aobus_native_tools//:glib_compile_resources"),
            executable = True,
            allow_single_file = True,
            cfg = "exec",
        ),
        "_glib_compile_resources_runtime": attr.label(
            default = Label("@aobus_native_tools//:glib_compile_resources_runtime"),
            cfg = "exec",
        ),
    },
)
