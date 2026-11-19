"""Native Bazel graph policy validation from folded analysis facts.

The standard py_test consumes one or more folded native-policy fact files
(rlocation keys or paths on the command line) and reuses the existing first-party
compile/link/archive flag oracles and the lexical Catch2 registration detector,
so the daily graph keeps the real native policy audit without any runtime Bazel,
aquery, query or graph schema. Each fact file names its own platform, mode,
profile and frontend toggles from the build configuration that produced it, and
is validated against that matrix; several files analyze several configurations
of the same graph in one test run. The ``ao check contracts`` command builds the same
fact producer under a caller-requested configuration and runs the same shared
validation below, so no second oracle exists.

The source audits read the physical checkout: every discovered registration
needs an executable owner from the folded owned-source closures (the executed
test roots or the built product roots; a ``cc_binary`` an executed root requires
is already built there), and every authored C++ file needs a declaring native
owner. Sources the configured universe cannot reach are deferred only by the
existing tidy-engine platform policy or by a frontend boundary that every
supplied fact file disables; compatible orphans always fail. Nothing silently
claims uncovered ground. The configured authored inventory also runs the
existing lexical stale-NOLINT oracle, because clang-tidy itself never emits the
checks a file's mode disables.

``--driver-only`` is the separate Linux wrapper diagnostic: it expands one
representative checkout source through the SDK compiler the captured command
line declares, resolved from the test's runfiles instead of a runtime Bazel
query, and keeps its streams under the test's declared output directory.
"""

from __future__ import annotations

import json
import os
import re
import subprocess
import sys
from collections.abc import Mapping, Sequence
from pathlib import Path
from typing import Any

from ao.core.bazel_native import BazelWorkspaceError
from ao.core.paths import absolute_path
from ao.graph.bazel_live_checkout import project_root
from ao.policy import bazel_contract
from ao.policy.tidy_policy import classify_relative, find_stale_nolint_suppressions, mode_buckets, platform_incompatible
from ao.portal import bazel_test_ownership, bazel_verify
from ao.portal.git_files import git_command

_SCHEMA = 1
_DOCUMENT_KEYS = frozenset(
    {
        "schema",
        "platform",
        "mode",
        "profile",
        "gtk_enabled",
        "system_media_enabled",
        "builtin_include_roots",
        "records",
        "executed",
        "built",
    }
)
_RECORD_KEYS = frozenset(
    {
        "label",
        "kind",
        "is_tool",
        "configuration",
        "quote_includes",
        "winui_projections",
        "declared",
        "compiles",
        "native_actions",
    }
)
_COMPILE_KEYS = frozenset({"argv", "env", "spa_inputs"})
_NATIVE_KEYS = frozenset({"mnemonic", "argv", "outputs"})
_EXECUTABLE_KEYS = frozenset({"label", "kind", "is_tool", "sources"})
_PLATFORMS = frozenset({"linux", "macos", "windows"})
_MODES = frozenset({"dbg", "opt", "fastbuild"})
_PROFILES = frozenset({None, "release", "sampling"})

# The lint policy's authored C++ vocabulary: translation units, headers and
# include fragments. The audits share it instead of restating a file taxonomy.
_CPP_SUFFIXES = (".c", ".cc", ".cpp", ".cxx", ".mm", ".h", ".hh", ".hpp", ".hxx", ".def")
_C_CPP_SOURCE_SUFFIXES = (".c", ".cc", ".cpp", ".cxx", ".mm")

# The separate Linux wrapper diagnostic: one representative checkout source per
# fact file, expanded through the declared SDK compiler from the runfiles.
_DRIVER_FLAG = "--driver-only"
_DRIVER_SOURCE_SUFFIXES = (".cpp", ".cc", ".cxx")
_DRIVER_EVIDENCE_DIRECTORY = "native-driver-policy"
_EXTERNAL_PREFIX = "external/"

# The frontend roots whose aggregate membership the two product toggles gate.
# The prefixes mirror the tidy engine's linux-owned path vocabulary, split per
# frontend; a disabled frontend's missing files are reported as deferred, never
# silently excused, and an enabled frontend keeps every one of them an error.
_GTK_FRONTEND_PREFIXES = ("app/linux-gtk/", "test/linux-gtk/")
_SYSTEM_MEDIA_FRONTEND_PREFIXES = (
    "app/include/ao/systemmedia/linux/",
    "app/systemmedia/linux/",
    "test/systemmedia/linux/",
)


