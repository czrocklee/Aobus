"""Whole-graph lint gate: declared replay actions, one report gate, one result.

The macro applies lint_graph_aspect to the existing completion roots, so the
per-translation-unit AobusLint report actions, their native compile
contexts and the checker verification are the only analysis producers. The
gate rule adds no target inventory: every uncovered header and include
fragment without a direct main receives one replay action that selects its real
consumer from the baseline reports' include evidence at execution time. One
gate action consumes the graph's declared report files. The ordinary py_test
consumer only reads the gate's completed result and log from runfiles
through the official python.runfiles library; no generated launcher script
and no test runs Bazel or an ao command.
"""

load("@aobus_lint_sdk//:flags.bzl", "LINT_SDK_HOST")
load("@aobus_native_tools//:catalog_tools.bzl", "CATALOG_TOOLS")
load("@rules_cc//cc:find_cc_toolchain.bzl", "use_cc_toolchain")
load("@rules_python//python:defs.bzl", "py_test")
load("//bazel/lint:lint.bzl", "LintGraphInfo", "LintVerificationInfo", "lint_graph_aspect", "native_environment")

_RESULT_SUPPORT = "//:ao_lint_result_support"

def _artifact_map(ctx, field):
    mapping = {}
    for root in ctx.attr.roots:
        for artifact in getattr(root[LintGraphInfo], field).to_list():
            mapping[artifact.path] = artifact
    return mapping

