"""Run the existing fixture assertions in an owned cacheable native action."""

load("@aobus_lint_sdk//:flags.bzl", "LINT_SDK_HOST")
load("@aobus_native_tools//:python_action_tools.bzl", "PYTHON_ACTION_TOOLS")
load("@bazel_skylib//rules/directory:providers.bzl", "DirectoryInfo")
load("@rules_cc//cc:find_cc_toolchain.bzl", "use_cc_toolchain")
load(":lint.bzl", "LintVerificationInfo", "native_environment")

# The test launcher interprets the completed suite result, not a retained log
# directory. This stdlib-only program also supports manifest-only runfiles.
_TEST_PROGRAM = """import json, os, pathlib, re, sys

def locate(key):
    directory = os.environ.get('TEST_SRCDIR', '')
    path = pathlib.Path(directory) / key
    if directory and path.is_file():
        return path
    manifest = os.environ.get('RUNFILES_MANIFEST_FILE')
    if not manifest and directory:
        manifest = str(pathlib.Path(directory) / 'MANIFEST')
    if manifest and pathlib.Path(manifest).is_file():
        def decode(value):
            return re.sub(r'\\\\([bsn])', lambda m: {'b': chr(92), 's': ' ', 'n': chr(10)}[m.group(1)], value)
        for line in pathlib.Path(manifest).read_text(encoding='utf-8').splitlines():
            escaped = line.startswith(' ')
            fields = (line[1:] if escaped else line).split(' ', 1)
            if len(fields) == 2:
                name, physical = (decode(v) for v in fields) if escaped else fields
                if name == key and pathlib.Path(physical).is_file():
                    return pathlib.Path(physical)
    raise ValueError('missing current lint fixture runfile: ' + key)

try:
    result = json.loads(locate(REPORT_KEY).read_text(encoding='utf-8'))
    if not isinstance(result, dict) or type(result.get('schema')) is not int or result['schema'] != 1:
        raise ValueError('malformed lint fixture result schema')
    if type(result.get('status')) is not int or result['status'] not in (0, 1) or result.get('platform') != PLATFORM_KEY:
        raise ValueError('malformed completed lint fixture result')
    sys.stdout.write(locate(LOG_KEY).read_text(encoding='utf-8'))
    status = result['status']
except (OSError, ValueError, KeyError) as error:
    print('Cannot read lint fixture result: ' + str(error), file=sys.stderr)
    status = 2
raise SystemExit(status)
"""

# The suite runs this many clang-tidy processes, and Bazel reserves as many CPUs.
_FIXTURE_JOBS = 2

def _fixture_resources(_os, _inputs_size):
    return {"cpu": _FIXTURE_JOBS}

def _python_string(value):
    return "'" + value.replace("\\", "\\\\").replace("'", "\\'").replace("\n", "\\n").replace("\r", "\\r") + "'"

def _fixture_impl(ctx):
    verification = ctx.attr._verification[LintVerificationInfo]
    report = ctx.actions.declare_file(ctx.label.name + ".fixture-result.json")
    log = ctx.actions.declare_file(ctx.label.name + ".fixture.log")
    work = ctx.actions.declare_directory(ctx.label.name + ".w")
    args = ctx.actions.args()
    args.add_all(["-I", "-S", "-B", "-X", "utf8"])
    args.add(ctx.file._helper)
    args.add_all(["--policy", verification.policy, "--sdk", verification.sdk, "--verification", verification.result, "--tidy", verification.host])
    if verification.plugin:
        args.add_all(["--plugin", verification.plugin])
    args.add_all(["--report", report, "--log", log, "--work", work], expand_directories = False)
    args.add_all(["--jobs", str(_FIXTURE_JOBS)])
    shim_inputs = []
    if LINT_SDK_HOST == "macos":
        shim = ctx.attr._expected_include[DirectoryInfo]
        args.add("--expected-include", shim.path)
        shim_inputs.append(shim.transitive_files)
    ctx.actions.run(
        executable = PYTHON_ACTION_TOOLS["python"],
        arguments = [args],
        inputs = depset(
            [ctx.file._helper, verification.result],
            transitive = [verification.inputs, ctx.attr._modules[DefaultInfo].files, ctx.attr._fixtures[DefaultInfo].files, ctx.attr._project_headers[DefaultInfo].files] + shim_inputs,
        ),
        outputs = [report, log, work],
        env = native_environment(ctx, portal = True),
        mnemonic = "AobusLintFixtures",
        resource_set = _fixture_resources,
        progress_message = "Verify Aobus lint fixtures and policy/error canaries",
    )
    program = _TEST_PROGRAM.replace("REPORT_KEY", _python_string(ctx.workspace_name + "/" + report.short_path)).replace("LOG_KEY", _python_string(ctx.workspace_name + "/" + log.short_path)).replace("PLATFORM_KEY", _python_string(LINT_SDK_HOST))
    command = "exec(" + _python_string(program) + ")"
    python = PYTHON_ACTION_TOOLS["python"]
    if LINT_SDK_HOST == "windows":
        launcher = ctx.actions.declare_file(ctx.label.name + ".bat")

        # The program contains single-quoted Python strings, so no embedded
        # command quotes are reparsed by CMD. Percent signs remain batch literals.
        if '"' in command:
            fail("Unexpected double quote in the native fixture launcher")
        content = '@echo off\r\n"%s" -I -S -B -X utf8 -c "%s"\r\nexit /b %%errorlevel%%\r\n' % (python.replace("%", "%%"), command.replace("%", "%%"))
    else:
        launcher = ctx.actions.declare_file(ctx.label.name)
        shell_command = "'" + command.replace("'", "'\\''") + "'"
        python = "'" + python.replace("'", "'\\''") + "'"
        content = "#!/bin/sh\nexec " + python + " -I -S -B -X utf8 -c " + shell_command + "\n"
    ctx.actions.write(launcher, content, is_executable = True)
    return [DefaultInfo(
        executable = launcher,
        runfiles = ctx.runfiles(files = [report, log], transitive_files = ctx.attr._python_tools[DefaultInfo].files),
    )]

lint_fixture_test = rule(
    implementation = _fixture_impl,
    test = True,
    attrs = {
        "_helper": attr.label(default = Label("//:script/ao/graph/bazel_lint_fixture.py"), allow_single_file = True),
        "_verification": attr.label(default = Label("//tool/lint:verification")),
        "_fixtures": attr.label(default = Label("//test/lint:fixtures")),
        "_project_headers": attr.label(default = Label("//include:common_headers")),
        "_modules": attr.label(default = Label("//:ao_lint_fixture_sources")),
        "_python_tools": attr.label(default = Label("@aobus_native_tools//:python_action_tools"), cfg = config.exec("test")),
    } | ({"_expected_include": attr.label(default = Label("@aobus_toolchain//:expected_include"), providers = [DirectoryInfo])} if LINT_SDK_HOST == "macos" else {}),
    fragments = ["cpp"],
    toolchains = use_cc_toolchain(),
    # The launcher needs the admitted native OS/CPU, not the product profile's
    # full target constraints. Reuse the mandatory native execution contract.
    exec_groups = {"test": exec_group(toolchains = use_cc_toolchain())},
)
