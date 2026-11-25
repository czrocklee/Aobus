"""Test-package executables with the mandatory Windows UTF-8 manifest pair.

Every executable in this package embeds the UTF-8 process manifest on Windows,
so the pair stays in one place instead of being repeated per target. Callers
pass only their own extra linker inputs and link options.

Context-bearing ``cc_test`` targets wrap the real test: the inner ``cc_test``
(``<name>.native``, tagged manual) keeps compilation, coverage instrumentation
and aspect ownership, while the thin ``native_context_test`` wrapper owns
Bazel's scheduling and injects the private-fixture context. Targets without a
context stay ordinary ``cc_test`` declarations.
"""

load("@aobus_native_tools//:test_tools.bzl", "TEST_TOOLS")
load("@platforms//host:constraints.bzl", "HOST_CONSTRAINTS")
load("@rules_cc//cc:defs.bzl", "cc_library", _cc_binary = "cc_binary", _cc_test = "cc_test")
load("//bazel:testing.bzl", "CONTEXT_ENV", "context_environment", "native_context_test")
load("//bazel:windows_utf8_manifest.bzl", "WINDOWS_UTF8_LINKER_INPUTS", "WINDOWS_UTF8_LINKOPTS")

_INNER_SUFFIX = ".native"
_SCHEDULING_ATTRIBUTES = ("size", "timeout", "flaky", "shard_count", "exec_properties")

# `local = True` constrains both TestRunners (including explicit .native runs)
# without propagating the `local` tag to compile, link, and lint actions.
_SHARED_ATTRIBUTES = ("local", "tags", "target_compatible_with", "testonly", "visibility")

def sanitizer_suppressions(name, profile):
    """Embed a reviewed rule file in Linux instrumented tests, including child probes.

    Args:
        name: Sanitizer runtime (lsan or tsan).
        profile: Config setting in which the runtime is active.
    """
    compatible = select({profile: [], "//conditions:default": ["@platforms//:incompatible"]})
    native.genrule(
        name = name + "_hook",
        srcs = ["sanitizer/" + name + ".supp"],
        outs = [name + "_suppressions.cpp"],
        cmd = """
{
  echo 'extern "C" const char* __%s_default_suppressions() { return R"supp('
  cat "$<"
  echo ')supp"; }'
} > "$@"
""" % name,
        target_compatible_with = compatible,
        testonly = True,
    )
    cc_library(
        name = name + "_defaults",
        srcs = [":" + name + "_hook"],
        alwayslink = True,
        linkstatic = True,
        target_compatible_with = compatible,
        testonly = True,
    )

_SUPPRESSIONS = select({
    ":linux_asan": [":lsan_defaults"],
    ":linux_tsan": [":tsan_defaults"],
    "//conditions:default": [],
})

def _symbolizer_env(env):
    tool = TEST_TOOLS.get("symbolizer")
    if not tool:
        return env
    return (env or {}) | select({
        "//bazel:asan": {key + "_SYMBOLIZER_PATH": tool for key in ["ASAN", "LSAN", "UBSAN"]},
        "//bazel:tsan": {"TSAN_SYMBOLIZER_PATH": tool},
        "//conditions:default": {},
    })

def cc_binary(name, additional_linker_inputs = [], linkopts = [], **kwargs):
    """Declare a cc_binary with the Windows UTF-8 manifest pair applied.

    Args:
        name: Target name.
        additional_linker_inputs: Caller-owned inputs appended to the manifest input.
        linkopts: Caller-owned options appended to the manifest link options.
        **kwargs: All other native cc_binary attributes, forwarded unchanged.
    """
    kwargs["deps"] = kwargs.get("deps", []) + _SUPPRESSIONS
    _cc_binary(
        name = name,
        additional_linker_inputs = WINDOWS_UTF8_LINKER_INPUTS + additional_linker_inputs,
        linkopts = WINDOWS_UTF8_LINKOPTS + linkopts,
        **kwargs
    )

def _fixture_runfiles_impl(ctx):
    # Bazel resolves configurable env_inherit here. Either a wrapper env entry
    # or an inherited name could otherwise override the declared executable.
    if CONTEXT_ENV in ctx.attr.env_inherit:
        fail("%s cannot be inherited: it selects the test's declared fixture inputs" % CONTEXT_ENV)
    for key in ctx.attr.env_keys + ctx.attr.env_inherit:
        if key in ctx.attr.owned_keys:
            fail("%s is owned by the Linux test fixture; do not set or inherit it" % key)
    files = depset(transitive = [target[DefaultInfo].files for target in ctx.attr.srcs])
    return [DefaultInfo(files = files, runfiles = ctx.runfiles(transitive_files = files))]

_fixture_runfiles = rule(
    implementation = _fixture_runfiles_impl,
    attrs = {
        "srcs": attr.label_list(),
        "owned_keys": attr.string_list(),
        "env_keys": attr.string_list(),
        "env_inherit": attr.string_list(),
    },
)

