"""Role libraries for the ``script/ao`` Python sources.

One call from the root ``BUILD.bazel`` defines every library in the root
package: no package is created under ``script/ao``, so the existing root file
labels keep resolving. The roles follow the real one-level package split
(``core``, ``policy``, ``graph``, ``portal``, ``command``) so a test or tool
pays only for the import closure it really uses; ``//:ao_python`` itself
remains the compatibility umbrella.

The membership lists are exact and complete. The macro fails analysis when an
existing ``script/ao/**/*.py`` file is missing from the role lists, and a
listed file that does not exist fails as a broken label, so a new module
cannot silently widen somebody's import closure and a typo cannot silently
drop one.

Declared role edges follow the import graph and stay acyclic:

    foundations <- policy <- graph <- portal <- commands
                                      portal <- check_entries

The completion library sits between portal and commands; ordinary portal
consumers do not need the completion driver. The standalone AppKit smoke
supervisor shares the real graph package initializer, not its tooling role.

The graph role carries the official runfiles library because the live-checkout
companion and the lint-result reader resolve their declared runfiles through
it. Portal modules still reach the command package: ``build_env`` imports the
parser and the command registry lazily, and ``windows_portal`` imports the
dispatch entry at module load. Those backedges are source cuts tracked outside
this metadata, so a consumer that loads ``windows_portal`` also needs the
command role in its runfiles. The graph role declares no runtime data: the
lint fixture metadata arrives as declared action inputs from
``//tool/lint:verification`` and the fixture source filegroup, not as a
``py_library`` data dependency.

``ao_python_live_checkout`` is the narrow shared root primitive (standard
library plus the official runfiles library) that the validation label and the
Bazel entry points depend on.

Most tooling tests depend on coarse package roles. Native test launchers,
lint readers, and live-checkout entry points use narrower libraries so their
runfiles follow the imports of the corresponding consumer.
"""

load("@rules_python//python:defs.bzl", "py_library")

_FOUNDATIONS = [
    "script/ao/core/__init__.py",
    "script/ao/core/bazel_native.py",
    "script/ao/core/build_lock.py",
    "script/ao/core/command_registry.py",
    "script/ao/core/jobs.py",
    "script/ao/core/paths.py",
    "script/ao/core/phases.py",
    "script/ao/core/proc.py",
]

_POLICY = [
    "script/ao/policy/__init__.py",
    "script/ao/policy/bazel_contract.py",
    "script/ao/policy/compiler_policy.py",
    "script/ao/policy/concept_scope.py",
    "script/ao/policy/dedup.py",
    "script/ao/policy/dependency_policy.py",
    "script/ao/policy/lint_toolchain.py",
    "script/ao/policy/lint_fixtures.py",
    "script/ao/policy/lint_ownership.py",
    "script/ao/policy/tidy_config.py",
    "script/ao/policy/tidy_diagnostics.py",
    "script/ao/policy/tidy_policy.py",
    "script/ao/policy/windows_llvm_sdk.py",
]

_GRAPH = [
    "script/ao/graph/bazel_graph_lint.py",
    "script/ao/graph/bazel_graph_lint_gate.py",
    "script/ao/graph/bazel_lint_action.py",
    "script/ao/graph/bazel_lint_fixture.py",
    "script/ao/graph/bazel_lint_reports.py",
    "script/ao/graph/bazel_lint_result.py",
    "script/ao/graph/bazel_lint_schema.py",
    "script/ao/graph/bazel_live_checkout.py",
    "script/ao/graph/bazel_test_context.py",
    "script/ao/graph/bazel_test_runner.py",
    "script/ao/graph/bazel_test_services.py",
    "script/ao/graph/bazel_unittest.py",
    "script/ao/graph/lint_test.py",
]

