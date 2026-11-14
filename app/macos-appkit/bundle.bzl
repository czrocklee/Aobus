"""Assemble the declared AppKit bundle tree and its run launcher."""

load("@aobus_native_tools//:app_version.bzl", "APP_VERSION")
load("@aobus_native_tools//:catalog_tools.bzl", "CATALOG_TOOLS")

def _aobus_appkit_bundle_impl(ctx):
    # The tree artifact name is the bundle contract; the launcher output keeps
    # the rule name so `bazel run` executes it beside the materialized tree.
    app = ctx.actions.declare_directory(ctx.attr.tree_name)
    launcher = ctx.actions.declare_file(ctx.label.name)
    arguments = ctx.actions.args()
    arguments.add_all(["-I", "-S", "-B", "-X", "utf8"])
    arguments.add(ctx.file._helper)
    arguments.add("--exe", ctx.file.exe)
    arguments.add("--plist", ctx.file.plist)
    arguments.add("--icon", ctx.file.icon)
    arguments.add("--license", ctx.file.license)
    arguments.add("--inventory", ctx.file.inventory)
    arguments.add("--macos-toolchain", ctx.file.macos_toolchain)
    arguments.add("--version", ctx.attr.version)
    arguments.add("--executable-name", ctx.attr.executable_name)
    if ctx.attr.bundle_identifier:
        arguments.add("--bundle-identifier", ctx.attr.bundle_identifier)
    if ctx.attr.display_name:
        arguments.add("--display-name", ctx.attr.display_name)
    arguments.add("--app", app.path)
    arguments.add("--launcher", launcher)
    env = {"LC_ALL": "C", "PYTHONUTF8": "1", "PYTHONDONTWRITEBYTECODE": "1"}
    if "SystemRoot" in CATALOG_TOOLS:
        env["SYSTEMROOT"] = CATALOG_TOOLS["SystemRoot"]
    ctx.actions.run(
        executable = CATALOG_TOOLS["python"],
        arguments = [arguments],
        inputs = depset(
            [
                ctx.file.exe,
                ctx.file.plist,
                ctx.file.icon,
                ctx.file.license,
                ctx.file.inventory,
                ctx.file.macos_toolchain,
                ctx.file._helper,
            ],
        ),
        tools = ctx.files._native_tools,
        outputs = [app, launcher],
        env = env,
        use_default_shell_env = False,
        mnemonic = "AobusAppkitBundle",
        progress_message = "Assembling " + ctx.attr.tree_name + " for %{label}",
    )
    return [
        DefaultInfo(
            executable = launcher,
            files = depset([app, launcher]),
            runfiles = ctx.runfiles(files = [app, launcher]),
        ),
    ]

aobus_appkit_bundle = rule(
    implementation = _aobus_appkit_bundle_impl,
    executable = True,
    attrs = {
        # The real linked product executable copied into Contents/MacOS.
        "exe": attr.label(mandatory = True, allow_single_file = True),
        "plist": attr.label(mandatory = True, allow_single_file = True),
        "icon": attr.label(mandatory = True, allow_single_file = True),
        "license": attr.label(mandatory = True, allow_single_file = True),
        "inventory": attr.label(mandatory = True, allow_single_file = True),
        "macos_toolchain": attr.label(mandatory = True, allow_single_file = True),
        # The application version comes from the native tools repository's
        # existing version declaration; Bazel adds no second authority.
        "version": attr.string(default = APP_VERSION),
        # The declared tree name is the bundle contract. The shipping product
        # keeps Aobus.app; a second consumer may declare its own tree.
        "tree_name": attr.string(default = "Aobus.app"),
        # The shipping bundle binary and plist share this executable name.
        "executable_name": attr.string(default = "aobus-appkit"),
        # Optional plist role overrides. Empty values keep the shared template's
        # shipping identifier and display name as the sole authority.
        "bundle_identifier": attr.string(default = ""),
        "display_name": attr.string(default = ""),
        "_helper": attr.label(default = Label(":build_bundle.py"), allow_single_file = True),
        "_native_tools": attr.label(default = Label("@aobus_native_tools//:catalog_tools"), cfg = "exec"),
    },
)
