"""Test-package executables with the mandatory Windows UTF-8 manifest pair.

Every executable in this package embeds the UTF-8 process manifest on Windows,
so the pair stays in one place instead of being repeated per target. Callers
pass only their own extra linker inputs and link options.
"""

load("@rules_cc//cc:defs.bzl", _cc_binary = "cc_binary", _cc_test = "cc_test")
load("//bazel:windows_utf8_manifest.bzl", "WINDOWS_UTF8_LINKER_INPUTS", "WINDOWS_UTF8_LINKOPTS")

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

def cc_test(name, additional_linker_inputs = [], linkopts = [], **kwargs):
    """Declare a cc_test with the Windows UTF-8 manifest pair applied.

    Args:
        name: Target name.
        additional_linker_inputs: Caller-owned inputs appended to the manifest input.
        linkopts: Caller-owned options appended to the manifest link options.
        **kwargs: All other native cc_test attributes, forwarded unchanged.
    """
    _cc_test(
        name = name,
        additional_linker_inputs = WINDOWS_UTF8_LINKER_INPUTS + additional_linker_inputs,
        linkopts = WINDOWS_UTF8_LINKOPTS + linkopts,
        **kwargs
    )
