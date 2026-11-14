"""Governed native profile settings, admission and lazily generated platforms.

The profile platforms are declared by the existing environment repository's
generated BUILD, so unconfigured queries never trigger native SDK admission.
Unix release IPO and sampling are opt-in toolchain features appended through
.bazelrc --features entries; Windows Release IPO is target-platform flags on
the standard MSVC toolchain. This file is fixed local policy, not a registry.
"""

_ProfileRequestInfo = provider(
    doc = "Selected boolean profile build-setting value for admission checks.",
    fields = ["enabled"],
)

def _request_impl(ctx):
    return [_ProfileRequestInfo(enabled = ctx.build_setting_value)]

profile_request_flag = rule(
    implementation = _request_impl,
    build_setting = config.bool(flag = True),
)

def _admission_impl(_target, ctx):
    release = ctx.attr._release_ipo[_ProfileRequestInfo].enabled
    sampling = ctx.attr._sampling[_ProfileRequestInfo].enabled
    if release and sampling:
        fail("Release IPO and the sampling profile cannot be enabled together")
    if not release and not sampling:
        return []
    if ctx.configuration.coverage_enabled:
        fail("Native profiles and coverage instrumentation must use separate runs")
    if ctx.var.get("COMPILATION_MODE") != "opt":
        fail("Aobus native profiles require -c opt")
    features = ctx.features
    selected = "aobus_release_ipo" if release else "aobus_sampling"
    other = "aobus_sampling" if release else "aobus_release_ipo"
    if "-" + selected in features:
        fail("The selected profile feature must not be explicitly disabled: " + selected)
    if other in features:
        fail("A native profile request cannot enable the other profile feature: " + other)
    if "thin_lto" in features or "full_lto" in features:
        fail("Aobus native profiles use the driver-managed policy, not Bazel's thin/full LTO pipeline")
    windows = ctx.target_platform_has_constraint(ctx.attr._windows[platform_common.ConstraintValueInfo])
    if sampling:
        if windows:
            fail("The sampling profile is not supported by the selected MSVC Windows toolchain")
        constraint = ctx.attr._sampling_profile
    else:
        constraint = ctx.attr._release_profile
    if not ctx.target_platform_has_constraint(constraint[platform_common.ConstraintValueInfo]):
        fail("A native profile request requires its governed target platform")

    # Unix IPO/sampling are appended rc features; a hand-composed bare request
    # without the selected feature would silently build ordinary opt output.
    # Windows Release IPO comes from the release platform's target flags instead.
    if not windows and selected not in features:
        fail("A Unix native profile request must enable its selected toolchain feature: " + selected)
    return []

profile_admission = aspect(
    implementation = _admission_impl,
    fragments = ["cpp"],
    attrs = {
        "_release_ipo": attr.label(default = Label("//bazel:release_ipo_enabled")),
        "_sampling": attr.label(default = Label("//bazel:sampling_enabled")),
        "_release_profile": attr.label(default = Label("//bazel:release_profile")),
        "_sampling_profile": attr.label(default = Label("//bazel:sampling_profile")),
        "_windows": attr.label(default = Label("@platforms//os:windows")),
    },
)

def _numeric_version(value, parts):
    fields = value.split(".")
    return len(fields) == parts and all([field and all([character in "0123456789" for character in field.elems()]) for field in fields])

def _windows_per_file_exception(vc_tools_version):
    # The selected VC directory tuple (14.51.*) admits the exact authored
    # exception; the servicing cl.exe version is not a second invented tuple.
    fields = vc_tools_version.split(".")
    return len(fields) >= 2 and fields[0] == "14" and fields[1] == "51"

_WINDOWS_PER_FILE_SOURCE = "app/windows-winui/MainWindow.xaml.cpp"

def build_profile_platforms(host_constraints, vc_tools_version = None, deployment = None):
    """Declare the governed native profile platforms from admitted raw facts.

    Args:
        host_constraints: Host constraint labels retaining the native OS and CPU.
        vc_tools_version: Selected Windows VCToolsVersion (14.x.y) or None.
        deployment: Governed macOS deployment target (x.y) or None.
    """
    linux = "@platforms//os:linux" in host_constraints
    macos = "@platforms//os:macos" in host_constraints or "@platforms//os:osx" in host_constraints
    windows = "@platforms//os:windows" in host_constraints
    if (linux and macos) or (linux and windows) or (macos and windows) or not (linux or macos or windows):
        fail("build_profile_platforms requires exactly one native host OS constraint")
    if linux and (vc_tools_version != None or deployment != None):
        fail("The Linux profile platforms admit no Windows or macOS facts")
    if macos and (vc_tools_version != None or deployment == None or not _numeric_version(deployment, 2)):
        fail("The macOS profile platforms require the governed deployment target")
    if windows and (deployment != None or vc_tools_version == None or not _numeric_version(vc_tools_version, 3)):
        fail("The Windows profile platforms require the selected VCToolsVersion")
    deployment_flags = [] if not macos else [
        "--macos_minimum_os=" + deployment,
        "--host_macos_minimum_os=" + deployment,
    ]
    native.platform(
        name = "debug_platform",
        # Bazel adds this tag during platform analysis. Declare it as well so
        # query and configured ownership observe the same metadata.
        tags = ["manual"],
        constraint_values = host_constraints + ["@aobus//bazel:debug_profile"],
        flags = list(deployment_flags),
    )
    release_flags = list(deployment_flags) + ["--compilation_mode=opt"]
    if windows:
        # Bazel 9.2 platform flags replace all previous values of repeatable
        # flags; the portal rejects forwarded --cxxopt/--linkopt/--per_file_copt
        # for Windows Release instead of silently discarding or merging them.
        # The standard MSVC baseline stays in BAZEL_CXXOPTS/BAZEL_COPTS repo
        # environment, which these target-platform flags do not wipe.
        release_flags = release_flags + ["--cxxopt=/GL", "--linkopt=/LTCG"]
        if _windows_per_file_exception(vc_tools_version):
            release_flags.append(
                "--per_file_copt=^" + _WINDOWS_PER_FILE_SOURCE.replace(".", "[.]") + "$@/GL-",
            )
    native.platform(
        name = "release_platform",
        tags = ["manual"],
        constraint_values = host_constraints + ["@aobus//bazel:release_profile"],
        flags = release_flags,
    )
    if not windows:
        # A direct bare Windows sampling request has no generated platform and
        # must fail platform resolution, not fall back to an ordinary opt build.
        native.platform(
            name = "profile_platform",
            tags = ["manual"],
            constraint_values = host_constraints + ["@aobus//bazel:sampling_profile"],
            flags = list(deployment_flags) + ["--compilation_mode=opt"],
        )
