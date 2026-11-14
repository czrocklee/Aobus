"""Declared coverage runtimes and a public label run-under strict collector."""

load("@aobus_native_tools//:catalog_tools.bzl", "CATALOG_TOOLS")

def _quote(value):
    return "'" + value.replace("'", "'\\''") + "'"

def _support_impl(ctx):
    # The stock runner's ROOT anchors the declared collector artifact. Its
    # selected Python payload already belongs to the admitted catalog SDK.
    runner = ctx.actions.declare_file("collect_coverage.sh")
    command = "#!/bin/sh\nexec " + _quote(CATALOG_TOOLS["python"])
    command += " -I -S -B -X utf8 \"${ROOT:?}/\"" + _quote(ctx.file.collector.path) + " \"$@\"\n"
    ctx.actions.write(runner, command, is_executable = True)
    files = [runner, ctx.file.collector]
    python = ctx.attr._python_runtime[DefaultInfo]
    return [DefaultInfo(
        executable = runner,
        files = depset(files),
        runfiles = ctx.runfiles(files = files, transitive_files = python.files).merge(python.default_runfiles),
    )]

coverage_support = rule(
    implementation = _support_impl,
    attrs = {
        "collector": attr.label(allow_single_file = [".py"], mandatory = True),
        "_python_runtime": attr.label(default = Label("@aobus_native_tools//:catalog_tools"), cfg = "exec"),
    },
    executable = True,
)

def _runtime_enabled_impl(_ctx):
    return []

coverage_runtime_enabled = rule(
    implementation = _runtime_enabled_impl,
    build_setting = config.bool(flag = True),
)

def _runfiles_key(ctx, file):
    return file.short_path[3:] if file.short_path.startswith("../") else ctx.workspace_name + "/" + file.short_path

def _merger_impl(ctx):
    runner = ctx.actions.declare_file(ctx.label.name + ".sh")
    if (ctx.attr.java_runtime == None) != (ctx.file.stock_payload == None):
        fail("Coverage requires both the declared Java runtime and stock payload")
    if ctx.attr.java_runtime == None:
        # Static dependencies must also stay absent in ordinary whole-graph and
        # unsupported-host analysis; an unexecuted body alone cannot do that.
        ctx.actions.write(
            runner,
            "#!/bin/sh\necho 'Run ao bazel coverage to select its declared Java runtime' >&2\nexit 1\n",
            is_executable = True,
        )
        return [DefaultInfo(executable = runner, files = depset([runner]), runfiles = ctx.runfiles(files = [runner]))]
    runtime_target = ctx.attr.java_runtime
    runtime = runtime_target[platform_common.ToolchainInfo].java_runtime
    java_files = [file for file in runtime.files.to_list() if file.path == runtime.java_executable_exec_path]
    if len(java_files) != 1 or runtime.java_executable_exec_path.startswith("/"):
        fail("Coverage requires one declared execroot-relative Java executable")
    java_key = _runfiles_key(ctx, java_files[0])
    if not java_key.endswith("/bin/java"):
        fail("Coverage requires the selected Linux JDK bin/java layout")
    java_home_key = java_key[:-len("/bin/java")]
    payload = ctx.file.stock_payload
    payload_key = _runfiles_key(ctx, payload)

    # The final CoverageReport spawn has no environment. The directory runfiles
    # carry the original stock deploy JAR and selected JavaRuntimeInfo.files.
    # The public import's default file is the precompiled runtime payload, not
    # its JavaInfo interface JAR or the source-less Main binary's bootstrap.
    command = """#!/bin/sh
set -eu
if [ -d "$0.runfiles" ]; then
  runfiles="$0.runfiles"
elif [ -n "${RUNFILES_DIR:-}" ] && [ -d "$RUNFILES_DIR" ]; then
  runfiles="$RUNFILES_DIR"
else
  case "$0" in
    *.runfiles/*) runfiles="${0%%.runfiles/*}.runfiles" ;;
    *) echo 'Coverage merger has no declared directory runfiles' >&2; exit 1 ;;
  esac
fi
[ -d "$runfiles" ] || { echo 'Coverage merger runfiles are unavailable' >&2; exit 1; }
runfiles="$(CDPATH= cd -- "$runfiles" && pwd -P)"
"""

    # Unix filename encoding follows the locale, not Java's UTF-8 content
    # charset. Keep empty-environment final reports and per-test merges UTF-8.
    command += "export LC_ALL=C.UTF-8\n"
    command += "export JAVABIN=\"$runfiles/" + java_key + "\"\n"
    command += "export JAVA_HOME=\"$runfiles/" + java_home_key + "\"\n"
    command += "[ -x \"$JAVABIN\" ] || { echo 'Declared coverage Java is unavailable' >&2; exit 1; }\n"
    command += "payload=\"$runfiles/" + payload_key + "\"\n"
    command += "[ -r \"$payload\" ] || { echo 'Declared coverage merger payload is unavailable' >&2; exit 1; }\n"
    command += "exec \"$JAVABIN\" -cp \"$payload\" com.google.devtools.coverageoutputgenerator.Main \"$@\"\n"
    ctx.actions.write(runner, command, is_executable = True)
    runfiles = ctx.runfiles(
        files = [runner, payload],
        transitive_files = runtime.files,
    ).merge(runtime_target[DefaultInfo].default_runfiles)
    return [DefaultInfo(executable = runner, files = depset([runner]), runfiles = runfiles)]

coverage_merger = rule(
    implementation = _merger_impl,
    executable = True,
    attrs = {
        "java_runtime": attr.label(providers = [platform_common.ToolchainInfo], cfg = "exec"),
        "stock_payload": attr.label(allow_single_file = [".jar"], cfg = "exec"),
    },
)
