"""Project validated SDK observations without loading BUILD graph declarations."""

_IMPORT_BUILD = """load("@aobus//bazel:imports.bzl", "sdk_imports")
load(":sdk_data.bzl", "CONFIG", "PACKAGES", "PLATFORM")

package(default_visibility = ["//visibility:public"])

sdk_imports(CONFIG, PACKAGES, PLATFORM)
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
    """Validate projected import names and return their public target names.

    Args:
        packages: Projected SDK packages with header, library, and indirect runtime names.

    Returns:
        Sorted public package and manifest target names.
    """
    names = {name: True for name in ["BUILD.bazel", "sdk_data.bzl", "_prepared_mode", "manifest.json"]}
    public = ["manifest.json"]
    indirect = {}
    for package in packages:
        _claim_target(names, package["name"])
        public.append(package["name"])
        for item in package["runtime"]:
            if "name" not in item:
                continue
            previous = indirect.get(item["name"])
            if previous == None:
                _claim_target(names, item["name"])
                indirect[item["name"]] = item
            elif previous != item:
                fail("conflicting shared indirect target: %s" % item["name"])

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
    """Validate configured root names against each other and generated files.

    Args:
        targets: Public SDK target names to expose through configured root aliases.

    Returns:
        Sorted target names after collision validation.
    """
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
    destination = subpackage + "/" + path
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

def _project_runtime(ctx, subpackage, path, source, projected, windows, runtime_sources, runtime_basenames):
    # Native admission supplies resolved physical paths. Reuse that identity,
    # never an equal basename or equal bytes; distinct hard-link paths stay distinct.
    identity = source.replace("\\", "/").lower() if windows else None
    if identity != None:
        name = _basename(source).lower()
        previous = runtime_basenames.get(name)
        if previous != None and previous != identity:
            fail("Windows runtime DLL basename has distinct source origins: %s, %s" % (previous, source))
        runtime_basenames[name] = identity
    if identity != None and identity in runtime_sources:
        return runtime_sources[identity]
    path = _project_file(ctx, subpackage, path, source, "runtime", projected, windows)
    if identity != None:
        runtime_sources[identity] = path
    return path

def materialize_imports(ctx, subpackage, config, packages, manifest):
    """Write sparse SDK files and data; return the graph's public target names.

    Python owns native admission. Header trees carry their lexical file paths
    and original anchor indices; includes independently refer to trees in the
    original ordered interface. Artifact and include paths in graph data are
    repository-relative; already-admitted linker flags are preserved verbatim.

    Args:
        ctx: Repository context used to link artifacts and write SDK data and BUILD files.
        subpackage: Destination debug/release or catalog-debug/catalog-release package.
        config: Admitted dependency configuration, either debug or release.
        packages: Admitted package interfaces with original artifact paths and header anchors.
        manifest: Admission metadata whose configuration identifies the native platform.

    Returns:
        Sorted public target names for the materialized SDK subpackage.
    """
    if config not in ["debug", "release"]:
        fail("unsupported SDK configuration: %s" % config)
    if subpackage not in ["debug", "release", "catalog-debug", "catalog-release"]:
        fail("unsupported SDK subpackage: %s" % subpackage)
    windows = manifest["configuration"]["platform"] == "windows"
    projected = {}
    shared_imports = {}

    # These maps belong to one configured subpackage, never target/exec modes together.
    # Identity is the admitted source spelling, not a host path, realpath, hash, or inode.
    runtime_sources = {}
    runtime_basenames = {}
    indirect_imports = {}
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
                    imported["runtime"] = _project_runtime(ctx, subpackage, prefix + _basename(library["runtime"]), library["runtime"], projected, windows, runtime_sources, runtime_basenames)
                if key != None:
                    shared_imports[key] = imported
            libraries.append(imported)

        # One indirect list. Linked DLL paths stay on library records.
        runtime = []
        for index, source in enumerate(package["runtime"]):
            basename = _basename(source)
            if windows and not basename.lower().endswith(".dll"):
                fail("Windows indirect runtime requires a .dll: %s" % basename)
            path = _project_runtime(ctx, subpackage, "artifacts/%s/runtime/%s/" % (name, index) + basename, source, projected, windows, runtime_sources, runtime_basenames)
            if windows:
                record = indirect_imports.get(path)
                if record == None:
                    record = {"name": name + "_indirect_" + str(index), "file": path}
                    indirect_imports[path] = record
                if record not in runtime:
                    runtime.append(record)
            else:
                runtime.append({"file": path})
        result.append({
            "name": name,
            "headers": headers,
            "includes": includes,
            "defines": package["defines"],
            "linkopts": package["linkopts"],
            "libraries": libraries,
            "runtime": runtime,
        })
    targets = import_targets(result)
    prefix = subpackage + "/"
    ctx.file(
        prefix + "sdk_data.bzl",
        "CONFIG = %s\nPLATFORM = %s\nPACKAGES = %s\n" % (repr(config), repr(manifest["configuration"]["platform"]), repr(result)),
        executable = False,
    )
    ctx.file(prefix + "manifest.json", json.encode(dict(manifest, targets = targets)) + "\n", executable = False)
    ctx.file(prefix + "BUILD.bazel", _IMPORT_BUILD, executable = False)
    return targets

def materialize_aliases(ctx, targets):
    """Write only target-name data and the configured root BUILD entrypoint."""
    targets = alias_targets(targets)
    ctx.file("sdk_targets.bzl", "TARGETS = %s\n" % repr(targets), executable = False)
    ctx.file("BUILD.bazel", _ALIAS_BUILD, executable = False)
