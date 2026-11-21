"""Analyze native compile contracts without building or executing products."""

import sys

from ao.core import bazel_native
from ao.core.build_lock import build_tree_lock
from ao.portal import bazel_sanitizers, bazel_verify, bazel_workflows, bazel_workspace


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
        # Sanitizer selection analyzes Debug contracts, not instrumentation.
        # Only the managed sanitizer gate proves the latter.
        return bazel_verify.verify(
            bazel_workspace.bazel_executable(),
            tree,
            prepared,
            "debug" if config in bazel_sanitizers.SANITIZER_CONFIGS else config,
            jobs=request.jobs,
            isolate_rc=True,
            compiler_include_roots=bazel_sanitizers.compiler_include_roots(
                config, host, compiler=bazel_workflows.selected_compiler(request)
            ),
        )
