"""Host-side orchestration of native Bazel compile-contract verification.

The compile/link/archive oracles and the declared policy data they read stay in
``bazel_contract``, which never runs a process or imports the portal SDK. This
module owns the runtime half: it builds the folded native policy facts under a
frozen prepared configuration, retains every invocation's streams and exits, and
on Linux expands one representative driver. Verification is analysis, not
compilation, linking or runtime acceptance.
"""

from __future__ import annotations

import json
import os
import posixpath
import re
import shlex
import subprocess
import sys
import tempfile
from collections.abc import Mapping, Sequence
from pathlib import Path
from typing import Any

from ao.core.paths import PROJECT_ROOT
from ao.policy.bazel_contract import (
    GCC_STACK_PROTECTOR_PATTERN,
    CompileAction,
    _consumes_spa,
    _defines,
    _driver_family,
    _first_party,
    _last,
)

FACTS_TARGET = "//:native_policy_inputs"


def _driver_file(value: str) -> Path:
    path = Path(value)
    if (
        any(ord(char) < 32 or ord(char) == 127 for char in value)
        or not path.is_absolute()
        or not path.is_file()
        or not os.access(path, os.X_OK)
    ):
        raise ValueError("representative compiler must be an absolute existing executable file: " + value)
    # Do not resolve symlinks: Nix/Clang multicall wrappers depend on argv[0].
    return path


def _verify_driver(
    action: CompileAction,
    mode: str,
    expected_environment: Mapping[str, str],
    evidence: dict[str, Any],
    *,
    compiler: Path | None = None,
) -> int:
    """Expand captured argv with only the compiler path resolved, never execute cc1.

    This wrapper diagnostic runs from PROJECT_ROOT for unstaged first-party
    sources, not as a literal Bazel action in an execution root. The trace
    follows the driver's lexical family: Clang prints one -cc1 command with
    -stack-protector levels, GCC prints one cc1/cc1plus command with its own
    -fstack-protector switches. Optimization, fortify and NDEBUG keep the same
    contract for both families.
    """
    policy_keys = list(expected_environment)
    if len(policy_keys) != 1 or not re.fullmatch(r"NIX_HARDENING_ENABLE_[A-Za-z0-9_]+", policy_keys[0]):
        raise ValueError("representative driver requires one explicit salted Nix hardening policy")
    policy = expected_environment[policy_keys[0]].split()
    driver = _driver_file(str(compiler) if compiler is not None else action.argv[0])
    family = _driver_family((str(driver),))
    if family is None:
        raise ValueError("representative driver must be a selected gcc or clang driver: " + driver.name)
    evidence["hardening_policy"] = policy
    evidence["captured_argv"] = list(action.argv)
    evidence["argv"] = [str(driver), *action.argv[1:], "-###"]
    evidence["cwd"] = str(PROJECT_ROOT)
    evidence["status"] = "running"
    with Path(evidence["stdout"]).open("wb") as out, Path(evidence["stderr"]).open("wb") as err:
        result = subprocess.run(
            evidence["argv"], cwd=PROJECT_ROOT, env=dict(action.environment), stdout=out, stderr=err
        )
    evidence["exit"] = result.returncode
    evidence["status"] = "exited"
    if result.returncode:
        evidence["status"] = "driver failed"
        return result.returncode
    text = Path(evidence["stderr"]).read_text(encoding="utf-8", errors="replace")
    protector_level: int | None = None
    protector_flag: str | None = None
    if family == "gcc":
        commands = []
        for line in text.splitlines():
            try:
                words = shlex.split(line)
            except ValueError:
                continue
            if words and posixpath.basename(words[0]) in {"cc1", "cc1plus"}:
                commands.append(words)
        if len(commands) != 1:
            raise ValueError("representative GCC driver must emit exactly one cc1 command")
        cc1 = commands[0]
        protector_flag = _last(cc1, lambda flag: bool(GCC_STACK_PROTECTOR_PATTERN.fullmatch(flag)))
    else:
        commands = [
            shlex.split(line)
            for line in text.splitlines()
            if re.search(r"""(?:^|\s)(?:"-cc1"|'-cc1'|-cc1)(?:\s|$)""", line)
        ]
        if len(commands) != 1 or len(commands[0]) < 2 or commands[0][1] != "-cc1" or commands[0].count("-cc1") != 1:
            raise ValueError("representative Clang driver must emit exactly one -cc1 command")
        cc1 = commands[0]
        arguments = iter(cc1)
        for argument in arguments:
            if argument == "-stack-protector":
                value = next(arguments, "")
                if not value.isdecimal():
                    raise ValueError("malformed Clang -stack-protector value")
                protector_level = int(value)
    evidence["cc1_argv"] = cc1
    optimization = _last(cc1, lambda flag: bool(re.fullmatch(r"-O(?:[0-3sgz]|fast)", flag)))
    defines = _defines(cc1)
    fortify = defines.get("_FORTIFY_SOURCE")
    evidence["effective"] = {
        "optimization": optimization,
        "stack_protector": protector_flag if family == "gcc" else protector_level,
        "fortify_source": fortify,
    }
    errors = []
    if mode in {"dbg", "fastbuild"}:
        if optimization not in {None, "-O0"}:
            errors.append(f"representative {mode} driver must remain unoptimized")
        if fortify not in {None, "0"}:
            errors.append(f"representative {mode} driver must not enable _FORTIFY_SOURCE")
        if "NDEBUG" in defines:
            errors.append(f"representative {mode} driver must not define NDEBUG")
    elif mode == "opt":
        if optimization not in {"-O2", "-O3"}:
            errors.append("representative optimized driver requires -O2 or -O3")
        if {"fortify", "fortify3"} & set(policy) and fortify not in {"2", "3"}:
            errors.append("Nix fortify policy requires effective _FORTIFY_SOURCE=2 or 3")
    else:
        raise ValueError("representative driver requires dbg, fastbuild or opt mode")
    if "stackprotector" in policy:
        if family == "gcc":
            protected = protector_flag in {"-fstack-protector-strong", "-fstack-protector-all"}
        else:
            protected = protector_level is not None and protector_level >= 2
        if not protected:
            errors.append("Nix stackprotector policy requires effective strong/all protection")
    if defines.get("gsl_CONFIG_CONTRACT_CHECKING_ON") != "1" or "gsl_CONFIG_CONTRACT_CHECKING_OFF" in defines:
        errors.append("representative driver must retain GSL contract checking")
    evidence["errors"] = errors
    evidence["status"] = "contract failed" if errors else "passed"
    return 1 if errors else 0


