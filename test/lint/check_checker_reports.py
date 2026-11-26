"""Exercise scoped checker forwarding against the native Debug build. Run outside Bazel."""

import json
import os
import re
import subprocess
import sys
from pathlib import Path

from ao.core import jobs
from ao.core.build_lock import build_tree_lock
from ao.graph.bazel_lint_reports import CHECKER_VERIFICATION, Report, main_output_group, read_reports
from ao.portal import bazel_workspace

CHECKER = "tool/lint/check/LocalInitializationStyleCheck.cpp"
HEADER = CHECKER.removesuffix(".cpp") + ".h"
PRODUCT = "lib/utility/Base64.cpp"
UTILITY = "//lib/utility:utility"
ASPECT = "//bazel/lint:lint.bzl%lint_aspect"


def check(tree: Path) -> None:
    windows = os.name == "nt"
    plugin = "libAobusLintPlugin.dylib" if sys.platform == "darwin" else "libAobusLintPlugin.so"
    checker = "//tool/lint:" + ("AobusClangTidy" if windows else plugin)
    bazel = bazel_workspace.invocation(bazel_workspace.bazel_executable(), tree / "configuration", isolate_rc=True)
    options = ["--config=debug", "--lockfile_mode=error", f"--jobs={jobs.default_workers()}"]
    if cache := os.environ.get("AOBUS_BAZEL_REMOTE_CACHE"):
        options.append("--remote_cache=" + bazel_workspace.validate_remote_cache(cache))
    env = bazel_workspace.launch_environment(isolate_rc=True)
    groups = ["aobus_lint_index"]
    for path in (CHECKER, HEADER, PRODUCT):
        group = main_output_group(path, windows=windows)
        if not group:
            raise ValueError(f"No per-main group for {path}")
        groups.append(group)
    bep = tree / "checker-reports.bep.json"
    parameters = "--aspects_parameters=aobus_lint_consumers={}"
    subprocess.run(
        [
            *bazel,
            "build",
            *options,
            f"--aspects={ASPECT}",
            parameters,
            "--output_groups=" + ",".join(groups),
            "--experimental_build_event_upload_strategy=local",
            "--remote_download_outputs=all",
            "--norun_validations",
            f"--build_event_json_file={bep}",
            CHECKER_VERIFICATION,
            UTILITY,
        ],
        env=env,
        check=True,
    )
    # read_reports binds the delivered checker index to the tool in this build's verification.json.
    inventories = read_reports(bep, [checker, UTILITY], checker_owner=checker, output_groups=groups)
    reports = [r for owner in inventories for r in owner.reports]
    pairs = {(r.identity.main, r.identity.source) for r in reports}
    if pairs != {(CHECKER, CHECKER), (HEADER, CHECKER), (PRODUCT, PRODUCT)} or any(
        r.outcome == "compiler_error" for r in reports
    ):
        raise ValueError(f"Expected complete analyses of only the three selected mains; got {pairs}")
    product_config = inventories[1].bep_configuration
    with bep.open(encoding="utf-8") as stream:
        modes = [
            event["configuration"]["makeVariable"]["COMPILATION_MODE"]
            for line in stream
            if (event := json.loads(line))["id"].get("configuration") == {"id": product_config}
        ]
    if modes != ["dbg"]:
        raise ValueError(f"Product reports did not use the requested Debug configuration: {modes}")
    check_compile_actions(bazel, options, env, parameters, reports, inventories[0].execroot)


def check_compile_actions(
    bazel: list[str], options: list[str], env: dict[str, str], parameters: str, reports: list[Report], execroot: Path
) -> None:
    # Read the actual lint action inputs even on cache hits. Independently capture CppCompile
    # argv/outputs from the verifier's dependencies and the ordinary product target; MSVC's
    # native aquery command line alone may contain only a response-file reference.
    stems = "|".join(re.escape(r.report.name.removesuffix(".report.json")) for r in reports)
    outputs = f".*([.]aobus-compile-context|/({stems})[.](report|context))[.]json"
    targets = f'filter("(tool/lint:|lib/utility:utility$)", deps(set({CHECKER_VERIFICATION} {UTILITY})))'
    query = f'outputs("{outputs}", {targets})'
    graph = json.loads(
        subprocess.check_output(
            [
                *bazel,
                "aquery",
                *options,
                f"--aspects={ASPECT},//bazel:compile_context.bzl%compile_context_aspect",
                parameters,
                "--include_aspects",
                "--include_file_write_contents",
                "--include_artifacts",
                "--output=jsonproto",
                query,
            ],
            env=env,
        )
    )
    native: dict[str, dict[tuple[str, ...], list[str]]] = {CHECKER: {}, PRODUCT: {}}
    fragments = {f["id"]: f for f in graph["pathFragments"]}
    artifacts = {a["id"]: a["pathFragmentId"] for a in graph["artifacts"]}

    def output_path(output: int) -> str:
        parts = []
        fragment = artifacts[output]
        while fragment:
            entry = fragments[fragment]
            parts.append(entry["label"])
            fragment = entry.get("parentId")
        return "/".join(reversed(parts))

    writers = {}
    inputs = {}
    for action in graph["actions"]:
        if action["mnemonic"] == "AobusLint":
            argv = action["arguments"]
            inputs[argv[argv.index("--report") + 1]] = argv[argv.index("--context") + 1]
            continue
        context = json.loads(action["fileContents"])
        if "main" in context:
            writers[output_path(action["primaryOutputId"])] = context
        else:
            for compile_action in context["compile_argv"]:
                for source, contexts in native.items():
                    if source in compile_action["argv"]:
                        contexts[tuple(compile_action["outputs"])] = compile_action["argv"]
    for report in reports:
        main, source, objects = report.identity.main, report.identity.source, report.identity.outputs
        context = writers[inputs[report.report.relative_to(execroot).as_posix()]]
        if (context["main"], context["source"], tuple(context["outputs"])) != (main, source, objects):
            raise ValueError(f"Lint action consumed a different native context for {main}")
        argv = context["argv"]
        if argv != native[source].get(objects):
            raise ValueError(f"{main}: lint argv differs from native compile of {objects}")
        if os.name == "nt":
            flags = set(argv) & {"/MT", "/MTd", "/MD", "/MDd", "/O2", "/Od", "/DNDEBUG"}
            expected = {"/MT", "/O2", "/DNDEBUG"}
            if main == PRODUCT:
                flags.update(set(argv) & {"/Z7"})
                expected = {"/MDd", "/Od", "/Z7"}
            if flags != expected:
                raise ValueError(f"{main}: expected {expected}; got {flags}")
            print(f"{main}: {' '.join(sorted(flags))}")
        print(f"{main}: matched native compile {objects}")
    # A Linux checker may have both PIC and non-PIC compiles. Neither may disappear.
    for main, source in ((CHECKER, CHECKER), (HEADER, CHECKER), (PRODUCT, PRODUCT)):
        delivered = {r.identity.outputs for r in reports if r.identity.main == main}
        if delivered != native[source].keys():
            raise ValueError(f"{main}: expected native {source} outputs {list(native[source])}; got {delivered}")


if __name__ == "__main__":
    tree = bazel_workspace.workspace_path(sys.argv[1], "debug")
    with build_tree_lock(tree):
        check(tree)