_PORTAL = [
    # Standalone completion-audit CLI shared by Bazel and the portal.
    "script/ao/guardrails.py",
    "script/ao/portal/__init__.py",
    "script/ao/portal/bazel_attempt.py",
    "script/ao/portal/bazel_check_tools.py",
    "script/ao/portal/bazel_checker_arch.py",
    "script/ao/portal/bazel_ci_summary.py",
    "script/ao/portal/bazel_compiler_probe.py",
    "script/ao/portal/bazel_concepts.py",
    "script/ao/portal/bazel_coverage.py",
    "script/ao/portal/bazel_dependencies.py",
    "script/ao/portal/bazel_deps.py",
    "script/ao/portal/bazel_environment.py",
    "script/ao/portal/bazel_headers.py",
    "script/ao/portal/bazel_host_tools.py",
    "script/ao/portal/bazel_lint.py",
    "script/ao/portal/bazel_lint_sdk.py",
    "script/ao/portal/bazel_native_smoke.py",
    "script/ao/portal/bazel_profiles.py",
    "script/ao/portal/bazel_providers.py",
    "script/ao/portal/bazel_provision.py",
    "script/ao/portal/bazel_remote_cache.py",
    "script/ao/portal/bazel_resolver.py",
    "script/ao/portal/bazel_test_ownership.py",
    "script/ao/portal/bazel_validation.py",
    "script/ao/portal/bazel_verify.py",
    "script/ao/portal/bazel_winui.py",
    "script/ao/portal/bazel_workflows.py",
    "script/ao/portal/bazel_workspace.py",
    "script/ao/portal/build_dir.py",
    "script/ao/portal/build_env.py",
    "script/ao/portal/buildifier.py",
    "script/ao/portal/compiler_rc.py",
    "script/ao/portal/concept_report.py",
    "script/ao/portal/contract_checks.py",
    "script/ao/portal/dependency_checks.py",
    "script/ao/portal/doc_check.py",
    "script/ao/portal/documentation_checks.py",
    "script/ao/portal/git_files.py",
    "script/ao/portal/guardrails.py",
    "script/ao/portal/host_checks.py",
    "script/ao/portal/lint_scope.py",
    "script/ao/portal/measurements.py",
    "script/ao/portal/name_audit.py",
    "script/ao/portal/performance_review.py",
    "script/ao/portal/prettier.py",
    "script/ao/portal/python_check.py",
    "script/ao/portal/python_env.py",
    "script/ao/portal/r2_credentials.py",
    "script/ao/portal/ready_tool.py",
    "script/ao/portal/setup_actions.py",
    "script/ao/portal/source_analyzer.py",
    "script/ao/portal/source_format.py",
    "script/ao/portal/source_lint.py",
    "script/ao/portal/source_scope.py",
    "script/ao/portal/test_audit.py",
    "script/ao/portal/test_registry.py",
    "script/ao/portal/tidy_engine.py",
    "script/ao/portal/vcpkg_binary_cache.py",
    "script/ao/portal/windows_portal.py",
    "script/ao/portal/winui.py",
]

_COMMANDS = [
    "script/ao/__main__.py",
    "script/ao/command/__init__.py",
    "script/ao/command/check.py",
    "script/ao/command/format.py",
    "script/ao/command/measure.py",
    "script/ao/command/options.py",
    "script/ao/command/setup.py",
]

_CHECK_ENTRIES = [
    "script/ao/portal/bazel_repository_checks.py",
]

# Runtime inputs read by the role's own modules, next to their sources.
# compiler_policy reads the shared and MSVC flag policies from the bazel
# package; dependency_policy reads the dependency contract and
# windows_llvm_sdk reads its admitted LLVM manifest.
_POLICY_DATA = [
    "script/ao/windows-llvm-sdk.json",
    "//bazel:compiler_policy.bzl",
    "//bazel:msvc_policy.bzl",
    "//:config/dependency-contract.json",
]

# The provisioning, toolchain and host-provider modules read the pinned
# manifests and requirement locks next to their sources; compiler_rc parses
# the MSVC flag policy and the Bazel host providers read the dependency
# contract for their evidence. Without the declared data a Windows copied
# runfiles tree cannot resolve them.
_PORTAL_DATA = [
    "script/ao/bazel-remote.json",
    "script/ao/bazel-toolchain.json",
    "script/ao/buildifier.json",
    "script/ao/docs-requirements.txt",
    "script/ao/macos-requirements.txt",
    "script/ao/macos-toolchain.json",
    "script/ao/prettier.json",
    "script/ao/toolchain.json",
    "script/ao/windows-requirements.txt",
    "//bazel:msvc_policy.bzl",
    "//:config/dependency-contract.json",
]

# name -> (source files, library deps)
_ROLES = {
    "ao_python_package": (["script/ao/__init__.py"], []),
    "ao_python_wheel_lock": (["script/ao/generate_python_requirements.py"], [":ao_python_package"]),
    "ao_python_foundations": (_FOUNDATIONS, [":ao_python_package"]),
    "ao_python_policy": (_POLICY, [":ao_python_foundations"]),
    "ao_python_graph_package": (["script/ao/graph/__init__.py"], [":ao_python_package"]),
    "ao_appkit_process": (["script/ao/graph/appkit_process.py"], [":ao_python_graph_package"]),
    "ao_python_graph": (
        _GRAPH,
        [":ao_python_foundations", ":ao_python_policy", ":ao_python_graph_package", "@rules_python//python/runfiles"],
    ),
    "ao_python_portal": (
        _PORTAL,
        [":ao_python_foundations", ":ao_python_policy", ":ao_python_graph"],
    ),
    "ao_python_completion": (["script/ao/portal/completion.py"], [":ao_python_portal"]),
    "ao_python_commands": (
        _COMMANDS,
        [
            ":ao_python_foundations",
            ":ao_python_policy",
            ":ao_python_graph",
            ":ao_python_portal",
            ":ao_python_completion",
        ],
    ),
    "ao_python_check_entries": (
        _CHECK_ENTRIES,
        # Repository targets call execution services directly, never CLI adapters.
        # The graph companion preserves the physical live-checkout boundary.
        [":ao_python_graph", ":ao_python_portal"],
    ),
}