def _rlocation(key: str, environ: Mapping[str, str], what: str = "native policy fact file") -> Path:
    """Resolve declared data with the official runfiles API, or a diagnostic path."""
    candidate = Path(key)
    if candidate.is_file():
        return candidate
    from python.runfiles import Runfiles

    runfiles = Runfiles.Create(dict(environ))
    located = runfiles.Rlocation(key) if runfiles is not None else None
    if located and Path(located).is_file():
        return Path(located)
    raise ValueError("cannot resolve " + what + ": " + key)


def _strings(value: Any, label: str) -> list[str]:
    if not isinstance(value, list) or any(not isinstance(item, str) for item in value):
        raise ValueError("native policy facts require a string list: " + label)
    return value


def _record_entry(entry: dict[str, Any], text: str) -> None:
    if set(entry) != _RECORD_KEYS:
        raise ValueError("folded record keys drifted: " + text)
    if not isinstance(entry["label"], str) or not isinstance(entry["kind"], str):
        raise ValueError("folded records require label and kind strings: " + text)
    if not isinstance(entry["is_tool"], bool) or not isinstance(entry["configuration"], str):
        raise ValueError("folded records require a boolean role and configuration path: " + text)
    _strings(entry["quote_includes"], "quote_includes")
    _strings(entry["declared"], "declared")
    projections = entry["winui_projections"]
    if not isinstance(projections, list) or any(
        len(_strings(pair, "winui_projections")) != 2 or not all(pair) or pair[0].casefold() == pair[1].casefold()
        for pair in projections
    ):
        raise ValueError("expected distinct project and SDK WinUI projection roots: " + text)
    for compile in entry["compiles"]:
        if not isinstance(compile, dict) or set(compile) != _COMPILE_KEYS:
            raise ValueError("folded compile keys drifted: " + text)
        if not _strings(compile["argv"], "argv") or not compile["argv"]:
            raise ValueError("folded compiles require a nonempty command line: " + text)
        _strings(compile["spa_inputs"], "spa_inputs")
        if not isinstance(compile["env"], dict) or any(
            not isinstance(key, str) or not isinstance(value, str) for key, value in compile["env"].items()
        ):
            raise ValueError("folded compile environments must map strings to strings: " + text)
    for action in entry["native_actions"]:
        if not isinstance(action, dict) or set(action) != _NATIVE_KEYS:
            raise ValueError("folded native action keys drifted: " + text)
        if not isinstance(action["mnemonic"], str) or not action["mnemonic"]:
            raise ValueError("folded native actions require a mnemonic: " + text)
        if not _strings(action["argv"], "argv") or not action["argv"]:
            raise ValueError("folded native actions require a nonempty command line: " + text)
        if not _strings(action["outputs"], "outputs") or not action["outputs"]:
            raise ValueError("folded native actions require declared outputs: " + text)


def _executable_entry(entry: dict[str, Any], text: str) -> None:
    if set(entry) != _EXECUTABLE_KEYS:
        raise ValueError("folded executable keys drifted: " + text)
    bazel_test_ownership._label(entry["label"])
    if entry["kind"] not in ("cc_binary", "cc_test"):
        raise ValueError("folded executables must be cc_binary or cc_test: " + text)
    if not isinstance(entry["is_tool"], bool):
        raise ValueError("folded executables require a boolean role: " + text)
    _strings(entry["sources"], "sources")


def _facts_document(path: Path) -> dict[str, Any]:
    """Read one folded fact file, failing closed on any schema drift."""
    document = json.loads(path.read_text(encoding="utf-8"))
    if not isinstance(document, dict) or set(document) != _DOCUMENT_KEYS:
        raise ValueError("native policy facts must hold exactly the documented keys: " + str(path))
    if document["schema"] != _SCHEMA:
        raise ValueError("unsupported native policy facts schema: " + str(path))
    if document["platform"] not in _PLATFORMS or document["mode"] not in _MODES:
        raise ValueError("native policy facts require a native platform and Bazel mode: " + str(path))
    if document["profile"] not in _PROFILES:
        raise ValueError("unsupported native policy profile: " + str(path))
    if not isinstance(document["gtk_enabled"], bool) or not isinstance(document["system_media_enabled"], bool):
        raise ValueError("native policy facts require boolean frontend toggles: " + str(path))
    _strings(document["builtin_include_roots"], "builtin_include_roots")
    for name in ("records", "executed", "built"):
        entries = []
        for item in _strings(document[name], name):
            entry = json.loads(item)
            if not isinstance(entry, dict):
                raise ValueError("folded native policy entries must be objects: " + item)
            if name == "records":
                _record_entry(entry, item)
            else:
                _executable_entry(entry, item)
            entries.append(entry)
        document[name] = entries
    return document


