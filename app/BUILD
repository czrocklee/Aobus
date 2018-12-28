cc_binary(
    name = "rml",
    srcs = ["rml/rml.cpp"],
    copts = ["-std=c++1z"],
    linkopts = ["-lboost_system", "-lboost_filesystem", "-lboost_timer", "-ltag"],
    deps = ["//ml:rs-ml", "//cli:rs-cli"]
)


