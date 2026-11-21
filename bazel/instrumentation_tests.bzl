"""Linux daily sanitizer canaries and coverage analysis in the native graph.

The bazel_skylib ``build_test`` compiles and links the profile probe without running it.
The official ``analysis_test`` rules additionally inspect the configured native
actions and providers, so a profile that silently stops instrumenting cannot
pass as an ordinary binary. No custom pipeline, runtime Bazel invocation,
query/aquery receipt or coverage execution is introduced here.
"""

load("@bazel_skylib//rules:analysis_test.bzl", "analysis_test")
load("@bazel_skylib//rules:build_test.bzl", "build_test")
load("@rules_cc//cc/common:cc_info.bzl", "CcInfo")
load(
    "//bazel:compiler_policy.bzl",
    "COVERAGE_OPTION_PREFIXES",
    "FORWARDING_WRAPPER_FLAGS",
    "FORWARDING_WRAPPER_PREFIXES",
    "GCOV_ATOMIC_UPDATE_FLAG",
    "GCOV_COUNTER_FLAGS",
    "GCOV_DRIVER_FLAGS",
    "UNSAFE_GCOV_COUNTER_FLAGS",
    "UNSAFE_GCOV_COUNTER_PREFIXES",
)
load("//bazel:testing.bzl", "NativeContextInfo")

_LINUX = ["@platforms//os:linux"]

# Keep this identical to the existing coverage command's default filter.
_INSTRUMENTATION_FILTER = "^//(app|lib|include|test)(/|:)"

_PROFILE_FEATURES = {
    "-aobus_release_ipo": True,
    "-aobus_sampling": True,
    "-asan": True,
    "-tsan": True,
    "-ubsan": True,
    "aobus_release_ipo": True,
    "aobus_sampling": True,
    "asan": True,
    "tsan": True,
    "ubsan": True,
}

_SANITIZER_FEATURES = {
    "asan": ["asan", "ubsan", "-treat_warnings_as_errors"],
    "tsan": ["tsan", "-treat_warnings_as_errors"],
}

_SANITIZER_PLATFORMS = {
    "asan": Label("//bazel:asan_platform"),
    "tsan": Label("//bazel:tsan_platform"),
}

_NATIVE_PLATFORM = Label("@aobus_environment//:native_platform")
_COVERAGE_SUPPORT = Label("//bazel/coverage:coverage_support")
_COVERAGE_MERGER = Label("//bazel/coverage:merger")

# The gcov/coverage vocabulary is shared with the Python coverage oracle
# through the plain literals in compiler_policy.bzl; parity cases pin it.
_COVERAGE_FLAGS = GCOV_COUNTER_FLAGS + GCOV_DRIVER_FLAGS
_LINK_MNEMONICS = ["CppLink", "CppLinkDynamicLibrary"]
_SANITIZER_FLAGS = ["-fsanitize=address", "-fsanitize=thread", "-fsanitize=undefined"]
_IPO_FLAGS = ["-flto", "-flto=auto", "-flto=thin", "-fno-lto"]
_OPTIMIZATION_FLAGS = ["-O0", "-O1", "-O2", "-O3", "-Og", "-Os", "-Oz", "-Ofast"]
_DEBUG_FLAGS = [
    "-g",
    "-g0",
    "-g1",
    "-g2",
    "-g3",
    "-ggdb",
    "-ggdb0",
    "-ggdb1",
    "-ggdb2",
    "-ggdb3",
    "-gline-tables-only",
]

def _instrumented_config_impl(settings, attr):
    profile = attr.profile
    if profile in ["coverage", "debug"]:
        platform = _NATIVE_PLATFORM
        features = [
            feature
            for feature in settings["//command_line_option:features"]
            if feature not in _PROFILE_FEATURES
        ]
        compiler = "gcc"
        coverage = profile == "coverage"
        asan = False
        tsan = False
    else:
        platform = _SANITIZER_PLATFORMS[profile]
        features = _SANITIZER_FEATURES[profile]
        compiler = settings["//command_line_option:compiler"] or "gcc"
        coverage = False
        asan = profile == "asan"
        tsan = profile == "tsan"

    return {
        "//bazel:asan_enabled": asan,
        "//bazel/coverage:runtime_enabled": coverage,
        "//bazel:release_ipo_enabled": False,
        "//bazel:sampling_enabled": False,
        "//bazel:tsan_enabled": tsan,
        "//command_line_option:collect_code_coverage": coverage,
        "//command_line_option:compilation_mode": "dbg",
        "//command_line_option:compiler": compiler,
        "//command_line_option:coverage_output_generator": _COVERAGE_MERGER if coverage else None,
        "//command_line_option:coverage_report_generator": _COVERAGE_MERGER if coverage else None,
        "//command_line_option:coverage_support": _COVERAGE_SUPPORT if coverage else None,
        "//command_line_option:features": features,
        "//command_line_option:instrument_test_targets": coverage,
        "//command_line_option:instrumentation_filter": _INSTRUMENTATION_FILTER if coverage else None,
        "//command_line_option:platforms": [platform],
        "//command_line_option:run_under": str(_COVERAGE_SUPPORT) if coverage else None,
        "//command_line_option:zip_undeclared_test_outputs": coverage,
    }