def _analysis_configuration(mode: str, profile: str | None) -> str:
    """Name the admitted analysis configuration; plain opt was never admitted."""
    if profile is None:
        if mode == "dbg":
            return "debug"
        if mode == "fastbuild":
            return "fastbuild"
    elif mode == "opt":
        if profile == "release":
            return "release"
        if profile == "sampling":
            return "profile"
    raise ValueError(f"unsupported native policy analysis configuration: {mode} {profile}")


def _explicit_cpp_language(argv: Sequence[str]) -> bool:
    """Whether the command line explicitly selects the C++ language."""
    iterator = iter(argv)
    for argument in iterator:
        if argument in {"-x", "--language"}:
            if next(iterator, "") == "c++":
                return True
        elif argument.startswith(("-x", "--language=")):
            value = argument.split("=", 1)[1] if "=" in argument else argument[2:]
            if value == "c++":
                return True
    return False


def _c_only_compile(argv: Sequence[str]) -> bool:
    """A positively identified C translation unit, never a C++ one.

    Generated GLib resource sources are C. The compile policy is C++-only, so
    they are excluded only when the command line names a ``.c`` source and no
    explicit C++ language or standard switch. A ``.c`` file compiled with
    ``-std=c++...`` keeps its C++ policy, including a missing-standard failure.
    """
    source = next((argument for argument in argv if argument.endswith(_C_CPP_SOURCE_SUFFIXES)), None)
    if source is None or not source.endswith(".c"):
        return False
    if any(re.fullmatch(r"--?std=c\+\+.*", argument) for argument in argv):
        return False
    return not _explicit_cpp_language(argv)


def _compile_actions(records: Sequence[dict[str, Any]]) -> list[bazel_contract.CompileAction]:
    actions = []
    for record in records:
        for compile in record["compiles"]:
            if _c_only_compile(compile["argv"]):
                continue
            actions.append(
                bazel_contract.CompileAction(
                    label=record["label"],
                    argv=tuple(compile["argv"]),
                    inputs=frozenset(compile["spa_inputs"]),
                    quote_includes=frozenset(record["quote_includes"]),
                    winui_projections=tuple((project, sdk) for project, sdk in record["winui_projections"]),
                    environment=tuple(sorted(compile["env"].items())),
                    is_tool=record["is_tool"],
                )
            )
    return actions


def _native_actions(records: Sequence[dict[str, Any]]) -> list[bazel_contract.NativeAction]:
    actions = []
    identities: set[tuple[str, str, str, tuple[str, ...]]] = set()
    for record in records:
        for action in record["native_actions"]:
            native = bazel_contract.NativeAction(
                label=record["label"],
                mnemonic=action["mnemonic"],
                argv=tuple(action["argv"]),
                outputs=tuple(action["outputs"]),
                is_tool=record["is_tool"],
            )
            identity = (native.label, record["configuration"], native.mnemonic, native.outputs)
            if identity in identities:
                raise ValueError("duplicate folded native action: " + str(identity))
            identities.add(identity)
            actions.append(native)
    return actions


def _windows_per_file_sources(
    platform: str, profile: str | None, *, tools_version: str | None = None
) -> frozenset[str]:
    """The exact authored Windows Release exception, from the selected SDK fact.

    The standard in-graph test inherits ``VCToolsVersion`` from its environment;
    a caller that holds the frozen request fact (the verify command's prepared
    workspace) passes it explicitly and never reads the ambient environment.
    """
    if platform != "windows" or profile != "release":
        return frozenset()
    version = tools_version if tools_version is not None else os.environ.get("VCToolsVersion")
    if not isinstance(version, str) or not re.fullmatch(r"[0-9]+\.[0-9]+\.[0-9]+", version):
        raise ValueError("Windows Release analysis requires the selected VCToolsVersion test environment fact")
    if version.split(".")[:2] == ["14", "51"]:
        return frozenset({bazel_contract.WINDOWS_PER_FILE_EXCEPTION_SOURCE})
    return frozenset()


