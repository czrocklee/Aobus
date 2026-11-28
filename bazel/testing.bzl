"""Native process tests over existing executables.

The ``native_test`` macro delegates to the official rules_python ``py_test``.
The existing product or probe is a declared runfile, and the smoke entrypoint
resolves it through the standard runfiles library before launching it. No C++
source is compiled a second time.

Context-bearing native ``cc_test`` targets wrap the real test in the thin
``native_context_test`` rule so the private Linux fixtures become a declared
per-target dependency instead of a global ``--run_under``. The wrapped
``cc_test`` keeps every provider Bazel and the aspects need: it stays the
coverage-instrumented owner (its ``InstrumentedFilesInfo`` is re-exported
verbatim), its runfiles stay the test's runfiles, and every aspect still
reaches it through the wrapper's declared attributes. Only Bazel's scheduling
moves to the wrapper. Analysis that needs the wrapped test's compilation
context reads the wrapper's ``NativeContextInfo`` projection; the wrapper never
forwards ``CcInfo`` itself.

On Windows, Bazel's official Python launcher derives its program path from its
own executable path, so the wrapper aliases the official runner's adjacent
program artifact (the ``.zip`` program, or the extensionless bootstrap when
zip output is disabled) beside the wrapper executable under the wrapper's
basename. The aliases forward the runner's own ``DefaultInfo`` artifacts; no
launcher script or zip is ever rewritten here.
"""

load("@rules_cc//cc:find_cc_toolchain.bzl", "use_cc_toolchain")
load("@rules_cc//cc/common:cc_info.bzl", "CcInfo")
load("@rules_python//python:defs.bzl", "py_test")

_SMOKE = "//:script/ao/portal/bazel_native_smoke.py"
_CONTEXT = "//:ao_native_test_context"
_ADMISSION = "//:ao_winui_admission"
_RUNFILES = "@rules_python//python/runfiles"
_RUNNER = "//:native_test_runner"

CONTEXT_ENV = "AOBUS_TEST_CONTEXT"
_EXECUTABLE_ENV = "AOBUS_TEST_EXECUTABLE"

NativeContextInfo = provider(
    doc = "Analysis-only projection of the wrapped native test's compilation context.",
    fields = {
        "compilation_context": "The wrapped test's CcInfo compilation context.",
        "inner": "Label of the wrapped native test that owns the context.",
    },
)

def context_environment(env, context):
    """Merge owned private-fixture markers into one test environment.

    Args:
        env: Caller-owned test environment dict or None.
        context: Owned private-fixture markers consumed by the test runner.
    Returns:
        A dict with the markers merged into any existing ``AOBUS_TEST_CONTEXT``
        value (extended, never overwritten), or None when nothing is set.
    """
    if env != None and type(env) != "dict":
        fail("test context requires a dict env")
    environment = dict(env) if env != None else {}
    if context:
        marker = ",".join(context)
        previous = environment.get(CONTEXT_ENV)
        environment[CONTEXT_ENV] = marker if not previous else previous + "," + marker
    return environment if environment else None

def _runfiles_key(ctx, file):
    # External-repository files address their own workspace-root key.
    return file.short_path[3:] if file.short_path.startswith("../") else ctx.workspace_name + "/" + file.short_path

