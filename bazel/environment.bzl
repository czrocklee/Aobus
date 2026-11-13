"""One governed host environment shared by dependency and tooling repositories."""

# These are bootstrap selectors and OS runtime state, not implicit compiler flags.
# The helper filters the produced SDK environment again before publishing it.
# test_bazel_environment_rules pins how they differ from the helper's and portal's lists.
_SELECTORS = [
    "PATH",
    "HOME",
    "USER",
    "LOGNAME",
    "PATHEXT",
    "SYSTEMROOT",
    "SYSTEMDRIVE",
    "COMSPEC",
    "WINDIR",
    "LOCALAPPDATA",
    "APPDATA",
    "USERPROFILE",
    "HOMEDRIVE",
    "HOMEPATH",
    "PROCESSOR_ARCHITECTURE",
    "PROCESSOR_ARCHITEW6432",
    "AOBUS_STATE_ROOT",
    "XDG_CACHE_HOME",
    "AOBUS_BAZEL_VCPKG_PREFIX",
    "AOBUS_LLVM_ROOT",
    "AOBUS_LIBCXX_EXPECTED_SHIM",
    "AOBUS_NUGET_PACKAGES_DIR",
    "AOBUS_LLVM_SDK_ROOT",
    "AOBUS_LLVM_SDK_CACHE_ROOT",
    "AOBUS_LINT_LLVM_CONFIG",
    "AOBUS_LINT_CLANG_TIDY",
    "AOBUS_LINT_CLANG",
    "AOBUS_LINT_CLANGXX",
    "AOBUS_LINT_XCRUN",
    "AOBUS_LINT_CLANG_ROOTS",
    "DEVELOPER_DIR",
    "SDKROOT",
    "VSINSTALLDIR",
    "VCPKG_ROOT",
    "VSDEVCMD",
    "BAZEL_VC",
    "BAZEL_VC_FULL_VERSION",
    "BAZEL_WINSDK_FULL_VERSION",
    "BAZEL_SH",
    "NIX_REMOTE",
    "NIX_SSL_CERT_FILE",
    "SSL_CERT_FILE",
]
_WINDOWS_SDK = [
    "PROGRAMFILES(X86)",
    "PROGRAMFILES",
    "PROGRAMW6432",
    "INCLUDE",
    "LIB",
    "VCTOOLSINSTALLDIR",
    "WINDOWSSDKDIR",
    "WINDOWSSDKVERSION",
    "VCTOOLSVERSION",
    "UCRTVERSION",
    "NETFXSDKDIR",
]

def _quote(value):
    return "'" + str(value).replace("'", "'\"'\"'") + "'"

def _profile_facts(ctx, root, host, data):
    """Admit raw selected facts, never compiler or link programs."""
    if host == "windows":
        compiler = data.get("compiler_environment")
        version = compiler.get("VCToolsVersion") if type(compiler) == "dict" else None
        if type(version) != "string" or not version:
            fail("The Windows environment must publish the selected compiler_environment.VCToolsVersion")
        return {"vc_tools_version": version}
    if host == "macos":
        policy = json.decode(ctx.read(root.get_child("script/ao/macos-toolchain.json")))
        deployment = policy.get("deploymentTarget") if type(policy) == "dict" else None
        if type(deployment) != "string" or not deployment:
            fail("The macOS environment requires the governed deployment target")
        return {"deployment": deployment}
    return {}

def _write_build(ctx, facts):
    """Declare profile platforms lazily in this existing repository."""
    lines = [
        'load("@platforms//host:constraints.bzl", "HOST_CONSTRAINTS")',
        'load("@aobus//bazel:build_profiles.bzl", "build_profile_platforms")',
        "",
        'package(default_visibility = ["//visibility:public"])',
        "",
        "build_profile_platforms(",
        "    host_constraints = HOST_CONSTRAINTS,",
    ]
    lines.extend(["    %s = %s," % (name, json.encode(facts[name])) for name in sorted(facts)])
    lines.extend([
        ")",
        "",
        'exports_files(["environment.json"])',
    ])
    ctx.file("BUILD.bazel", "\n".join(lines) + "\n")

