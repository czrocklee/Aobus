"""Declare the ready, host-installed tools used by live repository checks."""

load("//bazel:environment.bzl", "ENVIRONMENT_ATTRIBUTE", "repository_environment")

def _watch_path(ctx, source, seen):
    path = ctx.path(source)
    for entry in [path, path.realpath] if path.exists else [path]:
        if str(entry) not in seen:
            ctx.watch(entry)
            seen[str(entry)] = True
    return path

def _write_selection(ctx, selection):
    ctx.file("command.json", json.encode(selection) + "\n", executable = False)
    ctx.file("BUILD.bazel", 'exports_files(["command.json"], visibility = ["//visibility:public"])\n', executable = False)

def _check_tool_impl(ctx):
    if ctx.attr.tool == "actionlint" and ctx.os.name != "linux":
        _write_selection(ctx, {"name": "actionlint", "unavailable": "The pinned actionlint repository is only available on Linux"})
        return
    selected, environment = repository_environment(ctx)
    seen = {}
    _watch_path(ctx, selected["python"], seen)
    scratch = ctx.path("_tmp")
    ctx.file("_tmp/.keep", "", executable = False)
    for name in ["TEMP", "TMP", "TMPDIR"]:
        environment[name] = str(scratch)
    clean = {name: None for name in ctx.os.environ.keys()}
    clean.update(environment)
    resolver = ctx.path(ctx.attr._resolver)
    _watch_path(ctx, resolver, seen)
    inventory = scratch.get_child("tool.json")
    result = ctx.execute(
        [
            selected["python"],
            "-I",
            "-S",
            "-B",
            "-X",
            "utf8",
            "-X",
            "pycache_prefix=" + str(scratch.get_child("pycache")),
            str(resolver),
            ctx.attr.tool,
            str(inventory),
        ],
        environment = clean,
        timeout = 120,
        quiet = True,
    )
    if result.return_code:
        fail("Cannot admit ready %s (no provisioning attempted):\n%s\n%s" % (ctx.attr.tool, result.stdout, result.stderr))
    facts = json.decode(ctx.read(inventory))
    for source in facts["imports"] + facts["watch_files"]:
        _watch_path(ctx, source, seen)
    for directory in facts["directories"]:
        _watch_path(ctx, directory, seen).readdir(watch = "yes")
    for tree in facts["trees"]:
        ctx.watch_tree(ctx.path(tree))
    _write_selection(ctx, facts["selection"])

check_tool_repository = repository_rule(
    implementation = _check_tool_impl,
    attrs = {
        "tool": attr.string(mandatory = True),
        "_environment": ENVIRONMENT_ATTRIBUTE,
        "_resolver": attr.label(default = Label("//:script/ao/portal/bazel_check_tools.py"), allow_single_file = True),
    },
    # SDK refresh must not fetch optional check tools. The shared environment
    # and the resolver's exact watches invalidate this repository on demand.
)