_instrumented_config = transition(
    implementation = _instrumented_config_impl,
    inputs = ["//command_line_option:compiler", "//command_line_option:features"],
    outputs = [
        "//bazel:asan_enabled",
        "//bazel/coverage:runtime_enabled",
        "//bazel:release_ipo_enabled",
        "//bazel:sampling_enabled",
        "//bazel:tsan_enabled",
        "//command_line_option:collect_code_coverage",
        "//command_line_option:compilation_mode",
        "//command_line_option:compiler",
        "//command_line_option:coverage_output_generator",
        "//command_line_option:coverage_report_generator",
        "//command_line_option:coverage_support",
        "//command_line_option:features",
        "//command_line_option:instrument_test_targets",
        "//command_line_option:instrumentation_filter",
        "//command_line_option:platforms",
        "//command_line_option:run_under",
        "//command_line_option:zip_undeclared_test_outputs",
    ],
)

def _one_target(value):
    if type(value) == "list":
        if len(value) != 1:
            fail("instrumentation assertion requires exactly one target")
        return value[0]
    return value

_CompileFactsInfo = provider(
    doc = "Configured native compile and link command lines for one target.",
    fields = ["compile_argv", "link_argv"],
)

# Same configured native CppCompile/CppLink access as //bazel:compile_context.bzl;
# analysis tests cannot read that aspect's written JSON, so fold argv here.
def _compile_facts_impl(target, _ctx):
    return [_CompileFactsInfo(
        compile_argv = [action.argv for action in target.actions if action.mnemonic == "CppCompile"],
        link_argv = [action.argv for action in target.actions if action.mnemonic in _LINK_MNEMONICS],
    )]

_compile_facts_aspect = aspect(implementation = _compile_facts_impl)

def _last_flag(argv, flags):
    result = None
    for argument in argv:
        if argument in flags:
            result = argument
    return result

def _ndebug_defined(argv):
    result = False
    pending = None
    for argument in argv:
        if pending != None:
            prefix, pending = pending, None
            if argument.partition("=")[0] == "NDEBUG":
                result = prefix == "D"
            continue
        if argument[:2] in ["-D", "/D", "-U", "/U"]:
            value = argument[2:]
            if value:
                if value.partition("=")[0] == "NDEBUG":
                    result = argument[1] == "D"
            else:
                pending = argument[1]
    return result

def _check_debug_compile(argv, label, profile):
    optimization = _last_flag(argv, _OPTIMIZATION_FLAGS)
    if optimization not in [None, "-O0"]:
        fail("%s compile action for %s is optimized (%s)" % (profile, label, optimization))
    debug = _last_flag(argv, _DEBUG_FLAGS)
    if debug in [None, "-g0", "-ggdb0"]:
        fail("%s compile action for %s lacks enabled debug information" % (profile, label))
    if _ndebug_defined(argv):
        fail("%s compile action for %s defines NDEBUG" % (profile, label))

def _check_sanitizer_argv(argv_list, profile, phase, label):
    if profile == "asan":
        required = ["-fsanitize=address", "-fsanitize=undefined"]
        forbidden = ["-fsanitize=thread"]
    else:
        required = ["-fsanitize=thread"]
        forbidden = ["-fsanitize=address", "-fsanitize=undefined"]
    forbidden = forbidden + _COVERAGE_FLAGS + _IPO_FLAGS + [GCOV_ATOMIC_UPDATE_FLAG]
    for argv in argv_list:
        for flag in required:
            if flag not in argv:
                fail("%s %s action for %s lacks %s" % (profile, phase, label, flag))
        for flag in forbidden:
            if flag in argv:
                fail("%s %s action for %s unexpectedly contains %s" % (profile, phase, label, flag))
        if phase == "compile":
            _check_debug_compile(argv, label, profile)

