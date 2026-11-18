"""Expose configured quote roots and compile command lines for analysis-only native compile checks."""

load("@rules_cc//cc:find_cc_toolchain.bzl", "find_cc_toolchain", "use_cc_toolchain")
load("@rules_cc//cc/common:cc_common.bzl", "cc_common")
load("@rules_cc//cc/common:cc_info.bzl", "CcInfo")

def _compile_context_impl(target, ctx):
    if CcInfo not in target:
        return []

    # aquery reports only the reference to an MSVC compile param file, not its
    # contents; Starlark exposes the same action's expanded command line.
    compiles = [
        {"outputs": sorted([output.path for output in action.outputs.to_list()]), "argv": action.argv}
        for action in target.actions
        if action.mnemonic == "CppCompile"
    ]

    # Checked external compiles need their command lines, not quote-root evidence.
    if ctx.label.repo_name and not compiles:
        return []
    contexts = [target[CcInfo].compilation_context.quote_includes]

    # cc_library compiles with its private dependencies without exporting them.
    for dependency in getattr(ctx.rule.attr, "implementation_deps", []):
        contexts.append(dependency[CcInfo].compilation_context.quote_includes)
    output = ctx.actions.declare_file(ctx.label.name + ".aobus-compile-context.json")
    ctx.actions.write(output, json.encode({
        "quote_includes": depset(transitive = contexts).to_list(),
        "compile_argv": compiles,
    }))
    return []

compile_context_aspect = aspect(
    implementation = _compile_context_impl,
    attr_aspects = ["*"],
)

# Native policy facts for the standard in-graph validation test. One flat JSON
# record per configured owner; depset folding replaces the retired aquery joins.
#
# The record projects only the inputs the existing oracles actually consume:
# compile command lines with their environments and declared quote roots, the
# SPA-relevant declared input paths (the only reader of CompileAction.inputs is
# SPA detection), link/archive/ObjC++/linkstamp command lines with declared
# outputs, and the declared source/header files of native compile owners. The
# owned-source closure folds through the same dependency attributes the retired
# declared-graph audit used, so executable records carry their declared native
# source closure without reconstructing any graph. No nodes, dependency edges or
# configuration snapshots are emitted.
#
# The provider additionally folds the declared SDK compiler inputs the captured
# command lines name, so the Linux wrapper diagnostic can resolve that exact
# file from the test's runfiles instead of a runtime query. They are exported as
# an output group and never enter the JSON record.
ValidationFactsInfo = provider(
    doc = "Folded native policy facts of one configured target.",
    fields = [
        "records",
        "executables",
        "closure",
        "kind",
        "compilers",
    ],
)

_CPP_COMPILE_MNEMONIC = "CppCompile"
_NATIVE_MNEMONICS = (
    "CppLink",
    "ObjcLink",
    "ObjcExecutable",
    "CppArchive",
    "ObjcArchive",
    "ObjcCompile",
    "CppLinkstampCompile",
)
_EXECUTABLE_KINDS = ("cc_binary", "cc_test")
_COMPILE_KINDS = ("cc_library", "objc_library", "cc_binary", "cc_test")
_DECLARED_ATTRIBUTES = ("srcs", "hdrs", "textual_hdrs", "additional_compiler_inputs")
_CLOSURE_ATTRIBUTES = ("deps", "implementation_deps", "srcs", "actual")
_NATIVE_SOURCE_SUFFIXES = (".c", ".cc", ".cpp", ".cxx", ".mm")
_EXTERNAL_PREFIX = "external/"

def _sdk_compiler(action):
    """The declared SDK compiler input an execution-root command line names.

    The exported runfiles key is the execution-root path without the
    ``external/`` prefix, so the known external layout keeps the canonical
    repository directory. Nothing is guessed from Bazel output paths, and an
    absolute toolchain path stays a caller-owned fact.
    """
    compiler = action.argv[0]
    if not compiler.startswith(_EXTERNAL_PREFIX):
        return None
    key = compiler[len(_EXTERNAL_PREFIX):]
    if not key or ".." in key.split("/"):
        fail("CppCompile compiler path must not escape the execution root: " + compiler)
    for file in action.inputs.to_list():
        if file.path != compiler:
            continue
        if file.short_path != "../" + key:
            fail("SDK compiler input does not match its runfiles layout: " + compiler)
        return file
    return None

