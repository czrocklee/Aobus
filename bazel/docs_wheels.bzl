"""Download the selected wheelhouse with Bazel's hash-verified downloader."""

WHEEL_INPUTS = {
    "_manifest": attr.label(default = Label("//:script/ao/python-wheels.json"), allow_single_file = True),
    "_policy": attr.label(default = Label("//:script/ao/toolchain.json"), allow_single_file = True),
}

def download_wheels(ctx, target):
    """Fetch docs alone, or docs plus the native tools for the selected platform.

    Args:
        ctx: Repository context with WHEEL_INPUTS attributes.
        target: docs or the native platform of the admitted interpreter.

    Returns:
        Wheel filenames mapped to hash-locked, local pip requirements.
    """
    manifest = json.decode(ctx.read(ctx.attr._manifest))
    if manifest["schema"] != 1:
        fail("Unsupported Python wheel manifest schema")
    policy = json.decode(ctx.read(ctx.attr._policy))
    selected = {}
    packages = {}
    for wheel in manifest["artifacts"]:
        targets = wheel["targets"]

        # The shared checker runs strict mypy over doc_check.py. Its typed
        # markdown_it imports need the docs wheels even though mypy's own
        # distribution metadata does not require them.
        if "docs" not in targets and target not in targets:
            continue
        filename = wheel["filename"]
        url = wheel["url"]
        digest = wheel["sha256"]
        if not filename.endswith(".whl") or not all([c in "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_.-" for c in filename.elems()]):
            fail("Invalid wheel filename: " + filename)
        if not url.startswith("https://files.pythonhosted.org/packages/") or url.split("/")[-1] != filename or not digest:
            fail("Missing locked PyPI URL or SHA-256 for " + filename)
        name, version = filename.split("-")[:2]
        name = name.replace("_", "-").lower()
        if name in packages:
            fail("Duplicate package for %s: %s" % (target, filename))
        packages[name] = version

        # Keep the upstream basename for --distdir and allow the same SHA to
        # be served from Bazel's repository cache in other workspaces.
        ctx.download(url, output = "wheelhouse/" + filename, sha256 = digest)
        selected[filename] = "%s==%s --hash=sha256:%s" % (name, version, digest)
    required = ["markdown-it-py", "mdurl"] + ([] if target == "docs" else ["mypy", "ruff"])
    for name in required:
        actual = packages.get(name)
        expected = policy.get(name)
        if actual == None:
            fail("Missing %s wheel for %s" % (name, target))
        if actual != expected:
            fail("%s: %s wheel version %r differs from toolchain.json pin %r; use the exact wheel filename version spelling in toolchain.json" % (target, name, actual, expected))
    return selected

def _docs_wheels_impl(ctx):
    wheels = download_wheels(ctx, "docs")

    # Stable single-file labels; the runfile itself retains its wheel filename.
    aliases = [
        "alias(name = %s, actual = %s)" % (repr(filename.split("-")[0] + ".whl"), repr("wheelhouse/" + filename))
        for filename in wheels
    ]
    ctx.file("BUILD.bazel", 'package(default_visibility = ["//visibility:public"])\n' + "\n".join(aliases) + "\n", executable = False)

docs_wheels = repository_rule(implementation = _docs_wheels_impl, attrs = WHEEL_INPUTS)
