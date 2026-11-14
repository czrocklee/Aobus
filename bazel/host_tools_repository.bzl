"""Materialize only selected host SDK files and data, not BUILD declarations."""

_HOST_TOOLS_BUILD = """load("@aobus//bazel:host_tools.bzl", "host_tools")
load(":host_tools_data.bzl", "HOST_TOOLS")

package(default_visibility = ["//visibility:public"])

host_tools(HOST_TOOLS)
"""

def _basename(source):
    name = source.replace("\\", "/").split("/")[-1]
    if not name or name in [".", ".."] or ":" in name:
        fail("invalid host-tool basename: %s" % source)
    return name

def _link(ctx, source, destination, links):
    # Resolve the selected file, not a whole installation. repository_ctx owns
    # Windows file-copy fallback when native symlinks are unavailable.
    path = ctx.path(source).realpath
    ctx.symlink(path, destination)
    links.append({"source": str(path), "destination": destination})
    return destination

def materialize_host_tools(ctx, data):
    """Project admitted native paths and SDK bytes into the fixed tool interface.

    Args:
        ctx: Repository context used to link inputs and write tool data and BUILD files.
        data: Admitted host-tool paths, runtime inputs, catalog facts, and test metadata.
    """
    links = []
    gperf = data["gperf"]
    executable = _link(ctx, gperf["executable"], "bin/" + _basename(gperf["executable"]), links)
    runtime = [_link(ctx, source, "bin/" + _basename(source), links) for source in gperf["runtime"]]
    manifest_inputs = []
    for name in sorted(data["manifest_tools"]):
        manifest_inputs.append(_link(ctx, data["manifest_tools"][name], "windows-manifest/" + name + ".exe", links))

    catalog = data["catalog"]
    catalog_inputs = []
    for index, source in enumerate(catalog["artifacts"]):
        catalog_inputs.append(_link(ctx, source, "catalog-sdk/%s/" % index + _basename(source), links))

    resource = data.get("resource")
    resource_executable = ""
    resource_inputs = []
    if resource != None:
        source = resource["executable"]
        resource_executable = _link(ctx, source, "resource-tools/bin/" + _basename(source), links)
        for index, source in enumerate(resource["runtime"] + resource["evidence"]):
            resource_inputs.append(_link(ctx, source, "resource-tools/runtime/%s/" % index + _basename(source), links))

    dbus_inputs = []
    dbus = data.get("dbus")
    if dbus != None:
        for index, source in enumerate(dbus["artifacts"]):
            dbus_inputs.append(_link(ctx, source, "test-tools/%s/" % index + _basename(source), links))
        ctx.file("test-tools.json", json.encode(dbus["identity"]) + "\n", executable = False)
        dbus_inputs.append("test-tools.json")

    ctx.file("windows_manifest_tools.bzl", "WINDOWS_MANIFEST_TOOLS = %s\n" % repr(data["manifest_tools"]), executable = False)
    ctx.file("catalog_tools.bzl", "CATALOG_TOOLS = %s\n" % repr(catalog["tools"]), executable = False)
    ctx.file("test_tools.bzl", "TEST_TOOLS = %s\n" % repr(data["test_tools"]), executable = False)
    ctx.file("app_version.bzl", "APP_VERSION = %s\n" % repr(data["app_version"]), executable = False)
    ctx.file("host_tools_data.bzl", "HOST_TOOLS = %s\n" % repr({
        "gperf": executable,
        "gperf_runtime": runtime,
        "windows_manifest_inputs": manifest_inputs,
        "catalog_inputs": catalog_inputs,
        "resource_executable": resource_executable,
        "resource_inputs": resource_inputs,
        "dbus_inputs": dbus_inputs,
    }), executable = False)
    ctx.file("manifest.json", json.encode({
        "tool": gperf["identity"],
        "artifacts": links,
        "windows_manifest_tools": data["manifest_tools"],
        "catalog_tools": catalog["tools"],
        "catalog_imports": {"debug": "catalog-debug/manifest.json", "release": "catalog-release/manifest.json"},
        "resource_tool": resource["identity"] if resource != None else {},
    }) + "\n", executable = False)
    ctx.file("BUILD.bazel", _HOST_TOOLS_BUILD, executable = False)
