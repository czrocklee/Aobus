"""Native compile contexts and cacheable diagnostic-only reports, without product builds."""

load("@aobus_lint_sdk//:flags.bzl", "LINT_SDK_HOST")
load("@aobus_native_tools//:catalog_tools.bzl", "CATALOG_TOOLS")
load("@rules_cc//cc:find_cc_toolchain.bzl", "find_cc_toolchain", "use_cc_toolchain")
load("@rules_cc//cc/common:cc_common.bzl", "cc_common")
load("@rules_cc//cc/common:cc_info.bzl", "CcInfo")

LintVerificationInfo = provider(doc = "Checker registry verification outputs and native inputs reused by lint reports.", fields = ["result", "log", "inputs", "host", "plugin", "policy", "sdk"])
HeaderOriginInfo = provider(
    doc = "Checkout headers behind the virtual include symlinks of a compile closure.",
    fields = {"origins": "depset of newline-joined virtual and original exec paths"},
)

_SOURCE_SUFFIXES = ["c", "cc", "cpp", "cxx", "mm"]
_HEADER_SUFFIXES = ["h", "hh", "hpp", "hxx"]
_COMPILE_MNEMONICS = ["CppCompile", "ObjcCompile"]

# Only profiles whose producer inputs are known here are admitted. The portal
# does not forward arbitrary feature/copt overrides through its lint interface.
_SUPPORTED_FEATURES = ["aobus_warnings", "treat_warnings_as_errors", "external_include_paths", "static_link_msvcrt", "dynamic_link_msvcrt", "default_compile_flags", "aobus_release_ipo", "aobus_sampling"]

def native_environment(ctx, action = None, language = "c++", portal = False):
    """Use the pinned producer's feature inputs and native environment precedence.

    Only an action that imports the whole portal (the fixture suite) receives
    the Windows profile; report actions keep it out of their cache keys.

    Args:
        ctx: Rule context supplying the C++ toolchain, feature profile, and configuration.
        action: Native compile action whose environment is the base, or None for shell defaults.
        language: Compile language used to select toolchain features and action environment.
        portal: Include the selected Windows Python profile for actions importing the portal.

    Returns:
        Environment dictionary for the native diagnostic action.
    """
    for feature in ctx.features + ctx.disabled_features:
        if feature not in _SUPPORTED_FEATURES:
            fail("Unadmitted lint feature profile: " + feature)
    toolchain = find_cc_toolchain(ctx)
    features = cc_common.configure_features(
        ctx = ctx,
        cc_toolchain = toolchain,
        language = language,
        requested_features = ctx.features,
        unsupported_features = ctx.disabled_features,
    )
    variables = cc_common.create_compile_variables(cc_toolchain = toolchain, feature_configuration = features)
    action_name = "objc++-compile" if language == "objc++" else "c-compile" if language == "c" else "c++-compile"
    environment = dict(action.env if action != None else ctx.configuration.default_shell_env)
    if action != None:
        # CppCompileAction injects PWD by execution requirements, not host OS;
        # its action.env exposes only the fixed action environment.
        if "requires-darwin" not in cc_common.get_execution_requirements(feature_configuration = features, action_name = action_name):
            environment["PWD"] = "/proc/self/cwd"
    elif LINT_SDK_HOST == "linux":
        environment["PWD"] = "/proc/self/cwd"
    environment.update(cc_common.get_environment_variables(
        feature_configuration = features,
        action_name = action_name,
        variables = variables,
    ))
    environment["PYTHONDONTWRITEBYTECODE"] = "1"
    if LINT_SDK_HOST == "windows" and portal:
        # Portal modules resolve compatibility profile paths when imported.
        # Use the selected Python SDK's observed profile, not ambient inheritance.
        if not CATALOG_TOOLS.get("UserProfile"):
            fail("Native lint Python requires the selected Windows USERPROFILE")
        environment["USERPROFILE"] = CATALOG_TOOLS["UserProfile"]
    elif LINT_SDK_HOST == "windows":
        environment.pop("USERPROFILE", None)
    if LINT_SDK_HOST != "windows":
        environment["LC_ALL"] = "C.UTF-8"
    return environment