def _expected_hardening_environment(config: str, compiler: str = "gcc") -> dict[str, str]:
    """Independent verification oracle, never an input to toolchain construction."""
    from ao.portal import bazel_compiler_probe

    if config not in {"debug", "fastbuild", "release", "profile"}:
        raise ValueError(f"unsupported hardening verification configuration: {config}")
    if compiler not in {"gcc", "clang"}:
        raise ValueError(f"unsupported hardening verification compiler: {compiler}")
    observed = bazel_compiler_probe.nix_cc_wrapper(compiler=compiler)
    # Check the established unoptimized/optimized contract independently of the
    # Starlark generation helper, so a wrong generated environment can fail.
    # Fastbuild keeps its own unoptimized mode and the unoptimized policy, like
    # Debug. The sampling profile reuses the Release SDK and its optimized policy.
    flags = [
        flag
        for flag in observed["hardeningEnable"]
        if config in {"release", "profile"} or flag not in {"fortify", "fortify3"}
    ]
    return {"NIX_HARDENING_ENABLE_" + observed["suffixSalt"]: " ".join(flags)}


def _expected_archive(platform: str, compiler: str = "gcc") -> str:
    """Independent selected-archiver name, never a toolchain producer value."""
    if platform == "linux":
        from ao.portal import bazel_compiler_probe

        return Path(bazel_compiler_probe.nix_cc_wrapper(compiler=compiler)["tools"]["ar"]).name
    if platform == "macos":
        # The pinned LLVM/libtool role of the governed macOS toolchain policy.
        return "llvm-libtool-darwin"
    if platform == "windows":
        return "lib.exe"
    raise ValueError(f"unsupported native archive platform: {platform}")


def _gcc_version() -> str:
    """Selected GCC version from raw SDK metadata; fixtures pass the fact instead."""
    from ao.portal import bazel_compiler_probe

    version = bazel_compiler_probe.nix_cc_wrapper(compiler="gcc").get("version")
    if not isinstance(version, str) or not re.fullmatch(r"[0-9]+(?:\.[0-9]+)*", version):
        raise ValueError("Nix GCC wrapper metadata must carry the selected version")
    return version


