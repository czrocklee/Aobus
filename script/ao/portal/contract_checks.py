"""Analyze native compile contracts without building or executing products."""

import sys

from ao.core import bazel_native
from ao.core.build_lock import build_tree_lock
from ao.portal import bazel_compiler_probe, bazel_profiles, bazel_verify, bazel_workflows, bazel_workspace


def check(request: bazel_workflows.NativeRequest) -> int:
    config, host = bazel_workflows.validate_request(request)
    tree = bazel_workspace.workspace_path(request.path, config)
    with build_tree_lock(tree):
        bazel_native.require_supported_source()
        tree.mkdir(parents=True, exist_ok=True)
        prepared = bazel_workspace.prepare_workspace(
            tree, config, prefix=request.vcpkg_prefix, graph_options=bazel_workflows.feature_options(request)
        )
        print(f"Prepared Bazel configuration: {prepared}", file=sys.stderr)
        # Linux TSan uses the selected compiler's interface for libstdc++ lock annotations.
        roots: tuple[str, ...] = ()
        if config == "tsan" and host == "linux":
            wrapper = bazel_compiler_probe.nix_cc_wrapper(bazel_workflows.selected_compiler(request))
            roots = (str(wrapper["sanitizerIncludeDirectory"]),)
        return bazel_verify.verify(
            bazel_workspace.bazel_executable(),
            tree,
            prepared,
            "debug" if config in bazel_profiles.SANITIZER_CONFIGS else config,
            jobs=request.jobs,
            isolate_rc=True,
            compiler_include_roots=roots,
        )