def _arguments(ctx, command):
    args = ctx.actions.args()
    args.add_all(["-I", "-S", "-B", "-X", "utf8"])
    args.add(ctx.file._helper)
    args.add(command)
    return args

def _verification_impl(ctx):
    if len(ctx.files._host) != 1 or len(ctx.files._plugin) > 1:
        fail("Lint requires one native host and at most one plugin")
    host = ctx.files._host[0]
    plugin = ctx.files._plugin[0] if ctx.files._plugin else None
    result = ctx.actions.declare_file(ctx.label.name + ".json")
    log = ctx.actions.declare_file(ctx.label.name + ".log")
    inputs = depset(
        [ctx.file._helper, ctx.file._policy, ctx.file._sdk, host] + ([plugin] if plugin else []),
        transitive = [ctx.attr._modules[DefaultInfo].files, ctx.attr._tools[DefaultInfo].files, ctx.attr._native_tools[DefaultInfo].files],
    )
    args = _arguments(ctx, "verify")
    args.add_all(["--policy", ctx.file._policy, "--sdk", ctx.file._sdk, "--tidy", host])
    if plugin:
        args.add_all(["--plugin", plugin])
    args.add_all(["--output", result, "--log", log])
    ctx.actions.run(
        executable = CATALOG_TOOLS["python"],
        arguments = [args],
        inputs = inputs,
        outputs = [result, log],
        env = native_environment(ctx),
        mnemonic = "AobusLintVerify",
        progress_message = "Verify native Aobus checker registry",
    )
    return [
        DefaultInfo(files = depset([result])),
        LintVerificationInfo(result = result, log = log, inputs = inputs, host = host, plugin = plugin, policy = ctx.file._policy, sdk = ctx.file._sdk),
    ]

lint_verification = rule(
    implementation = _verification_impl,
    attrs = {
        "_helper": attr.label(default = Label("//:script/ao/core/bazel_lint_action.py"), allow_single_file = True),
        "_policy": attr.label(default = Label("@aobus_lint_policy//:policy.json"), allow_single_file = True),
        "_sdk": attr.label(default = Label("@aobus_lint_sdk//:metadata.json"), allow_single_file = True),
        "_host": attr.label(default = Label("//tool/lint:host"), cfg = "exec"),
        "_plugin": attr.label(default = Label("//tool/lint:plugin"), cfg = "exec"),
        "_tools": attr.label(default = Label("//tool/lint:tools"), cfg = "exec"),
        "_modules": attr.label(default = Label("//:ao_lint_action_sources")),
        "_native_tools": attr.label(default = Label("@aobus_native_tools//:catalog_tools"), cfg = "exec"),
    },
    fragments = ["cpp"],
    toolchains = use_cc_toolchain(),
)

def _header_inputs(target, ctx):
    groups = [target[CcInfo].compilation_context.headers] if CcInfo in target else []
    own = []
    own.extend([f for f in getattr(ctx.rule.files, "srcs", []) if f.extension not in _SOURCE_SUFFIXES])
    for attribute in ["hdrs", "textual_hdrs", "additional_compiler_inputs"]:
        own.extend(getattr(ctx.rule.files, attribute, []))
    for attribute in ["deps", "implementation_deps"]:
        for dependency in getattr(ctx.rule.attr, attribute, []):
            if CcInfo in dependency:
                groups.append(dependency[CcInfo].compilation_context.headers)

                # A header-only direct dependency is an approved companion
                # interface; an implementation dependency's compiled sources
                # do not become this owner's lint scope.
                if not [a for a in dependency.actions if a.mnemonic in _COMPILE_MNEMONICS]:
                    own.extend(dependency[CcInfo].compilation_context.headers.to_list())
    headers = {}
    for file in own:
        if file.is_source and not file.short_path.startswith("../") and file.extension in _HEADER_SUFFIXES:
            headers[file.path] = file
    return depset(own, transitive = groups), headers

