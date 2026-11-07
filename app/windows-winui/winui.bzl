"""Native SDK generation and executable-local resource assembly for WinUI."""

load("@aobus_native_tools//:catalog_tools.bzl", "CATALOG_TOOLS")
load("@aobus_native_tools//:test_tools.bzl", "TEST_TOOLS")
load("@rules_cc//cc:defs.bzl", "cc_binary")
load("@rules_cc//cc/common:cc_common.bzl", "cc_common")
load("@rules_cc//cc/common:cc_info.bzl", "CcInfo")
load("//bazel:windows_utf8_manifest.bzl", "WINDOWS_UTF8_LINKER_INPUTS", "WINDOWS_UTF8_LINKOPTS")

_WinuiGeneratedInfo = provider(fields = ["xbf"])
_WinuiResourcesInfo = provider(fields = ["private"])
_WRAPPERS = ["module", "metadata", "types", "types_impl"]

def _configuration(ctx):
    mode = ctx.var.get("COMPILATION_MODE", "dbg")
    if mode not in ["dbg", "opt"]:
        fail("WinUI generation supports only dbg and opt compilation modes")
    return "Release" if mode == "opt" else "Debug"

def _arguments(ctx, action):
    arguments = ctx.actions.args()
    arguments.use_param_file("@%s", use_always = True)
    arguments.set_param_file_format("multiline")
    arguments.add("--action", action)
    return arguments

def _run(ctx, arguments, inputs, outputs, mnemonic, sdk = True):
    environment = {"PYTHONUTF8": "1", "PYTHONDONTWRITEBYTECODE": "1"}
    if "SystemRoot" in CATALOG_TOOLS:
        environment["SYSTEMROOT"] = CATALOG_TOOLS["SystemRoot"]
    ctx.actions.run(
        executable = CATALOG_TOOLS["python"],
        # Python must receive its startup flags and script on real argv; only
        # the helper's potentially large declared-input lists use a param file.
        arguments = ["-I", "-S", "-B", "-X", "utf8", ctx.file._helper.path, arguments],
        inputs = depset([ctx.file._helper] + inputs),
        tools = ctx.files._native_tools + (ctx.files._sdk_tools if sdk else []),
        outputs = outputs,
        env = environment,
        use_default_shell_env = False,
        mnemonic = mnemonic,
        progress_message = "Preparing WinUI outputs for %{label}",
    )

def _codegen_impl(ctx):
    # Full-CLR MSBuild appends long GUID-based batch paths under this tree.
    # Keep the private component compact; public headers/src/xbf stay stable.
    private = ctx.actions.declare_directory(ctx.label.name + "/p")
    headers = ctx.actions.declare_directory(ctx.label.name + "/headers")
    wrappers = [ctx.actions.declare_file(ctx.label.name + "/src/" + role + ".cpp") for role in _WRAPPERS]
    xbf = [ctx.actions.declare_file(ctx.label.name + "/xbf/" + name) for name in ["App.xbf", "MainWindow.xbf"]]
    arguments = _arguments(ctx, "generate")
    arguments.add("--sdk", ctx.file._sdk)
    arguments.add("--private", private.path)
    arguments.add("--headers", headers.path)
    for file in xbf:
        arguments.add("--xbf", file)
    arguments.add("--configuration", _configuration(ctx))
    arguments.add("--idl", ctx.file.idl)
    for file in ctx.files.srcs:
        arguments.add("--source", file)
    for file in ctx.files.hdrs:
        arguments.add("--header", file)
    for file in ctx.files.xaml:
        arguments.add("--xaml", file)
    for role, file in zip(_WRAPPERS, wrappers):
        arguments.add("--cpp")
        arguments.add_all([role, file.path])
    _run(
        ctx,
        arguments,
        [ctx.file._sdk, ctx.file._project, ctx.file.idl] + ctx.files.srcs + ctx.files.hdrs + ctx.files.xaml,
        [private, headers] + wrappers + xbf,
        "AobusWinuiGenerate",
    )
    return [
        DefaultInfo(files = depset([headers] + xbf)),
        OutputGroupInfo(cpp = depset(wrappers), diagnostics = depset([private])),
        _WinuiGeneratedInfo(xbf = xbf),
        CcInfo(compilation_context = cc_common.create_compilation_context(
            headers = depset([headers]),
            includes = depset([headers.path]),
        )),
    ]

