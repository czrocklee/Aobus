"""Fold native policy facts from the completion roots into one analysis input.

The rule is a non-test input producer for the standard in-graph validation
py_test: it applies the validation facts aspect to the existing executable
roots, folds the per-owner flat records and the per-executable owned-source
closures into depsets, and writes one JSON file of native analysis facts. It
never consumes a dependency's files, so building it (or a configuration
transition alias around it) forces no C++ compilation; the JSON write is its
only action. The declared SDK compiler inputs the captured command lines name
are exported as the ``native_policy_compilers`` output group, so the Linux
wrapper diagnostic resolves that exact file from the test's runfiles. No
runtime Bazel, query or aquery is involved anywhere.

``native_validation_tests`` declares the standard validation py_tests around
one facts producer; the root BUILD file calls it so the public ``//:native_policy*``
labels stay in the root package.

Scalar facts come from the build configuration itself: the target platform's
OS constraints, the COMPILATION_MODE make variable, the governed profile
toolchain features, and the same frontend config settings that gate root
membership. Apply any compilation-mode transition to this whole target (for
example a debug analysis alias), never to its root attributes: the folded
action facts and these scalars must share one configuration.
"""

load("@rules_cc//cc:find_cc_toolchain.bzl", "find_cc_toolchain", "use_cc_toolchain")
load("@rules_python//python:defs.bzl", "py_test")
load("//bazel:compile_context.bzl", "ValidationFactsInfo", "validation_facts_aspect")
load("//bazel:instrumentation_tests.bzl", "debug_policy_inputs")

_OS_ATTRIBUTES = (
    ("linux", "_linux", "@platforms//os:linux"),
    ("macos", "_macos", "@platforms//os:macos"),
    ("windows", "_windows", "@platforms//os:windows"),
)
_MODES = ("dbg", "opt", "fastbuild")
_RELEASE_FEATURE = "aobus_release_ipo"
_SAMPLING_FEATURE = "aobus_sampling"

def _native_policy_facts_impl(ctx):
    selected = [
        name
        for name, attribute, _label in _OS_ATTRIBUTES
        if ctx.target_platform_has_constraint(getattr(ctx.attr, attribute)[platform_common.ConstraintValueInfo])
    ]
    if len(selected) != 1:
        fail("Native policy facts require exactly one native OS constraint, got: " + str(selected))
    platform = selected[0]

    mode = ctx.var.get("COMPILATION_MODE")
    if mode not in _MODES:
        fail("Native policy facts require a Bazel compilation mode, got: " + str(mode))

    release = _RELEASE_FEATURE in ctx.features
    sampling = _SAMPLING_FEATURE in ctx.features
    if release and sampling:
        fail("Release IPO and the sampling profile cannot be analyzed together")
    profile = "release" if release else ("sampling" if sampling else None)

    toolchain = find_cc_toolchain(ctx)
    if not hasattr(toolchain, "built_in_include_directories"):
        fail("Native policy facts require the selected toolchain's built-in include directories")
    directories = toolchain.built_in_include_directories
    builtin = sorted(directories.to_list() if type(directories) == "depset" else directories)

    records = []
    executed = []
    built = []
    compilers = []
    for dep in ctx.attr.test_roots:
        records.append(dep[ValidationFactsInfo].records)
        executed.append(dep[ValidationFactsInfo].executables)
        compilers.append(dep[ValidationFactsInfo].compilers)
    for dep in ctx.attr.product_roots:
        records.append(dep[ValidationFactsInfo].records)
        built.append(dep[ValidationFactsInfo].executables)
        compilers.append(dep[ValidationFactsInfo].compilers)
    for dep in ctx.attr.tool_roots:
        records.append(dep[ValidationFactsInfo].records)
        compilers.append(dep[ValidationFactsInfo].compilers)

    output = ctx.actions.declare_file(ctx.label.name + ".aobus-native-policy.json")
    ctx.actions.write(output, json.encode({
        "schema": 1,
        "platform": platform,
        "mode": mode,
        "profile": profile,
        "gtk_enabled": ctx.attr.gtk_enabled == "true",
        "system_media_enabled": ctx.attr.system_media_enabled == "true",
        "builtin_include_roots": builtin,
        "records": sorted(depset(transitive = records).to_list()),
        "executed": sorted(depset(transitive = executed).to_list()),
        "built": sorted(depset(transitive = built).to_list()),
    }))
    return [
        # One facts JSON stays the only default output.
        DefaultInfo(files = depset([output])),
        OutputGroupInfo(native_policy_compilers = depset(transitive = compilers)),
    ]

