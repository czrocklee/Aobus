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

load("@rules_cc//cc:defs.bzl", _cc_binary = "cc_binary", _cc_test = "cc_test")
load("//bazel:testing.bzl", "context_environment", "native_context_test")
load("//bazel:windows_utf8_manifest.bzl", "WINDOWS_UTF8_LINKER_INPUTS", "WINDOWS_UTF8_LINKOPTS")

_INNER_SUFFIX = ".native"
_SCHEDULING_ATTRIBUTES = ("size", "timeout", "flaky", "local", "shard_count", "exec_properties")
_SHARED_ATTRIBUTES = ("tags", "target_compatible_with", "testonly", "visibility")

def cc_binary(name, additional_linker_inputs = [], linkopts = [], **kwargs):
    """Declare a cc_binary with the Windows UTF-8 manifest pair applied.

    Args:
        name: Target name.
        additional_linker_inputs: Caller-owned inputs appended to the manifest input.
        linkopts: Caller-owned options appended to the manifest link options.
        **kwargs: All other native cc_binary attributes, forwarded unchanged.
    """
    _cc_binary(
        name = name,
        additional_linker_inputs = WINDOWS_UTF8_LINKER_INPUTS + additional_linker_inputs,
        linkopts = WINDOWS_UTF8_LINKOPTS + linkopts,
        **kwargs
    )

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
        env: Caller-owned test environment; a dict is merged with context.
        args: Test arguments; the wrapper forwards them to the wrapped test.
        **kwargs: All other native cc_test attributes, forwarded unchanged.
    """
    if not context:
        _cc_test(
            name = name,
            additional_linker_inputs = WINDOWS_UTF8_LINKER_INPUTS + additional_linker_inputs,
            linkopts = WINDOWS_UTF8_LINKOPTS + linkopts,
            args = args,
            env = env,
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
    inner_attributes["env"] = env
    inner_attributes["data"] = data
    inner_attributes["tags"] = (shared.get("tags") or []) + ["manual"]
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
