"""Exercise the Linux header-sharing decision with admitted dependency observations."""

load("//bazel:import_repository.bzl", "shared_linux_header_packages")

def _tree(source):
    return {"index": 0, "root": "/observation", "files": [{"path": "marker.h", "source": source}]}

def _side(platform, headers, includes, define):
    return {
        "manifest": {"configuration": {"platform": platform}},
        "packages": [{"name": "example", "headers": headers, "includes": includes, "defines": [define]}],
    }

def _selector_test_impl(_ctx):
    headers = [_tree("/observed/source/marker.h")]
    includes = [{"tree": 0, "path": ""}, {"tree": 0, "path": "nested"}]
    for platform, debug, release, release_includes, expected in [
        ("linux", headers, headers, includes, {"example": True}),
        ("linux", headers, [_tree("/other/source/marker.h")], includes, {}),
        ("linux", headers, headers, includes[::-1], {}),
        ("linux", [], [], includes, {}),
        ("macos", headers, headers, includes, {}),
    ]:
        actual = shared_linux_header_packages({
            "debug": _side(platform, debug, includes, "MODE=1"),
            "release": _side(platform, release, release_includes, "MODE=2"),
        })
        if actual != expected:
            return [AnalysisTestResultInfo(success = False, message = "expected %s, got %s for %s: %s / %s, %s" % (expected, actual, platform, debug, release, release_includes))]
    return [AnalysisTestResultInfo(success = True, message = "")]

header_share_selector_test = rule(
    implementation = _selector_test_impl,
    analysis_test = True,
    test = True,
    # The generated test script needs Linux, not the target's build profile.
    exec_groups = {"test": exec_group(exec_compatible_with = ["@platforms//os:linux"])},
)
