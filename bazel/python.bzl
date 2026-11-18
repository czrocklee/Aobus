"""Official rules_python runtime declarations for the admitted native Python SDK.

The host-tools repository materializes the admitted native interpreter path;
this macro projects it into the official py_runtime, py_runtime_pair and
toolchain API. No hermetic Python runtime is downloaded or rebuilt: the
platform runtime honestly requires the installed native SDK, exactly like the
other admitted host tools.
"""

load("@rules_python//python:py_runtime.bzl", "py_runtime")
load("@rules_python//python:py_runtime_pair.bzl", "py_runtime_pair")

def python_runtime_toolchain(data):
    """Declare the official platform runtime and toolchain for admitted host facts.

    Args:
        data: Python runtime facts produced by materialize_host_tools: the
            installed interpreter location, the pinned version and
            implementation name, and the honest runtime contract of the
            selected native SDK.
    """
    version_info = {}
    for key, value in zip(("major", "minor", "micro"), data["version"].split(".")):
        version_info[key] = value
    py_runtime(
        name = "python3_runtime",
        # rules_python exec tools split paths with Bazel's slash-based helpers,
        # even on Windows. Preserve the lexical venv path, not native separators.
        interpreter_path = data["executable"].replace("\\", "/"),
        python_version = "PY3",
        implementation_name = data["implementation"],
        interpreter_version_info = version_info,
    )
    py_runtime_pair(
        name = "python3_runtime_pair",
        py3_runtime = ":python3_runtime",
    )
    native.toolchain(
        name = "python3_toolchain",
        toolchain = ":python3_runtime_pair",
        toolchain_type = "@rules_python//python:toolchain_type",
    )
