"""Normal Clang warning policy shared by the Unix toolchains."""

UNIX_WARNINGS = [
    "-Wall",
    "-Wextra",
    "-Wpedantic",
    "-Wunused",
    "-Wformat=2",
    "-Woverloaded-virtual",
    "-Wnon-virtual-dtor",
    "-Wcast-align",
    "-Werror",
    "-Wno-null-dereference",
    "-Wno-double-promotion",
    "-Wno-conversion",
    "-Wno-sign-conversion",
    "-Wno-old-style-cast",
    "-Wno-float-conversion",
    "-Wno-c2y-extensions",
]
