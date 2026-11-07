"""Declare WinUI SDK consumer roles over repository-projected native inputs."""

load("@rules_cc//cc:defs.bzl", "cc_import", "cc_library")

def winui_sdk_imports(sdk):
    native.exports_files(["sdk.json", "sdk-diagnostics.json"])
    cc_library(
        name = "headers",
        hdrs = sdk["headers"] if sdk != None else [],
        includes = sdk["includes"] if sdk != None else [],
        defines = sdk["defines"] if sdk != None else [],
    )
    libraries = []
    for index, library in enumerate(sdk["libraries"] if sdk != None else []):
        name = "library_" + str(index)
        if library["kind"] == "interface":
            cc_import(name = name, interface_library = library["file"], system_provided = True)
        elif library["kind"] == "static":
            cc_import(name = name, static_library = library["file"])
        else:
            fail("unsupported WinUI library kind: %s" % library["kind"])
        libraries.append(":" + name)
    cc_library(
        name = "bootstrap",
        srcs = sdk["sources"] if sdk != None else [],
        alwayslink = sdk != None,
        deps = [":headers"] + libraries if sdk != None else [],
        linkopts = sdk["system_libraries"] if sdk != None else [],
    )
    native.filegroup(name = "runtime", srcs = sdk["runtime"] if sdk != None else [])
    native.filegroup(name = "tools", srcs = sdk["tools"] if sdk != None else [])
