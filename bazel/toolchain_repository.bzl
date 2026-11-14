"""Materialize observed compiler SDK facts using the existing toolchain graphs."""

_LINUX_BUILD = """load("@aobus//bazel:linux_toolchain.bzl", "linux_toolchain")
load(":native_paths.bzl", "BUILTIN_INCLUDE_DIRECTORIES", "COMPILER_RESOURCE_DIRECTORY", "CONSTRAINT_CPU", "NIX_CC_WRAPPER", "SANITIZER_INCLUDE_DIRECTORY", "TOOLS")

package(default_visibility = ["//visibility:public"])

linux_toolchain(
    tools = TOOLS,
    builtin_include_directories = BUILTIN_INCLUDE_DIRECTORIES,
    compiler_resource_directory = COMPILER_RESOURCE_DIRECTORY,
    sanitizer_include_directory = SANITIZER_INCLUDE_DIRECTORY,
    nix_cc_wrapper = NIX_CC_WRAPPER,
    constraint_cpu = CONSTRAINT_CPU,
)
"""

_MACOS_BUILD = """load("@aobus//bazel:macos_cc_config.bzl", "macos_toolchain")
load(":native_paths.bzl", "CONSTRAINT_CPU", "CPU", "DEPLOYMENT", "DEVELOPER_DIR", "LINKER", "LLVM", "LLVM_REAL", "LLVM_VERSION", "SDK", "SHIM", "TOOLS")

package(default_visibility = ["//visibility:public"])

macos_toolchain(
    llvm = LLVM,
    llvm_real = LLVM_REAL,
    llvm_version = LLVM_VERSION,
    sdk = SDK,
    shim = SHIM,
    linker = LINKER,
    developer_dir = DEVELOPER_DIR,
    deployment = DEPLOYMENT,
    cpu = CPU,
    constraint_cpu = CONSTRAINT_CPU,
    tools = TOOLS,
)
"""

def _project_tools(tools, roles, platform):
    """Give selected lexical tools sparse input labels without resolving dispatch."""
    if type(tools) != "dict" or sorted(tools) != sorted(roles):
        fail("invalid %s compiler tool roles" % platform)
    inputs = {}
    sources = {}
    destinations = {}
    for role in sorted(roles):
        source = tools[role]
        if type(source) != "string":
            fail("invalid %s compiler tool path: %s" % (platform, role))
        basename = source.rsplit("/", 1)[-1]
        if (
            not basename or
            basename in [".", ".."] or
            ":" in basename or
            "\\" in basename or
            any([basename[index] < " " or basename[index] == "\177" for index in range(len(basename))])
        ):
            fail("invalid %s compiler tool basename: %s" % (platform, basename))
        destination = "tools/" + basename
        if destination in destinations:
            fail("duplicate %s compiler tool basename: %s" % (platform, basename))
        destinations[destination] = True
        inputs[role] = destination
        sources[destination] = source
    return inputs, sources

def materialize_toolchain(ctx, data):
    """Project raw SDK observations into the fixed native toolchain interfaces.

    Args:
        ctx: Repository context used to link tool inputs and write native facts and BUILD files.
        data: Admitted platform, compiler SDK observations, and identity metadata.
    """
    platform = data["platform"]
    if platform not in ["linux", "macos"]:
        fail("unsupported native toolchain platform: %s" % platform)
    sdk = data["sdk"]
    sources = {}
    if platform == "linux":
        machine = sdk["machine"]
        if type(machine) != "string" or machine.lower() not in ["x86_64", "aarch64"]:
            fail("unsupported Linux compiler CPU: %s" % machine)
        tools, sources = _project_tools(sdk["tools"], ["clang", "clang++", "ar", "strip", "objcopy", "gcov"], platform)
        paths = {
            "TOOLS": tools,
            "BUILTIN_INCLUDE_DIRECTORIES": sdk["builtin_include_directories"],
            "COMPILER_RESOURCE_DIRECTORY": sdk["compiler_resource_directory"],
            "SANITIZER_INCLUDE_DIRECTORY": sdk["sanitizer_include_directory"],
            "NIX_CC_WRAPPER": sdk["nix_cc_wrapper"],
            "CONSTRAINT_CPU": machine.lower(),
        }
        build = _LINUX_BUILD
    else:
        machine = sdk["machine"]
        if type(machine) != "string" or machine.lower() not in ["x86_64", "arm64"]:
            fail("unsupported macOS CPU: %s" % machine)
        cpu = machine.lower()
        tools, sources = _project_tools(sdk["tools"], ["clang", "clang++", "ar", "strip", "ld", "dsymutil", "apple_strip", "xcrun"], platform)
        sources["runtime/libLTO.dylib"] = sdk["liblto"]
        sources["include/__expected/expected.h"] = sdk["shim"] + "/__expected/expected.h"
        paths = {
            "LLVM": sdk["llvm"],
            "LLVM_REAL": sdk["llvm_real"],
            "LLVM_VERSION": sdk["llvm_version"],
            "SDK": sdk["sdk"],
            "SHIM": sdk["shim"],
            "LINKER": sdk["linker"],
            "DEVELOPER_DIR": sdk["developer_dir"],
            "DEPLOYMENT": sdk["deployment"],
            "CPU": cpu,
            "CONSTRAINT_CPU": "x86_64" if cpu == "x86_64" else "aarch64",
            "TOOLS": tools,
        }
        build = _MACOS_BUILD
    for destination in sorted(sources):
        ctx.symlink(ctx.path(sources[destination]), destination)
    ctx.file(
        "native_paths.bzl",
        "# Selected native compiler SDK facts.\n" + "\n".join([name + " = " + repr(paths[name]) for name in sorted(paths)]) + "\n",
        executable = False,
    )
    ctx.file("identity.json", json.encode(data["identity"]) + "\n", executable = False)
    ctx.file("BUILD.bazel", build, executable = False)