def verify(
    executable: Path,
    tree: Path,
    prepared: Path,
    config: str,
    jobs: int = 2,
    *,
    isolate_rc: bool = False,
    compiler_include_roots: Sequence[str] = (),
    remote_cache: str | None = None,
) -> int:
    """Analyze the folded native policy facts of the requested configuration.

    The facts producer is the standard in-graph validation input: building it
    under the caller's frozen prepared configuration analyzes the completion
    roots (``//:check_cpp_tests`` plus ``//:check_products`` on every host)
    without compiling any C++; its single JSON write is the only executed
    action. That universe replaces the retired query-expanded default-tests
    root explicitly, without silent weakening: the folded portable suites are a
    superset of the old tests root (they additionally own the backend, CLI,
    lint and TUI members plus the conditional GTK suite), product roots are
    analyzed on every host instead of macOS native actions only, and the
    retired native-process harness members own no compile actions of their own
    while their executables stay covered by the product roots. The shared facts
    validation owns every verdict through the existing compile/link/archive
    oracles and the independent SDK probes; Linux additionally expands one
    representative driver through the declared SDK compiler, resolved from the
    real execution root instead of a query. The facts must describe exactly the
    requested analysis configuration — a drifted frozen configuration fails
    closed before any verdict or driver stage (a sanitizer request passes its
    Debug analysis name, and the instrumented dbg facts satisfy it). The single
    configured DefaultInfo output file is located by one exact cquery of the
    facts target, never by guessing an output-tree layout; the declared output
    filename stays the Starlark macro's contract. remote_cache forwards the
    managed caller's trusted cache to the facts build; the standalone CLI has
    no such option and passes none. config names the requested analysis
    (debug/fastbuild/release/profile). Retain each invocation's stdout/stderr
    and actual exits in a unique directory. No stage runs after the first
    failure. This is analysis evidence, not a product build/test.
    """
    from ao.core import bazel_native as native
    from ao.portal import bazel_validation
    from ao.portal import bazel_workspace as workspace

    host = native.native_platform()
    if host not in {"linux", "macos", "windows"}:
        print("Bazel compile-contract verify requires native Linux, macOS or Windows", file=sys.stderr)
        return 1
    if config not in {"debug", "fastbuild", "release", "profile"} or jobs < 1:
        print(
            "Bazel compile-contract verify requires debug/fastbuild/release/profile and positive jobs",
            file=sys.stderr,
        )
        return 1
    attempts = tree / "contracts" / config
    attempts.mkdir(parents=True, exist_ok=True)
    artifacts = Path(tempfile.mkdtemp(prefix="verify-", dir=attempts))
    receipt_path = artifacts / "receipt.json"
    facts: dict[str, Any] = {
        "name": "facts",
        "argv": None,
        "cwd": str(PROJECT_ROOT),
        "target": FACTS_TARGET,
        "status": "unrun",
        "exit": None,
        "stdout": str(artifacts / "facts.stdout.log"),
        "stderr": str(artifacts / "facts.stderr.log"),
    }
    files: dict[str, Any] = {
        "name": "files",
        "argv": None,
        "cwd": str(PROJECT_ROOT),
        "target": FACTS_TARGET,
        "status": "unrun",
        "exit": None,
        "stdout": str(artifacts / "files.stdout.log"),
        "stderr": str(artifacts / "files.stderr.log"),
    }
    execution_root: dict[str, Any] = {
        "name": "execution_root",
        "argv": None,
        "cwd": str(PROJECT_ROOT),
        "status": "unrun",
        "exit": None,
        "stdout": str(artifacts / "execution_root.stdout.log"),
        "stderr": str(artifacts / "execution_root.stderr.log"),
    }
    driver: dict[str, Any] = {
        "status": "unrun" if host == "linux" else "not applicable",
        "argv": None,
        "reason": "Linux Nix wrapper diagnostic",
        "exit": None,
        "stdout": str(artifacts / "driver.stdout.log"),
        "stderr": str(artifacts / "driver.stderr.log"),
        "scope": (
            "one representative first-party wrapper expansion with only compiler path resolution, "
            "not literal action execution or whole-graph compilation/linking"
        ),
    }
    receipt: dict[str, Any] = {
        "config": config,
        "platform": host,
        "executable": str(executable),
        "prepared": str(prepared),
        "compiler_include_roots": list(compiler_include_roots),
        "source": str(PROJECT_ROOT),
        "python": sys.version,
        "jobs": jobs,
        "evidence": (
            "folded native policy facts of the completion roots built under the frozen configuration; "
            "the existing compile/link/archive oracles and independent SDK probes; "
            "Linux additionally resolves one declared SDK compiler from the execution root and expands "
            "its driver; no product compilation, linking or runtime certification"
        ),
        "facts": facts,
        "files": files,
        "execution_root": execution_root,
        "remote_cache": remote_cache,
        "driver": driver,
        "status": "incomplete",
        "exit": None,
    }

    def save() -> None:
        receipt_path.write_text(json.dumps(receipt, indent=2, sort_keys=True) + "\n", encoding="utf-8")

    def capture(stage: dict[str, Any], message: str) -> int:
        stage["status"] = "running"
        save()
        print(message, file=sys.stderr, flush=True)
        with Path(stage["stdout"]).open("wb") as out, Path(stage["stderr"]).open("wb") as err:
            result = subprocess.run(
                stage["argv"],
                cwd=PROJECT_ROOT,
                env=workspace.client_environment(isolate_rc=isolate_rc),
                stdout=out,
                stderr=err,
            )
        stage["exit"] = result.returncode
        stage["status"] = "exited"
        save()
        return result.returncode

    def require_execution_root() -> int:
        """Capture the real execution root once, if any consumer needs it.

        The configured DefaultInfo file path and the declared SDK compiler
        share one resolution; the actual Bazel exit is preserved on failure.
        """
        if "execution_root_path" in receipt:
            return 0
        execution_root["argv"] = [
            *workspace.invocation(executable, prepared, isolate_rc=isolate_rc),
            "info",
            "execution_root",
        ]
        status = capture(execution_root, f"Bazel execution root ({config}); receipt: {receipt_path}")
        if status:
            execution_root["status"] = "failed"
            receipt["status"] = "execution root failed"
            receipt["exit"] = status
            print(f"Bazel info exited {status}; stderr: {execution_root['stderr']}", file=sys.stderr)
            return status
        execution_root["status"] = "passed"
        reported = next(
            (
                line.strip()
                for line in Path(execution_root["stdout"]).read_text(encoding="utf-8").splitlines()
                if line.strip()
            ),
            "",
        )
        if not reported or not Path(reported).is_absolute():
            raise ValueError("Bazel info must report an absolute execution root")
        receipt["execution_root_path"] = reported
        return 0

    save()
    try:
        native.require_supported_source()
        facts["argv"] = [
            *workspace.invocation(executable, prepared, isolate_rc=isolate_rc),
            "build",
            f"--jobs={jobs}",
            *([f"--remote_cache={remote_cache}"] if remote_cache else []),
            FACTS_TARGET,
        ]
        status = capture(facts, f"Bazel native policy facts ({config}); receipt: {receipt_path}")
        if status:
            facts["status"] = "failed"
            receipt["status"] = "facts build failed"
            receipt["exit"] = status
            print(f"Bazel facts build exited {status}; stderr: {facts['stderr']}", file=sys.stderr)
            return status
        facts["status"] = "passed"
        # No output-tree layout is guessed: one exact configured query names
        # the producer's single DefaultInfo file. The declared output-group
        # compilers stay untouched; this reads the default output group only.
        files["argv"] = [
            *workspace.invocation(executable, prepared, isolate_rc=isolate_rc),
            "cquery",
            FACTS_TARGET,
            "--output=files",
        ]
        status = capture(files, f"Bazel facts output file ({config}); receipt: {receipt_path}")
        if status:
            files["status"] = "failed"
            receipt["status"] = "facts output query failed"
            receipt["exit"] = status
            print(f"Bazel cquery exited {status}; stderr: {files['stderr']}", file=sys.stderr)
            return status
        files["status"] = "passed"
        listed = [
            line.strip() for line in Path(files["stdout"]).read_text(encoding="utf-8").splitlines() if line.strip()
        ]
        if len(listed) != 1:
            raise ValueError("the facts producer must declare exactly one DefaultInfo output file, got: " + str(listed))
        facts_path = Path(listed[0])
        if not facts_path.is_absolute():
            status = require_execution_root()
            if status:
                return status
            facts_path = Path(receipt["execution_root_path"]) / facts_path
        if not facts_path.is_file():
            raise ValueError("the configured DefaultInfo output is missing: " + str(facts_path))
        receipt["facts_path"] = str(facts_path)
        document = bazel_validation._facts_document(facts_path)
        if document["platform"] != host:
            raise ValueError("native policy facts platform disagrees with the native host: " + document["platform"])
        configuration = bazel_validation._analysis_configuration(document["mode"], document["profile"])
        if configuration != config:
            raise ValueError(
                f"the prepared configuration produced {configuration} native policy facts, "
                f"not the requested {config}; the frozen launch configuration drifted"
            )
        receipt.update(
            mode=document["mode"],
            profile=document["profile"],
            analysis_configuration=configuration,
            gtk_enabled=document["gtk_enabled"],
            system_media_enabled=document["system_media_enabled"],
        )
        # The frozen request, never the ambient environment, owns the Windows
        # Release per-file exception fact.
        windows_tools_version: str | None = None
        if host == "windows" and document["profile"] == "release":
            request = json.loads((prepared / "request.json").read_text(encoding="utf-8"))
            compiler = request.get("compiler_environment")
            version = compiler.get("VCToolsVersion") if isinstance(compiler, dict) else None
            if not isinstance(version, str) or not re.fullmatch(r"[0-9]+\.[0-9]+\.[0-9]+", version):
                raise ValueError(
                    "Windows Release verification requires the selected VCToolsVersion from the prepared request"
                )
            windows_tools_version = version
            receipt["vc_tools_version"] = version
        errors = bazel_validation.validate_native_policy(
            document,
            compiler_include_roots=compiler_include_roots,
            windows_tools_version=windows_tools_version,
        )
        actions = bazel_validation._compile_actions(document["records"])
        first_party = [action for action in actions if _first_party(action)]
        receipt["counts"] = {
            "compiles": len(actions),
            "first_party": len(first_party),
            "execution_compiles": sum(action.is_tool for action in first_party),
            "spa": sum(_consumes_spa(action) for action in first_party),
        }
        receipt["violations"] = errors
        save()
        if errors:
            receipt["status"] = "contract failed"
            receipt["exit"] = 1
            print(f"Native policy violations ({config}):", file=sys.stderr)
            for error in errors:
                print(f"  {error}", file=sys.stderr)
            return 1
        if host == "linux":
            representative, driver_configuration, primary, expected_environment = bazel_validation._driver_inputs(
                document, PROJECT_ROOT
            )
            captured_compiler = representative.argv[0]
            if Path(captured_compiler).is_absolute():
                compiler = _driver_file(captured_compiler)
            else:
                if not captured_compiler.startswith("external/") or ".." in captured_compiler.split("/"):
                    raise ValueError(
                        "representative compiler must be a declared external SDK file: " + captured_compiler
                    )
                status = require_execution_root()
                if status:
                    return status
                compiler = _driver_file(str(Path(receipt["execution_root_path"]) / captured_compiler))
            driver.update(
                label=representative.label,
                primary_compiler=primary,
                captured_argv=list(representative.argv),
                environment=dict(representative.environment),
                cwd=str(PROJECT_ROOT),
                source_facts=receipt["facts_path"],
            )
            driver["argv"] = [str(compiler), *representative.argv[1:], "-###"]
            driver["status"] = "running"
            save()
            print(
                f"Bazel representative {primary} driver ({driver_configuration}); receipt: {receipt_path}",
                file=sys.stderr,
                flush=True,
            )
            status = _verify_driver(representative, document["mode"], expected_environment, driver, compiler=compiler)
            save()
            if status:
                receipt["status"] = "driver failed"
                receipt["exit"] = status
                print(f"Representative driver gate failed ({status}); stderr: {driver['stderr']}", file=sys.stderr)
                if driver.get("errors"):
                    print("\n".join(driver["errors"]), file=sys.stderr)
                return status
        receipt["status"] = "passed"
        receipt["exit"] = 0
        print(f"Bazel compile contracts passed ({config}); receipt: {receipt_path}")
        return 0
    except (
        native.BazelWorkspaceError,
        OSError,
        subprocess.SubprocessError,
        ValueError,
        KeyError,
        TypeError,
        StopIteration,
        RecursionError,
    ) as exc:
        receipt["status"] = "error"
        receipt["exit"] = 1
        receipt["error"] = str(exc)
        for stage in (facts, files, execution_root, driver):
            if stage["status"] in {"running", "exited"}:
                stage["status"] = "error"
                stage["error"] = str(exc)
        print(f"Bazel compile-contract verify: {exc}; receipt: {receipt_path}", file=sys.stderr)
        return 1
    finally:
        save()
