load("//bazel:rs.bzl", "rs_cc_binary", "rs_cc_library")

genrule(
    name = "fbs_meta_schema",
    srcs = [
        "fbs/Metadata.fbs",
        "fbs/Attributes.fbs",
    ],
    outs = ["Metadata.bfbs"],
    cmd = "$(location @conan_flatc//:flatc) -o $(@D) -b --schema $(SRCS)",
    tools = ["@conan_flatc//:flatc"],
)

genrule(
    name = "fbs_prop_schema",
    srcs = [
        "fbs/Properties.fbs",
        "fbs/Attributes.fbs",
    ],
    outs = ["Properties.bfbs"],
    cmd = "$(location @conan_flatc//:flatc) -o $(@D) -b --schema $(SRCS)",
    tools = ["@conan_flatc//:flatc"],
)

genrule(
    name = "fbs_track_gen",
    srcs = glob(["fbs/*.fbs"]),
    outs = ["include/rs/ml/fbs/Track_generated.h"],
    cmd = "$(location @conan_flatc//:flatc) -o $(@D) --cpp --gen-all --gen-object-api --scoped-enums $(location fbs/Track.fbs)",
    tools = ["@conan_flatc//:flatc"],
)

genrule(
    name = "fbs_list_gen",
    srcs = glob(["fbs/*.fbs"]),
    outs = ["include/rs/ml/fbs/List_generated.h"],
    cmd = "$(location @conan_flatc//:flatc) -o $(@D) --cpp --gen-all --gen-object-api --scoped-enums $(location fbs/List.fbs)",
    tools = ["@conan_flatc//:flatc"],
)

rs_cc_library(
    name = "ml_gen_interface",
    hdrs = glob(["gen/*.h"]),
    includes = ["gen"],
    visibility = ["//visibility:public"],
    deps = [
        "@conan_boost//:headers",
        "@conan_flatbuffers//:flatbuffers",
    ],
)

rs_cc_binary(
    name = "ml_gen",
    srcs = [
        "gen/Generator.h",
        "gen/gen.cpp",
    ],
    defines = ["BOOST_ERROR_CODE_HEADER_ONLY"],
    linkopts = [
        "-ldl",
    ],
    visibility = ["//visibility:public"],
    deps = [
        "@conan_boost//:boost_filesystem",
        "@conan_boost//:boost_program_options",
        "@conan_flatbuffers//:flatbuffers",
    ],
)

rs_cc_binary(
    name = "track_field_accesor_gen.so",
    srcs = ["src/query/TrackFieldAccessorGenerator.cpp"],
    linkshared = 1,
    deps = [":ml_gen_interface"],
)

genrule(
    name = "track_field_accesor",
    srcs = [
        ":track_field_accesor_gen.so",
        ":fbs_meta_schema",
        ":fbs_prop_schema",
    ],
    outs = ["include/rs/ml/query/gen/TrackFieldAccessor.h"],
    cmd = "$(location :ml_gen) --out-dir $(@D) --meta-schema $(location :fbs_meta_schema) --prop-schema $(location :fbs_prop_schema) --generators $(execpath :track_field_accesor_gen.so)",
    tools = [":ml_gen"],
)

rs_cc_library(
    name = "rs-ml",
    srcs = [
        "src/core/LMDBDatabase.cpp",
        "src/core/ResourceStore.cpp",
        "src/core/MusicLibrary.cpp",
        "src/query/Expression.cpp",
        "src/query/Parser.cpp",
        "src/query/Serializer.cpp",
        "src/query/TrackFilter.cpp",
        "src/file/MusicFile.cpp",
    ] + [":track_field_accesor"],
    hdrs = glob(["include/**/*.h"]) + [
        "//ml:fbs_track_gen",
        "//ml:fbs_list_gen",
    ],
    includes = ["include"],
    visibility = ["//visibility:public"],
    deps = [
        "@com_drycpp_lmdbxx//:lmdb++",
        "@conan_boost//:headers",
        "@conan_flatbuffers//:flatbuffers",
    ],
)