def _component(file):
    # Mirrors tidyengine.component_parts, which selects Bazel owners and pairs
    # CMake headers; a tooling test compares both over every tracked file.
    parts = file.short_path.split("/")[:-1]
    if parts[:2] == ["include", "ao"]:
        return "/".join(["lib"] + parts[2:])
    if parts[:4] == ["app", "include", "ao", "rt"]:
        return "/".join(["app", "runtime"] + parts[4:])
    if parts[:3] == ["app", "include", "ao"]:
        return "/".join(["app"] + parts[3:])
    if len(parts) >= 5 and parts[0] == "app" and parts[2:4] == ["include", "ao"]:
        return "/".join(["app", parts[1]] + parts[5:])
    if parts and parts[0] in ["app", "lib", "test", "tool"]:
        return "/".join(parts)
    return None

def _paired(header, source):
    component = _component(header)
    if component == None or component != _component(source):
        return False
    stem = header.basename.rpartition(".")[0].lower()
    candidate = source.basename.rpartition(".")[0].lower()
    return candidate in [stem + suffix for suffix in ["", "linux", "macos", "posix", "windows"]]

def _decode_consumers(selection):
    """Decode the portal's whole consumer selection before any owner lookup.

    Every owner's entries validate, including owners this analysis did not
    request, so a malformed selection fails here instead of binding a wrong
    native context elsewhere. Invalid JSON fails the analysis in decode.
    """
    consumers = json.decode(selection)
    if type(consumers) != "dict":
        fail("Invalid native lint consumer selection: %s" % selection)
    for owner, entries in consumers.items():
        if type(owner) != "string" or type(entries) != "list":
            fail("Invalid native lint consumer owner: %s" % owner)
        for entry in entries:
            if type(entry) != "list" or len(entry) != 2 or type(entry[0]) != "string" or type(entry[1]) != "string":
                fail("Invalid native lint consumer entry for %s" % owner)
    return consumers

def _consumer_mains(label, selected, found, headers, pairs):
    """Bind the portal's consumers to this owner's sources and checkout headers.

    The portal names a real consumer for a selected header or include fragment
    without a companion, from the includes a first report pass recorded. The
    main is then analyzed with that consumer's native context.
    """
    consumers = {}
    if selected:
        closure = {f.path: f for f in headers.to_list() if f.is_source}
        for source_path, main_path in selected:
            if source_path not in found or main_path not in closure or main_path in pairs or main_path in consumers:
                fail("Invalid native lint consumer %s for %s in %s" % (source_path, main_path, label))
            consumers[main_path] = (source_path, closure[main_path])
    return consumers

def _action_key(action):
    return sorted([file.path for file in action.outputs.to_list()])[0]

def _header_origin_impl(target, ctx):
    """Bind virtual headers to their native symlink action's exact source input.

    Include prefixes and Windows shortened directories do not identify the
    original header. The producing action does; generated originals stay
    unmapped, as in the CMake routes.
    """
    transitive = []
    for attribute in ["deps", "implementation_deps"]:
        transitive.extend([d[HeaderOriginInfo].origins for d in getattr(ctx.rule.attr, attribute, []) if HeaderOriginInfo in d])
    origins = []
    if CcInfo in target and not ctx.label.repo_name:
        context = target[CcInfo].compilation_context
        declared = {f.path: f for attribute in ["hdrs", "textual_hdrs"] for f in getattr(ctx.rule.files, attribute, [])}
        virtuals = {f.path: f for f in context.direct_public_headers + context.direct_textual_headers if not f.is_source and "/_virtual_includes/" in f.path}
        producers = {}
        for action in target.actions:
            outputs = [f for f in action.outputs.to_list() if f.path in virtuals]
            if not outputs:
                continue
            inputs = action.inputs.to_list()
            if action.mnemonic != "Symlink" or len(outputs) != 1 or len(inputs) != 1 or inputs[0].path not in declared:
                fail("Unsupported virtual include producer in %s" % ctx.label)
            virtual = outputs[0].path
            if virtual in producers:
                fail("Duplicate virtual include producer for %s" % virtual)
            producers[virtual] = inputs[0]
        for virtual in sorted(virtuals):
            if virtual not in producers:
                fail("No native symlink producer for virtual include %s in %s" % (virtual, ctx.label))
            original = producers[virtual]
            if original.is_source:
                origins.append(virtual + "\n" + original.path)
    return [HeaderOriginInfo(origins = depset(origins, transitive = transitive))]