def validate_native_policy(
    document: dict[str, Any],
    *,
    compiler_include_roots: Sequence[str] = (),
    windows_tools_version: str | None = None,
) -> list[str]:
    """Run the existing compile/link/archive oracles over one folded fact file.

    ``compiler_include_roots`` admits compiler-owned SDK include roots the
    requested profile adds (the Linux TSan interface root); the standard
    in-graph analysis passes none. ``windows_tools_version`` is the frozen
    selected VCToolsVersion fact for the Windows Release per-file exception;
    ``None`` follows the test's inherited environment as before.
    """
    platform = document["platform"]
    mode = document["mode"]
    profile = document["profile"]
    configuration = _analysis_configuration(mode, profile)
    actions = _compile_actions(document["records"])
    first_party = [action for action in actions if bazel_contract._first_party(action)]
    if not first_party:
        return ["no first-party CppCompile actions matched"]
    # An empty mapping still checks the common cleared action channels, like
    # the retired query-based verify did on every non-Linux host; None would
    # silently drop those ambient-channel assertions.
    expected_environment: dict[str, str] | None = {}
    expected_tool_environment: dict[str, str] | None = None
    expected_archive: str
    expected_tool_archive: str | None = None
    gcc_version: str | None = None
    if platform == "linux":
        # The captured target actions name the selected compiler; the salted
        # target policy, the archivers and the GCC version fact follow that
        # primary family. Execution stays explicit GCC.
        primary = bazel_contract._primary_compiler([action for action in first_party if not action.is_tool])
        expected_environment = bazel_verify._expected_hardening_environment(configuration, compiler=primary)
        expected_tool_environment = bazel_verify._expected_hardening_environment("release", compiler="gcc")
        expected_archive = bazel_verify._expected_archive(platform, compiler=primary)
        expected_tool_archive = bazel_verify._expected_archive(platform, compiler="gcc")
        gcc_version = bazel_verify._gcc_version()
    else:
        expected_archive = bazel_verify._expected_archive(platform)
    errors = bazel_contract.compile_contract_errors(
        actions,
        platform,
        mode,
        compiler_include_roots=[*document["builtin_include_roots"], *compiler_include_roots],
        require_spa=platform == "linux",
        expected_environment=expected_environment,
        expected_tool_environment=expected_tool_environment,
        profile=profile,
        windows_per_file_sources=_windows_per_file_sources(platform, profile, tools_version=windows_tools_version),
        gcc_version=gcc_version,
    )
    if platform == "windows" and not any(action.winui_projections for action in first_party if not action.is_tool):
        errors.append("no native WinUI projection consumer in the configured Windows product graph")
    native_errors, _counts = bazel_contract.link_archive_contract_errors(
        _native_actions(document["records"]),
        platform,
        profile,
        expected_archive=expected_archive,
        expected_tool_archive=expected_tool_archive,
        mode=mode,
    )
    return errors + native_errors


def _frontend_deferred(path: str, documents: Sequence[dict[str, Any]]) -> str | None:
    """Defer only a frontend every supplied fact file disables.

    One disabled configuration cannot excuse a source another analyzed
    configuration still enables, and an empty document set defers nothing.
    """
    if not documents:
        return None
    if all(not document["gtk_enabled"] for document in documents) and path.startswith(_GTK_FRONTEND_PREFIXES):
        return "gtk frontend disabled"
    if all(not document["system_media_enabled"] for document in documents) and path.startswith(
        _SYSTEM_MEDIA_FRONTEND_PREFIXES
    ):
        return "system-media frontend disabled"
    return None


def _observed_platform(documents: Sequence[dict[str, Any]]) -> str:
    """The one native platform the supplied facts analyze; fail closed on drift.

    Deferral is a property of the analyzed graph, never of the machine running
    the audit: the declared platform names the platform that owns every
    non-deferrable source, so no ambient host profile, build root or checkout
    identity is ever resolved here.
    """
    platforms: set[str] = {document["platform"] for document in documents}
    if len(platforms) != 1:
        raise ValueError("native policy facts must share one native platform: " + str(sorted(platforms)))
    return platforms.pop()


def _deferred_reason(path: str, platform: str, documents: Sequence[dict[str, Any]]) -> str | None:
    """Defer only by the declared platform policy and disabled frontends."""
    if platform_incompatible(path, platform):
        return "owned by another native platform"
    return _frontend_deferred(path, documents)