native_policy_facts = rule(
    implementation = _native_policy_facts_impl,
    attrs = {
        # The existing executable roots own the reachable owner set; no second
        # target inventory is declared here. The frontend facts mirror the same
        # config settings that gate these roots' membership, so a disabled
        # frontend's deferral boundary follows the actual configured universe.
        "test_roots": attr.label_list(
            aspects = [validation_facts_aspect],
            default = [Label("//:check_cpp_tests")],
        ),
        "product_roots": attr.label_list(
            aspects = [validation_facts_aspect],
            default = [Label("//:check_products")],
        ),
        # Additional analysis-only roots (for example Linux portal tooling).
        # The default is empty: the coverage merger is a shell rule with no
        # native actions, and it is Linux-only, which would silently skip this
        # whole input target on the other native hosts.
        "tool_roots": attr.label_list(aspects = [validation_facts_aspect]),
        "gtk_enabled": attr.string(mandatory = True, values = ["true", "false"]),
        "system_media_enabled": attr.string(mandatory = True, values = ["true", "false"]),
        "_linux": attr.label(default = Label("@platforms//os:linux")),
        "_macos": attr.label(default = Label("@platforms//os:macos")),
        "_windows": attr.label(default = Label("@platforms//os:windows")),
    },
    fragments = ["cpp"],
    toolchains = use_cc_toolchain(),
)

def native_validation_tests(
        name = "native_policy",
        inputs = "native_policy_inputs",
        debug_inputs = "native_policy_debug_inputs",
        compilers = "native_policy_compilers",
        driver = "native_driver_policy",
        python = "ao_python"):
    """Declare the standard in-graph native policy validation tests.

    One analysis-only facts producer feeds the checkout-owned validation
    py_test on every host and the Linux wrapper-driver py_test; the Linux Debug
    transition alias adds the dbg projection of the same graph to the standard
    test. The names default to the public root labels; the calling package owns
    the ``py_library`` that provides ``main_module = ao.portal.bazel_validation``.

    Args:
        name: Standard validation py_test name.
        inputs: Folded native policy facts producer.
        debug_inputs: Linux Debug projection of the facts producer.
        compilers: Output-group filegroup of the declared SDK compilers.
        driver: Linux representative-driver py_test name.
        python: py_library carrying the validation module.
    """
    native_policy_facts(
        name = inputs,
        testonly = True,
        gtk_enabled = select({
            "//bazel:linux_gtk_enabled": "true",
            "//conditions:default": "false",
        }),
        system_media_enabled = select({
            "//bazel:linux_system_media_enabled": "true",
            "//conditions:default": "false",
        }),
    )
    debug_policy_inputs(
        name = debug_inputs,
        target = ":" + inputs,
    )
    py_test(
        name = name,
        args = ["$(rlocationpath :" + inputs + ")"] + select({
            "@platforms//os:linux": ["$(rlocationpath :" + debug_inputs + ")"],
            "//conditions:default": [],
        }),
        data = [
            "VERSION.txt",
            ":" + inputs,
        ] + select({
            "@platforms//os:linux": [":" + debug_inputs],
            "//conditions:default": [],
        }),
        # The authored-source audit runs live Git. Compiler comparison uses the
        # Linux shell report or the Windows toolset version, respectively.
        env_inherit = ["PATH"] + select({
            "@platforms//os:linux": ["AOBUS_NIX_DEPENDENCY_REPORT"],
            # Bazel inherits exact client spellings: Python uppercases the name
            # for the managed launch; a direct developer-shell launch need not.
            "@platforms//os:windows": ["VCTOOLSVERSION", "VCToolsVersion"],
            "//conditions:default": [],
        }),
        main_module = "ao.portal.bazel_validation",
        tags = [
            "external",
            "local",
        ],
        deps = [
            ":" + python,
            "@rules_python//python/runfiles",
        ],
    )
    native.filegroup(
        name = compilers,
        testonly = True,
        srcs = [":" + inputs],
        output_group = "native_policy_compilers",
    )
    py_test(
        name = driver,
        args = [
            "--driver-only",
            "$(rlocationpath :" + inputs + ")",
            "$(rlocationpath :" + debug_inputs + ")",
        ],
        data = [
            "VERSION.txt",
            ":" + compilers,
            ":" + debug_inputs,
            ":" + inputs,
        ],
        env_inherit = ["AOBUS_NIX_DEPENDENCY_REPORT"],
        main_module = "ao.portal.bazel_validation",
        tags = [
            "external",
            "local",
        ],
        target_compatible_with = ["@platforms//os:linux"],
        deps = [
            ":" + python,
            "@rules_python//python/runfiles",
        ],
    )