winui_codegen = rule(
    implementation = _codegen_impl,
    attrs = {
        "srcs": attr.label_list(allow_files = [".cpp"], mandatory = True),
        "hdrs": attr.label_list(allow_files = [".h"], mandatory = True),
        "xaml": attr.label_list(allow_files = [".xaml"], mandatory = True),
        "idl": attr.label(allow_single_file = [".idl"], mandatory = True),
        "_project": attr.label(default = Label(":msbuild_project.py"), allow_single_file = True),
        "_helper": attr.label(default = Label(":build_winui.py"), allow_single_file = True),
        "_sdk": attr.label(default = Label("@aobus_winui_sdk//:sdk.json"), allow_single_file = True),
        "_sdk_tools": attr.label(default = Label("@aobus_winui_sdk//:tools"), cfg = "exec"),
        "_native_tools": attr.label(default = Label("@aobus_native_tools//:catalog_tools"), cfg = "exec"),
    },
    provides = [CcInfo, _WinuiGeneratedInfo],
)

def _resources_impl(ctx):
    xbf = ctx.attr.generated[_WinuiGeneratedInfo].xbf
    catalogs = ctx.files.catalog
    if len(catalogs) != 1 or not catalogs[0].is_directory:
        fail("WinUI resources require the catalog's one intermediates tree")
    # PRI generation uses the same Full-CLR temporary batch path convention.
    private = ctx.actions.declare_directory(ctx.label.name + "/p")
    arguments = _arguments(ctx, "resources")
    arguments.add("--sdk", ctx.file._sdk)
    arguments.add("--private", private.path)
    arguments.add("--configuration", _configuration(ctx))
    for file in xbf:
        arguments.add("--xbf", file)
    arguments.add("--catalog", catalogs[0].path)
    for file in ctx.files.assets + ctx.files.srcs:
        arguments.add("--asset", file)
    _run(
        ctx,
        arguments,
        [ctx.file._sdk, ctx.file._project] + xbf + catalogs + ctx.files.assets + ctx.files.srcs,
        [private],
        "AobusWinuiResources",
    )
    return [DefaultInfo(files = depset([private])), _WinuiResourcesInfo(private = private)]

winui_resources = rule(
    implementation = _resources_impl,
    attrs = {
        "generated": attr.label(providers = [_WinuiGeneratedInfo], mandatory = True),
        "catalog": attr.label(mandatory = True),
        "assets": attr.label_list(allow_files = True, mandatory = True),
        "srcs": attr.label_list(allow_files = [".yaml"], mandatory = True),
        "_project": attr.label(default = Label(":msbuild_project.py"), allow_single_file = True),
        "_helper": attr.label(default = Label(":build_winui.py"), allow_single_file = True),
        "_sdk": attr.label(default = Label("@aobus_winui_sdk//:sdk.json"), allow_single_file = True),
        "_sdk_tools": attr.label(default = Label("@aobus_winui_sdk//:tools"), cfg = "exec"),
        "_native_tools": attr.label(default = Label("@aobus_native_tools//:catalog_tools"), cfg = "exec"),
    },
    provides = [_WinuiResourcesInfo],
)

