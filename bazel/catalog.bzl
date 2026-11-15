"""Generate the interactive catalog from its sole authored package inventory."""

load("@aobus_native_tools//:catalog_tools.bzl", "CATALOG_TOOLS")

def _catalog_impl(ctx):
    if len(ctx.attr.tools) != 1:
        fail("catalog generation requires exactly one compiled catalog compiler")
    compiler = ctx.attr.tools[0][DefaultInfo].files_to_run
    if not compiler.executable:
        fail("catalog tools must contain the compiled catalog compiler")
    private = ctx.actions.declare_directory(ctx.label.name + "/private")
    cpp = ctx.actions.declare_file(ctx.label.name + "/EmbeddedCatalogData.cpp")
    header = ctx.actions.declare_file(ctx.label.name + "/CatalogLocaleTags.h")
    data = ctx.actions.declare_file(ctx.label.name + "/aobus_messages.dat")
    arguments = ctx.actions.args()
    arguments.add_all(["-I", "-S", "-B", "-X", "utf8", "-X", "pycache_prefix=" + private.path + "/pycache"])
    arguments.add(ctx.file._helper)
    arguments.add("--inventory", ctx.file.inventory)
    arguments.add("--template", ctx.file.locale_template)
    arguments.add("--compiler", compiler.executable)
    arguments.add("--genrb", CATALOG_TOOLS["genrb"])
    arguments.add("--pkgdata", CATALOG_TOOLS["pkgdata"])
    arguments.add("--private", private.path)
    arguments.add("--cpp", cpp)
    arguments.add("--header", header)
    arguments.add("--dat", data)
    for source in ctx.files.srcs:
        arguments.add("--source", source)
    env = {"LC_ALL": "C.UTF-8", "PYTHONUTF8": "1", "PYTHONDONTWRITEBYTECODE": "1"}
    if "SystemRoot" in CATALOG_TOOLS:
        env["SYSTEMROOT"] = CATALOG_TOOLS["SystemRoot"]
        arguments.add("--systemroot", CATALOG_TOOLS["SystemRoot"])
    ctx.actions.run(
        executable = CATALOG_TOOLS["python"],
        arguments = [arguments],
        inputs = depset([ctx.file.inventory, ctx.file.locale_template, ctx.file._helper] + ctx.files.srcs),
        tools = [compiler] + ctx.files._native_tools,
        outputs = [cpp, header, data, private],
        env = env,
        use_default_shell_env = False,
        mnemonic = "AobusCatalog",
        progress_message = "Compiling and embedding interactive catalogs for %{label}",
    )
    return [
        DefaultInfo(files = depset([cpp, header])),
        OutputGroupInfo(cpp = depset([cpp]), header = depset([header]), dat = depset([data]), intermediates = depset([private])),
    ]

catalog = rule(
    implementation = _catalog_impl,
    attrs = {
        "inventory": attr.label(allow_single_file = [".lst"], mandatory = True),
        "srcs": attr.label_list(allow_files = [".txt"], mandatory = True),
        "locale_template": attr.label(allow_single_file = [".h.in"], mandatory = True),
        # Keep this public exec edge visible to the executable validation aspect.
        "tools": attr.label_list(default = [Label("//tool/catalog:ao_catalog_compiler")], cfg = "exec"),
        "_helper": attr.label(default = Label("//tool/catalog:build_catalog.py"), allow_single_file = True),
        "_native_tools": attr.label(default = Label("@aobus_native_tools//:catalog_tools"), cfg = "exec"),
    },
)
