load("@bazel_tools//tools/build_defs/repo:http.bzl", "http_archive")
load("@bazel_tools//tools/build_defs/repo:git.bzl", "new_git_repository")
load("//build:conanbuildinfo.bzl", "conan_basic_setup", "CONAN_QT_ROOT")

conan_basic_setup()

http_archive(
    name = "lmdb", 
    build_file = "lmdb.BUILD",
    sha256 = "44602436c52c29d4f301f55f6fd8115f945469b868348e3cddaf91ab2473ea26",
    strip_prefix = "lmdb-LMDB_0.9.24/libraries/liblmdb",
    urls = ["https://github.com/LMDB/lmdb/archive/LMDB_0.9.24.tar.gz"],
)

new_git_repository(
    name = "com_drycpp_lmdbxx",
    branch = "master",
    build_file = "lmdbxx.BUILD",
    remote = "https://github.com/drycpp/lmdbxx.git",
)

new_local_repository(
    name = "qt",
    build_file = "external/qt.BUILD",
    path = CONAN_QT_ROOT,
)