def _lint_graph_gate_impl(ctx):
    verification = ctx.attr._verification[LintVerificationInfo]

    # One record per configured owner; the same owner reached through several
    # roots contributes identical record bytes, never a second inventory.
    records = {}
    for root in ctx.attr.roots:
        if LintGraphInfo not in root:
            fail("Lint graph roots must be declared with the lint graph aspect")
        for text in root[LintGraphInfo].records.to_list():
            record = json.decode(text)
            key = (record["owner"], record["configuration"])
            if key in records:
                if records[key] != record:
                    fail("Conflicting lint graph records for " + record["owner"])
                continue
            records[key] = record
    ordered = [records[key] for key in sorted(records)]

    indices = _artifact_map(ctx, "indices")
    reports = _artifact_map(ctx, "reports")
    logs = _artifact_map(ctx, "logs")
    contexts = _artifact_map(ctx, "contexts")
    uncovered_files = _artifact_map(ctx, "uncovered_files")

    # "owner|configuration" to that owner's declared compile inputs; replay
    # actions stage exactly the candidate consumers' native dependency
    # context (compiler wrapper, native SDK, first-party and generated
    # headers), never the whole graph's inputs.
    owner_inputs = {}
    for root in ctx.attr.roots:
        owner_inputs.update(root[LintGraphInfo].owner_inputs)

    # Match the gate's path-wide coverage: a source not uncovered by its own
    # owner has a declared direct report. The gate still validates that report.
    direct_mains = {}
    items = {}
    for record in ordered:
        owner_uncovered = {item["path"]: None for item in record["uncovered"]}
        for path in record["sources"]:
            if path not in owner_uncovered:
                direct_mains[path] = None
        for item in record["uncovered"]:
            entry = items.setdefault(item["path"], {"reason": item["reason"], "owners": []})
            if entry["reason"] != item["reason"]:
                fail("Conflicting uncovered reasons for " + item["path"])
            entry["owners"].append(record["owner"])

    modules = ctx.attr._modules[DefaultInfo].files
    replay_entries = []
    replay_files = []

    # Enumerate before skipping direct mains so retained replays keep their
    # output paths and cache keys. String hashes are not collision-free.
    for ordinal, path in enumerate(sorted(items)):
        if path not in uncovered_files:
            fail("Uncovered lint item has no declared file: " + path)
        if path in direct_mains:
            continue
        candidates = []
        seen = {}
        for record in ordered:
            if path not in record["closure_items"]:
                continue
            for unit in record["tus"]:
                identity = (record["owner"], record["configuration"], unit["source"])
                if identity in seen:
                    continue
                seen[identity] = None
                candidates.append({
                    "owner": record["owner"],
                    "configuration": record["configuration"],
                    "source": unit["source"],
                    "context": unit["context"],
                    "report": unit["report"],
                    "env": unit["env"],
                })
        stem = "%x" % ordinal
        prefix = ctx.label.name + ".aobus-lint-graph/" + stem
        candidates_file = ctx.actions.declare_file(prefix + ".candidates.json")
        ctx.actions.write(candidates_file, json.encode({
            "schema": 1,
            "item": path,
            "platform": LINT_SDK_HOST,
            "candidates": candidates,
        }))
        replay = ctx.actions.declare_file(prefix + ".replay.json")
        report = ctx.actions.declare_file(prefix + ".report.json")
        log = ctx.actions.declare_file(prefix + ".log")
        args = ctx.actions.args()
        args.add_all(["-I", "-S", "-B", "-X", "utf8"])
        args.add(ctx.file._driver)
        args.add_all(["replay", "--candidates", candidates_file])
        args.add_all(["--policy", verification.policy, "--sdk", verification.sdk, "--verification", verification.result, "--tidy", verification.host])
        if verification.plugin:
            args.add_all(["--plugin", verification.plugin])
        args.add_all(["--helper", ctx.file._helper, "--replay", replay, "--report", report, "--log", log])
        candidate_inputs = []
        candidate_owners = {}
        for candidate in candidates:
            if candidate["context"] not in contexts or candidate["report"] not in reports:
                fail("Lint graph replay candidate artifact is not declared: " + path)
            candidate_inputs.append(contexts[candidate["context"]])
            candidate_inputs.append(reports[candidate["report"]])
            candidate_owners[candidate["owner"] + "|" + candidate["configuration"]] = None
        compile_inputs = []
        for key in sorted(candidate_owners):
            if key not in owner_inputs:
                fail("Lint graph replay candidate has no declared compile context: " + key)
            compile_inputs.append(owner_inputs[key])
        inputs = depset(
            [candidates_file, uncovered_files[path], ctx.file._helper, ctx.file._driver, verification.result, verification.policy, verification.sdk, verification.host] + ([verification.plugin] if verification.plugin else []),
            transitive = [
                verification.inputs,
                modules,
                depset(candidate_inputs),
                depset(transitive = compile_inputs),
            ],
        )
        ctx.actions.run(
            executable = CATALOG_TOOLS["python"],
            arguments = [args],
            inputs = inputs,
            outputs = [replay, report, log],
            env = native_environment(ctx),
            mnemonic = "AobusLintGraphReplay",
            progress_message = "Replay lint consumer for " + path,
        )
        replay_entries.append({"item": path, "replay": replay.path, "report": report.path, "log": log.path})
        replay_files.extend([replay, report, log])

    inventory = ctx.actions.declare_file(ctx.label.name + ".aobus-lint-graph.inventory.json")
    ctx.actions.write(inventory, json.encode({
        "schema": 1,
        "platform": LINT_SDK_HOST,
        "owners": [
            {"owner": record["owner"], "configuration": record["configuration"], "index": record["index"]}
            for record in ordered
        ],
        "replays": replay_entries,
    }))
    result = ctx.actions.declare_file(ctx.label.name + ".aobus-lint-graph.result.json")
    gate_log = ctx.actions.declare_file(ctx.label.name + ".aobus-lint-graph.log")
    coverage = ctx.actions.declare_file(ctx.label.name + ".aobus-lint-graph.coverage.json")
    args = ctx.actions.args()
    args.add_all(["-I", "-S", "-B", "-X", "utf8"])

    # The gate entrypoint is its own module: gate-only edits never change the
    # replay actions' input closure, so cached replays survive gate fixes.
    args.add(ctx.file._gate)
    args.add_all(["gate", "--inventory", inventory, "--result", result, "--log", gate_log, "--coverage", coverage])
    gate_inputs = depset(
        [inventory, ctx.file._gate, ctx.file._driver, ctx.file._helper],
        transitive = [
            modules,
            depset(list(indices.values()) + list(reports.values()) + list(logs.values()) + replay_files),
        ],
    )
    ctx.actions.run(
        executable = CATALOG_TOOLS["python"],
        arguments = [args],
        inputs = gate_inputs,
        outputs = [result, gate_log, coverage],
        env = native_environment(ctx),
        mnemonic = "AobusLintGraphGate",
        progress_message = "Gate whole-graph lint reports",
    )
    return [
        DefaultInfo(files = depset([result, gate_log])),
        OutputGroupInfo(
            aobus_lint_result = depset([result]),
            aobus_lint_log = depset([gate_log]),
            aobus_lint_coverage = depset([coverage]),
        ),
    ]