def _compile_assertion_impl(ctx):
    target = _one_target(ctx.attr.target)
    if _CompileFactsInfo not in target:
        fail("instrumentation compile analysis did not visit %s" % target.label)
    facts = target[_CompileFactsInfo]
    if not facts.compile_argv:
        fail("instrumentation compile analysis found no CppCompile action for %s" % target.label)
    if not facts.link_argv:
        fail("instrumentation compile analysis found no CppLink action for %s" % target.label)
    _check_sanitizer_argv(facts.compile_argv, ctx.attr.profile, "compile", target.label)
    _check_sanitizer_argv(facts.link_argv, ctx.attr.profile, "link", target.label)
    return []

_CoverageFactsInfo = provider(
    doc = "Instrumented source and metadata declarations from the configured coverage graph.",
    fields = ["application_compile", "compile_count", "instrumented_files", "metadata_files", "private_mappings", "test_count"],
)

def _in_instrumentation_filter(label):
    return label.package.split("/")[0] in ["app", "include", "lib", "test"]

def _coverage_option(argument):
    option = argument.lstrip("-")
    return argument.startswith("-") and (
        option == "coverage" or any([option.startswith(prefix) for prefix in COVERAGE_OPTION_PREFIXES])
    )

def _reject_forwarded_coverage(argv, label):
    wrappers = FORWARDING_WRAPPER_FLAGS
    forwarded = False
    for argument in argv:
        wrapper, separator, payload = argument.partition("=")
        is_wrapper = wrapper in wrappers or any([
            wrapper.startswith(prefix)
            for prefix in FORWARDING_WRAPPER_PREFIXES
        ])
        if argument.startswith("-Wp,") or argument.startswith("-Wl,") or argument.startswith("-Wa,"):
            relevant = any([_coverage_option(token) for token in argument.split(",")[1:]])
        elif is_wrapper and separator:
            relevant = _coverage_option(payload)
        else:
            relevant = forwarded and _coverage_option(argument)
        if relevant:
            fail("forwarded coverage option cannot prove safe counters: %s: %s" % (label, argument))
        forwarded = is_wrapper and not separator

def _check_coverage_compile_argv(argv, label):
    flags = {argument: True for argument in argv}
    for required in GCOV_COUNTER_FLAGS:
        if required not in flags:
            fail("coverage compile action for %s lacks %s" % (label, required))
    if GCOV_ATOMIC_UPDATE_FLAG not in flags:
        fail("coverage compile action for %s lacks %s" % (label, GCOV_ATOMIC_UPDATE_FLAG))
    _check_debug_compile(argv, label, "coverage")
    _reject_forwarded_coverage(argv, label)
    for argument in argv:
        option = argument.lstrip("-")
        if argument in UNSAFE_GCOV_COUNTER_FLAGS:
            fail("unsafe gcov counter option: %s: %s" % (label, argument))
        if any([option.startswith(prefix) for prefix in UNSAFE_GCOV_COUNTER_PREFIXES]) and argument != GCOV_ATOMIC_UPDATE_FLAG:
            fail("unsafe gcov counter option: %s: %s" % (label, argument))
    for flag in _SANITIZER_FLAGS + _IPO_FLAGS:
        if flag in flags:
            fail("coverage compile action for %s unexpectedly contains %s" % (label, flag))

def _check_coverage_link_argv(argv, label):
    if not any([flag in argv for flag in _COVERAGE_FLAGS]):
        fail("coverage link action for %s lacks coverage instrumentation" % label)
    for flag in _SANITIZER_FLAGS + _IPO_FLAGS:
        if flag in argv:
            fail("coverage link action for %s unexpectedly contains %s" % (label, flag))

