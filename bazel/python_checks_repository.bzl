"""Materialize Ruff and mypy from the selected native tool provider."""

load(":docs_wheels.bzl", "WHEEL_INPUTS", "download_wheels")
load(":environment.bzl", "ENVIRONMENT_ATTRIBUTE", "repository_environment")

def _run(ctx, argv, environment):
    result = ctx.execute(argv, environment = environment, timeout = 600)
    if result.return_code:
        fail("Cannot prepare locked Python checks:\n%s\n%s" % (result.stdout, result.stderr))
    return result.stdout.strip()

def _native_tools(ctx, selected, environment):
    windows = selected["host"] == "windows"
    bindir = "Scripts/" if windows else "bin/"
    python = bindir + ("python.exe" if windows else "python")
    clean = {name: None for name in ctx.os.environ.keys()}
    clean.update(environment)
    environment = clean
    environment["PIP_CONFIG_FILE"] = "nul" if windows else "/dev/null"
    ctx.file("_tmp/.keep", "", executable = False)
    for name in ["TEMP", "TMP", "TMPDIR"]:
        environment[name] = str(ctx.path("_tmp"))
    base = ctx.path(selected["python"])

    # Rebuild the copied interpreter even if replacement at the same path
    # regenerated identical environment JSON.
    ctx.watch(base)
    if str(base.realpath) != str(base):
        ctx.watch(base.realpath)
    base = str(base)

    # Select the interpreter's process architecture, not the Bazel JVM's.
    # pip checks the selected wheels' Python/ABI/platform tags on install.
    probe = ctx.path(ctx.attr._checker_arch)
    ctx.watch(probe)
    arch = _run(ctx, [base, "-I", "-S", "-B", str(probe)], environment)
    wheels = download_wheels(ctx, selected["host"] + "-" + arch)

    # pip resolves exact pins only against the local wheelhouse. Do not follow
    # dependencies from METADATA: `pip check` verifies the installed closure
    # offline. Neither the installer nor pip becomes a checker runfile.
    requirements = "_tmp/checks.txt"
    ctx.file(requirements, "\n".join(wheels.values()) + "\n", executable = False)
    _run(ctx, [base, "-I", "-m", "venv", "--copies", "_installer"], environment)
    _run(ctx, [base, "-I", "-m", "venv", "--copies", "--without-pip", "payload"], environment)
    runtime = "payload/" + python
    pip = [
        str(ctx.path("_installer/" + python)),
        "-I",
        "-m",
        "pip",
        "--isolated",
        "--disable-pip-version-check",
        "--no-cache-dir",
        "--python",
        str(ctx.path(runtime)),
    ]
    _run(ctx, pip + [
        "install",
        "--no-input",
        "--no-compile",
        "--no-index",
        "--find-links",
        str(ctx.path("wheelhouse")),
        "--only-binary=:all:",
        "--no-deps",
        "--require-hashes",
        "-r",
        str(ctx.path(requirements)),
    ], environment)
    _run(ctx, pip + ["check"], environment)
    for directory in ["_installer", "_tmp", "wheelhouse"]:
        ctx.delete(directory)
    return "payload/" + bindir + ("ruff.exe" if windows else "ruff"), runtime

def _python_checks_impl(ctx):
    selected, environment = repository_environment(ctx)
    host = selected.get("host")
    if host == "linux":
        # withPackages' wrappers reference their immutable Nix store closures.
        bindir = ctx.path(selected["python"]).dirname
        for name in ["ruff", "mypy"]:
            tool = bindir.get_child(name)
            if not tool.exists or not str(tool.realpath).startswith("/nix/store/"):
                fail("Expected pinned Nix checker at " + str(tool))
            ctx.symlink(tool, "bin/" + name)
        ruff, mypy = "bin/ruff", "bin/mypy"
        runtime = "[]"
    elif host in ["windows", "macos"]:
        ruff, mypy = _native_tools(ctx, selected, environment)
        runtime = 'glob(["payload/**"], exclude = ["payload/bin/*", "payload/Scripts/*.exe", "**/__pycache__/**"])'
    else:
        fail("Unsupported host in shared native environment: %r" % host)
    ctx.file("BUILD.bazel", """package(default_visibility = ["//visibility:public"])
filegroup(name = "ruff", srcs = [%s])
filegroup(name = "mypy", srcs = [%s], data = %s)
""" % (repr(ruff), repr(mypy), runtime))

python_checks_repository = repository_rule(
    implementation = _python_checks_impl,
    attrs = dict(
        WHEEL_INPUTS,
        _checker_arch = attr.label(default = Label("//:script/ao/portal/bazel_checker_arch.py"), allow_single_file = True),
        _environment = ENVIRONMENT_ATTRIBUTE,
    ),
    # SDK refresh must not fetch optional Python tools.
)
