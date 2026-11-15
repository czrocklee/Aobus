"""Static Clang flag values for the governed Linux toolchain and consumers.

The Linux toolchain keeps action, mode and input wiring; this module owns the
literal compile/link flag values it and Linux consumers apply. The shared
language standard and Unix warning policy stay in compiler_policy.bzl.
"""

load(":compiler_policy.bzl", "CLANG_EXTRA_WARNINGS", "CXX_STANDARD", "UNIX_WARNINGS")

CPP_STANDARD_ARGS = ["-std=c++%d" % CXX_STANDARD]

# Base arguments for every compile action, including reproducible-build
# redaction of the date/time builtin macros.
BASE_COMPILE_ARGS = [
    "-fno-omit-frame-pointer",
    "-fcolor-diagnostics",
    "-no-canonical-prefixes",
    "-Wno-builtin-macro-redefined",
    "-D__DATE__=\"redacted\"",
    "-D__TIME__=\"redacted\"",
    "-D__TIMESTAMP__=\"redacted\"",
]

# Pinned Linux diagnostics applied before the shared Unix warning policy.
EXTRA_WARNING_ARGS = [
    "-Wthread-safety",
    "-Wself-assign",
    "-Wunused-but-set-parameter",
    "-Wno-free-nonheap-object",
]

DBG_COMPILE_ARGS = ["-O0", "-g"]

OPT_COMPILE_ARGS = [
    "-O2",
    "-DNDEBUG",
    "-ffunction-sections",
    "-fdata-sections",
    "-g0",
]

# Optimized link policy paired with function/data sections.
OPT_LINK_ARGS = ["-Wl,--gc-sections"]

FASTBUILD_COMPILE_ARGS = ["-O0", "-g0"]

# Driver-managed ThinLTO release IPO. ThinLTO under the pinned LLVM 22 can
# localize vtable copies across shared objects, so exact-vptr identity is not
# admitted for Linux C++ compilation.
RELEASE_IPO_COMPILE_ARGS = ["-flto=thin", "-fno-assume-unique-vtables"]
RELEASE_IPO_LINK_ARGS = ["-flto=thin"]

# Sampling keeps the Release object policy but adds debug information and
# frame pointers without any IPO.
SAMPLING_COMPILE_ARGS = ["-O2", "-g", "-fno-omit-frame-pointer", "-fno-lto"]
SAMPLING_LINK_ARGS = ["-fno-lto"]

SANITIZER_FLAGS = {
    "asan": ["-fsanitize=address"],
    "ubsan": ["-fsanitize=undefined", "-fno-sanitize-recover=all"],
    "tsan": ["-fsanitize=thread"],
}

GCOV_ATOMIC_UPDATE_ARGS = ["-fprofile-update=atomic"]

# Two concrete compiler policies share the rule-based Nix action wiring.
POLICY = struct(
    base_compile = BASE_COMPILE_ARGS,
    cpp_standard = CPP_STANDARD_ARGS,
    warnings = EXTRA_WARNING_ARGS + UNIX_WARNINGS + CLANG_EXTRA_WARNINGS,
    debug_compile = DBG_COMPILE_ARGS,
    optimized_compile = OPT_COMPILE_ARGS,
    optimized_link = OPT_LINK_ARGS,
    fastbuild_compile = FASTBUILD_COMPILE_ARGS,
    release_compile = RELEASE_IPO_COMPILE_ARGS,
    release_link = RELEASE_IPO_LINK_ARGS,
    sampling_compile = SAMPLING_COMPILE_ARGS,
    sampling_link = SAMPLING_LINK_ARGS,
    sanitizers = SANITIZER_FLAGS,
    coverage_compile = GCOV_ATOMIC_UPDATE_ARGS,
    test_warnings = [],
)