def _native_context_test_impl(ctx):
    inner = ctx.attr.inner
    inner_default = inner[DefaultInfo]
    runner = ctx.attr.runner[DefaultInfo]
    runner_executable = runner.files_to_run.executable
    inner_executable = inner_default.files_to_run.executable
    if runner_executable == None or inner_executable == None:
        fail("The native context test requires executable runner and wrapped test")

    # The test executable must be an output of this rule: one declared symlink
    # to the official runner launcher, no generated shell or batch script.
    # Windows launches the runner through its ``.exe`` launcher artifact;
    # deriving the suffix from the runner keeps CreateProcess from appending
    # its own extension to a wrongly bare declared name. Linux stays unchanged.
    suffix = "." + runner_executable.extension if runner_executable.extension else ""
    executable = ctx.actions.declare_file(ctx.label.name + suffix)
    ctx.actions.symlink(output = executable, target_file = runner_executable, is_executable = True)

    # Bazel's official Windows Python launcher (``python_launcher.cc``) derives
    # its program path from its own launcher path: the ``.zip`` program beside
    # the executable (the Windows default) or the extensionless bootstrap
    # when zip output is disabled. The wrapper executable therefore needs the
    # official runner program aliased beside itself under the wrapper's
    # basename. The aliases forward the runner's own ``DefaultInfo`` program
    # artifacts; nothing is repackaged or rewritten. Extensionless launchers
    # (Linux) are self-contained and need no sidecar.
    program_aliases = []
    if runner_executable.extension:
        stem = runner_executable.basename[:-(len(runner_executable.extension) + 1)]
        program = {}
        for file in runner.files.to_list():
            if file.dirname == runner_executable.dirname and file.basename in (stem, stem + ".zip"):
                program.setdefault(file.basename, file)
        if not program:
            fail("The native context test requires the official runner program beside its launcher")
        for basename in sorted(program):
            file = program[basename]
            extension = "." + file.extension if file.extension else ""
            alias = ctx.actions.declare_file(ctx.label.name + extension)
            ctx.actions.symlink(output = alias, target_file = file)
            program_aliases.append(alias)

    # The wrapper's runfiles carry everything the runner and the wrapped test
    # need at execution: both executables themselves, the runner's and inner
    # target files, plain-file data dependencies, and every default runfiles
    # tree. On Windows the launcher opens its adjacent alias, not the original
    # runner program. Default outputs alone do not order it before TestRunner.
    runfiles = ctx.runfiles(
        files = [executable, runner_executable, inner_executable] + program_aliases,
        transitive_files = depset(transitive = [runner.files, inner_default.files] + [
            dep[DefaultInfo].files
            for dep in ctx.attr.data + ctx.attr.deps
        ]),
    ).merge(runner.default_runfiles).merge(inner_default.default_runfiles)
    for dep in ctx.attr.data + ctx.attr.deps:
        runfiles = runfiles.merge(dep[DefaultInfo].default_runfiles)

    # Inherit any runtime environment the wrapped test exports, then overlay
    # the wrapper's own env; location references expand against the declared
    # data and the wrapped test exactly like a native test's env attribute.
    inherited = inner[RunEnvironmentInfo].environment if RunEnvironmentInfo in inner else {}
    inherited_names = inner[RunEnvironmentInfo].inherited_environment if RunEnvironmentInfo in inner else []
    environment = {key: ctx.expand_location(value, ctx.attr.data + [inner]) for key, value in inherited.items()}
    environment.update({key: ctx.expand_location(value, ctx.attr.data + [inner]) for key, value in ctx.attr.env.items()})
    environment[_EXECUTABLE_ENV] = _runfiles_key(ctx, inner_executable)

    # The wrapper's files stay the executable plus the wrapped test's own
    # DefaultInfo files. Analysis roots that reach compile actions through
    # DefaultInfo (concept and coverage analysis) therefore keep the real
    # C++ translation units; Bazel still schedules the wrapper executable.
    files = depset([executable], transitive = [inner_default.files])

    # The launcher sidecar aliases join the default outputs so a test build
    # materializes them beside the executable, exactly like the official
    # py_test's own program zip.
    if program_aliases:
        files = depset(program_aliases, transitive = [files])
    providers = [
        DefaultInfo(
            executable = executable,
            files = files,
            runfiles = runfiles,
        ),
        # Bazel applies this environment to the test process; the declared
        # `env` attribute of a Starlark test rule is not applied by itself.
        RunEnvironmentInfo(environment = environment, inherited_environment = inherited_names),
    ]

    # Coverage: the wrapped cc_test stays the instrumented owner. Re-export
    # its provider verbatim so bazel coverage keeps the same per-test scope.
    if InstrumentedFilesInfo in inner:
        providers.append(inner[InstrumentedFilesInfo])

    # Analysis-only coverage provenance: the wrapper deliberately exposes no
    # CcInfo, because forwarding it would apply authored-lint and Windows
    # manifest aspects to the Python launcher. Project only the wrapped test's
    # compilation context instead of mirroring the provider graph.
    if CcInfo in inner:
        providers.append(
            NativeContextInfo(
                compilation_context = inner[CcInfo].compilation_context,
                inner = ctx.attr.inner.label,
            ),
        )
    return providers

native_context_test = rule(
    implementation = _native_context_test_impl,
    test = True,
    # Product profile markers constrain compilation, not native execution.
    # Match the mandatory C++ execution contract, as the lint fixture does.
    exec_groups = {"test": exec_group(toolchains = use_cc_toolchain())},
    attrs = {
        # Bazel's TestActionBuilder recognizes these attributes for native
        # coverage collection, in addition to InstrumentedFilesInfo.
        "_collect_cc_coverage": attr.label(
            default = Label("@bazel_tools//tools/test:collect_cc_coverage"),
            executable = True,
            cfg = config.exec(exec_group = "test"),
        ),
        "_lcov_merger": attr.label(
            default = configuration_field(fragment = "coverage", name = "output_generator"),
            executable = True,
            cfg = config.exec(exec_group = "test"),
        ),
        "inner": attr.label(
            mandatory = True,
            doc = "The wrapped native test; the real cc_test keeps providers, aspects and instrumentation.",
        ),
        "runner": attr.label(
            default = Label(_RUNNER),
            cfg = "exec",
            executable = True,
            doc = "Fixture runner executable; it launches the wrapped test from runfiles.",
        ),
        "env": attr.string_dict(
            doc = "Test environment forwarded to the wrapped test, context markers included.",
        ),
        "deps": attr.label_list(
            doc = "Wrapped-test reachability for attribute-propagating aspects; the inner test rides here.",
        ),
        "data": attr.label_list(
            doc = "Runtime dependencies; location references in env expand against them.",
        ),
    },
)

def native_test(
        name,
        exe,
        args = [],
        data = [],
        env = {},
        env_inherit = [],
        context = [],
        **kwargs):
    """Declare a py_test that launches an existing executable from runfiles.

    Args:
        name: Test target name.
        exe: Existing executable target placed in the test's runfiles.
        args: Arguments after the resolved executable path.
        data: Additional runfiles for the executable or smoke entrypoint.
        env: Test environment additions.
        env_inherit: Caller environment names to forward.
        context: Owned private-fixture markers consumed by the test runner.
        **kwargs: Other py_test attributes.
    """
    environment = context_environment(env, context) or {}
    deps = [_CONTEXT, _RUNFILES] + select({
        "@platforms//os:windows": [_ADMISSION],
        "//conditions:default": [],
    }) + list(kwargs.pop("deps", []))
    py_test(
        name = name,
        srcs = [_SMOKE, "//:script/ao/portal/__init__.py"],
        main = _SMOKE,
        args = ["$(rlocationpath %s)" % exe] + list(args),
        data = [exe] + list(data),
        deps = deps,
        env = environment,
        env_inherit = env_inherit,
        python_version = "PY3",
        **kwargs
    )
