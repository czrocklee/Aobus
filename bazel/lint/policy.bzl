"""Render the existing lint policy without introducing a scope-dependent cache key."""

load("//bazel:environment.bzl", "ENVIRONMENT_ATTRIBUTE", "repository_environment")

def _policy_impl(ctx):
    selected, environment = repository_environment(ctx)
    python = selected["python"]
    root = ctx.path(ctx.attr.root).dirname
    helper = ctx.path(ctx.attr.helper)
    ctx.watch_tree(root.get_child("script/ao"))
    ctx.watch_tree(root.get_child("test/lint/fixture"))
    selection = ctx.getenv("AOBUS_BAZEL_LINT_CONFIGURATION")
    if selection:
        configuration = ctx.path(selection)
        ctx.watch(configuration)
    else:
        ctx.file("configuration.json", json.encode({
            "schema": 1,
            "source_root": str(root),
            "check": None,
            "header_filter": None,
            "clang_tidy_args": [],
        }))
        configuration = ctx.path("configuration.json")
    clean = {key: None for key in ctx.os.environ.keys()}
    clean.update(environment)
    ctx.file("_tmp/.keep", "", executable = False)
    for key in ["TMP", "TEMP", "TMPDIR"]:
        clean[key] = str(ctx.path("_tmp"))
    clean["PYTHONDONTWRITEBYTECODE"] = "1"
    result = ctx.execute(
        [python, "-I", "-S", "-B", "-X", "utf8", str(helper), "prepare", "--configuration", str(configuration), "--output", str(ctx.path("policy.json"))],
        environment = clean,
        quiet = True,
    )
    if result.return_code:
        fail("Cannot render diagnostic-only lint policy:\n%s\n%s" % (result.stdout, result.stderr))
    ctx.file("BUILD.bazel", 'package(default_visibility = ["//visibility:public"])\nexports_files(["policy.json"])\n')

lint_policy_repository = repository_rule(
    implementation = _policy_impl,
    attrs = {
        "_environment": ENVIRONMENT_ATTRIBUTE,
        "root": attr.label(default = Label("//:CMakeLists.txt"), allow_single_file = True),
        "helper": attr.label(default = Label("//:script/ao/core/bazel_lint_action.py"), allow_single_file = True),
    },
    configure = True,
)

def _consumers_impl(ctx):
    # The portal's per-run consumer selection stays out of the policy, which
    # every report action reads, so selecting a consumer reruns no report.
    consumers = {}
    selection = ctx.getenv("AOBUS_BAZEL_LINT_CONSUMERS")
    if selection:
        path = ctx.path(selection)
        ctx.watch(path)
        data = json.decode(ctx.read(path))
        if type(data) != "dict" or data.get("schema") != 1 or type(data.get("consumers")) != "dict":
            fail("Invalid native lint consumer selection: %s" % selection)
        consumers = data["consumers"]
        for owner, entries in consumers.items():
            if type(owner) != "string" or type(entries) != "list":
                fail("Invalid native lint consumer owner: %s" % owner)
            for entry in entries:
                if type(entry) != "list" or len(entry) != 2 or type(entry[0]) != "string" or type(entry[1]) != "string":
                    fail("Invalid native lint consumer entry for %s" % owner)
    ctx.file("consumers.bzl", "LINT_CONSUMERS = %s\n" % json.encode(consumers))
    ctx.file("BUILD.bazel", "")

lint_consumers_repository = repository_rule(
    implementation = _consumers_impl,
    configure = True,
)

def _policy_extension_impl(_ctx):
    lint_policy_repository(name = "aobus_lint_policy")
    lint_consumers_repository(name = "aobus_lint_consumers")

lint_policy = module_extension(implementation = _policy_extension_impl)
