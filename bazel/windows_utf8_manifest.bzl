"""Cache Windows executable resource checks in the build graph."""

load("@aobus_native_tools//:windows_manifest_tools.bzl", "WINDOWS_MANIFEST_TOOLS")
load("@rules_cc//cc/common:cc_info.bzl", "CcInfo")

WINDOWS_UTF8_LINKER_INPUTS = select({
    "@platforms//os:windows": ["//app/windows:utf8-process.manifest"],
    "//conditions:default": [],
})

WINDOWS_UTF8_LINKOPTS = select({
    "@platforms//os:windows": [
        "/MANIFEST:EMBED",
        "/MANIFESTINPUT:$(execpath //app/windows:utf8-process.manifest)",
    ],
    "//conditions:default": [],
})

def _validation_outputs(target):
    if OutputGroupInfo in target:
        return getattr(target[OutputGroupInfo], "_validation", depset())
    return depset()

def _windows_utf8_manifest_impl(target, ctx):
    validations = []

    # Private libraries and generated source/header filegroups can own exec tools.
    for name in ["deps", "implementation_deps", "srcs", "hdrs", "data", "tools"]:
        for dependency in getattr(ctx.rule.attr, name, []):
            validations.append(_validation_outputs(dependency))
    if not ctx.label.repo_name and CcInfo in target:
        executable = target[DefaultInfo].files_to_run.executable
        if executable and ctx.target_platform_has_constraint(ctx.attr._windows[platform_common.ConstraintValueInfo]):
            if not WINDOWS_MANIFEST_TOOLS:
                fail("Windows manifest verification requires the selected native SDK tools")
            manifest = ctx.actions.declare_file(ctx.label.name + ".utf8_manifest.xml")
            stamp = ctx.actions.declare_file(ctx.label.name + ".utf8_manifest.ok")
            arguments = ctx.actions.args()
            arguments.add_all(["-NoLogo", "-NoProfile", "-NonInteractive", "-ExecutionPolicy", "Bypass", "-File"])
            arguments.add(ctx.file._verifier)
            arguments.add("-Mt", WINDOWS_MANIFEST_TOOLS["mt"])
            arguments.add("-Binary", executable)
            arguments.add("-Manifest", manifest)
            arguments.add("-Stamp", stamp)
            ctx.actions.run(
                executable = WINDOWS_MANIFEST_TOOLS["powershell"],
                arguments = [arguments],
                inputs = [executable, ctx.file._verifier],
                tools = ctx.files._tools,
                outputs = [manifest, stamp],
                mnemonic = "WindowsUtf8Manifest",
                progress_message = "Checking embedded UTF-8 manifest for %{label}",
                # PowerShell native-command dispatch needs an executable extension.
                env = {"PATHEXT": ".EXE"},
                use_default_shell_env = True,
            )
            validations.append(depset([manifest, stamp]))
    return [OutputGroupInfo(_validation = depset(transitive = validations))]

windows_utf8_manifest_aspect = aspect(
    implementation = _windows_utf8_manifest_impl,
    attr_aspects = ["deps", "implementation_deps", "srcs", "hdrs", "data", "tools"],
    attrs = {
        "_verifier": attr.label(default = Label("//bazel:verify_windows_utf8_manifest.ps1"), allow_single_file = True),
        "_tools": attr.label(default = Label("@aobus_native_tools//:windows_manifest_tools"), cfg = "exec"),
        "_windows": attr.label(default = Label("@platforms//os:windows")),
    },
    apply_to_generating_rules = True,
    exec_compatible_with = ["@platforms//os:windows"],
)

def _manifest_checks_impl(ctx):
    outputs = depset(transitive = [_validation_outputs(binary) for binary in ctx.attr.binaries])
    return [DefaultInfo(files = outputs), OutputGroupInfo(_validation = outputs)]

windows_utf8_manifest_checks = rule(
    implementation = _manifest_checks_impl,
    attrs = {
        "binaries": attr.label_list(aspects = [windows_utf8_manifest_aspect], mandatory = True),
    },
)