_ROLE_DATA = {
    "ao_python_policy": _POLICY_DATA,
    "ao_python_portal": _PORTAL_DATA,
}

# Narrow labels with an exact, minimal closure for one consumer each. Their
# sources also belong to a role above; a source may back several targets. The
# roles stay the coarse import buckets while these labels are the real narrow
# dependencies for their consumers.
_NATIVE_TEST_SUPPORT = [
    "script/ao/core/__init__.py",
    "script/ao/core/bazel_native.py",
    "script/ao/core/paths.py",
    "script/ao/graph/bazel_test_runner.py",
    "script/ao/graph/bazel_test_services.py",
]

# The UIA admission probes read only these modules; keep the closure minimal
# so Windows test targets do not drag the portal or command layers in. The
# package initializers are real sources, not Bazel's implicit empty package.
_WINUI_ADMISSION = [
    "script/ao/core/__init__.py",
    "script/ao/core/paths.py",
    "script/ao/policy/__init__.py",
    "script/ao/policy/dependency_policy.py",
    "script/ao/portal/__init__.py",
    "script/ao/portal/build_dir.py",
    "script/ao/portal/winui.py",
]

# Reading the portal's BEP and diagnostic logs needs the report schema and deduplication.
_LINT_REPORT_READER = [
    "script/ao/core/__init__.py",
    "script/ao/core/paths.py",
    "script/ao/graph/__init__.py",
    "script/ao/graph/bazel_lint_reports.py",
    "script/ao/graph/bazel_lint_schema.py",
    "script/ao/policy/__init__.py",
    "script/ao/policy/dedup.py",
    "script/ao/policy/tidy_diagnostics.py",
]

# The native validation test composes the portal's live facts build, so the
# portal role is its real coarse closure; the live-checkout companion supplies
# the physical root and re-execution.
_VALIDATION = [
    "script/ao/portal/bazel_validation.py",
]

_UNITTEST_ENTRY = [
    "script/ao/graph/bazel_unittest.py",
]

# The shared physical-root primitive and live re-execution companion: one
# narrow standard-library library plus the official runfiles dependency, so
# the validation label and both entry points read the same checkout boundary
# without depending on each other.
_LIVE_CHECKOUT = [
    "script/ao/__init__.py",
    "script/ao/graph/__init__.py",
    "script/ao/graph/bazel_live_checkout.py",
]

