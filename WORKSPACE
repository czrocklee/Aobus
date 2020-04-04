load("@bazel_tools//tools/build_defs/repo:http.bzl", "http_archive")
load("@bazel_tools//tools/build_defs/repo:git.bzl", "git_repository", "new_git_repository")

http_archive(
    name = "com_google_flatbuffers",
    #sha256 = "3714e3db8c51e43028e10ad7adffb9a36fc4aa5b1a363c2d0c4303dd1be59a7c",
    strip_prefix = "flatbuffers-1.12.0",
    urls = ["https://github.com/google/flatbuffers/archive/v1.12.0.tar.gz"],
)

http_archive(
    name = "lmdb", 
    build_file = "lmdb.BUILD",
    #sha256 = "3714e3db8c51e43028e10ad7adffb9a36fc4aa5b1a363c2d0c4303dd1be59a7c",
    strip_prefix = "lmdb-LMDB_0.9.24/libraries/liblmdb",
    urls = ["https://github.com/LMDB/lmdb/archive/LMDB_0.9.24.tar.gz"],
)

new_git_repository(
    name = "com_drycpp_lmdbxx",
    branch = "master",
    build_file = "lmdbxx.BUILD",
    remote = "https://github.com/drycpp/lmdbxx.git",
)

http_archive(
    name = "taglib",
    build_file = "taglib.BUILD",
    #build_file_content = """filegroup(name = "all", srcs = glob(["**"]), visibility = ["//visibility:public"])""",
    sha256 = "b6d1a5a610aae6ff39d93de5efd0fdc787aa9e9dc1e7026fa4c961b26563526b",
    strip_prefix = "taglib-1.11.1",
    urls = ["https://github.com/taglib/taglib/archive/v1.11.1.tar.gz"],
)

git_repository(
    name = "com_justbuchanan_rules_qt",
    branch = "master",
    remote = "https://github.com/justbuchanan/bazel_rules_qt.git",
)

new_local_repository(
    name = "qt",
    build_file = "@com_justbuchanan_rules_qt//:qt.BUILD",
    path = "/usr/include/qt",  # May need configuring for your installation
    # For Qt5 on Ubuntu 16.04
    # path = "/usr/include/x86_64-linux-gnu/qt5/"
)

''' git_repository(
    name = "com_github_nelhage_rules_boost",
    commit = "4ee400beca08f524e7ea3be3ca41cce34454272f",
    remote = "https://github.com/nelhage/rules_boost",
) '''

git_repository(
    name = "com_github_nelhage_rules_boost",
    branch = "boost_dll",
    remote = "https://github.com/czrocklee/rules_boost.git",
)

load("@com_github_nelhage_rules_boost//:boost/boost.bzl", "boost_deps")

boost_deps()

git_repository(
    name = "rules_foreign_cc",
    commit = "456425521973736ef346d93d3d6ba07d807047df",
    remote = "https://github.com/bazelbuild/rules_foreign_cc.git",
)

load("@rules_foreign_cc//:workspace_definitions.bzl", "rules_foreign_cc_dependencies")

rules_foreign_cc_dependencies(register_default_tools = True)
