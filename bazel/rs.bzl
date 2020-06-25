def default_copts():
    return select({
        "@bazel_tools//src/conditions:linux_x86_64": ["-std=c++17", "-fvisibility=hidden"],
        "@bazel_tools//src/conditions:windows": ["/std:c++latest"],
    })

DEFINES = ["_GLIBCXX_USE_CXX11_ABI=1"]

def rs_cc_library(name, defines = [], **kwargs):
    native.cc_library(
        name = name,
        defines = defines + DEFINES,
        copts = default_copts(),
        **kwargs
    )

def rs_cc_binary(name, defines = [], **kwargs):
    native.cc_binary(
        name = name,
        defines = defines + DEFINES,
        copts = default_copts(),
        **kwargs
    )

def rs_cc_test(name, defines = [], **kwargs):
    native.cc_test(
        name = name,
        defines = defines + DEFINES,
        copts = default_copts(),
        **kwargs
    )
