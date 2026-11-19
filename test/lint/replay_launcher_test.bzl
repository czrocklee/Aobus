"""Run the replay launcher with native test execution in every build profile."""

load("@rules_cc//cc:find_cc_toolchain.bzl", "use_cc_toolchain")
load("@rules_python//python/api:executables.bzl", "executables")

# Like native_context_test, require a native C++ execution platform instead of
# Bazel's default all-target-constraints test toolchain. Keep rules_python's
# bootstrap/runfiles behavior and the actual replay launcher in target config.
_builder = executables.py_test_rule_builder()
_builder.exec_groups()["test"] = exec_group(toolchains = use_cc_toolchain())
replay_launcher_py_test = _builder.build()
