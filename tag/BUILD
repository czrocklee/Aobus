load("//bazel:rs.bzl", "rs_cc_library")

rs_cc_library(
    name = "rs-tag",
    srcs = [
        "src/rs/tag/Decoder.h",
        "src/rs/tag/File.cpp",
        "src/rs/tag/flac/File.cpp",
        "src/rs/tag/flac/MetadataBlock.cpp",
        "src/rs/tag/flac/MetadataBlock.h",
        "src/rs/tag/flac/MetadataBlockLayout.h",
        "src/rs/tag/mp4/Atom.cpp",
        "src/rs/tag/mp4/Atom.h",
        "src/rs/tag/mp4/AtomLayout.h",
        "src/rs/tag/mp4/File.cpp",
        "src/rs/tag/mpeg/File.cpp",
        "src/rs/tag/mpeg/id3v2/Frame.h",
        "src/rs/tag/mpeg/id3v2/Layout.h",
        "src/rs/tag/mpeg/id3v2/Reader.cpp",
        "src/rs/tag/mpeg/id3v2/Reader.h",
    ],
    hdrs = glob(["include/**/*.h"]),
    defines = ["BOOST_ENDIAN_NO_CTORS"],  #suppress warning 'ignoring packed attribute because of unpacked non-POD field'
    includes = ["include"],
    visibility = ["//visibility:public"],
    deps = [
        "//common:rs-common",
        "@conan_boost//:headers",
        "@conan_boost//:boost_locale",
    ],
)
