"""Shared language requirements and GNU-style values, not one compiler policy.

Compiler-specific flags belong to gcc_linux_policy.bzl, clang_linux_policy.bzl,
clang_macos_policy.bzl and msvc_policy.bzl. Consumers bind the named header
requirements only where needed. Python reads the plain literal declarations.
"""

CXX_STANDARD = 26

UNIX_TEST_COPTS = ["-Wno-array-bounds"]

# These Linux SDK header requirements use the same GNU-style spelling in both
# supported compilers; they are never enabled for unrelated consumers.
GTK_HEADER_SSE_COPTS = ["-mfpmath=sse", "-msse", "-msse2"]
PIPEWIRE_SPA_COPTS = ["-fno-strict-aliasing", "-fno-strict-overflow"]
GRESOURCE_WARNING_COPTS = ["-Wno-overlength-strings"]

UNIX_LLVM_RTTI_COPTS = {
    "YES": ["-frtti"],
    "NO": ["-fno-rtti"],
}

UNIX_LLVM_RELEASE_COPTS = ["-DNDEBUG"]
UNIX_LLVM_HOST_COPTS = ["-fno-sanitize=all"]

CLANG_EXTRA_WARNINGS = ["-Wno-c2y-extensions"]

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
]
