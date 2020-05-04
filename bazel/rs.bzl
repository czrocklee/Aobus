def default_copts():
    return select({
        "@bazel_tools//src/conditions:linux_x86_64": ["-std=c++17"],
        "@bazel_tools//src/conditions:windows": ["/std:c++latest"],
    })

def rs_cc_library(name, **kwargs):
    native.cc_library(
        name = name,
        copts = select({
            "@bazel_tools//src/conditions:linux_x86_64": ["-std=c++17"],
            "@bazel_tools//src/conditions:windows": ["/std:c++latest"],
        }),
        **kwargs
    )

def rs_cc_binary(name, **kwargs):
    native.cc_binary(
        name = name,
        copts = select({
            "@bazel_tools//src/conditions:linux_x86_64": ["-std=c++17"],
            "@bazel_tools//src/conditions:windows": ["/std:c++latest"],
        }),
        **kwargs
    )

def rs_cc_test(name, **kwargs):
    native.cc_test(
        name = name,
        copts = default_copts(),
        **kwargs
    )
