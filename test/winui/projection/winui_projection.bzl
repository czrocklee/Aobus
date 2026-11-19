"""Assert the generated WinUI compile interface and its private edge in analysis."""

load("@rules_cc//cc:find_cc_toolchain.bzl", "use_cc_toolchain")
load("//bazel:compile_context.bzl", "ValidationFactsInfo", "validation_facts_aspect")
load("//bazel:winui_outputs.bzl", "WinuiGeneratedInfo")

WINDOWS = ["@platforms//os:windows"]

_MSVC_INCLUDE_PREFIXES = (
    ("/external:I", "external"),
    ("-external:I", "external"),
    ("/I", "ordinary"),
    ("-I", "ordinary"),
)

def _windows_path(path):
    return path.replace("\\", "/").lower()

def _include_classes(argv, roots):
    classes = {root: {} for root in roots}
    separate_path_kind = None
    for arg in argv:
        if separate_path_kind != None:
            path = _windows_path(arg)
            if path in classes:
                classes[path][separate_path_kind] = True
            separate_path_kind = None
            continue
        for prefix, kind in _MSVC_INCLUDE_PREFIXES:
            if arg.startswith(prefix):
                path = _windows_path(arg[len(prefix):])
                if path in classes:
                    classes[path][kind] = True
                elif not path:
                    separate_path_kind = kind
                break
    return [sorted(classes[root]) for root in roots]

def _projection_test_impl(ctx):
    generated = ctx.attr.generated[WinuiGeneratedInfo]
    pair = (generated.headers.path, generated.sdk_headers.path)
    roots = [_windows_path(path) for path in pair]
    if roots[0] == roots[1]:
        return [AnalysisTestResultInfo(success = False, message = "WinUI projection roots must be distinct")]

    # Read the production fold once, but only assert on owned target rows. The
    # downstream fold must also carry its private consumer's positive record.
    checks = [
        (ctx.attr.public_consumer, True, True),
        (ctx.attr.private_consumer, False, True),
        (ctx.attr.downstream, False, False),
    ]
    owned = {str(target.label): [] for target, _, _ in checks}
    for entry in ctx.attr.downstream[ValidationFactsInfo].records.to_list():
        record = json.decode(entry)
        if record["label"] in owned and record["configuration"] == ctx.bin_dir.path and not record["is_tool"]:
            owned[record["label"]].append(record)

    errors = []
    for target, exports, consumes in checks:
        label = str(target.label)
        expected_export = [pair] if exports else []
        actual_export = target[ValidationFactsInfo].winui_projections.to_list()
        if actual_export != expected_export:
            errors.append("%s: expected exported projection %s, got %s" % (label, expected_export, actual_export))

        records = owned[label]
        if len(records) != 1 or not records[0]["compiles"]:
            errors.append("%s: expected one owned target record with CppCompile rows" % label)
            continue
        record = records[0]
        expected_pairs = [list(pair)] if consumes else []
        if record["winui_projections"] != expected_pairs:
            errors.append("%s: expected compile projection %s, got %s" % (label, expected_pairs, record["winui_projections"]))

        expected_classes = [["ordinary"], ["external"]] if consumes else [[], []]
        for index, compile in enumerate(record["compiles"]):
            actual_classes = _include_classes(compile["argv"], roots)
            if actual_classes != expected_classes:
                errors.append("%s compile %d: expected project/SDK include classes %s, got %s" % (label, index, expected_classes, actual_classes))

    return [AnalysisTestResultInfo(success = not errors, message = "; ".join(errors) if errors else "WinUI projection edges preserve the compile interface")]

winui_projection_test = rule(
    implementation = _projection_test_impl,
    analysis_test = True,
    test = True,
    attrs = {
        "downstream": attr.label(aspects = [validation_facts_aspect], mandatory = True),
        "generated": attr.label(providers = [WinuiGeneratedInfo], mandatory = True),
        "private_consumer": attr.label(aspects = [validation_facts_aspect], mandatory = True),
        "public_consumer": attr.label(aspects = [validation_facts_aspect], mandatory = True),
    },
    # Release's profile marker belongs to the target graph, not the native
    # execution platform needed to run Bazel's generated analysis-test script.
    exec_groups = {"test": exec_group(toolchains = use_cc_toolchain(), exec_compatible_with = WINDOWS)},
)

def _downstream_source_impl(ctx):
    output = ctx.actions.declare_file(ctx.label.name + ".cpp")
    ctx.actions.write(output, "static_assert(true);\n")
    return [DefaultInfo(files = depset([output]))]

downstream_source = rule(implementation = _downstream_source_impl)
