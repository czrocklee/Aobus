load("//bazel:rs.bzl", "rs_cc_binary")

rs_cc_binary(
    name = "tag",
    srcs = [
        "main.cpp",
    ],
    defines = ["BOOST_ERROR_CODE_HEADER_ONLY"],

    #    linkstatic = False,
    deps = [
        #"@boost//:interprocess",
        #"@boost//:system",
        "//tag:rs-tag",
        "@conan_qt//:Qt6Core",

    ],
)
