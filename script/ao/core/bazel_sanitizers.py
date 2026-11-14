"""Bounded managed sanitizer tests, separate from whole-project completion.

The BUILD graph selects suites. This controller freezes test runtime inputs before
configured analysis and reconciles BEP summaries, executed or cached, with full configuration IDs.
It neither changes SDK modes nor performs the normal whole-project ownership audit.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import subprocess
import sys
import tempfile
from collections.abc import Callable, Mapping, Sequence
from contextlib import ExitStack
from dataclasses import asdict, dataclass
from pathlib import Path
from types import MappingProxyType
from typing import Any

from . import bazel_check, bazel_contract, bazel_test_ownership, bazel_workspace
from . import bazel_native as native
from .buildlock import build_tree_lock
from .paths import PROJECT_ROOT

SANITIZER_CONFIGS = ("asan", "tsan")
_OPTIONS = ("ASAN_OPTIONS", "LSAN_OPTIONS", "UBSAN_OPTIONS", "TSAN_OPTIONS")
_SYMBOLIZERS = ("ASAN_SYMBOLIZER_PATH", "LSAN_SYMBOLIZER_PATH", "UBSAN_SYMBOLIZER_PATH", "TSAN_SYMBOLIZER_PATH")
_NATIVE_ROOT = "//:check_native_tests"


def validate_request(config: str, platform: str) -> None:
    """Pure preflight, safe before VS/SDK setup, tree selection or locking."""
    from .bazel_profiles import validate_request as validate_profile

    validate_profile(config, platform)


def compilation_config(config: str) -> str:
    """Map a product profile to the existing mode-neutral SDK configuration."""
    from .bazel_profiles import compilation_config as profile_compilation_config

    return profile_compilation_config(config)


def _symbolizer(platform: str, caller_environment: Mapping[str, str]) -> Path | None:
    """Admit an existing tool from the selected LLVM provider, never ambient PATH."""
    if platform == "windows":
        return None
    if platform == "linux":
        from .bazel_compiler_probe import nix_cc_wrapper

        tools = nix_cc_wrapper().get("tools")
        ar = tools.get("ar") if isinstance(tools, dict) else None
        if not isinstance(ar, str) or not Path(ar).is_absolute():
            raise native.BazelWorkspaceError("selected Nix LLVM archive-tool metadata is missing or not absolute")
        selected = Path(ar).with_name("llvm-symbolizer")
    else:
        llvm = Path(caller_environment.get("AOBUS_LLVM_ROOT", ""))
        if not llvm.is_absolute():
            raise native.BazelWorkspaceError("selected macOS AOBUS_LLVM_ROOT is missing or not absolute")
        selected = llvm / "bin/llvm-symbolizer"
    if not selected.is_file() or not os.access(selected, os.X_OK):
        raise native.BazelWorkspaceError(f"selected LLVM symbolizer is missing or not executable: {selected}")
    return selected.resolve(strict=True)


def _selected_tsan_interface_root() -> str:
    """Read the selected compiler SDK interface root, without generating flag policy.

    This is resolver data, not a flag/toolchain producer or a version inferred
    from a store basename. SDK admission owns the selected provider identity.
    """
    from .bazel_compiler_probe import nix_cc_wrapper

    return str(nix_cc_wrapper()["sanitizerIncludeDirectory"])


def compiler_include_roots(config: str, platform: str) -> tuple[str, ...]:
    """Admit the compiler-owned SDK root that the Linux TSan compile profile adds.

    libstdc++ annotates its atomic shared-pointer locks through this interface,
    so the compile-contract checker must accept it as a compiler include root.
    """
    if config == "tsan" and platform == "linux":
        return (_selected_tsan_interface_root(),)
    return ()


_OPTION_SEPARATORS = ":, \t\r\n"


def _parse_options(text: str, origin: str) -> list[tuple[str, str]]:
    """Read compiler-rt's option delimiters and literal single/double quotes.

    This is not shell syntax: backslashes are ordinary path characters, and
    quotes delimit a whole value rather than introducing escape sequences.
    Reject incomplete options instead of retaining an untracked rule path.
    """
    if "\x00" in text:
        raise native.BazelWorkspaceError(f"{origin} contains a NUL character")
    result = []
    position = 0
    while position < len(text):
        if text[position] in _OPTION_SEPARATORS:
            position += 1
            continue
        start = position
        while position < len(text) and text[position] not in _OPTION_SEPARATORS + "=":
            position += 1
        if position == start or position == len(text) or text[position] != "=":
            raise native.BazelWorkspaceError(f"{origin} requires name=value options")
        name = text[start:position]
        position += 1
        if position < len(text) and text[position] in "\"'":
            quote = text[position]
            position += 1
            end = text.find(quote, position)
            if end < 0:
                raise native.BazelWorkspaceError(f"{origin} contains an unterminated quoted value")
            value = text[position:end]
            position = end + 1
            if position < len(text) and text[position] not in _OPTION_SEPARATORS:
                raise native.BazelWorkspaceError(f"{origin} requires a separator after a quoted value")
        else:
            start = position
            while position < len(text) and text[position] not in _OPTION_SEPARATORS:
                position += 1
            value = text[start:position]
        result.append((name, value))
    return result


def _render_option(name: str, value: str) -> str:
    """Quote values only where compiler-rt would otherwise split or unquote them."""
    if "\x00" in value:
        raise native.BazelWorkspaceError(f"{name} contains a NUL character")
    if any(character in _OPTION_SEPARATORS for character in value) or value.startswith(('"', "'")):
        quote = '"' if '"' not in value else "'"
        if quote in value:
            raise native.BazelWorkspaceError(f"{name} cannot be represented without unsupported quote escaping")
        value = quote + value + quote
    return f"{name}={value}"


@dataclass(frozen=True)
class FrozenRuntime:
    environment: Mapping[str, str]
    suppression_hashes: Mapping[str, str]
    input_hashes: Mapping[str, str]
    symbolizer: Path | None = None

    def verify(self) -> None:
        for name, expected in self.suppression_hashes.items():
            if native.sha256(Path(name)) != expected:
                raise native.BazelWorkspaceError(f"frozen sanitizer suppression changed: {name}")
        if self.symbolizer is not None and native.sha256(self.symbolizer) != self.input_hashes[str(self.symbolizer)]:
            raise native.BazelWorkspaceError(f"frozen sanitizer symbolizer changed: {self.symbolizer}")

    def receipt(self) -> dict[str, Any]:
        return {
            "environment": dict(self.environment),
            "suppression_files": dict(self.suppression_hashes),
            "source_files": dict(self.input_hashes),
            "transport": "explicit test_env; content-addressed absolute suppression paths; local tests only",
            "symbolizer": str(self.symbolizer) if self.symbolizer is not None else None,
            "symbolization": (
                "existing selected LLVM tool; default source frames require native validation; "
                "caller symbolize=0 retained"
                if self.symbolizer is not None
                else "native MSVC runtime/PDB path; no LLVM selection; compatibility unvalidated"
            ),
        }


def freeze_runtime(config: str, platform: str, caller_environment: Mapping[str, str], directory: Path) -> FrozenRuntime:
    """Snapshot explicit runtime policy without CMake tree-name inference or global writes.

    Use compiler-rt option syntax and the existing governed suppression files,
    enforcing positive exit codes without changing CMake's runtime policy.
    Content-addressed suppression bytes under `directory` and an already-selected
    Unix LLVM symbolizer bind configured analysis and later local tests to the
    same inputs.
    """
    validate_request(config, platform)
    if config not in SANITIZER_CONFIGS:
        raise native.BazelWorkspaceError("managed sanitizer runtime requires asan or tsan")
    from ..command import test as native_tests

    caller = dict(caller_environment)
    environment = {name: caller.get(name, "") for name in _OPTIONS}
    environment.update({name: caller[name] for name in _SYMBOLIZERS if name in caller})
    active = ["TSAN_OPTIONS"] if config == "tsan" else ["ASAN_OPTIONS"]
    if config == "asan" and platform != "windows":
        active.append("UBSAN_OPTIONS")
        if platform == "macos":
            active.append("LSAN_OPTIONS")
    parsed = {name: _parse_options(environment[name], name) for name in active}
    symbolizer = _symbolizer(platform, caller)

    def require_symbolizer_path(value: str, origin: str) -> None:
        if symbolizer is None:
            return
        try:
            matches = Path(value).is_absolute() and Path(value).samefile(symbolizer)
        except OSError:
            matches = False
        if not matches:
            raise native.BazelWorkspaceError(f"{origin} conflicts with the governed LLVM symbolizer: {value!r}")

    if symbolizer is not None:
        for name in _SYMBOLIZERS:
            if name in caller:
                require_symbolizer_path(caller[name], name)
        names = (
            ("ASAN_SYMBOLIZER_PATH", "UBSAN_SYMBOLIZER_PATH", "LSAN_SYMBOLIZER_PATH")
            if config == "asan"
            else ("TSAN_SYMBOLIZER_PATH",)
        )
        environment.update(dict.fromkeys(names, str(symbolizer)))
    if platform == "windows":
        # VS admission belongs to the portal. Freeze its already-selected DLL
        # search path, never discover an alternative toolset or runtime here.
        environment["PATH"] = caller.get("PATH", "")
    runtime = directory / "runtime"
    runtime.mkdir(parents=True, exist_ok=True)
    inputs = {str(symbolizer): native.sha256(symbolizer)} if symbolizer is not None else {}
    snapshots: dict[str, str] = {}
    symbolizer_options = [("external_symbolizer_path", str(symbolizer))] if symbolizer is not None else []

    def retained_options(name: str, owned: set[str]) -> list[tuple[str, str]]:
        retained = []
        for key, value in parsed[name]:
            if key == "external_symbolizer_path" and symbolizer is not None:
                require_symbolizer_path(value, name)
            elif key not in owned:
                retained.append((key, value))
        return retained

    def read_suppression(path: Path) -> bytes:
        path = path.absolute()
        content = path.read_bytes()
        inputs[str(path)] = hashlib.sha256(content).hexdigest()
        return content

    def retain(name: str, content: bytes) -> Path:
        # The path names its bytes: unchanged rules keep the hashed test
        # environment, and so cached results, stable; changed rules miss.
        digest = hashlib.sha256(content).hexdigest()
        path = (runtime / digest / name).absolute()
        path.parent.mkdir(exist_ok=True)
        if not path.is_file() or path.read_bytes() != content:
            staged = path.with_name(f".{name}.{os.getpid()}.partial")
            staged.write_bytes(content)
            os.replace(staged, path)
        snapshots[str(path)] = digest
        return path

    def caller_rules(name: str, options: list[tuple[str, str]]) -> list[tuple[str, str]]:
        # CMake keeps caller rule files by path. Bind each one, in place, to
        # content-addressed bytes so its runtime precedence is unchanged.
        frozen = []
        for key, value in options:
            if key != "suppressions":
                frozen.append((key, value))
                continue
            if not value:
                raise native.BazelWorkspaceError(f"{name} contains an empty suppressions path")
            prefix = name.split("_", 1)[0].lower()
            copy = retain(f"{prefix}-caller-{len(frozen)}.supp", read_suppression(Path(value)))
            frozen.append(("suppressions", str(copy)))
        return frozen

    if config == "asan" and platform == "linux":
        suppression = retain("lsan.supp", read_suppression(native_tests._LSAN_SUPP_PATH))
        # Match CMake: Linux LSAN_OPTIONS replaces, rather than merges, caller values.
        environment["LSAN_OPTIONS"] = _render_option("suppressions", str(suppression)) + ":exitcode=23"
    elif config == "asan" and platform == "macos":
        leak_options = parsed["LSAN_OPTIONS"]
        if any(key == "suppressions" for key, _ in leak_options):
            environment["LSAN_OPTIONS"] = ":".join(
                _render_option(key, value) for key, value in caller_rules("LSAN_OPTIONS", leak_options)
            )
    if config == "asan":
        retained = caller_rules("ASAN_OPTIONS", retained_options("ASAN_OPTIONS", {"halt_on_error", "exitcode"}))
        environment["ASAN_OPTIONS"] = ":".join(
            _render_option(key, value)
            for key, value in (*retained, *symbolizer_options, ("halt_on_error", "1"), ("exitcode", "1"))
        )
    if config == "asan" and platform != "windows":
        retained = caller_rules(
            "UBSAN_OPTIONS", retained_options("UBSAN_OPTIONS", {"halt_on_error", "print_stacktrace", "exitcode"})
        )
        environment["UBSAN_OPTIONS"] = ":".join(
            _render_option(key, value)
            for key, value in (
                *retained,
                *symbolizer_options,
                ("halt_on_error", "1"),
                ("print_stacktrace", "1"),
                ("exitcode", "1"),
            )
        )
    if config == "tsan":
        retained = []
        paths = []
        for key, value in retained_options("TSAN_OPTIONS", {"halt_on_error", "second_deadlock_stack", "exitcode"}):
            if key == "suppressions":
                if not value:
                    raise native.BazelWorkspaceError("TSAN_OPTIONS contains an empty suppressions path")
                paths.append(Path(value))
            else:
                retained.append((key, value))
        # Keep CMake's caller-before-governed suppression order, while enforcing
        # this managed Bazel route's stricter nonzero report status.
        contents = [read_suppression(path).decode("utf-8").rstrip() for path in (*paths, native_tests._TSAN_SUPP_PATH)]
        suppression = retain("tsan.supp", ("\n\n".join(contents) + "\n").encode("utf-8"))
        environment["TSAN_OPTIONS"] = ":".join(
            _render_option(key, value)
            for key, value in (
                *retained,
                ("suppressions", str(suppression)),
                *symbolizer_options,
                ("halt_on_error", "1"),
                ("second_deadlock_stack", "1"),
                ("exitcode", "66"),
            )
        )
    return FrozenRuntime(
        MappingProxyType(environment), MappingProxyType(snapshots), MappingProxyType(inputs), symbolizer
    )


class _Run(bazel_check._Check):
    """Reuse retained phase execution, not the normal check's ownership contract."""

    def __init__(self, tree: Path, profile: str, platform: str, jobs: int):
        directory = tree / "sanitizers" / profile
        directory.mkdir(parents=True, exist_ok=True)
        self.directory = Path(tempfile.mkdtemp(prefix="run-", dir=directory))
        self.path = self.directory / "receipt.json"
        self.log = tree / "build.log"
        self._prepared: Path | None = None
        self._runtime: FrozenRuntime | None = None
        self.receipt: dict[str, Any] = {
            "schema": 1,
            "source": str(PROJECT_ROOT),
            "tree": str(tree),
            "config": profile,
            "compilation_config": compilation_config(profile),
            "platform": platform,
            "jobs": jobs,
            "started_at": bazel_check._timestamp(),
            "status": "running",
            "exit": None,
            "phases": [],
            "scope": {
                "tests": f"//:{profile}_tests",
                "native_tests": _NATIVE_ROOT,
                "claim": "sanitizer tests; normal whole completion ownership separately required",
                "ownership_audit": "not run; selected configured test identities only",
                "excluded": ["tooling", "lint", "performance", "manual GUI/hardware", "whole-project completion"],
            },
        }
        self.save()
        print(f"Bazel sanitizer tests ({profile}); receipt: {self.path}", flush=True)

    def freeze(self, prepared: Path, runtime: FrozenRuntime) -> None:
        self._prepared = prepared
        self._runtime = runtime
        self.receipt.update(prepared=str(prepared), runtime=runtime.receipt())
        self.save()

    def _verify_snapshot(self) -> None:
        if self._runtime is not None:
            self._runtime.verify()

    def phase(self, name: str, invoke: Callable[[Any, Any], int], *, argv: Sequence[str] | None = None) -> Path:
        self._verify_snapshot()
        result = super().phase(name, invoke, argv=argv)
        self._verify_snapshot()
        return result