def _check_coverage_test_runner(target, metadata):
    # Native FileWrite content is not exposed by Bazel 9.2's Action API. Check
    # its declared producer/runner inputs here and the private-header provider
    # at the project-owned rules_cc patch boundary below.
    runners = [action for action in target.actions if action.mnemonic == "TestRunner"]
    if len(runners) != 1:
        fail("coverage analysis requires exactly one TestRunner for %s" % target.label)
    file_writes = [action for action in target.actions if action.mnemonic == "FileWrite"]
    mapping_outputs = [
        output
        for action in file_writes
        for output in action.outputs.to_list()
        if output.basename == "reported_to_actual_sources.txt"
    ]
    if len(mapping_outputs) != 1:
        fail("coverage analysis requires one target-owned standard source mapping for %s" % target.label)
    manifest_outputs = [
        output
        for action in file_writes
        for output in action.outputs.to_list()
        if output.basename.endswith(".instrumented_files")
    ]
    if len(manifest_outputs) != 1:
        fail("coverage analysis requires one target-owned instrumentation manifest for %s" % target.label)
    inputs = runners[0].inputs.to_list()
    input_paths = {file.path: True for file in inputs}
    for file in metadata:
        if file.path not in input_paths:
            fail("coverage metadata %s is not an input of the %s TestRunner" % (file.path, target.label))
    basenames = {file.basename: True for file in inputs}
    if "reported_to_actual_sources.txt" not in basenames:
        fail("coverage TestRunner for %s lacks the standard source mapping" % target.label)
    if not any([name.endswith(".instrumented_files") for name in basenames]):
        fail("coverage TestRunner for %s lacks the instrumentation manifest" % target.label)
    if "merger.sh" not in basenames or "merger.sh.runfiles" not in basenames:
        fail("coverage TestRunner for %s lacks the declared merger runfiles" % target.label)
    if "collect_coverage.sh" not in basenames:
        fail("coverage TestRunner for %s lacks the declared coverage support" % target.label)

def _virtual_header_mappings(context):
    if hasattr(context, "virtual_to_original_headers"):
        return context.virtual_to_original_headers().to_list()
    return context._virtual_to_original_headers.to_list()

def _check_private_header_mappings(target, ctx):
    # This is the repository's rules_cc patch boundary. Native FileWrite actions
    # do not expose their mapping text through Action.content in Bazel 9.2;
    # assert the private mappings on the provider that produces that text.
    if CcInfo not in target:
        return 0
    mappings = _virtual_header_mappings(target[CcInfo].compilation_context)
    count = 0
    for dependency in getattr(ctx.rule.attr, "implementation_deps", []):
        for mapping in _virtual_header_mappings(dependency[CcInfo].compilation_context):
            if mapping not in mappings:
                fail("coverage drops private implementation-header mapping for %s: %s" % (ctx.label, mapping))
            count += 1
    return count