def registration_audit(documents: Sequence[dict[str, Any]], root: Path) -> tuple[list[str], dict[str, str]]:
    """Require a folded executable owner for every discovered registration."""
    platform = _observed_platform(documents)
    registrations = bazel_test_ownership.discover_registration_sources(root)
    owners: dict[str, set[str]] = {}
    kinds: dict[str, str] = {}
    executed: set[str] = set()
    built: set[str] = set()
    for document in documents:
        for role, reached in (("executed", executed), ("built", built)):
            for entry in document[role]:
                label = bazel_test_ownership._label(entry["label"])
                kinds[label] = entry["kind"]
                reached.add(label)
                for source in entry["sources"]:
                    owners.setdefault(source, set()).add(label)
    errors: list[str] = []
    deferred: dict[str, str] = {}
    for source in sorted(registrations):
        if source not in owners:
            reason = _deferred_reason(source, platform, documents)
            if reason is None:
                errors.append(f"{source}: no declared executable owner")
            else:
                deferred[source] = reason
            continue
        for label in sorted(owners[source]):
            if kinds[label] == "cc_test":
                if label not in executed:
                    errors.append(f"{label}: test is not in the executed test selection")
            elif label not in built and label not in executed:
                errors.append(f"{label}: test executable is absent from the completion roots")
    return errors, deferred


def _authored_cpp_inventory(manifest: bytes, root: Path) -> list[str]:
    """Decode one NUL-delimited git manifest into the authored C++ inventory.

    ``git ls-files --cached --others --exclude-standard -z`` names tracked and
    untracked checkout files without shell quoting, so a name may contain
    spaces, newlines or non-UTF-8 bytes. Only applicable authored C++ paths
    outside the shared lint policy's ignored tooling paths, and present on
    disk, survive; the manifest's trailing NUL decodes to no name.
    """
    return sorted(
        name
        for name in os.fsdecode(manifest).split("\0")
        if name.endswith(_CPP_SUFFIXES)
        and classify_relative(name, explicit=False) != "IGNORE"
        and (root / name).is_file()
    )


def _authored_cpp_manifest(root: Path) -> list[str]:
    """Every authored C++ file outside the shared lint policy's ignored paths."""
    completed = subprocess.run(
        git_command("ls-files", "--cached", "--others", "--exclude-standard", "-z", source_discovery=True),
        cwd=root,
        capture_output=True,
        check=True,
    )
    return _authored_cpp_inventory(completed.stdout, root)


def _declared_sources(documents: Sequence[dict[str, Any]]) -> set[str]:
    declared: set[str] = set()
    for document in documents:
        for record in document["records"]:
            declared.update(record["declared"])
    return declared


def authored_cpp_audit(
    documents: Sequence[dict[str, Any]], manifest: Sequence[str]
) -> tuple[list[str], dict[str, str]]:
    """Require a declaring native owner for every authored C++ file."""
    declared = _declared_sources(documents)
    platform = _observed_platform(documents)
    errors: list[str] = []
    deferred: dict[str, str] = {}
    for path in manifest:
        if path in declared:
            continue
        reason = _deferred_reason(path, platform, documents)
        if reason is None:
            errors.append(path)
        else:
            deferred[path] = reason
    return errors, deferred


def stale_suppression_audit(documents: Sequence[dict[str, Any]], root: Path, manifest: Sequence[str]) -> list[str]:
    """Reuse the lexical stale-NOLINT oracle over the configured inventory.

    clang-tidy never emits a check the file's mode disables, so only this
    shared lexical audit can retire suppressions that name disabled checks.
    The scanned set is exactly the authored compatible inventory the folded
    universe declares; deferred sources belong to their owning platform's gate.
    """
    declared = _declared_sources(documents)
    selected = [path for path in manifest if path in declared]
    buckets = {mode: [absolute_path(root) / path for path in names] for mode, names in mode_buckets(selected).items()}
    errors = []
    for issue in find_stale_nolint_suppressions(buckets):
        try:
            display = issue.path.relative_to(absolute_path(root)).as_posix()
        except ValueError:
            display = issue.path.as_posix()
        errors.append(f"{display}:{issue.line}: {issue.check}")
    return errors


def _print_deferred(paths: Mapping[str, str], title: str) -> None:
    if not paths:
        return
    print(f"Deferred {title} outside the configured universe:", file=sys.stderr)
    for path in sorted(paths):
        print(f"  {path} ({paths[path]})", file=sys.stderr)