header_origin_aspect = aspect(
    implementation = _header_origin_impl,
    attr_aspects = ["deps", "implementation_deps"],
    provides = [HeaderOriginInfo],
)

def _output_stem(identity):
    # Native outputs distinguish source/PIC contexts sharing a main. Hash the
    # complete report identity, not its ordinal, so unrelated sources do not
    # invalidate unchanged reports. Keep the bare hash short for Windows paths.
    return "%x" % (hash(json.encode(identity)) & 0xffffffff)

def _lint_impl(target, ctx, profile = "lint"):
    if ctx.label.repo_name:
        fail("Lint roots must be project owners, not external SDK targets")
    actions = sorted([a for a in target.actions if a.mnemonic in _COMPILE_MNEMONICS], key = _action_key)
    expected = {f.path: f for f in getattr(ctx.rule.files, "srcs", []) if f.extension in _SOURCE_SUFFIXES}
    native = []
    found = {}
    for action in actions:
        candidates = [f for f in action.inputs.to_list() if f.extension in _SOURCE_SUFFIXES and f.path in action.argv]
        if len(candidates) != 1:
            fail("Unsupported native lint source template for " + str(ctx.label))
        source = candidates[0]
        found[source.path] = source
        native.append((action, source))
    if sorted(expected.keys()) != sorted(found.keys()):
        fail("Native lint capture does not cover the configured owner's eligible srcs: " + str(ctx.label))
    headers, own_headers = _header_inputs(target, ctx)
    origins = {}
    for entry in target[HeaderOriginInfo].origins.to_list():
        virtual, _, original = entry.partition("\n")
        origins[virtual] = original

    # Compile actions discover headers at execution, so the owner's declared
    # header closure lists the virtual spellings; null marks generated ones.
    virtual_headers = {f.path: origins.get(f.path) for f in headers.to_list() if not f.is_source and "/_virtual_includes/" in f.path}
    pairs = {}
    uncovered = []
    for path, header in sorted(own_headers.items()):
        candidates = [source for source in found.values() if _paired(header, source)]
        if len(candidates) == 1:
            pairs[path] = candidates[0].path
        else:
            uncovered.append({"path": path, "reason": "no unambiguous same-component native header companion"})

    consumers = _consumer_mains(ctx.label, _decode_consumers(ctx.attr.aobus_lint_consumers).get(str(ctx.label), []), found, headers, pairs)
    uncovered = [item for item in uncovered if item["path"] not in consumers]
    verification = ctx.attr._verification[LintVerificationInfo]
    index = ctx.actions.declare_file(ctx.label.name + ".aobus-lint.index.json")
    artifacts = [index]
    entries = []
    capture = []
    stems = {}
    for action, source in native:
        mains = [source] + [header for path, header in sorted(own_headers.items()) if pairs.get(path) == source.path]
        mains.extend([main for _, (consumer, main) in sorted(consumers.items()) if consumer == source.path])
        language = "objc++" if source.extension == "mm" else "c" if source.extension == "c" else "c++"
        environment = native_environment(ctx, action, language)
        capture.append({"source": source.path, "argv": action.argv, "raw_env": action.env, "effective_env": environment, "outputs": [f.path for f in action.outputs.to_list()]})
        for main in mains:
            identity = {
                "owner": str(ctx.label),
                "configuration": ctx.bin_dir.path,
                "source": source.path,
                "main": main.path,
                "outputs": sorted([f.path for f in action.outputs.to_list()]),
                # The analyzer profile keeps one mode: its action derives the
                # test-file check exception from the main path, and the lint
                # gate rejects analyzer identities before reading a report.
                "mode": _identity_mode(main, profile),
                "platform": LINT_SDK_HOST,
            }
            stem = _output_stem(identity)
            if stem in stems:
                fail("Lint output stem %s is shared by %s and %s" % (stem, stems[stem], identity))
            stems[stem] = identity
            stem = ctx.label.name + ".aobus-lint/" + stem
            context = ctx.actions.declare_file(stem + ".context.json")
            report = ctx.actions.declare_file(stem + ".report.json")
            log = ctx.actions.declare_file(stem + ".log")
            ctx.actions.write(context, json.encode(dict(identity, schema = 1, argv = action.argv, virtual_headers = virtual_headers)))
            args = _arguments(ctx, "analyze")
            args.add_all(["--context", context, "--policy", verification.policy, "--sdk", verification.sdk, "--verification", verification.result, "--tidy", verification.host])
            if verification.plugin:
                args.add_all(["--plugin", verification.plugin])
            args.add_all(["--report", report, "--log", log])
            ctx.actions.run(
                executable = CATALOG_TOOLS["python"],
                arguments = [args],
                inputs = depset([context, main, verification.result], transitive = [action.inputs, headers, verification.inputs]),
                outputs = [report, log],
                env = environment,
                mnemonic = "AobusLint",
                progress_message = "Lint " + main.short_path,
            )
            entries.append({"identity": identity, "report": report.path, "log": log.path})
            artifacts.extend([report, log])
    ctx.actions.write(index, json.encode({
        "schema": 1,
        "owner": str(ctx.label),
        "configuration": ctx.bin_dir.path,
        "sources": sorted({path: None for path in found.keys() + own_headers.keys() + consumers.keys()}.keys()),
        "uncovered": uncovered,
        "reports": entries,
    }))
    metadata = ctx.actions.declare_file(ctx.label.name + ".aobus-lint.capture.json")
    ctx.actions.write(metadata, json.encode({"schema": 1, "owner": str(ctx.label), "actions": capture}))
    return [OutputGroupInfo(aobus_lint = depset(artifacts), aobus_lint_contexts = depset([metadata]))]

