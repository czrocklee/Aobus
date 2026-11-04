"""Lazy, ready-only import of the governed native lint LLVM SDK."""

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

def _lint_sdk_impl(ctx):
    python = ctx.getenv("AOBUS_BAZEL_PYTHON")
    if not python:
        fail("Import the existing lint SDK through ./ao bazel and its governed Python")
    ctx.watch(ctx.path(python))
    name = ctx.os.name.lower()
    host = "windows" if name.startswith("windows") else "macos" if name.startswith("mac") else name
    if host not in ["linux", "macos", "windows"]:
        fail("Unsupported lint execution host: " + name)
    environment = {}
    for key in _ENVIRONMENT:
        value = ctx.getenv(key)
        if value != None:
            environment[key] = value
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
    plan = json.decode(ctx.read("_projection.json"))
    for path in plan["watch_files"]:
        ctx.watch(ctx.path(path))
    for path in plan["watch_directories"]:
        directory = ctx.path(path)
        if directory.exists and directory.is_dir:
            directory.readdir(watch = "yes")
        else:
            ctx.watch(directory)
    for item in plan["links"]:
        ctx.symlink(ctx.path(item["source"]), item["destination"])
    for path, content in plan["files"].items():
        ctx.file(path, content, executable = False)

lint_sdk_repository = repository_rule(
    implementation = _lint_sdk_impl,
    attrs = {
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
