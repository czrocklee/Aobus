"""MSVC policy consumed through upstream discovery, profiles and SDK imports.

The marked .bazelrc region is a checked-in projection of MSVC_REPOSITORY_ENV,
not a second place to edit flags. Native Bazel validates it before loading the
normal dependency/toolchain graph; ao format updates it.
"""

MSVC_REPOSITORY_ENV = {
    "BAZEL_COPTS": ["/W4"],
    "BAZEL_CXXOPTS": [
        "/std:c++latest",
        "/external:W0",
        "/utf-8",
        "/permissive-",
        "/Zc:__cplusplus",
        "/Zc:preprocessor",
        "/DWIN32_LEAN_AND_MEAN",
        "/DUNICODE",
        "/D_UNICODE",
        "/DWINVER=0x0A00",
    ],
    "BAZEL_WIN32_WINNT": ["/D_WIN32_WINNT=0x0A00"],
}

MSVC_RELEASE_COPTS = ["/GL"]
MSVC_RELEASE_LINKOPTS = ["/LTCG"]
MSVC_RELEASE_EXCEPTION_COPTS = ["/GL-"]
MSVC_ASAN_COPTS = ["/fsanitize=address", "/D_DISABLE_STL_ANNOTATION"]

# These supplement the upstream toolchain only for the SDK-linked checker.
# The language standard remains the ordinary MSVC toolchain policy above.
MSVC_LLVM_COPTS = [
    "/EHs-c-",
    "/D_HAS_EXCEPTIONS=0",
    "/DCLANG_BUILD_STATIC",
    "/DLLVM_BUILD_STATIC",
    "/wd4702",
]
MSVC_LLVM_RTTI_COPTS = {
    "YES": ["/GR"],
    "NO": ["/GR-"],
}
MSVC_LLVM_HOST_COPTS = ["/DNOMINMAX", "/D_CRT_SECURE_NO_WARNINGS", "/bigobj"]

MSVC_MSBUILD_LANGUAGE_STANDARD = "stdcpplatest"
MSVC_WINUI_COPTS = ["/bigobj"]

_RC_BEGIN = "# BEGIN GENERATED MSVC FLAGS: bazel/msvc_policy.bzl"
_RC_END = "# END GENERATED MSVC FLAGS"

def msvc_rc_region():
    """Render the native rc inlets without replacing upstream MSVC mechanics."""
    return "\n".join([_RC_BEGIN] + [
        "build:windows --repo_env=" + name + "=" + ":".join([
            flag.replace(":", "%:")
            for flag in MSVC_REPOSITORY_ENV[name]
        ])
        for name in sorted(MSVC_REPOSITORY_ENV)
    ] + [_RC_END])

def validate_msvc_rc(ctx):
    """Reject a stale or independently overridden checked-in projection.

    Args:
        ctx: Module context reading the rc before native repository declarations.
    """
    text = ctx.read(Label("//:.bazelrc"))
    if text.count(_RC_BEGIN) != 1 or text.count(_RC_END) != 1:
        fail(".bazelrc requires one generated MSVC flags region; run ./ao format bazel/msvc_policy.bzl")
    before, region = text.split(_RC_BEGIN, 1)
    region, after = region.split(_RC_END, 1)
    if _RC_BEGIN + region + _RC_END != msvc_rc_region():
        fail(".bazelrc MSVC flags are stale; run ./ao format bazel/msvc_policy.bzl")
    for line in (before + after).splitlines():
        line = line.strip()
        if line.startswith("#"):
            continue
        for name in MSVC_REPOSITORY_ENV:
            # Reserve these inlets for the view and their canonical clearing
            # lines, including split/quoted native option spellings.
            if name in line and line != "common --repo_env==" + name:
                fail("MSVC flags must be edited in bazel/msvc_policy.bzl, not outside the generated rc region: " + name)