def aobus_python_libraries():
    """Define the role libraries and the compatible ``ao_python`` umbrella."""
    existing = native.glob(["script/ao/**/*.py"])

    assigned = {}
    for role, (files, _) in _ROLES.items():
        for name in files:
            assigned[name] = assigned.get(name, 0) + 1
    duplicates = [name for name, count in sorted(assigned.items()) if count > 1]
    if duplicates:
        fail("python_libraries.bzl: sources assigned to more than one role: ", ", ".join(duplicates))
    unassigned = [name for name in sorted(existing) if name not in assigned]
    if unassigned:
        fail(
            "python_libraries.bzl: unassigned script/ao sources; add each file to exactly one role: ",
            ", ".join(unassigned),
        )

    for role, (files, deps) in _ROLES.items():
        py_library(
            name = role,
            srcs = files,
            data = _ROLE_DATA.get(role, []),
            imports = ["script"],
            deps = deps,
        )

    # Process smoke tests and the private fixture runner share only the context parser.
    py_library(
        name = "ao_native_test_context",
        srcs = [
            "script/ao/graph/__init__.py",
            "script/ao/graph/bazel_test_context.py",
        ],
        imports = ["script"],
        deps = [":ao_python_package"],
    )

    py_library(
        name = "ao_native_test_support",
        srcs = _NATIVE_TEST_SUPPORT,
        imports = ["script"],
        deps = [
            ":ao_native_test_context",
            "@rules_python//python/runfiles",
        ],
    )

    py_library(
        name = "ao_winui_admission",
        srcs = _WINUI_ADMISSION,
        # winui.require_runtime reads the Windows App Runtime contract; the
        # .vsconfig belongs to required_components, not UIA admission, so it
        # stays undeclared here.
        data = [":config/dependency-contract.json"],
        imports = ["script"],
        deps = [":ao_python_package"],
    )

    py_library(
        name = "ao_lint_report_reader",
        srcs = _LINT_REPORT_READER,
        imports = ["script"],
        deps = [":ao_python_package"],
    )

    py_library(
        name = "ao_lint_result_support",
        srcs = [
            "script/ao/graph/__init__.py",
            "script/ao/graph/bazel_lint_result.py",
        ],
        imports = ["script"],
        deps = [
            ":ao_python_package",
            "@rules_python//python/runfiles",
        ],
    )

    py_library(
        name = "ao_python_validation",
        srcs = _VALIDATION,
        imports = ["script"],
        deps = [
            ":ao_python_portal",
            ":ao_python_live_checkout",
        ],
    )

    py_library(
        name = "ao_python_unittest_entry",
        srcs = _UNITTEST_ENTRY,
        imports = ["script"],
        deps = [":ao_python_live_checkout"],
    )

    py_library(
        name = "ao_python_live_checkout",
        srcs = _LIVE_CHECKOUT,
        imports = ["script"],
        deps = ["@rules_python//python/runfiles"],
    )

    # The compatibility umbrella: every role plus the shared runtime data
    # inputs. The root ``ao_python_sources`` filegroup is the declared AST
    # inventory and compatibility all-source input for tooling and the
    # package-boundary audit; the lint fixture action uses its own narrow
    # source filegroup, and the libraries do not duplicate either.
    py_library(
        name = "ao_python",
        data = native.glob([
            "script/ao/*.json",
            "script/ao/*requirements.txt",
        ]),
        imports = ["script"],
        deps = [":" + role for role in _ROLES],
    )

def _portal_initializer_runfiles_impl(ctx):
    runfiles = ctx.attr.subject[DefaultInfo].default_runfiles
    initializer = ctx.file.initializer
    empty = runfiles.empty_filenames.to_list()
    path = initializer.short_path
    return [AnalysisTestResultInfo(
        success = initializer in runfiles.files.to_list() and path not in empty and ctx.workspace_name + "/" + path not in empty,
        message = "The portal module's runfiles must include its real source initializer, not a synthetic empty file.",
    )]

# Guard the project-side data declaration: Bazel 9.2 sandbox reuse can
# truncate a source initializer when a later test requests an empty one.
portal_initializer_runfiles_test = rule(
    implementation = _portal_initializer_runfiles_impl,
    analysis_test = True,
    attrs = {
        "initializer": attr.label(allow_single_file = True, mandatory = True),
        "subject": attr.label(mandatory = True),
    },
)

def _isolated_python_runfiles_impl(ctx):
    runfiles = ctx.attr.subject[DefaultInfo].default_runfiles.files.to_list()
    unexpected = sorted([file.short_path for file in runfiles if file.short_path in ctx.attr.excluded_paths])
    return [AnalysisTestResultInfo(
        success = not unexpected,
        message = "%s should not ship unrelated Python sources: %s" % (ctx.attr.subject.label, ", ".join(unexpected)),
    )]

isolated_python_runfiles_test = rule(
    implementation = _isolated_python_runfiles_impl,
    analysis_test = True,
    attrs = {
        "excluded_paths": attr.string_list(mandatory = True),
        "subject": attr.label(mandatory = True),
    },
)

def aobus_assert_unit_modules(declared):
    """Fail analysis unless the declared unit modules match the glob exactly.

    The calling package's ``test_*.py`` glob is the authority: a new test
    module must be added to exactly one category dict in the calling BUILD
    file with its audited sources, deps, and data, and a module listed twice
    is just as wrong. Analysis fails loudly either way; nothing is skipped or
    double-declared silently.

    Args:
      declared: the unit module names from every category dict of the
        calling BUILD file.
    """
    counts = {}
    for module in declared:
        counts[module] = counts.get(module, 0) + 1
    duplicates = sorted([module for module, count in counts.items() if count > 1])
    if duplicates:
        fail("BUILD.bazel: modules declared in more than one unit category: ", ", ".join(duplicates))
    undeclared = sorted([name[:-3] for name in native.glob(["test_*.py"]) if name[:-3] not in counts])
    if undeclared:
        fail(
            "BUILD.bazel: undeclared test modules; add each to one unit category with audited deps: ",
            ", ".join(undeclared),
        )