def _coverage_facts_impl(target, ctx):
    if ctx.configuration.is_tool_configuration():
        for action in target.actions:
            if action.mnemonic == "CppCompile" or action.mnemonic in _LINK_MNEMONICS:
                if any([flag in action.argv for flag in _COVERAGE_FLAGS + [GCOV_ATOMIC_UPDATE_FLAG]]):
                    fail("coverage instrumentation leaked into execution tool %s" % ctx.label)
        return []
    if not ctx.configuration.coverage_enabled or ctx.label.repo_name:
        return []
    kind = ctx.rule.kind
    if kind == "test_suite":
        facts = []
        for dependency in getattr(ctx.rule.attr, "tests", []):
            if _CoverageFactsInfo not in dependency:
                fail("coverage analysis skipped selected test target: %s" % dependency.label)
            facts.append(dependency[_CoverageFactsInfo])
        if not facts:
            fail("coverage analysis selected no test targets")
        application_compile = 0
        compile_count = 0
        private_mappings = 0
        test_count = 0
        for fact in facts:
            application_compile += fact.application_compile
            compile_count += fact.compile_count
            private_mappings += fact.private_mappings
            test_count += fact.test_count
        return [_CoverageFactsInfo(
            application_compile = application_compile,
            compile_count = compile_count,
            instrumented_files = depset(transitive = [fact.instrumented_files for fact in facts]),
            metadata_files = depset(transitive = [fact.metadata_files for fact in facts]),
            private_mappings = private_mappings,
            test_count = test_count,
        )]

    if InstrumentedFilesInfo not in target:
        return []
    if not _in_instrumentation_filter(ctx.label):
        return []

    info = target[InstrumentedFilesInfo]
    sources = info.instrumented_files.to_list()
    metadata = info.metadata_files.to_list()
    compiles = [action for action in target.actions if action.mnemonic == "CppCompile"]
    links = [action for action in target.actions if action.mnemonic in _LINK_MNEMONICS]

    # InstrumentedFilesInfo includes dependency metadata, including on header-only
    # owners. Carry application reachability along those same native edges.
    application_compile = 0
    private_mappings = _check_private_header_mappings(target, ctx)
    for attribute in ["deps", "implementation_deps", "srcs"]:
        for dependency in getattr(ctx.rule.attr, attribute, []):
            if type(dependency) == "Target" and _CoverageFactsInfo in dependency:
                application_compile += dependency[_CoverageFactsInfo].application_compile
                private_mappings += dependency[_CoverageFactsInfo].private_mappings

    if NativeContextInfo in target:
        # The wrapper owns the executed TestRunner while its inner cc_test
        # owns compilation. Check both capabilities rather than treating the
        # wrapper as an uninstrumented header-only library.
        inner = ctx.rule.attr.inner
        if _CoverageFactsInfo not in inner:
            fail("coverage wrapper lacks its native compile facts: %s" % ctx.label)
        _check_coverage_test_runner(target, metadata)
        return [_CoverageFactsInfo(
            application_compile = application_compile,
            compile_count = inner[_CoverageFactsInfo].compile_count,
            instrumented_files = depset(sources),
            metadata_files = depset(metadata),
            private_mappings = private_mappings,
            test_count = 1,
        )]

    for action in links:
        _check_coverage_link_argv(action.argv, ctx.label)
    if kind in ["cc_binary", "cc_test"] and not links:
        fail("coverage analysis found no CppLink action for %s" % ctx.label)

    if not compiles:
        if kind == "cc_test":
            fail("coverage analysis found no CppCompile action for selected test %s" % ctx.label)
        return [_CoverageFactsInfo(
            application_compile = application_compile,
            compile_count = 0,
            instrumented_files = depset(sources),
            metadata_files = depset(metadata),
            private_mappings = private_mappings,
            test_count = 0,
        )]

    if not sources:
        fail("coverage analysis found CppCompile actions without instrumented sources for %s" % ctx.label)
    if not metadata:
        fail("coverage analysis found no coverage metadata for %s" % ctx.label)
    for file in metadata:
        if not file.basename.endswith(".gcno"):
            fail("coverage metadata for %s is not a .gcno file: %s" % (ctx.label, file.path))
    for action in compiles:
        _check_coverage_compile_argv(action.argv, ctx.label)
        if ctx.label.package.startswith("app/") and any([
            argument.startswith("app/") and argument.endswith(".cpp")
            for argument in action.argv
        ]):
            application_compile += 1
    if kind == "cc_test":
        _check_coverage_test_runner(target, metadata)

    return [_CoverageFactsInfo(
        application_compile = application_compile,
        compile_count = len(compiles),
        instrumented_files = depset(sources),
        metadata_files = depset(metadata),
        private_mappings = private_mappings,
        test_count = 1 if kind == "cc_test" else 0,
    )]

# Native provider assertions cover the project-owned private-header patch;
# generated manifest bytes remain the trusted rules_cc/Bazel producer's job.
_coverage_facts_aspect = aspect(
    implementation = _coverage_facts_impl,
    attr_aspects = ["*"],
)

def _coverage_assertion_impl(ctx):
    target = _one_target(ctx.attr.target)
    if _CoverageFactsInfo not in target:
        fail("coverage analysis did not produce facts for %s" % target.label)
    facts = target[_CoverageFactsInfo]
    if facts.test_count == 0:
        fail("coverage analysis selected no C++ tests")
    if ctx.attr.require_application and facts.application_compile == 0:
        fail("coverage analysis selected no application implementation compile")
    if facts.compile_count == 0:
        fail("coverage analysis selected no C++ compile actions")
    if not facts.instrumented_files.to_list():
        fail("coverage analysis selected no instrumented sources")
    if not facts.metadata_files.to_list():
        fail("coverage analysis selected no coverage metadata")
    if ctx.attr.require_application and facts.private_mappings == 0:
        fail("coverage analysis selected no private implementation-header mapping")
    return []

def _instrumented_alias_impl(ctx):
    info = _one_target(ctx.attr.target)[DefaultInfo]
    if ctx.attr.profile == "debug":
        files = info.files.to_list()
        if len(files) != 1:
            fail("Debug policy input requires exactly one analysis fact file")

        # Both configurations feed one py_test. Give the Debug projection a
        # distinct logical runfiles name rather than colliding with fastbuild.
        output = ctx.actions.declare_file(ctx.label.name + ".aobus-native-policy.json")
        ctx.actions.symlink(output = output, target_file = files[0])
        return [DefaultInfo(files = depset([output]))]
    return [DefaultInfo(
        files = info.files,
        runfiles = info.default_runfiles,
    )]

