load("//bazel:rs.bzl", "rs_cc_binary")

rs_cc_binary(
    name = "rml",
    srcs = [
        "rml/ListCommand.cpp",
        "rml/ListCommand.h",
        "rml/TrackCommand.cpp",
        "rml/TrackCommand.h",
        "rml/rml.cpp",
    ],
    defines = ["BOOST_ERROR_CODE_HEADER_ONLY"],
    linkopts = [
        "-ltag",
    ],
    #    linkstatic = False,
    deps = [
        "//cli:rs-cli",
        "//ml:rs-ml",
        "@boost//:timer",
        "@boost//:filesystem",
        "@boost//:system"
    ],
)
