
genrule(
    name = "fbs_meta_schema",
    srcs = ["fbs/Metadata.fbs", "fbs/Attributes.fbs"],
    outs = ["Metadata.bfbs"],
    cmd = "$(location @flatbuffers//:flatc) -o $(@D) -b --schema $(SRCS)",
    tools = ["@flatbuffers//:flatc"],
)

genrule(
    name = "fbs_prop_schema",
    srcs = ["fbs/Properties.fbs", "fbs/Attributes.fbs"],
    outs = ["Properties.bfbs"],
    cmd = "$(location @flatbuffers//:flatc) -o $(@D) -b --schema $(SRCS)",
    tools = ["@flatbuffers//:flatc"],
)

genrule(
    name = "fbs_track_gen",
    srcs = glob(["fbs/*.fbs"]),
    outs = ["include/rs/ml/core/Track_generated.h"],
    cmd = "$(location @flatbuffers//:flatc) -o $(@D) --cpp --gen-all --gen-object-api $(location fbs/Track.fbs)",
    tools = ["@flatbuffers//:flatc"],
)

genrule(
    name = "fbs_list_gen",
    srcs = glob(["fbs/*.fbs"]),
    outs = ["include/rs/ml/core/List_generated.h"],
    cmd = "$(location @flatbuffers//:flatc) -o $(@D) --cpp --gen-all --gen-object-api $(location fbs/List.fbs)",
    tools = ["@flatbuffers//:flatc"],
)

cc_library(
    name = "ml_gen_interface",
    hdrs = glob(["gen/*.h"]),
    includes = ["gen"],
    copts = ["-std=c++1z"],
    deps = ["@flatbuffers//:flatbuffers"],
    visibility = ["//visibility:public"],
)

cc_binary(
    name = "ml_gen",
    srcs = ["gen/gen.cpp", "gen/Generator.h"],
    copts = ["-std=c++1z"],
    linkopts = ["-lboost_system", "-lboost_filesystem", "-lboost_program_options", "-ldl"],
    deps = ["@flatbuffers//:flatbuffers"],
    visibility = ["//visibility:public"],
)

cc_binary(
    name = "track_field_accesor_gen.so",
    srcs = ["src/query/TrackFieldAccessorGenerator.cpp"],
    copts = ["-std=c++1z"],
    deps = [":ml_gen_interface"],
    linkshared = 1,
)

genrule(
    name = "track_field_accesor",
    srcs = [":track_field_accesor_gen.so", ":fbs_meta_schema", ":fbs_prop_schema"],
    outs = ["include/rs/ml/query/gen/TrackFieldAccessor.h"],
    cmd = "$(location :ml_gen) --out-dir $(@D) --meta-schema $(location :fbs_meta_schema) --prop-schema $(location :fbs_prop_schema) --generators $(execpath :track_field_accesor_gen.so)",
    tools = [":ml_gen"],
)

cc_library(
    name = "rs-ml",
    srcs = [
#        "src/core/DynamicList.cpp", 
#        "src/core/DynamicTrackListImpl.h",
        "src/core/LMDBDatabase.cpp",
        "src/core/MusicLibrary.cpp",
#        "src/core/TrackListZero.cpp",
        "src/query/Expression.cpp",
        "src/query/Parser.cpp",
        "src/query/Serializer.cpp",
        "src/query/TrackFilter.cpp",
    ] + [':track_field_accesor'],
    hdrs = glob(["include/**/*.h"]) + ["//ml:fbs_track_gen", "//ml:fbs_list_gen"],
#   include_prefix = "include/rs",
    copts = ["-std=c++1z"],
    includes = ["include"],
    linkopts = ['-llmdb'],

    visibility = ["//visibility:public"],
)


