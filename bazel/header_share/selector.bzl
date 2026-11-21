"""Exercise header-sharing decisions with admitted dependency observations."""

load("//bazel:import_repository.bzl", "shared_file_header_packages")

def _tree(source):
    return {"index": 0, "root": "/observation", "files": [{"path": "marker.h", "source": source}]}

def _side(configuration, packages):
    return {
        "manifest": {"configuration": configuration},
        "packages": packages,
    }

def _package(name, headers, includes, define):
    return {"name": name, "headers": headers, "includes": includes, "defines": [define]}

def _selector_test_impl(_ctx):
    headers = [_tree("/observed/source/marker.h")]
    includes = [{"tree": 0, "path": ""}, {"tree": 0, "path": "nested"}]
    debug = _package("example", headers, includes, "MODE=1")
    release = _package("example", headers, includes, "MODE=2")
    linux = {"platform": "linux", "provider": {"name": "nix"}}
    windows = {"platform": "windows", "provider": {"name": "vcpkg"}}
    macos = {"platform": "macos", "provider": {"name": "vcpkg"}}
    extra_headers = [dict(headers[0], files = headers[0]["files"] + [{"path": "extra.h", "source": "/observed/source/extra.h"}])]
    for name, dc, rc, debug_package, release_package, expected in [
        ("linux", linux, linux, debug, release, {"example": True}),
        ("windows", windows, windows, debug, release, {"example": True}),
        ("mixed os", windows, linux, debug, release, {}),
        ("mixed provider", windows, dict(windows, provider = {"name": "other"}), debug, release, {}),
        ("missing provider", {"platform": "windows"}, windows, debug, release, {}),
        ("different name", windows, windows, debug, dict(release, name = "other"), {}),
        ("source", windows, windows, debug, dict(release, headers = [_tree("/other/source/marker.h")]), {}),
        ("membership", windows, windows, debug, dict(release, headers = extra_headers), {}),
        ("include order", windows, windows, debug, dict(release, includes = includes[::-1]), {}),
        ("empty", windows, windows, dict(debug, headers = [], includes = []), dict(release, headers = [], includes = []), {}),
        ("macos", macos, macos, debug, release, {}),
    ]:
        actual = shared_file_header_packages({
            "debug": _side(dc, [debug_package]),
            "release": _side(rc, [release_package]),
        })
        if actual != expected:
            return [AnalysisTestResultInfo(success = False, message = "%s: expected %s, got %s" % (name, expected, actual))]
    return [AnalysisTestResultInfo(success = True, message = "")]

header_share_selector_test = rule(
    implementation = _selector_test_impl,
    analysis_test = True,
    test = True,
    # The generated test script needs Linux, not the target's build profile.
    exec_groups = {"test": exec_group(exec_compatible_with = ["@platforms//os:linux"])},
)