def _native_action_mnemonic(mnemonic):
    # The policy checker's LTO pipeline pattern for real Bazel action mnemonics.
    return (
        mnemonic in _NATIVE_MNEMONICS or
        mnemonic.startswith("Lto") or
        "LTO" in mnemonic
    )

def _spa_header(path):
    # The checker's SPA pattern: a spa or spa-0.2 directory segment and a .h leaf.
    segments = path.split("/")
    return path.endswith(".h") and ("spa" in segments[:-1] or "spa-0.2" in segments[:-1])

def _attribute_targets(ctx, name):
    value = getattr(ctx.rule.attr, name, None)
    if value == None:
        return []
    return value if type(value) == "list" else [value]

def _target_sdk_compiler(target, main_repo, is_tool):
    """The declared SDK compiler one target's compiles invoke, if any.

    One configuration selects one toolchain, so the first expanded compile
    action names the same compiler as every other action of that target. The
    scan stays per target instead of per action.
    """
    if not main_repo or is_tool:
        return None
    for action in target.actions:
        if action.mnemonic == _CPP_COMPILE_MNEMONIC and action.argv:
            return _sdk_compiler(action)
    return None

def _compile_environment(ctx):
    # Action.env exposes fixed values, not feature-provided native environment
    # entries. Query the same toolchain feature producer used by CppCompile.
    # Current compile env is static and mode-selected: it expands no per-action
    # variables and no env-producing feature is toggled at rule level. Revisit
    # this projection if either native-toolchain assumption changes.
    toolchain = find_cc_toolchain(ctx)
    features = cc_common.configure_features(
        ctx = ctx,
        cc_toolchain = toolchain,
        language = "c++",
        requested_features = ctx.features,
        unsupported_features = ctx.disabled_features,
    )
    variables = cc_common.create_compile_variables(cc_toolchain = toolchain, feature_configuration = features)
    return cc_common.get_environment_variables(
        feature_configuration = features,
        action_name = "c++-compile",
        variables = variables,
    )

