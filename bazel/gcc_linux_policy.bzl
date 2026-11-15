"""GCC policy for the native Linux toolchain, independent of Clang defaults."""

load(":compiler_policy.bzl", "CXX_STANDARD", "UNIX_WARNINGS")

BASE_COMPILE_ARGS = [
    "-fno-omit-frame-pointer",
    "-fdiagnostics-color=always",
    # Keep declared system-header symlinks in dependency output, not their
    # absolute store/output-base destinations.
    "-fno-canonical-system-headers",
    "-no-canonical-prefixes",
    "-Wno-builtin-macro-redefined",
    "-D__DATE__=\"redacted\"",
    "-D__TIME__=\"redacted\"",
    "-D__TIMESTAMP__=\"redacted\"",
]
CPP_STANDARD_ARGS = ["-std=c++%d" % CXX_STANDARD]
DBG_COMPILE_ARGS = ["-O0", "-g"]
OPT_COMPILE_ARGS = ["-O2", "-DNDEBUG", "-ffunction-sections", "-fdata-sections", "-g0"]
OPT_LINK_ARGS = ["-Wl,--gc-sections"]
FASTBUILD_COMPILE_ARGS = ["-O0", "-g0"]

# Driver-managed GCC LTO, with plugin-aware gcc-ar for non-fat objects. This
# does not enable Bazel's separate ThinLTO index/backend action pipeline.
RELEASE_IPO_COMPILE_ARGS = ["-flto=auto", "-fno-fat-lto-objects"]
RELEASE_IPO_LINK_ARGS = ["-flto=auto"]

# The compile-time -Werror never reaches LTRANS; only the link driver
# propagates its own -Werror to the link-time optimizer, so late LTRANS
# warnings fail the Release link instead of passing silently. The toolchain
# wires these as the GCC-only release_ipo_warnings link args under the repo
# warning feature, so external repositories that opt out keep their own policy.
GCC_RELEASE_LINK_WARNING_ARGS = ["-Werror"]
SAMPLING_COMPILE_ARGS = ["-O2", "-g", "-fno-omit-frame-pointer", "-fno-lto"]
SAMPLING_LINK_ARGS = ["-fno-lto"]
SANITIZER_FLAGS = {
    "asan": ["-fsanitize=address"],
    "ubsan": ["-fsanitize=undefined", "-fno-sanitize-recover=all"],
    "tsan": ["-fsanitize=thread"],
}
GCOV_COMPILE_ARGS = ["-fprofile-update=atomic", "-fcanon-prefix-map"]

# GCC preserves ../ include spellings in notes, but the stock merger filters
# exact manifest names. Map canonical authored subroots back to those names
# before that filter, including sparse sandboxes with individual file symlinks.
# Profile-only maps leave runtime __FILE__ strings and line counts unchanged.
GCOV_SOURCE_ROOTS = ["app", "lib", "include"]
GCOV_SOURCE_MAP_ARG = "-fprofile-prefix-map=%s/%s=%s"

# Preserve the pre-Bazel version-scoped compatibility contracts. GCC 16 cannot
# serialize Boost.PFR's C++26 structured-binding packs into LTO objects.
GCC16_PFR_COPTS = ["-DBOOST_PFR_USE_CPP26=0"]
GCC15_TEST_COPTS = ["-Wno-mismatched-new-delete"]

POLICY = struct(
    base_compile = BASE_COMPILE_ARGS,
    cpp_standard = CPP_STANDARD_ARGS,
    warnings = UNIX_WARNINGS,
    debug_compile = DBG_COMPILE_ARGS,
    optimized_compile = OPT_COMPILE_ARGS,
    optimized_link = OPT_LINK_ARGS,
    fastbuild_compile = FASTBUILD_COMPILE_ARGS,
    release_compile = RELEASE_IPO_COMPILE_ARGS,
    release_link = RELEASE_IPO_LINK_ARGS,
    sampling_compile = SAMPLING_COMPILE_ARGS,
    sampling_link = SAMPLING_LINK_ARGS,
    sanitizers = SANITIZER_FLAGS,
    coverage_compile = GCOV_COMPILE_ARGS,
    test_warnings = GCC15_TEST_COPTS,
)
