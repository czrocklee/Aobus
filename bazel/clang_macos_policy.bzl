"""Static Clang flag values for the governed macOS toolchain composition.

The macOS toolchain keeps Apple mechanics, action coverage and input wiring;
this module owns the literal compile/link flag values it applies. The shared
language standard and Unix warning policy stay in compiler_policy.bzl.
"""

load(":compiler_policy.bzl", "CXX_STANDARD")

# C++ compilation policy: the project standard plus the SDK libc++ ABI.
CPP_POLICY_ARGS = ["-std=c++%d" % CXX_STANDARD, "-stdlib=libc++"]

# Standard-library selection for the final link actions.
LINK_STDLIB_ARGS = ["-stdlib=libc++"]

# Bootstrap the apple_support wrapper with the same language requirement as
# the other Clang translation units; retain its native library/mode defaults.
COMPILER_ENTRY_FLAGS = " ".join([
    "--no-default-config",
    "-std=c++%d" % CXX_STANDARD,
    "-stdlib=libc++",
    "-O2",
])

# Driver-managed ThinLTO release IPO: C++ objects and final links only.
RELEASE_IPO_COMPILE_ARGS = ["-flto=thin"]
RELEASE_IPO_LINK_ARGS = ["-flto=thin"]

# Sampling covers C++ and Objective-C++ objects with Release optimization
# plus effective debug information and frame pointers, and pins final links
# against any IPO.
SAMPLING_COMPILE_ARGS = ["-O2", "-g", "-fno-omit-frame-pointer", "-fno-lto"]
SAMPLING_LINK_ARGS = ["-fno-lto"]
