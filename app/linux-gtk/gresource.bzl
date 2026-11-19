"""Compile a GResource bundle from an ordered (source, alias) inventory."""

_PREFIX = "/org/aobus"
_C_NAME = "aobus"

def _xml_text(text):
    return text.replace("&", "&amp;").replace("<", "&lt;").replace(">", "&gt;").replace('"', "&quot;")

def _gresource_c_impl(ctx):
    # Distinct labels, then the ordered alias inventory. A label_list cannot
    # repeat a label, and ctx.files.srcs would flatten filegroups, so a
    # multi-file label and an empty one could offset and still match a length
    # check. The same File may be referenced by more than one alias.
    targets = ctx.attr.srcs
    aliases = ctx.attr.aliases
    source_index = ctx.attr.source_index
    if len(source_index) != len(aliases):
        fail("GResource references (%d) and aliases (%d) differ in length" % (len(source_index), len(aliases)))
    files_by_source = []
    for target in targets:
        members = target[DefaultInfo].files.to_list()
        if len(members) != 1:
            fail("GResource source %s must be exactly one file, got %d" % (target.label, len(members)))
        files_by_source.append(members[0])
    files = []
    for index in source_index:
        if index < 0 or index >= len(files_by_source):
            fail("GResource source index %d is outside the distinct source list" % index)
        files.append(files_by_source[index])

    # Generated XML is the only specification GLib reads. Each body is the
    # declared File.path, not a second relative lookup, so an unsandboxed
    # execroot cannot contribute an added member. Duplicate aliases are left
    # to glib-compile-resources, which rejects them.
    lines = [
        '<?xml version="1.0" encoding="UTF-8"?>',
        "<gresources>",
        '  <gresource prefix="%s">' % _PREFIX,
    ]
    for file, alias in zip(files, aliases):
        lines.append('    <file alias="%s">%s</file>' % (_xml_text(alias), _xml_text(file.path)))
    lines.extend([
        "  </gresource>",
        "</gresources>",
        "",
    ])
    xml = ctx.actions.declare_file(ctx.label.name + ".gresource.xml")
    ctx.actions.write(output = xml, content = "\n".join(lines))

    arguments = ctx.actions.args()
    arguments.add("--target=" + ctx.outputs.out.path)
    arguments.add("--sourcedir=.")
    arguments.add("--generate-source")
    arguments.add("--c-name=" + _C_NAME)
    arguments.add(xml.path)
    ctx.actions.run(
        executable = ctx.executable._glib_compile_resources,
        arguments = [arguments],
        inputs = files + [xml],
        tools = depset(
            [ctx.executable._glib_compile_resources],
            transitive = [ctx.attr._glib_compile_resources_runtime[DefaultInfo].files],
        ),
        outputs = [ctx.outputs.out],
        mnemonic = "AobusGResource",
        progress_message = "Compiling GResources for " + ctx.outputs.out.short_path,
    )
    return [DefaultInfo(files = depset([ctx.outputs.out]))]

_gresource_c = rule(
    implementation = _gresource_c_impl,
    attrs = {
        "aliases": attr.string_list(mandatory = True),
        "out": attr.output(mandatory = True),
        "source_index": attr.int_list(mandatory = True),
        "srcs": attr.label_list(allow_files = True, mandatory = True),
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

def gresource_c(name, resources, out, **kwargs):
    """Compile resources from an ordered (label, alias) sequence.

    The same source may be listed under several aliases. Those aliases stay in
    inventory order. The rule receives each canonical label once, because a
    label list rejects duplicates; it does not glob a package or add a search
    root.

    Args:
        name: Target name.
        resources: Ordered (source label, public alias) pairs.
        out: Generated C source.
        **kwargs: Forwarded rule attributes, such as target compatibility.
    """
    srcs = []
    seen = {}
    source_index = []
    aliases = []
    for label, alias in resources:
        key = str(native.package_relative_label(label))
        index = seen.get(key)
        if index == None:
            index = len(srcs)
            seen[key] = index
            srcs.append(label)
        source_index.append(index)
        aliases.append(alias)
    _gresource_c(
        name = name,
        srcs = srcs,
        source_index = source_index,
        aliases = aliases,
        out = out,
        **kwargs
    )
