"""Project validated SDK observations without loading BUILD graph declarations."""

_IMPORT_BUILD = """load("@aobus//bazel:imports.bzl", "sdk_imports")
load(":sdk_data.bzl", "CONFIG", "PACKAGES")

package(default_visibility = ["//visibility:public"])

sdk_imports(CONFIG, PACKAGES)
"""

_ALIAS_BUILD = """load("@aobus//bazel:imports.bzl", "configured_aliases")
load(":sdk_targets.bzl", "TARGETS")

package(default_visibility = ["//visibility:public"])

configured_aliases(TARGETS)
"""

def _claim_target(names, name):
    if name in names:
        fail("generated target name collision: %s" % name)
    names[name] = True

def import_targets(packages):
    """Validate projected import names and return their public target names."""
    names = {name: True for name in ["BUILD.bazel", "sdk_data.bzl", "_prepared_mode", "manifest.json"]}
    public = ["manifest.json"]
    for package in packages:
        for name in [package["name"], package["name"] + "_runtime", package["name"] + "_runtime_extra"]:
            _claim_target(names, name)
            public.append(name)

    imports = {}
    for package in packages:
        for header in package["headers"]:
            _claim_target(names, header["name"])
        for library in package["libraries"]:
            name = library["name"]
            if name in imports:
                if library != imports[name]:
                    fail("conflicting shared import target: %s" % name)
            else:
                _claim_target(names, name)
                imports[name] = library
    return sorted(public)

def alias_targets(targets):
    """Validate configured root names against each other and generated files."""
    names = {name: True for name in ["BUILD.bazel", "sdk_targets.bzl", "_debug", "_release"]}
    for name in targets:
        _claim_target(names, name)
    return sorted(targets)

def _relative_path(value, allow_root = False):
    if allow_root and value in ["", "."]:
        return ""
    if not value or value.startswith("/") or "\\" in value or ":" in value or any([part in ["", ".", ".."] for part in value.split("/")]):
        fail("invalid artifact-relative path: %s" % value)
    return value

def _basename(value):
    return _relative_path(value.replace("\\", "/").split("/")[-1])

def _project_file(ctx, subpackage, path, source, role, projected, windows):
    destination = (subpackage + "/" if subpackage else "") + path
    key = destination.lower() if windows else destination
    identity = source.replace("\\", "/").lower() if windows else source
    previous = projected.get(key)
    if previous != None:
        if previous != (identity, role):
            fail("generated artifact path collision: %s" % destination)
    else:
        projected[key] = (identity, role)
        # Only selected files are linked, never a whole provider header tree.
        ctx.symlink(ctx.path(source), destination)
    return path

def materialize_imports(ctx, subpackage, config, packages, manifest):
    """Write sparse SDK files and data; return the graph's public target names.

    Python owns native admission. Header trees carry their lexical file paths
    and original anchor indices; includes independently refer to trees in the
    original ordered interface. Artifact and include paths in graph data are
    repository-relative; already-admitted linker flags are preserved verbatim.
    """
    if config not in ["debug", "release"]:
        fail("unsupported SDK configuration: %s" % config)
    if subpackage not in ["", "debug", "release", "catalog-debug", "catalog-release"]:
        fail("unsupported SDK subpackage: %s" % subpackage)
    windows = manifest["configuration"]["platform"] == "windows"
    projected = {}
    shared_imports = {}
    result = []
    for package in packages:
        name = package["name"]
        headers = []
        prefixes = []
        for tree in package["headers"]:
            identity = name + "_" + str(tree["index"])
            prefix = "artifacts/_headers/" + identity
            prefixes.append(prefix)
            files = []
            for file in tree["files"]:
                files.append(_project_file(ctx, subpackage, prefix + "/" + _relative_path(file["path"]), file["source"], "header", projected, windows))
            headers.append({"name": "_headers_" + identity, "files": files})
        includes = []
        for include in package["includes"]:
            path = _relative_path(include["path"], allow_root = True)
            includes.append(prefixes[include["tree"]] + ("/" + path if path else ""))

        libraries = []
        runtime = []
        for library in package["libraries"]:
            index = library["index"]
            key = (library["source"], library["runtime"]) if windows and library["kind"] == "shared" else None
            if key != None and key in shared_imports:
                imported = shared_imports[key]
            else:
                prefix = "artifacts/%s/lib/%s/" % (name, index)
                imported = {
                    "name": name + "_import_" + str(index),
                    "kind": library["kind"],
                    "file": _project_file(ctx, subpackage, prefix + _relative_path(library["basename"]), library["source"], "library", projected, windows),
                    "runtime": None,
                }
                if library["runtime"] != None:
                    imported["runtime"] = _project_file(ctx, subpackage, prefix + _basename(library["runtime"]), library["runtime"], "runtime", projected, windows)
                if key != None:
                    shared_imports[key] = imported
            libraries.append(imported)
            if imported["runtime"] != None:
                runtime.append(imported["runtime"])
        runtime_extra = []
        for index, source in enumerate(package["runtime"]):
            path = _project_file(ctx, subpackage, "artifacts/%s/runtime/%s/" % (name, index) + _basename(source), source, "runtime", projected, windows)
            runtime.append(path)
            runtime_extra.append(path)
        result.append({
            "name": name,
            "headers": headers,
            "includes": includes,
            "defines": package["defines"],
            "linkopts": package["linkopts"],
            "libraries": libraries,
            "runtime": runtime,
            "runtime_extra": runtime_extra,
        })
    targets = import_targets(result)
    prefix = subpackage + "/" if subpackage else ""
    ctx.file(prefix + "sdk_data.bzl", "CONFIG = %s\nPACKAGES = %s\n" % (repr(config), repr(result)), executable = False)
    ctx.file(prefix + "manifest.json", json.encode(dict(manifest, targets = targets)) + "\n", executable = False)
    ctx.file(prefix + "BUILD.bazel", _IMPORT_BUILD, executable = False)
    return targets

def materialize_aliases(ctx, targets):
    """Write only target-name data and the configured root BUILD entrypoint."""
    targets = alias_targets(targets)
    ctx.file("sdk_targets.bzl", "TARGETS = %s\n" % repr(targets), executable = False)
    ctx.file("BUILD.bazel", _ALIAS_BUILD, executable = False)