def _app_impl(ctx):
    # Keep the real linked binary on deps so the existing executable manifest
    # aspect traverses it. There is no shell launcher or second compiler here.
    if len(ctx.attr.deps) != 1:
        fail("WinUI application assembly requires exactly one linked executable dependency")
    executable = ctx.attr.deps[0][DefaultInfo].files_to_run.executable
    if executable == None:
        fail("WinUI application dependency is not an executable")
    resources = ctx.attr.resources[_WinuiResourcesInfo].private
    identity = ctx.attr.identity
    if not identity or identity[0] not in "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_":
        fail("WinUI executable identity must start with an ASCII letter, digit or underscore")
    for character in identity.elems():
        if character not in "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_-":
            fail("Unsupported WinUI executable identity: " + identity)
    exe = ctx.actions.declare_file(ctx.label.name + "/" + identity + ".exe")
    pri = ctx.actions.declare_file(ctx.label.name + "/" + identity + ".pri")
    app_xbf = ctx.actions.declare_file(ctx.label.name + "/App.xbf")
    window_xbf = ctx.actions.declare_file(ctx.label.name + "/MainWindow.xbf")
    assets = ctx.actions.declare_directory(ctx.label.name + "/Assets")
    arguments = _arguments(ctx, "stage")
    arguments.add("--identity", identity)
    arguments.add("--exe", executable)
    arguments.add("--resources", resources.path)
    for option, file in [
        ("--output-exe", exe),
        ("--output-pri", pri),
        ("--output-app-xbf", app_xbf),
        ("--output-window-xbf", window_xbf),
        ("--output-assets", assets),
    ]:
        arguments.add(option, file.path)
    runtime_outputs = []
    names = {file.basename.lower(): True for file in [exe, pri, app_xbf, window_xbf, assets]}
    for file in ctx.files.runtime:
        name = file.basename
        if file.is_directory or name.lower() in names:
            fail("WinUI runtime payload has a directory or colliding basename: " + name)
        names[name.lower()] = True
        output = ctx.actions.declare_file(ctx.label.name + "/" + name)
        runtime_outputs.append(output)
        arguments.add("--runtime")
        arguments.add_all([file.path, output.path])
    outputs = [exe, pri, app_xbf, window_xbf, assets] + runtime_outputs
    _run(
        ctx,
        arguments,
        [executable, resources] + ctx.files.runtime,
        outputs,
        "AobusWinuiApp",
        sdk = False,
    )
    return [DefaultInfo(
        executable = exe,
        files = depset(outputs),
        runfiles = ctx.runfiles(files = outputs),
    )]

winui_app = rule(
    implementation = _app_impl,
    executable = True,
    attrs = {
        "deps": attr.label_list(mandatory = True),
        "identity": attr.string(default = "Aobus"),
        "resources": attr.label(providers = [_WinuiResourcesInfo], mandatory = True),
        "runtime": attr.label(allow_files = True, mandatory = True),
        "_helper": attr.label(default = Label(":build_winui.py"), allow_single_file = True),
        "_native_tools": attr.label(default = Label("@aobus_native_tools//:catalog_tools"), cfg = "exec"),
    },
)

def winui_probe(name, srcs, console = False, checkout_root = False, copts = [], linkopts = [], deps = []):
    """Link and stage one of CMake's eight independently entered WinUI probes."""
    local_defines = []
    if checkout_root:
        # The native-tools repository projects the original request's checkout,
        # not Bazel's execution root or a compiler-only mapped source view.
        root = TEST_TOOLS["source_root"].replace("\\", "/")
        if not root:
            fail("The quality tooltip probe requires the original checkout root from native tools")
        local_defines = ["'AOBUS_QUALITY_PROBE_CHECKOUT_ROOT=\"{}\"'".format(root.replace("'", "'\\''"))]
    cc_binary(
        name = name + "_binary",
        srcs = srcs,
        additional_linker_inputs = WINDOWS_UTF8_LINKER_INPUTS,
        copts = ["/bigobj"] + copts,
        local_defines = local_defines,
        linkopts = WINDOWS_UTF8_LINKOPTS + [
            # App.xaml contributes the shipping wWinMain object; each probe
            # must still enter its own main, including GUI-subsystem probes.
            "/ENTRY:mainCRTStartup",
            "/SUBSYSTEM:CONSOLE" if console else "/SUBSYSTEM:WINDOWS",
            "runtimeobject.lib",
        ] + linkopts,
        linkstatic = True,
        target_compatible_with = ["@platforms//os:windows"],
        testonly = True,
        tags = ["manual"],
        visibility = ["//visibility:private"],
        deps = [":winui", "@aobus_winui_sdk//:bootstrap"] + deps,
    )
    # A superset of each probe's CMake deployment: both root-relative XBFs
    # plus Assets (including the automation probe's shipping shell layouts).
    # Reuse Aobus.pri bytes under the executable's own MRT lookup basename.
    winui_app(
        name = name,
        deps = [":" + name + "_binary"],
        identity = name,
        resources = ":resources",
        runtime = "@aobus_winui_sdk//:runtime",
        target_compatible_with = ["@platforms//os:windows"],
        testonly = True,
        tags = ["manual"],
        visibility = ["//test:__pkg__"],
    )