def _validation_facts_impl(target, ctx):
    dep_records = []
    dep_executables = []
    dep_compilers = []
    folded_closures = []
    kind = ""
    if ctx.rule:
        kind = ctx.rule.kind
        for name in dir(ctx.rule.attr):
            for item in _attribute_targets(ctx, name):
                if type(item) != "Target" or ValidationFactsInfo not in item:
                    continue
                info = item[ValidationFactsInfo]
                dep_records.append(info.records)
                dep_executables.append(info.executables)
                dep_compilers.append(info.compilers)

                # The owned-source closure folds through the same dependency
                # attributes the retired declared-graph audit used, and never
                # through another executable's closure.
                if name in _CLOSURE_ATTRIBUTES and info.kind not in _EXECUTABLE_KINDS:
                    folded_closures.append(info.closure)

    # Source files carry no rule, no actions and no declared inputs of their own.
    main_repo = bool(ctx.rule) and not ctx.label.repo_name

    # LLVM-facing checker hosts have their own admitted ABI, not the product's
    # GSL/client matrix. The previous default-test audit did not include them;
    # native builds, SDK verification and full authored lint still cover them.
    client_policy = main_repo and "aobus-lint-sdk-host" not in getattr(ctx.rule.attr, "tags", [])
    is_tool = ctx.configuration.is_tool_configuration()
    declared = []
    own_sources = []
    quote_includes = []
    compiles = []
    native_actions = []
    compilers = []
    compile_environment = None
    declared_spa_inputs = []
    if main_repo:
        if kind in _COMPILE_KINDS:
            for name in _DECLARED_ATTRIBUTES:
                for file in getattr(ctx.rule.files, name, []):
                    declared.append(file.path)
            declared = sorted({path: None for path in declared}.keys())
            own_sources = [path for path in declared if path.endswith(_NATIVE_SOURCE_SUFFIXES)]
        if CcInfo in target:
            contexts = [target[CcInfo].compilation_context.quote_includes]
            headers = []

            # cc_library compiles with its private dependencies without exporting them.
            for dependency in getattr(ctx.rule.attr, "implementation_deps", []):
                if CcInfo in dependency:
                    contexts.append(dependency[CcInfo].compilation_context.quote_includes)
            quote_includes = sorted({path: None for path in depset(transitive = contexts).to_list()}.keys())

            # Action.inputs omits discoverable SDK headers before execution.
            # Follow this owner's direct SDK capabilities, not first-party
            # re-export closures containing unrelated private backend SDKs.
            # SPA consumers declare that SDK directly; this supplements own
            # declared inputs without pretending to reproduce header discovery.
            for attribute in ["deps", "implementation_deps"]:
                for dependency in getattr(ctx.rule.attr, attribute, []):
                    if dependency.label.repo_name and CcInfo in dependency:
                        headers.append(dependency[CcInfo].compilation_context.headers)
            declared_spa_inputs = [path for path in declared if _spa_header(path)] + [file.path for file in depset(transitive = headers).to_list() if _spa_header(file.path)]
    if ctx.rule:
        # The Linux wrapper diagnostic expands one target action through this
        # declared SDK input instead of a runtime query.
        compiler = _target_sdk_compiler(target, main_repo, is_tool)
        if compiler != None:
            compilers.append(compiler)
        for action in target.actions:
            mnemonic = action.mnemonic
            argv = action.argv
            if mnemonic == _CPP_COMPILE_MNEMONIC:
                # External compile command lines were never policy inputs of the
                # retired aquery oracle; only first-party compiles are checked.
                if not client_policy:
                    continue
                if not argv:
                    fail("CppCompile action has no expanded command line")
                if compile_environment == None:
                    compile_environment = _compile_environment(ctx)
                environment = dict(action.env)
                environment.update(compile_environment)
                compiles.append({
                    "argv": list(argv),
                    "env": {key: environment[key] for key in sorted(environment)},
                    "spa_inputs": sorted({
                        path: None
                        for path in declared_spa_inputs + [file.path for file in action.inputs.to_list() if _spa_header(file.path)]
                    }.keys()),
                })
            elif _native_action_mnemonic(mnemonic):
                # External repositories keep link/archive policy evidence.
                if not argv:
                    fail(mnemonic + " action has no expanded command line")
                native_actions.append({
                    "mnemonic": mnemonic,
                    "argv": list(argv),
                    "outputs": sorted({
                        output.path: None
                        for output in action.outputs.to_list()
                    }.keys()),
                })

    record = None
    if ctx.rule and (compiles or native_actions or declared or quote_includes):
        record = json.encode({
            "label": str(ctx.label),
            "kind": kind,
            "is_tool": is_tool,
            "configuration": ctx.bin_dir.path,
            "quote_includes": quote_includes,
            "declared": declared,
            "compiles": compiles,
            "native_actions": native_actions,
        })
    executable = None
    if main_repo and kind in _EXECUTABLE_KINDS:
        executable = json.encode({
            "label": str(ctx.label),
            "kind": kind,
            "is_tool": is_tool,
            "sources": sorted({
                path: None
                for path in depset(own_sources, transitive = folded_closures).to_list()
            }.keys()),
        })
    return [ValidationFactsInfo(
        kind = kind,
        records = depset([record] if record else [], transitive = dep_records),
        executables = depset([executable] if executable else [], transitive = dep_executables),
        closure = depset(own_sources, transitive = folded_closures),
        compilers = depset(compilers, transitive = dep_compilers),
    )]

validation_facts_aspect = aspect(
    implementation = _validation_facts_impl,
    # "*" must be the only entry: propagate along every attribute, including
    # test_suite members and exec-configured tools, so no configured owner is
    # skipped. Skipped (incompatible) targets are unreachable by design; their
    # registrations are deferred by the existing platform policy instead.
    attr_aspects = ["*"],
    fragments = ["cpp"],
    toolchains = use_cc_toolchain(),
)