lint_graph_gate = rule(
    implementation = _lint_graph_gate_impl,
    attrs = {
        # The existing completion roots own the reachable owner set; no second
        # inventory is declared here.
        "roots": attr.label_list(
            aspects = [lint_graph_aspect],
            default = [
                Label("//:check_products"),
                Label("//:check_cpp_tests"),
            ],
        ),
        "_driver": attr.label(default = Label("//:script/ao/graph/bazel_graph_lint.py"), allow_single_file = True),
        "_gate": attr.label(default = Label("//:script/ao/graph/bazel_graph_lint_gate.py"), allow_single_file = True),
        "_helper": attr.label(default = Label("//:script/ao/graph/bazel_lint_action.py"), allow_single_file = True),
        "_modules": attr.label(default = Label("//:ao_lint_action_sources")),
        "_verification": attr.label(default = Label("//tool/lint:verification")),
    },
    fragments = ["cpp"],
    toolchains = use_cc_toolchain(),
)

def lint_graph_test(
        name,
        roots = None,
        size = None,
        tags = [],
        timeout = None,
        visibility = None,
        testonly = None):
    """Declare the whole-graph lint gate and its official result consumer.

    Args:
        name: Test target name.
        roots: Completion roots owning the reachable lint graph; the gate's
            default roots apply when omitted.
        size: Test size forwarded to the result consumer.
        tags: Test tags forwarded to the result consumer.
        timeout: Test timeout forwarded to the result consumer.
        visibility: Test visibility forwarded to the result consumer.
        testonly: Testonly restriction forwarded to the result consumer.
    """
    gate = name + ".gate"
    gate_attributes = {"name": gate, "testonly": True}
    if roots:
        gate_attributes["roots"] = roots
    lint_graph_gate(**gate_attributes)

    # Each filegroup selects exactly one gate output, so the py_test arguments
    # expand to single runfiles keys for the official runfiles library.
    result = name + ".gate_result"
    log = name + ".gate_log"
    native.filegroup(
        name = result,
        srcs = [":" + gate],
        output_group = "aobus_lint_result",
        testonly = True,
    )
    native.filegroup(
        name = log,
        srcs = [":" + gate],
        output_group = "aobus_lint_log",
        testonly = True,
    )

    test_attributes = {
        "name": name,
        "main_module": "ao.graph.bazel_lint_result",
        "args": [
            "$(rlocationpath :%s)" % result,
            "$(rlocationpath :%s)" % log,
            LINT_SDK_HOST,
        ],
        "data": [":" + result, ":" + log],
        "deps": [_RESULT_SUPPORT],
        "tags": list(tags),
    }
    if size != None:
        test_attributes["size"] = size
    if timeout != None:
        test_attributes["timeout"] = timeout
    if visibility != None:
        test_attributes["visibility"] = visibility
    if testonly != None:
        test_attributes["testonly"] = testonly
    py_test(**test_attributes)