def run(arguments: Sequence[str], environ: Mapping[str, str]) -> int:
    """Validate every named fact file plus the checkout-owned source audits."""
    if not arguments:
        print("Usage: bazel_validation <native-policy-facts.json>...", file=sys.stderr)
        return 2
    root = project_root(environ)
    try:
        documents = [_facts_document(_rlocation(argument, environ)) for argument in arguments]
        violations = [
            (argument, validate_native_policy(document))
            for argument, document in zip(arguments, documents, strict=True)
        ]
        for argument, errors in violations:
            if errors:
                print(f"Native policy violations ({argument}):", file=sys.stderr)
                for error in errors:
                    print(f"  {error}", file=sys.stderr)
        registration_errors, registration_deferred = registration_audit(documents, root)
        manifest = _authored_cpp_manifest(root)
        orphan_errors, orphan_deferred = authored_cpp_audit(documents, manifest)
        stale_errors = stale_suppression_audit(documents, root, manifest)
        if orphan_errors:
            print("ERROR: authored C++ sources no native owner declares:", file=sys.stderr)
            for path in orphan_errors:
                print(f"  {path}", file=sys.stderr)
            print("Declare the authored C++ sources in a native rule or remove them.", file=sys.stderr)
        if stale_errors:
            print("ERROR: stale NOLINT suppressions name checks disabled for their file mode:", file=sys.stderr)
            for suppression in stale_errors:
                print(f"  {suppression}", file=sys.stderr)
            print("Remove these suppressions; the configured check cannot emit there.", file=sys.stderr)
        if registration_errors:
            print("Test registration ownership violations:", file=sys.stderr)
            for error in registration_errors:
                print(f"  {error}", file=sys.stderr)
        _print_deferred(registration_deferred, "test registrations")
        _print_deferred(orphan_deferred, "authored C++ sources")
        if any(errors for _argument, errors in violations) or registration_errors or orphan_errors or stale_errors:
            return 1
        print(
            f"Native policy validation passed: {len(documents)} fact file(s), "
            f"{len(registration_deferred) + len(orphan_deferred)} deferred boundary file(s)."
        )
        return 0
    except (OSError, UnicodeError, ValueError, KeyError, TypeError, ImportError, subprocess.SubprocessError) as error:
        print(f"Native policy validation error: {error}", file=sys.stderr)
        return 1


def _checkout_source(action: bazel_contract.CompileAction, root: Path) -> str | None:
    """The action's first declared C++ source that exists in the checkout."""
    for argument in action.argv[1:]:
        if not argument.endswith(_DRIVER_SOURCE_SUFFIXES):
            continue
        if Path(argument).is_absolute() or argument.startswith(("bazel-out/", _EXTERNAL_PREFIX)):
            continue
        if ".." in Path(argument).parts or not (root / argument).is_file():
            continue
        return argument
    return None


def _representative_action(
    actions: Sequence[bazel_contract.CompileAction], root: Path
) -> bazel_contract.CompileAction | None:
    """The first target-configuration action with a checkout C++ source."""
    for action in actions:
        if action.is_tool or not bazel_contract._first_party(action):
            continue
        if _checkout_source(action, root) is not None:
            return action
    return None


def _sdk_compiler(value: str, environ: Mapping[str, str]) -> Path:
    """Resolve the declared SDK compiler the captured command line names.

    The native policy facts export the actual declared action input. Its
    execution-root path ``external/<canonical repo>/...`` is the runfiles key
    without that prefix, so the test's runfiles resolve the same file. An
    absolute admitted compiler path is used as captured. No artifact path is
    guessed, and the lexical driver name survives for the Nix multicall wrapper.
    """
    if Path(value).is_absolute():
        return Path(value)
    if not value.startswith(_EXTERNAL_PREFIX):
        raise ValueError("representative compiler must be a declared external SDK file: " + value)
    key = value.removeprefix(_EXTERNAL_PREFIX)
    if not key or ".." in key.split("/"):
        raise ValueError("representative compiler path must not escape the execution root: " + value)
    compiler = _rlocation(key, environ, "SDK compiler file")
    if not compiler.as_posix().endswith("/" + key):
        raise ValueError("resolved SDK compiler does not correspond to the captured path: " + value)
    return compiler


