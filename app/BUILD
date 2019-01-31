cc_binary(
    name = "rml",
    srcs = [
      "rml/rml.cpp", 
      "rml/ListCommand.h",
      "rml/ListCommand.cpp" 
    ],
    copts = ["-std=c++1z"],
    linkopts = ["-lboost_system", "-lboost_filesystem", "-lboost_timer", "-ltag"],
#    linkstatic = False,
    deps = ["//ml:rs-ml", "//cli:rs-cli"]
)