def _identity_mode(main, profile):
    if profile == "analyzer":
        return "ANALYZER"
    return "RELAXED" if main.short_path.startswith("test/") and not main.short_path.startswith("test/lint/fixture/") else "STRICT"

lint_aspect = aspect(
    implementation = _lint_impl,
    attr_aspects = [],
    requires = [header_origin_aspect],
    attrs = {
        # The portal names real consumers per run through --aspects_parameters;
        # the selection stays out of the policy every report action reads.
        "aobus_lint_consumers": attr.string(default = "{}"),
        "_helper": attr.label(default = Label("//:script/ao/core/bazel_lint_action.py"), allow_single_file = True),
        "_verification": attr.label(default = Label("//tool/lint:verification")),
    },
    fragments = ["cpp"],
    toolchains = use_cc_toolchain(),
)

def _analyzer_impl(target, ctx):
    # The explicit clang-analyzer profile: same native compile contexts and
    # consumer passes, never an alias the ordinary lint gate could accept.
    return _lint_impl(target, ctx, profile = "analyzer")

analyzer_aspect = aspect(
    implementation = _analyzer_impl,
    attr_aspects = [],
    requires = [header_origin_aspect],
    attrs = {
        "aobus_lint_consumers": attr.string(default = "{}"),
        "_helper": attr.label(default = Label("//:script/ao/core/bazel_lint_action.py"), allow_single_file = True),
        "_verification": attr.label(default = Label("//tool/lint:verification")),
    },
    fragments = ["cpp"],
    toolchains = use_cc_toolchain(),
)