def _host(ctx):
    name = ctx.os.name.lower()
    return "windows" if name.startswith("windows") else "macos" if name.startswith("mac") else name

def _environment_impl(ctx):
    host = _host(ctx)
    if host not in ["linux", "macos", "windows"]:
        fail("Unsupported native Bazel host: " + host)
    helper = ctx.path(ctx.attr.helper)
    root = ctx.path(ctx.attr.root).dirname
    ctx.watch(helper)
    ctx.file("_tmp/.keep", "", executable = False)
    environment = {name: None for name in ctx.os.environ.keys()}
    selected = {}
    keys = {name.upper(): name for name in ctx.os.environ.keys()} if host == "windows" else {}
    for name in _SELECTORS + (_WINDOWS_SDK if host == "windows" else []):
        lookup = keys.get(name.upper(), name) if host == "windows" else name
        value = ctx.getenv(lookup)
        if value != None:
            environment[name] = value
            selected[name] = value
    for name in ["TMP", "TEMP", "TMPDIR"]:
        environment[name] = str(ctx.path("_tmp"))
    environment["PYTHONDONTWRITEBYTECODE"] = "1"
    arguments = ["--output", str(ctx.path("environment.json"))]
    request_key = keys.get("AOBUS_BAZEL_CONFIGURATION", "AOBUS_BAZEL_CONFIGURATION")
    request = ctx.getenv(request_key)
    if request:
        request = ctx.path(request)
        ctx.watch(request)
        arguments.extend(["--request", str(request)])
    python_key = keys.get("AOBUS_BAZEL_PYTHON", "AOBUS_BAZEL_PYTHON")
    python = ctx.getenv(python_key)
    if python:
        python = ctx.path(python)
        if not python.exists:
            fail("Explicit native Python is missing: " + str(python))
        ctx.watch(python)
    if host == "linux":
        # A caller need not enter nix-shell or supply a report/interpreter. Nix
        # remains the authority; all projection repositories reuse this result.
        nix = ctx.which("nix-shell")
        if not nix:
            fail("Native Linux Bazel requires Nix installed and nix-shell on PATH")
        ctx.watch(nix)
        for name in ["shell.nix", "nixpkgs.json", "script/ao/toolchain.json", "script/ao/bazel-toolchain.json"]:
            ctx.watch(root.get_child(name))
        interpreter = _quote(python) if python else '"$AO_NIX_PORTAL_PYTHON"'
        command = "exec " + interpreter + " -I -S -B -X utf8 -X "
        command += _quote("pycache_prefix=" + str(ctx.path("_tmp/pycache"))) + " " + _quote(helper)
        command += " " + " ".join([_quote(argument) for argument in arguments])
        argv = [str(nix), "--pure"]
        for name in selected:
            if name.startswith("AOBUS_") or name in ["XDG_CACHE_HOME"]:
                argv.extend(["--keep", name])
        argv.extend([str(root.get_child("shell.nix")), "--run", command])
    elif host == "macos":
        bootstrap = root.get_child("script/ao/macos-vcpkg-bootstrap.sh")
        ctx.watch(bootstrap)
        ctx.watch(root.get_child("script/ao/macos-toolchain.json"))
        for triplet in ["x64-aobus-osx", "arm64-aobus-osx"]:
            ctx.watch(root.get_child("cmake/vcpkg-triplets/" + triplet + ".cmake"))

        # SDK installation stays explicit. Only the existing compatibility shim
        # is materialized here, within this repository rather than shared state.
        command = "set -e; source " + _quote(bootstrap) + "; "
        command += "aobus_macos_prepare_python " + _quote(root) + "; "
        command += "original_state=$AOBUS_STATE_ROOT; export AOBUS_STATE_ROOT=" + _quote(ctx.path("_state")) + "; "
        command += "aobus_macos_prepare_bazel_environment " + _quote(root) + "; "
        command += "export AOBUS_STATE_ROOT=$original_state; "
        command += "export DEVELOPER_DIR=${DEVELOPER_DIR:-$(/usr/bin/xcode-select -p)}; "
        command += "export SDKROOT=${SDKROOT:-$(/usr/bin/xcrun --show-sdk-path)}; "
        interpreter = _quote(python) if python else '"$AOBUS_PYTHON"'
        command += "exec " + interpreter + " -I -S -B -X utf8 -X "
        command += _quote("pycache_prefix=" + str(ctx.path("_tmp/pycache"))) + " " + _quote(helper)
        command += " " + " ".join([_quote(argument) for argument in arguments])
        argv = ["/bin/bash", "-c", command]
    else:
        ctx.watch(root.get_child("script/ao/toolchain.json"))
        if not python:
            state = selected.get("AOBUS_STATE_ROOT")
            if not state:
                local = selected.get("LOCALAPPDATA")
                if not local:
                    fail("Native Windows Bazel requires LOCALAPPDATA or an explicit AOBUS_STATE_ROOT")
                state = local.replace("\\", "/") + "/Aobus"
            version = json.decode(ctx.read(root.get_child("script/ao/toolchain.json")))["python"]
            python = ctx.path(state.replace("\\", "/") + "/tools/python/" + version + "/python.exe")
            if not python.exists:
                fail("Prepare the governed Windows base Python first; repository fetch never installs Python: " + str(python))
        ctx.watch(python)
        argv = [str(python), "-I", "-S", "-B", "-X", "utf8", "-X", "pycache_prefix=" + str(ctx.path("_tmp/pycache")), str(helper)] + arguments
    ctx.report_progress("Selecting governed " + host + " environment")
    result = ctx.execute(argv, environment = environment, timeout = 1800, quiet = True)
    if result.return_code:
        fail("Cannot prepare the ready native Bazel environment:\n%s\n%s" % (result.stdout, result.stderr))
    data = json.decode(ctx.read("environment.json"))
    if type(data) != "dict" or data.get("schema") != 1 or data.get("host") != host:
        fail("Invalid native environment metadata")
    repository_root = str(ctx.path(".")).replace("\\", "/").rstrip("/")
    if host == "windows":
        repository_root = repository_root.lower()
    for name in data["watch_files"]:
        path = ctx.path(name)
        normalized = str(path).replace("\\", "/")
        if host == "windows":
            normalized = normalized.lower()
        if normalized != repository_root and not normalized.startswith(repository_root + "/"):
            ctx.watch(path)
    for name in data["watch_directories"]:
        directory = ctx.path(name)
        normalized = str(directory).replace("\\", "/")
        if host == "windows":
            normalized = normalized.lower()
        if normalized == repository_root or normalized.startswith(repository_root + "/"):
            continue
        if directory.exists and directory.is_dir:
            directory.readdir(watch = "yes")
        else:
            ctx.watch(directory)
    _write_build(ctx, _profile_facts(ctx, root, host, data))

environment_repository = repository_rule(
    implementation = _environment_impl,
    attrs = {
        "root": attr.label(default = Label("//:MODULE.bazel"), allow_single_file = True),
        "helper": attr.label(default = Label("//:script/ao/core/bazel_environment.py"), allow_single_file = True),
    },
    configure = True,
)

def repository_environment(ctx):
    """Read the same validated environment for every native projection consumer.

    Args:
        ctx: Repository context with the shared _environment input attribute.

    Returns:
        A pair containing the validated metadata and a copy of its environment dictionary.
    """
    path = ctx.path(ctx.attr._environment)
    data = json.decode(ctx.read(path))
    if type(data) != "dict" or data.get("schema") != 1 or type(data.get("environment")) != "dict" or type(data.get("python")) != "string":
        fail("Invalid shared native environment")
    ctx.watch(path)
    return data, dict(data["environment"])

ENVIRONMENT_ATTRIBUTE = attr.label(default = Label("@aobus_environment//:environment.json"), allow_single_file = True)