def _add_linux_fixture_inputs(name, context_markers, attrs):
    if "@platforms//os:linux" not in HOST_CONSTRAINTS:
        return
    context = [marker.strip() for marker in context_markers.split(",")]
    display = "gtk" in context
    audio = display or "audio" in context
    if not (audio or "dbus" in context):
        return

    # The public wrapper inherits both the inner test's runfiles and its env.
    # MPRIS cases also launch the declared D-Bus daemon inside the native test.
    inputs = ["@aobus_native_tools//:dbus_daemon"]
    tools = ["dbus_daemon"]
    if audio:
        inputs.append("@aobus_native_tools//:pipewire_tools")
        tools.extend(["pipewire", "wireplumber", "pw_cli"])
    if display:
        inputs.append("@aobus_native_tools//:xvfb")
        tools.append("xvfb")

    fixture_env = {"AOBUS_TEST_" + tool.upper(): TEST_TOOLS[tool] for tool in tools}
    group = name + ".linux_fixture_inputs"
    env = attrs["env"] or {}
    _fixture_runfiles(
        name = group,
        srcs = inputs,
        owned_keys = fixture_env.keys(),
        env_keys = env.keys(),
        env_inherit = attrs.get("env_inherit", []),
        tags = ["manual"],
        target_compatible_with = attrs.get("target_compatible_with", []),
        testonly = True,
        visibility = ["//visibility:private"],
    )
    attrs["data"] = attrs["data"] + select({"@platforms//os:linux": [":" + group], "//conditions:default": []})
    attrs["env"] = select({
        "@platforms//os:linux": env | fixture_env,
        "//conditions:default": env,
    })

def cc_test(
        name,
        additional_linker_inputs = [],
        linkopts = [],
        context = [],
        env = None,
        args = [],
        **kwargs):
    """Declare a cc_test with the Windows UTF-8 manifest pair applied.

    Args:
        name: Test target name.
        additional_linker_inputs: Caller-owned inputs appended to the manifest input.
        linkopts: Caller-owned options appended to the manifest link options.
        context: Owned private-fixture markers consumed by the test runner.
        env: Caller-owned environment dict. A selected Linux fixture reserves
            its tool variables in both env and env_inherit.
        args: Test arguments; the wrapper forwards them to the wrapped test.
        **kwargs: All other native cc_test attributes, forwarded unchanged.
    """
    kwargs["deps"] = kwargs.get("deps", []) + _SUPPRESSIONS
    kwargs["env_inherit"] = kwargs.get("env_inherit", []) + select({":windows_asan": ["PATH"], "//conditions:default": []})
    if TEST_TOOLS.get("symbolizer"):
        tool = ["@aobus_native_tools//:sanitizer_symbolizer"]
        kwargs["data"] = kwargs.get("data", []) + select({"//bazel:asan": tool, "//bazel:tsan": tool, "//conditions:default": []})
    if not context:
        _cc_test(
            name = name,
            additional_linker_inputs = WINDOWS_UTF8_LINKER_INPUTS + additional_linker_inputs,
            linkopts = WINDOWS_UTF8_LINKOPTS + linkopts,
            args = args,
            env = _symbolizer_env(env),
            **kwargs
        )
        return

    environment = context_environment(env, context)

    # The inner cc_test is the real test: compilation, coverage instrumentation
    # and every aspect still apply to it. The manual tag keeps Bazel's default
    # wildcards on the wrapper, so the test never runs twice.
    inner = name + _INNER_SUFFIX
    wrapper = {attribute: kwargs.pop(attribute) for attribute in _SCHEDULING_ATTRIBUTES if attribute in kwargs}
    shared = {attribute: kwargs.pop(attribute) for attribute in _SHARED_ATTRIBUTES if attribute in kwargs}
    data = kwargs.pop("data", [])
    inner_attributes = dict(kwargs)
    inner_attributes.update(shared)
    inner_attributes["args"] = args
    inner_attributes["data"] = data
    inner_attributes["env"] = env
    inner_attributes["tags"] = (shared.get("tags") or []) + ["manual"]
    _add_linux_fixture_inputs(name, environment[CONTEXT_ENV], inner_attributes)
    inner_attributes["env"] = _symbolizer_env(inner_attributes["env"])
    _cc_test(
        name = inner,
        additional_linker_inputs = WINDOWS_UTF8_LINKER_INPUTS + additional_linker_inputs,
        linkopts = WINDOWS_UTF8_LINKOPTS + linkopts,
        **inner_attributes
    )

    # The caller's data rides on the wrapper too: the wrapper expands location
    # references in env against its own data, and its runfiles are the runfiles
    # the wrapped test resolves at execution. The inner test rides in `deps`
    # (select-safe, unlike a data list) so attribute-propagating aspects still
    # reach the real cc_test through the wrapper.
    wrapper.update(shared)
    wrapper["inner"] = ":" + inner
    wrapper["args"] = args
    wrapper["deps"] = [":" + inner]
    wrapper["data"] = data
    wrapper["env"] = environment or {}
    native_context_test(name = name, **wrapper)
