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
            "alpha": False,
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
        "root": attr.label(default = Label("//:MODULE.bazel"), allow_single_file = True),
        "helper": attr.label(default = Label("//:script/ao/core/bazel_lint_action.py"), allow_single_file = True),
    },
    configure = True,
)

def _policy_extension_impl(_ctx):
    lint_policy_repository(name = "aobus_lint_policy")

lint_policy = module_extension(implementation = _policy_extension_impl)
