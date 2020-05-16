load("//bazel:rs.bzl", "rs_cc_binary")

rs_cc_binary(
    name = "rml",
    srcs = [
        "ListCommand.cpp",
        "ListCommand.h",
        "TrackCommand.cpp",
        "TrackCommand.h",
        "rml.cpp",
    ],
    defines = ["BOOST_ERROR_CODE_HEADER_ONLY"],

    deps = [
        "//cli:rs-cli",
        "//ml:rs-ml",
        "//tag:rs-tag",
        "@conan_boost//:boost_timer",
    ],
)
