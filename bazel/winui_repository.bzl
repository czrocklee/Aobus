"""Project admitted WinUI SDK files; native discovery supplies no Bazel program."""

_WINUI_BUILD = """load("@aobus//bazel:winui_sdk.bzl", "winui_sdk_imports")
load(":sdk_data.bzl", "SDK")

package(default_visibility = ["//visibility:public"])

winui_sdk_imports(SDK)
"""

def _sdk_path(location):
    package = location["package"]
    path = location["path"]
    if not package or package in [".", ".."] or any([part in package for part in ["/", "\\", ":"]]):
        fail("invalid WinUI SDK provider name: %s" % package)
    if path in ["", "."]:
        return "sdk/" + package
    if path.startswith("/") or "\\" in path or ":" in path or any([part in ["", ".", ".."] for part in path.split("/")]):
        fail("invalid WinUI SDK provider-relative path: %s" % path)
    return "sdk/" + package + "/" + path

def materialize_winui(ctx, subpackage, facts):
    """Retain the SDK's sparse input closure and its fixed public interfaces."""
    if subpackage not in ["debug", "release"]:
        fail("unsupported WinUI SDK subpackage: %s" % subpackage)
    prefix = subpackage + "/"
    data = None
    if facts != None:
        projected = {}
        destinations = {}
        for file in facts["sdk_files"]:
            source = file["source"]
            destination = _sdk_path(file)
            key = destination.lower()
            if key in destinations and destinations[key] != source:
                fail("WinUI SDK projection collision: %s" % destination)
            destinations[key] = source
            ctx.symlink(ctx.path(source), prefix + destination)
            projected[source] = destination
        tools = [projected[source] for source in facts["tools"]]
        for index, source in enumerate(facts["native_inputs"]):
            destination = "native-tools/%s/" % index + source.replace("\\", "/").split("/")[-1]
            ctx.symlink(ctx.path(source), prefix + destination)
            tools.append(destination)
        data = {
            "includes": [_sdk_path(location) for location in facts["includes"]],
            "defines": facts["defines"],
            "headers": [projected[source] for source in facts["headers"]],
            "sources": [projected[source] for source in facts["sources"]],
            "libraries": [dict(library, file = projected[library["file"]]) for library in facts["libraries"]],
            "system_libraries": facts["system_libraries"],
            "runtime": [projected[source] for source in facts["runtime"]],
            "tools": tools,
        }
    ctx.file(prefix + "sdk.json", json.encode(facts["execution"] if facts != None else {}) + "\n", executable = False)
    ctx.file(prefix + "sdk-diagnostics.json", json.encode(facts["diagnostics"] if facts != None else {}) + "\n", executable = False)
    ctx.file(prefix + "sdk_data.bzl", "SDK = %s\n" % repr(data), executable = False)
    ctx.file(prefix + "BUILD.bazel", _WINUI_BUILD, executable = False)
    return ["bootstrap", "headers", "runtime", "sdk-diagnostics.json", "sdk.json", "tools"]
