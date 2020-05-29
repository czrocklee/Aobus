load("//bazel:rs.bzl", "rs_cc_binary", "rs_cc_library")

rs_cc_library(
    name = "rs-tag",
    srcs = [
        "src/rs/tag/File.cpp",
        "src/rs/tag/flac/File.cpp",
        "src/rs/tag/flac/MetadataBlock.h",
        "src/rs/tag/flac/MetadataBlockLayout.h",
        "src/rs/tag/mp4/Atom.cpp",
        "src/rs/tag/mp4/Atom.h",
        "src/rs/tag/mp4/AtomLayout.h",
        "src/rs/tag/mp4/File.cpp",
    ],
    hdrs = glob(["include/**/*.h"]),
    defines = ["BOOST_ENDIAN_NO_CTORS"],  #suppress warning 'ignoring packed attribute because of unpacked non-POD field'
    includes = ["include"],
    visibility = ["//visibility:public"],
    deps = ["//common:rs-common"],
)
