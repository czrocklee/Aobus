"""The fixed public interface of the admitted native host-tool SDK."""

def host_tools(data):
    """Declare tools and their action inputs without provider selection logic.

    Args:
        data: Projected host-tool labels and input lists produced by materialize_host_tools.
    """
    native.exports_files([data["gperf"]] + data["gperf_runtime"])
    native.alias(name = "gperf", actual = data["gperf"])
    native.filegroup(name = "gperf_runtime", srcs = data["gperf_runtime"])
    native.filegroup(name = "windows_manifest_tools", srcs = data["windows_manifest_inputs"])
    native.exports_files([
        "windows_manifest_tools.bzl",
        "catalog_tools.bzl",
        "app_version.bzl",
        "test_tools.bzl",
    ])
    if data["resource_executable"]:
        native.exports_files([data["resource_executable"]])
        native.alias(name = "glib_compile_resources", actual = data["resource_executable"])
    native.filegroup(name = "glib_compile_resources_runtime", srcs = data["resource_inputs"])
    native.filegroup(name = "dbus_daemon", srcs = data["dbus_inputs"])
    native.filegroup(name = "catalog_tools", srcs = data["catalog_inputs"])
    native.config_setting(name = "_catalog_debug", values = {"compilation_mode": "dbg"})
    native.config_setting(name = "_catalog_release", values = {"compilation_mode": "opt"})
    native.alias(
        name = "catalog_icu",
        actual = select({
            ":_catalog_debug": "//catalog-debug:icu",
            ":_catalog_release": "//catalog-release:icu",
        }, no_match_error = "Catalog ICU requires dbg or opt"),
    )
