"""Lazy, ready-only import of the governed native lint LLVM SDK."""

load("//bazel:environment.bzl", "ENVIRONMENT_ATTRIBUTE", "repository_environment")

_ENVIRONMENT = [
    "PATH",
    "PATHEXT",
    "SYSTEMROOT",
    "WINDIR",
    "HOME",
    "LOCALAPPDATA",
    "AOBUS_STATE_ROOT",
    "AOBUS_LLVM_SDK_ROOT",
    "AOBUS_LLVM_SDK_CACHE_ROOT",
    "AOBUS_LLVM_ROOT",
    "AOBUS_LIBCXX_EXPECTED_SHIM",
    "AOBUS_LINT_LLVM_CONFIG",
    "AOBUS_LINT_CLANG_TIDY",
    "AOBUS_LINT_CLANG",
    "AOBUS_LINT_CLANGXX",
    "AOBUS_LINT_XCRUN",
    "AOBUS_LINT_CLANG_ROOTS",
    "NIXPKGS_CMAKE_PREFIX_PATH",
    "SDKROOT",
    "DEVELOPER_DIR",
    "VSINSTALLDIR",
]

_SDK_BUILD = """load("@aobus//bazel/lint:imports.bzl", "lint_sdk_imports")
load(":sdk_data.bzl", "SDK")

package(default_visibility = ["//visibility:public"])

lint_sdk_imports(SDK)
"""

def _basename(path):
    # Facts arrive in the portable posix wire form; a POSIX name may contain a
    # literal backslash, which is not a separator here.
    return path.split("/")[-1]

def _project_sdk(ctx, host, facts):
    """Materialize admitted native files and write the repository's graph data.

    Native admission stays in the helper. This projection owns the repository
    layout, target names and consumer interfaces, not the SDK selection.
    """
    windows = host == "windows"
    includes = []
    for index, source in enumerate(facts["includes"]):
        destination = "include/%d" % index
        ctx.symlink(ctx.path(source), destination)
        includes.append(destination)
    tidy = "bin/clang-tidy" + (".exe" if windows else "")
    ctx.symlink(ctx.path(facts["clang_tidy"]), tidy)
    compiler = "bin/clang-cl.exe" if windows else "bin/clang++"
    ctx.symlink(ctx.path(facts["clang"]), compiler)
    ctx.symlink(ctx.path(facts["resource_dir"]).get_child("include"), "resource/include")
    libraries = []
    for index, source in enumerate(facts["libraries"]):
        destination = "lib/%d/%s" % (index, _basename(source))
        ctx.symlink(ctx.path(source), destination)
        libraries.append(destination)
    runtime = []
    for index, source in enumerate(facts["runtime"]):
        destination = "runtime/%d/%s" % (index, _basename(source))
        ctx.symlink(ctx.path(source), destination)
        runtime.append(destination)
    ctx.file(
        "sdk_data.bzl",
        "SDK = %s\n" % repr({
            "host": host,
            "clang_tidy": tidy,
            "clang": compiler,
            "includes": includes,
            "libraries": libraries,
            "runtime": runtime,
            "linkopts": facts["linkopts"],
        }),
        executable = False,
    )
    ctx.file(
        "metadata.json",
        json.encode({
            "schema": 1,
            "platform": host,
            "version": facts["version"],
            "sdk_root": facts["sdk_root"],
            "clang": facts["clang"],
            "resource_dir": facts["resource_dir"],
            "host_kind": "standalone" if windows else "plugin",
            "system_arguments": facts["system_arguments"],
            "analysis_arguments": facts["analysis_arguments"],
            "fixture_arguments": facts["fixture_arguments"],
            "includes": facts["includes"],
            "libraries": facts["libraries"],
            "copts": facts["copts"],
            "linkopts": facts["linkopts"],
        }) + "\n",
        executable = False,
    )
    ctx.file(
        "flags.bzl",
        "LINT_SDK_HOST = %s\nLINT_SDK_COPTS = %s\n" % (repr(host), repr(facts["copts"])),
        executable = False,
    )
    ctx.file("BUILD.bazel", _SDK_BUILD, executable = False)

def _lint_sdk_impl(ctx):
    selected, prepared = repository_environment(ctx)
    python = selected["python"]
    ctx.watch(ctx.path(python))
    name = ctx.os.name.lower()
    host = "windows" if name.startswith("windows") else "macos" if name.startswith("mac") else name
    if host not in ["linux", "macos", "windows"]:
        fail("Unsupported lint execution host: " + name)
    environment = {}
    for key in _ENVIRONMENT:
        if key in prepared:
            environment[key] = prepared[key]
    helper = ctx.path(ctx.attr.helper)
    windows_manifest = ctx.path(ctx.attr.windows_manifest)
    macos_contract = ctx.path(ctx.attr.macos_contract)
    modules = [ctx.path(label) for label in ctx.attr.modules]
    for source in [helper, windows_manifest, macos_contract] + modules + [ctx.path(label) for label in ctx.attr.policy]:
        ctx.watch(source)
    # The helper and its modules import only the standard library. Do not inherit
    # Python, compiler or client variables outside the explicit native selection above.
    clean = {key: None for key in ctx.os.environ.keys()}
    clean.update(environment)
    ctx.file("_tmp/.keep", "", executable = False)
    for key in ["TEMP", "TMP", "TMPDIR"]:
        clean[key] = str(ctx.path("_tmp"))
    clean["PYTHONDONTWRITEBYTECODE"] = "1"
    result = ctx.execute(
        [
            python,
            "-I",
            "-S",
            "-B",
            "-X",
            "utf8",
            str(helper),
            "--host",
            host,
            "--windows-manifest",
            str(windows_manifest),
            "--macos-contract",
            str(macos_contract),
            "--environment-json",
            json.encode(environment),
            "--output",
            str(ctx.path("_projection.json")),
        ],
        environment = clean,
        quiet = True,
        timeout = 120,
    )
    if result.return_code:
        fail("Cannot import ready lint SDK (no provisioning attempted):\n%s\n%s" % (result.stdout, result.stderr))
    facts = json.decode(ctx.read("_projection.json"))
    for path in facts["watch_files"]:
        ctx.watch(ctx.path(path))
    for path in facts["watch_directories"]:
        directory = ctx.path(path)
        if directory.exists and directory.is_dir:
            directory.readdir(watch = "yes")
        else:
            ctx.watch(directory)
    _project_sdk(ctx, host, facts)

lint_sdk_repository = repository_rule(
    implementation = _lint_sdk_impl,
    attrs = {
        "_environment": ENVIRONMENT_ATTRIBUTE,
        "helper": attr.label(default = Label("//:script/ao/core/bazel_lint_sdk.py"), allow_single_file = True),
        # The helper's package imports; test_bazel_lint_sdk compares them with its real imports.
        "modules": attr.label_list(
            default = [
                Label("//:script/ao/__init__.py"),
                Label("//:script/ao/core/__init__.py"),
                Label("//:script/ao/core/windows_llvm_sdk.py"),
            ],
            allow_files = True,
        ),
        "windows_manifest": attr.label(default = Label("//:script/ao/windows-llvm-sdk.json"), allow_single_file = True),
        "macos_contract": attr.label(default = Label("//:script/ao/macos-toolchain.json"), allow_single_file = True),
        "policy": attr.label_list(default = [Label("//:nixpkgs.json"), Label("//:shell.nix")], allow_files = True),
    },
    configure = True,
)

def _lint_sdk_extension_impl(_ctx):
    lint_sdk_repository(name = "aobus_lint_sdk")

lint_sdk = module_extension(implementation = _lint_sdk_extension_impl)
