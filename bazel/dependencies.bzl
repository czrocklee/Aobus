"""Let Bazel own governed dependency resolution, invalidation and materialization."""

load("//bazel:environment.bzl", "ENVIRONMENT_ATTRIBUTE", "environment_repository", "repository_environment")
load("//bazel:import_repository.bzl", "materialize_aliases", "materialize_imports")
load("//bazel:host_tools_repository.bzl", "materialize_host_tools")
load("//bazel:toolchain_repository.bzl", "materialize_toolchain")

def _watch_path(ctx, path):
    value = str(path)
    if ctx.os.name.lower().startswith("windows"):
        value = value.replace("\\", "/").lower()
    return value.rstrip("/")

def _governed_repository_impl(ctx):
    selected, environment = repository_environment(ctx)
    request = ctx.path(ctx.attr._environment)
    python = selected["python"]
    ctx.watch(ctx.path(python))
    # The resolver's audited inventory names its own source, each imported
    # portal module and the package directories searched, not unrelated modules.
    resolver = ctx.path(ctx.attr.resolver)
    for source in ctx.attr.policy:
        ctx.watch(ctx.path(source))
    # A nix-shell's TEMP/TMP changes on every invocation. Resolver scratch is
    # repository-owned, not a semantic dependency on that ephemeral directory.
    # Bazel clears this repository root before each new implementation call.
    # With -B, this directory prefix stays free of automatic import caches.
    ctx.file("_tmp/.keep", "", executable = False)
    for name in ["TEMP", "TMP", "TMPDIR"]:
        environment[name] = str(ctx.path("_tmp"))
    # None really unsets a repository subprocess variable in Bazel 9.2. Merely
    # passing {} inherits the client environment. Enumerate names, never log or
    # serialize unrelated client values. Windows runtime keys are explicit above.
    clean = {name: None for name in ctx.os.environ.keys()}
    clean.update(environment)
    clean["PYTHONDONTWRITEBYTECODE"] = "1"
    output = ctx.path("_resolution")
    result = ctx.execute(
        [
            python,
            "-I",
            "-S",
            "-B",
            "-X",
            "utf8",
            "-X",
            "pycache_prefix=" + str(ctx.path("_tmp")),
            str(resolver),
            "--request",
            str(request),
            "--group",
            ctx.attr.group,
            "--output",
            str(output),
            "--environment-json",
            json.encode(environment),
        ],
        environment = clean,
        timeout = 600,
        quiet = True,
    )
    inventory = output.get_child("repository.json")
    if not inventory.exists:
        fail("Resolver did not produce its read inventory: %s\n%s" % (result.stdout, result.stderr))
    resolved = json.decode(ctx.read(inventory))
    repository_root = _watch_path(ctx, ctx.path("."))
    for path in resolved["watch_files"]:
        candidate = ctx.path(path)
        normalized = _watch_path(ctx, candidate)
        if normalized == repository_root or normalized.startswith(repository_root + "/"):
            continue  # Generated state and cwd search probes are not inputs.
        ctx.watch(candidate)
    for path in resolved["watch_directories"]:
        directory = ctx.path(path)
        normalized = _watch_path(ctx, directory)
        if normalized == repository_root or normalized.startswith(repository_root + "/"):
            continue
        if directory.exists and directory.is_dir:
            directory.readdir(watch = "yes")
        else:
            ctx.watch(directory)
    if result.return_code or "error" in resolved:
        fail("Governed dependency resolution failed: %s\n%s\n%s" % (resolved.get("error", result.stdout), resolved.get("traceback", ""), result.stderr))
    for item in resolved["links"]:
        destination = item["destination"]
        if destination.startswith("/") or ".." in destination.split("/"):
            fail("Invalid artifact destination: %s" % destination)
        # Native symlinks on POSIX; Bazel owns Windows file-copy fallback and
        # watches its sources. No privilege grant or whole-provider junction.
        ctx.symlink(ctx.path(item["source"]), destination)
    for name, content in resolved["files"].items():
        ctx.file(name, content, executable = False)
    interfaces = {}
    for subpackage, projection in resolved.get("imports", {}).items():
        interfaces[subpackage] = materialize_imports(
            ctx,
            subpackage,
            projection["config"],
            projection["packages"],
            projection["manifest"],
        )
    if "host_tools" in resolved:
        materialize_host_tools(ctx, resolved["host_tools"])
    if "toolchain" in resolved:
        materialize_toolchain(ctx, resolved["toolchain"])
    if ctx.attr.group in ["native", "test"] and "" not in interfaces:
        if sorted(interfaces.keys()) != ["debug", "release"]:
            fail("Configured SDK imports require Debug and Release interfaces")
        if interfaces["debug"] != interfaces["release"]:
            fail("Debug and Release SDKs expose different consumer interfaces")
        materialize_aliases(ctx, interfaces["debug"])
    elif "aliases" in resolved:
        materialize_aliases(ctx, resolved["aliases"])

_governed_repository = repository_rule(
    implementation = _governed_repository_impl,
    attrs = {
        "group": attr.string(mandatory = True, values = ["native", "test", "tools", "toolchain", "winui"]),
        "_environment": ENVIRONMENT_ATTRIBUTE,
        "resolver": attr.label(default = Label("//:script/ao/core/bazel_resolver.py"), allow_single_file = True),
        "policy": attr.label_list(allow_files = True, default = [
            Label("//:config/bazel/dependencies.json"),
            Label("//:dependency-contract.json"),
        ]),
    },
    configure = True,
)

def _dependencies_impl(_ctx):
    environment_repository(name = "aobus_environment")
    for name, group in [
        ("aobus_native_deps", "native"),
        ("aobus_test_deps", "test"),
        ("aobus_native_tools", "tools"),
        ("aobus_toolchain", "toolchain"),
        # Evaluated only when a WinUI consumer requests this installed SDK.
        ("aobus_winui_sdk", "winui"),
    ]:
        _governed_repository(name = name, group = group)

dependencies = module_extension(implementation = _dependencies_impl)