def _complete(args: argparse.Namespace, tree: Path, attempt: _Run, prepared: Path, executable: Path) -> None:
    root = f"//:{args.config}_tests"
    native_selection = attempt.bazel(
        "native-test-selection",
        executable,
        prepared,
        "cquery",
        [f"labels(srcs, {_NATIVE_ROOT})", "--output=starlark", "--starlark:expr=str(target.label)"],
    )
    native_tests = bazel_check._labels(native_selection.read_text(encoding="utf-8"), allow_empty=True)
    roots = [root, *native_tests]
    attempt.receipt.update(roots=roots, native_tests=native_tests)
    selection = attempt.bazel("test-selection", executable, prepared, "query", [f"tests({root})", "--output=label"])
    labels = bazel_check._labels(
        "\n".join([*bazel_check._labels(selection.read_text(encoding="utf-8")), *native_tests])
    )
    expression = "set(" + " ".join(json.dumps(label) for label in labels) + ")"
    configured_path = attempt.bazel(
        "configured-tests", executable, prepared, "cquery", [expression, "--output=jsonproto"]
    )
    configured = bazel_test_ownership.extract_configured_targets(
        json.loads(configured_path.read_text(encoding="utf-8"))
    )
    if {target.label for target in configured} != set(labels):
        raise ValueError("sanitizer configured selection differs from the expanded suite")
    compatibility_path = attempt.bazel(
        "compatibility",
        executable,
        prepared,
        "cquery",
        [expression, "--output=starlark", f"--starlark:expr={bazel_check._COMPATIBILITY}"],
    )
    incompatible = bazel_test_ownership.extract_compatibility(
        compatibility_path.read_text(encoding="utf-8"), configured
    )
    expected = configured - incompatible
    if not expected:
        raise ValueError("sanitizer selection contains no compatible tests")
    attempt.receipt.update(
        selected_tests=[asdict(target) for target in sorted(expected)],
        incompatible_tests=[asdict(target) for target in sorted(incompatible)],
    )
    attempt.save()
    attempt.python(
        "compile-contracts",
        lambda: bazel_contract.verify(
            executable,
            tree,
            prepared,
            "debug",
            jobs=args.jobs,
            isolate_rc=True,
            compiler_include_roots=compiler_include_roots(args.config, attempt.receipt["platform"]),
        ),
    )
    # Instrumentation itself is proven when the build compiles the sanitizer
    # profile canary in //test:utility_test, not by re-reading command lines.
    attempt.bazel(
        "build",
        executable,
        prepared,
        "build",
        [*roots, f"--build_event_json_file={attempt.directory / 'build.bep.json'}"],
        remote_cache=args.remote_cache,
    )
    attempt.bazel(
        "tests",
        executable,
        prepared,
        "test",
        [*roots, f"--build_event_json_file={attempt.directory / 'tests.bep.json'}"],
        remote_cache=args.remote_cache,
    )

    def results() -> int:
        tested = [
            result | {"executed": result["runs"] - result["cached"]}
            for result in bazel_check._test_results(attempt.directory / "tests.bep.json", expected, fresh=_fresh(args))
        ]
        attempt.receipt["test_results"] = tested
        return 0

    attempt.python("test-results", results)