_instrumented_alias = rule(
    implementation = _instrumented_alias_impl,
    attrs = {
        "profile": attr.string(mandatory = True, values = ["asan", "tsan", "debug"]),
        "target": attr.label(cfg = _instrumented_config, mandatory = True),
        "_allowlist_function_transition": attr.label(default = "@bazel_tools//tools/allowlists/function_transition_allowlist"),
    },
)

_compile_assertion = rule(
    implementation = _compile_assertion_impl,
    attrs = {
        "profile": attr.string(mandatory = True, values = ["asan", "tsan"]),
        "target": attr.label(aspects = [_compile_facts_aspect], cfg = _instrumented_config, mandatory = True),
        "_allowlist_function_transition": attr.label(default = "@bazel_tools//tools/allowlists/function_transition_allowlist"),
    },
)

def _coverage_merger_admission_impl(_target, ctx):
    # Ordinary builds intentionally leave this runtime disabled. Coverage
    # analysis must select the real declared Java runtime and stock payload.
    if ctx.rule.attr.java_runtime == None or ctx.rule.attr.stock_payload == None:
        fail("coverage analysis requires the declared Java runtime and stock merger payload")
    return []

_coverage_merger_admission = aspect(implementation = _coverage_merger_admission_impl)

_coverage_assertion = rule(
    implementation = _coverage_assertion_impl,
    attrs = {
        "profile": attr.string(mandatory = True, values = ["coverage"]),
        "_merger": attr.label(default = _COVERAGE_MERGER, aspects = [_coverage_merger_admission], cfg = _instrumented_config),
        "require_application": attr.bool(default = True),
        "target": attr.label(aspects = [_coverage_facts_aspect], cfg = _instrumented_config, mandatory = True),
        "_allowlist_function_transition": attr.label(default = "@bazel_tools//tools/allowlists/function_transition_allowlist"),
    },
)

def debug_policy_inputs(name, target):
    """Analyze a whole policy-input producer in Linux Debug without building C++."""
    _instrumented_alias(
        name = name,
        profile = "debug",
        target = target,
        target_compatible_with = _LINUX,
        testonly = True,
    )

def instrumentation_tests(
        sanitizer_target,
        name = "instrumentation_tests",
        coverage = "//:coverage_default_tests",
        require_application = True):
    """Declare Linux-only sanitizer canaries and coverage analysis tests.

    Args:
        sanitizer_target: Compiler instrumentation probe to build and link.
        name: Aggregate test suite name and target-name prefix.
        coverage: Existing coverage test suite analyzed with the standard coverage configuration.
        require_application: Require one app/*.cpp target compile, matching the default suite.
    """
    for profile in ["asan", "tsan"]:
        alias = name + "_" + profile + "_alias"
        _instrumented_alias(
            name = alias,
            profile = profile,
            target = sanitizer_target,
            target_compatible_with = _LINUX,
            testonly = True,
        )
        build_test(
            name = name + "_" + profile + "_build",
            size = "small",
            target_compatible_with = _LINUX,
            targets = [":" + alias],
        )

        assertion = name + "_" + profile + "_assert"
        _compile_assertion(
            name = assertion,
            profile = profile,
            target = sanitizer_target,
            target_compatible_with = _LINUX,
            testonly = True,
        )
        analysis_test(
            name = name + "_" + profile + "_compile",
            size = "small",
            target_compatible_with = _LINUX,
            targets = [":" + assertion],
        )

    coverage_assertion = name + "_coverage_assert"
    _coverage_assertion(
        name = coverage_assertion,
        profile = "coverage",
        target = coverage,
        require_application = require_application,
        target_compatible_with = _LINUX,
        testonly = True,
    )
    analysis_test(
        name = name + "_coverage",
        size = "small",
        target_compatible_with = _LINUX,
        targets = [":" + coverage_assertion],
    )

    native.test_suite(
        name = name,
        target_compatible_with = _LINUX,
        tests = [
            ":" + name + "_asan_build",
            ":" + name + "_asan_compile",
            ":" + name + "_coverage",
            ":" + name + "_tsan_build",
            ":" + name + "_tsan_compile",
        ],
    )