def _driver_inputs(document: dict[str, Any], root: Path) -> tuple[Any, str, str, dict[str, str]]:
    """Resolve one Linux fact file's representative driver inputs.

    Shared by the in-graph wrapper diagnostic and the verify command: the
    representative target-configuration checkout action, the admitted analysis
    configuration, the primary compiler family its captured actions name, and
    that family's salted Nix hardening policy. Compiler resolution and evidence
    retention belong to the caller.
    """
    actions = _compile_actions(document["records"])
    representative = _representative_action(actions, root)
    if representative is None:
        raise ValueError("representative driver requires a target-configuration checkout source")
    configuration = _analysis_configuration(document["mode"], document["profile"])
    target_actions = [action for action in actions if not action.is_tool and bazel_contract._first_party(action)]
    primary = bazel_contract._primary_compiler(target_actions)
    expected_environment = bazel_verify._expected_hardening_environment(configuration, compiler=primary)
    if not expected_environment:
        raise ValueError("empty Nix hardening environment policy")
    return representative, configuration, primary, expected_environment


def _driver_evidence(environ: Mapping[str, str], index: int) -> dict[str, Any]:
    """Keep one driver run's streams under the test's declared output root."""
    directory = environ.get("TEST_UNDECLARED_OUTPUTS_DIR") or environ.get("TEST_TMPDIR")
    if not directory:
        raise ValueError("representative driver requires TEST_UNDECLARED_OUTPUTS_DIR or TEST_TMPDIR")
    root = Path(directory) / _DRIVER_EVIDENCE_DIRECTORY
    root.mkdir(parents=True, exist_ok=True)
    return {
        "stdout": str(root / f"driver-{index}.stdout.log"),
        "stderr": str(root / f"driver-{index}.stderr.log"),
        "status": "unrun",
        "exit": None,
    }


def _print_driver_evidence(evidence: Mapping[str, Any]) -> None:
    for name in ("stdout", "stderr"):
        try:
            text = Path(evidence[name]).read_text(encoding="utf-8", errors="replace")
        except OSError:
            continue
        if text:
            print(f"Representative driver {name}:", file=sys.stderr)
            print(text, file=sys.stderr)
    for error in evidence.get("errors", ()):
        print(f"  {error}", file=sys.stderr)


def run_driver(arguments: Sequence[str], environ: Mapping[str, str]) -> int:
    """Expand one representative checkout compile per Linux fact file.

    The compiler is the declared SDK input the captured command line names and
    is resolved from the test's runfiles; no Bazel query, aquery or artifact
    lookup is involved. The existing driver oracle owns the verdict.
    """
    if not arguments:
        print("Usage: bazel_validation --driver-only <native-policy-facts.json>...", file=sys.stderr)
        return 2
    root = project_root(environ)
    try:
        documents = [_facts_document(_rlocation(argument, environ)) for argument in arguments]
        linux = [document for document in documents if document["platform"] == "linux"]
        if not linux:
            raise ValueError("representative driver requires a Linux native policy fact file")
        for index, document in enumerate(linux):
            representative, configuration, primary, expected_environment = _driver_inputs(document, root)
            compiler = _sdk_compiler(representative.argv[0], environ)
            evidence = _driver_evidence(environ, index)
            print(
                f"Representative {primary} driver ({configuration}); evidence: {evidence['stderr']}",
                file=sys.stderr,
                flush=True,
            )
            status = bazel_verify._verify_driver(
                representative, document["mode"], expected_environment, evidence, compiler=compiler
            )
            if status:
                print(f"Representative driver gate failed ({status}); stderr: {evidence['stderr']}", file=sys.stderr)
                _print_driver_evidence(evidence)
                return status
        print(f"Representative driver policy passed: {len(linux)} fact file(s).")
        return 0
    except (
        BazelWorkspaceError,
        OSError,
        UnicodeError,
        ValueError,
        KeyError,
        TypeError,
        ImportError,
        subprocess.SubprocessError,
    ) as error:
        print(f"Native driver policy error: {error}", file=sys.stderr)
        return 1


def main(argv: Sequence[str] | None = None) -> int:
    """Entry point for the standard in-graph validation py_test."""
    arguments = list(sys.argv[1:] if argv is None else argv)
    try:
        if arguments[:1] == [_DRIVER_FLAG]:
            return run_driver(arguments[1:], os.environ)
        return run(arguments, os.environ)
    except KeyboardInterrupt:
        return 130


if __name__ == "__main__":
    raise SystemExit(main())