def _fresh(args: argparse.Namespace) -> bool:
    """`ao check` admits cached sanitizer results unless --fresh requests execution."""
    return bool(getattr(args, "fresh", False))


def run(args: argparse.Namespace) -> int:
    platform = native.native_platform()
    validate_request(args.config, platform)
    if args.config not in SANITIZER_CONFIGS or args.targets:
        raise native.BazelWorkspaceError("managed sanitizer tests require asan/tsan and the default graph-owned scope")
    if args.jobs < 1:
        raise native.BazelWorkspaceError("sanitizer jobs must be positive")
    caller_environment = dict(os.environ)
    tree = bazel_workspace.workspace_path(args.path, args.config)
    with build_tree_lock(tree):
        tree.mkdir(parents=True, exist_ok=True)
        attempt = _Run(tree, args.config, platform, args.jobs)
        attempt.receipt["test_cache_policy"] = "fresh" if _fresh(args) else "auto"
        try:
            native.require_supported_source()
            from .bazel_workflows import feature_options

            base = bazel_workspace.prepare_workspace(
                tree, args.config, prefix=args.vcpkg_prefix, graph_options=feature_options(args)
            )
            executable = bazel_workspace.bazel_executable()
            attempt.receipt.update(base_prepared=str(base), executable=str(executable))
            attempt.save()

            def complete() -> int:
                with ExitStack() as services:
                    environment: dict[str, str] = {}
                    if platform == "linux":
                        from ..command.test import virtual_gtk_displays
                        from .bazel_test_services import private_pipewire, stable_service_root

                        displays = services.enter_context(
                            virtual_gtk_displays(1, ipc_root=stable_service_root(tree, "gtk-bus"))
                        )
                        environment = services.enter_context(
                            private_pipewire(
                                attempt.directory, displays[0], runtime_root=stable_service_root(tree, "pipewire")
                            )
                        )
                        attempt.receipt["service_lifecycle"] = str(attempt.directory / "pipewire-lifecycle.json")
                    # Snapshots outlive the attempt so unchanged rules keep one path.
                    runtime = freeze_runtime(args.config, platform, caller_environment, attempt.directory.parent)
                    prepared = bazel_check._context_configuration(
                        base, attempt.directory, environment | dict(runtime.environment), fresh=_fresh(args)
                    )
                    # The standalone test runner still delegates to a spawn
                    # strategy. Pin that spawn locally as suppression files and
                    # native fatal-process contracts are not sandbox fixtures.
                    with (prepared / "aobus.bazelrc").open("a", encoding="utf-8") as output:
                        output.write("build --test_strategy=standalone\n")
                        output.write("build --strategy=TestRunner=local\n")
                    attempt.freeze(prepared, runtime)
                    _complete(args, tree, attempt, prepared, executable)
                return 0

            attempt.python("native-context", complete)
            attempt.receipt.update(status="passed", exit=0)
            tested = attempt.receipt["test_results"]
            executed = sum(result["executed"] for result in tested)
            cached = sum(result["cached"] for result in tested)
            print(f"Sanitizer test summaries: {len(tested)} passed; {executed} executed runs, {cached} cached runs.")
            print(f"Bazel sanitizer tests passed ({args.config}); receipt: {attempt.path}")
            return 0
        except bazel_check._PhaseFailure as error:
            attempt.receipt.update(status="failed", exit=error.status, error=str(error))
            return error.status
        except (SystemExit, KeyboardInterrupt) as error:
            status = bazel_check._controlled_exit_status(error)
            attempt.receipt.update(
                status="interrupted" if isinstance(error, KeyboardInterrupt) else "failed",
                exit=status,
                error=str(error) or type(error).__name__,
                writer_state="not established by a stopped controller",
            )
            return status
        except (
            native.BazelWorkspaceError,
            OSError,
            subprocess.SubprocessError,
            ValueError,
            KeyError,
            TypeError,
        ) as error:
            attempt.receipt.update(status="error", exit=1, error=str(error))
            return 1
        finally:
            if attempt.receipt["status"] == "running":
                attempt.receipt.update(status="error", exit=1, error="sanitizer tests stopped before a terminal result")
            attempt.receipt["finished_at"] = bazel_check._timestamp()
            attempt.save()
            if attempt.receipt["exit"]:
                print(
                    f"Bazel sanitizer tests stopped: {attempt.receipt.get('error')}; receipt: {attempt.path}",
                    file=sys.stderr,
                )
