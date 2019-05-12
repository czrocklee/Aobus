load("@bazel_tools//tools/build_defs/repo:http.bzl", "http_archive")
load("@bazel_tools//tools/build_defs/repo:git.bzl", "git_repository")

http_archive(
    name = "flatbuffers",
    urls = ["https://github.com/google/flatbuffers/archive/v1.10.0.tar.gz"],
    sha256 = "3714e3db8c51e43028e10ad7adffb9a36fc4aa5b1a363c2d0c4303dd1be59a7c",
#    build_file = "@//:BUILD",
    strip_prefix = "flatbuffers-1.10.0",
)

git_repository(
    name = "bazel_rules_qt",
    remote = "https://github.com/justbuchanan/bazel_rules_qt.git",
    commit = "master",
)
new_local_repository(
    name = "qt",
    build_file = "@bazel_rules_qt//:qt.BUILD",
    path = "/usr/include/x86_64-linux-gnu/qt5/"
)

"""
git_repository(
    name = "com_github_nelhage_rules_boost",
    commit = "54eeadb9899973110312f133adf79fe9ccd1b45c",
    remote = "https://github.com/nelhage/rules_boost",
)
"""
git_repository(
    name = "com_github_nelhage_rules_boost",
    branch = "boost_dll",
    remote = "https://github.com/czrocklee/rules_boost.git",
)

load("@com_github_nelhage_rules_boost//:boost/boost.bzl", "boost_deps")
boost_deps()
