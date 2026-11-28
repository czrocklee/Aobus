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
load("@aobus_native_tools//:python_action_tools.bzl", "PYTHON_ACTION_TOOLS")
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

def _replay_id(path, used, kind):
    # A short, deterministic basename, never an ordinal or a raw exec path.
    key = ("0000000%x" % (hash(path) & 0xffffffff))[-8:]
    if key in used:
        fail("Lint replay %s ID %s is shared by %s and %s" % (kind, key, used[key], path))
    used[key] = path
    return key

def _expand_replay(template_ctx, *, input_directories, output_directories, additional_inputs, tools, additional_params):
    # The selector publishes exactly one marker. Its name binds the analysis
    # to a declared native action; its contents bind the same context in Python.
    markers = input_directories["route"].children
    if len(markers) != 1:
        fail("Expected one lint replay selection for " + additional_params["item"])
    marker = markers[0]
    route = marker.tree_relative_path
    directory = output_directories["result"]
    replay = template_ctx.declare_file("replay.json", directory = directory)
    report = template_ctx.declare_file("report.json", directory = directory)
    log = template_ctx.declare_file("log", directory = directory)
    args = template_ctx.args()

    # Start the outer replay with the same isolated base interpreter as Select.
    args.add_all([additional_params["python"], "-I", "-S", "-B", "-X", "utf8", additional_inputs["driver"], "replay", "--selection", marker])
    args.add_all(["--replay", replay, "--report", report, "--log", log])
    inputs = [additional_inputs["python_modules"], additional_inputs["python_runtime"]]
    if route != "none":
        key = "native_" + route
        if key not in additional_inputs:
            fail("Unknown lint replay selection: " + route)
        inputs.extend([additional_inputs[key], additional_inputs["analysis"]])
        args.add_all(["--context", additional_inputs["context_" + route], "--driver", additional_inputs["driver"]])
        for flag in ["policy", "sdk", "verification", "tidy", "plugin"]:
            if flag in additional_inputs:
                args.add_all(["--" + flag, additional_inputs[flag]])
    template_ctx.run(
        executable = tools["runner"],
        arguments = [args],
        inputs = depset([marker, additional_inputs["driver"]], transitive = inputs),
        outputs = [replay, report, log],
        progress_message = "Replay lint consumer for " + additional_params["item"],
    )

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

    compile_inputs = {}
    for root in ctx.attr.roots:
        compile_inputs.update(root[LintGraphInfo].compile_inputs)

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

    modules = depset([ctx.file._helper], transitive = [ctx.attr._modules[DefaultInfo].files])
    routing_modules = depset([ctx.file._ownership], transitive = [modules])
    python = ctx.attr._python_tools[DefaultInfo].files
    environment = native_environment(ctx)
    replay_entries = []
    replay_files = []

    item_ids = {}
    for path in sorted(items):
        if path not in uncovered_files:
            fail("Uncovered lint item has no declared file: " + path)
        if path in direct_mains:
            continue
        candidates = []
        route_ids = {}
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
                    "route": _replay_id(unit["context"], route_ids, "consumer context"),
                })
        stem = _replay_id(path, item_ids, "item")

        # Offset both eight-character IDs with a short parent directory so
        # even the selected marker path is no longer on Windows than before.
        prefix = ctx.label.name + ".lr/" + stem
        candidates_file = ctx.actions.declare_file(prefix + ".candidates.json")
        ctx.actions.write(candidates_file, json.encode({
            "schema": 1,
            "item": path,
            "platform": LINT_SDK_HOST,
            "candidates": candidates,
        }))
        route = ctx.actions.declare_directory(prefix + ".route")
        result_dir = ctx.actions.declare_directory(prefix + ".result")
        available = {
            "driver": ctx.file._driver,
            "python_modules": modules,
            "python_runtime": python,
            "analysis": depset([uncovered_files[path], verification.result], transitive = [verification.inputs]),
            "policy": verification.policy,
            "sdk": verification.sdk,
            "verification": verification.result,
            "tidy": verification.host,
        }
        if verification.plugin:
            available["plugin"] = verification.plugin
        candidate_reports = []
        for candidate in candidates:
            context = candidate["context"]
            key = candidate["route"]
            available["context_" + key] = contexts[context]
            available["native_" + key] = compile_inputs[context]
            candidate_reports.append(reports[candidate["report"]])
        args = ctx.actions.args()
        args.add_all(["-I", "-S", "-B", "-X", "utf8", ctx.file._driver, "select", "--candidates", candidates_file, "--routes", route.path])
        ctx.actions.run(
            executable = PYTHON_ACTION_TOOLS["python"],
            arguments = [args],
            inputs = depset([candidates_file, ctx.file._driver] + candidate_reports, transitive = [routing_modules, python]),
            outputs = [route],
            env = environment,
            mnemonic = "AobusLintSelect",
            progress_message = "Select lint consumer for " + path,
        )
        ctx.actions.map_directory(
            input_directories = {"route": route},
            output_directories = {"result": result_dir},
            additional_inputs = available,
            tools = {"runner": ctx.attr._replay_tool[DefaultInfo].files_to_run},
            additional_params = {"item": path, "python": PYTHON_ACTION_TOOLS["python"]},
            implementation = _expand_replay,
            env = environment,
            mnemonic = "AobusLintGraphReplay",
        )
        replay_entries.append({"item": path, "replay": result_dir.path + "/replay.json", "report": result_dir.path + "/report.json", "log": result_dir.path + "/log"})
        replay_files.append(result_dir)

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
        [inventory, ctx.file._gate, ctx.file._dedup, ctx.file._driver],
        transitive = [
            routing_modules,
            python,
            depset(list(indices.values()) + list(reports.values()) + list(logs.values()) + replay_files),
        ],
    )
    ctx.actions.run(
        executable = PYTHON_ACTION_TOOLS["python"],
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
        "_replay_tool": attr.label(default = Label("//tool/lint:replay_launcher"), executable = True, cfg = "exec"),
        "_dedup": attr.label(default = Label("//:script/ao/policy/dedup.py"), allow_single_file = True),
        "_gate": attr.label(default = Label("//:script/ao/graph/bazel_graph_lint_gate.py"), allow_single_file = True),
        "_ownership": attr.label(default = Label("//:script/ao/policy/lint_ownership.py"), allow_single_file = True),
        "_helper": attr.label(default = Label("//:script/ao/graph/bazel_lint_action.py"), allow_single_file = True),
        "_modules": attr.label(default = Label("//:ao_lint_action_sources")),
        "_python_tools": attr.label(default = Label("@aobus_native_tools//:python_action_tools"), cfg = "exec"),
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
