"""Non-LLVM launcher for cacheable Bazel lint verification and context reports."""

from __future__ import annotations

import argparse
import json
import os
import posixpath
import re
import shlex
import subprocess
import sys
import tempfile
from pathlib import Path
from typing import Any

# Supports both the portal package and a declared Python script action. The
# lexical path keeps imports inside the action's declared module closure.
if __package__ in (None, ""):
    sys.path.insert(0, str(Path(__file__).absolute().parents[2]))

from ao.core import tidyconfig, tidypolicy
from ao.core.bazel_lint_reports import Identity, completed_outcome, exec_path, load_json
from ao.core.tidydiagnostics import parse_diagnostic_header


def _json(path: Path, value: object) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(value, indent=2) + "\n", encoding="utf-8")


def _arguments(value: object, name: str) -> list[str]:
    if not isinstance(value, list) or any(not isinstance(arg, str) or not arg or "\x00" in arg for arg in value):
        raise ValueError(f"{name} must be an array of nonempty strings")
    return value


def validate_configuration(data: dict[str, Any]) -> dict[str, Any]:
    args = _arguments(data.get("clang_tidy_args"), "clang_tidy_args")
    tidyconfig.validate_diagnostic_arguments(args)
    # Raw options may only adjust diagnostic presentation. Native compiler
    # context and keyed policy fields own every other analysis input.
    supported = {
        "--quiet",
        "--system-headers",
        "--system-headers=true",
        "--system-headers=false",
        "--enable-check-profile",
        "--use-color=false",
    }
    for argument in args:
        if argument not in supported:
            raise ValueError(f"unsupported Bazel clang-tidy diagnostic argument: {argument}")
    root = data.get("source_root")
    if not isinstance(root, str) or not Path(root).is_absolute() or not Path(root).is_dir():
        raise ValueError("source_root must be an existing absolute checkout")
    for name in ("check", "header_filter"):
        if data.get(name) is not None and (not isinstance(data[name], str) or not data[name] or "\x00" in data[name]):
            raise ValueError(f"{name} must be null or a nonempty string")
    return {
        "source_root": root,
        "check": data.get("check"),
        "header_filter": data.get("header_filter"),
        "clang_tidy_args": args,
    }


def prepare(args: argparse.Namespace) -> int:
    """Repository-time policy rendering; never starts clang-tidy or CMake."""
    # Repository-time only: cacheable actions never import the portal commands.
    from ao.command.tidy import expected_aobus_checks

    configuration = validate_configuration(load_json(args.configuration))
    expected = sorted(expected_aobus_checks(Path(configuration["source_root"]) / "test/lint/fixture"))
    _json(args.output, {"schema": 1, **configuration, "expected_checks": expected})
    return 0


def _policy(path: Path) -> dict[str, Any]:
    data = load_json(path)
    validate_configuration(data)
    expected = _arguments(data.get("expected_checks"), "expected checks")
    if (
        not expected
        or len(set(expected)) != len(expected)
        or any(re.fullmatch(r"aobus-[a-z0-9-]+", item) is None for item in expected)
    ):
        raise ValueError("invalid fixture-derived checker registry")
    return data


def _sdk(path: Path) -> dict[str, Any]:
    data = load_json(path)
    version = data.get("version")
    if not isinstance(version, str) or re.fullmatch(r"[0-9]+\.[0-9]+\.[0-9]+", version) is None:
        raise ValueError("SDK metadata requires an exact LLVM version")
    if data.get("platform") not in ("linux", "macos", "windows"):
        raise ValueError("SDK metadata requires the native platform")
    expected_kind = "standalone" if data["platform"] == "windows" else "plugin"
    if data.get("host_kind") != expected_kind:
        raise ValueError("SDK host kind differs from its native platform")
    for key in ("system_arguments", "analysis_arguments"):
        arguments = _arguments(data.get(key), key)
        if any(argument.startswith("@") for argument in arguments):
            raise ValueError("SDK arguments contain an unresolved response")
    if data["platform"] == "windows":
        resource = data.get("resource_dir")
        if not isinstance(resource, str) or not (Path(resource) / "include").is_dir():
            raise ValueError("Windows SDK metadata requires a materialized resource_dir")
        if data["analysis_arguments"] != list(tidypolicy.windows_analysis_arguments(Path(resource))):
            raise ValueError("Windows SDK arguments differ from the shared audited analysis arguments")
    return data


def _tools(args: argparse.Namespace, sdk: dict[str, Any]) -> tuple[str, list[str]]:
    if not args.tidy.is_file():
        raise ValueError(f"missing clang-tidy host: {args.tidy}")
    if sdk["platform"] == "windows":
        if args.plugin is not None:
            raise ValueError("Windows uses the linked AobusClangTidy host, not a plugin")
    elif args.plugin is None:
        raise ValueError("Unix analysis requires the Aobus clang-tidy plugin")
    if args.plugin is not None and not args.plugin.is_file():
        raise ValueError(f"missing clang-tidy plugin: {args.plugin}")
    return str(args.tidy.absolute()), [f"-load={args.plugin.absolute()}"] if args.plugin is not None else []


def _invoke(command: list[str], *, env: dict[str, str] | None = None) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, encoding="utf-8", env=env
    )


def _verification_identity(args: argparse.Namespace, policy: dict[str, Any], sdk: dict[str, Any]) -> dict[str, Any]:
    return {
        "sdk": sdk,
        "expected_checks": policy["expected_checks"],
        "tidy": str(args.tidy),
        "plugin": str(args.plugin) if args.plugin is not None else None,
    }


def load_verified_tools(args: argparse.Namespace) -> tuple[dict[str, Any], dict[str, Any], str, list[str]]:
    """Load declared policy/SDK/tools against a cached verification, without children.

    Callers supply ``policy``, ``sdk``, ``verification`` and ``tidy`` Paths plus
    ``plugin`` (a Path or None). The returned host and optional load argument
    can be used by independent analysis consumers without repeating preflight.
    """
    policy, sdk = _policy(args.policy), _sdk(args.sdk)
    verification = load_json(args.verification)
    expected = {"schema": 1, "verified": True, **_verification_identity(args, policy, sdk)}
    if verification.get("verified") is not True or verification != expected:
        raise ValueError("verification does not describe the declared host/SDK/registry")
    host, plugin = _tools(args, sdk)
    return policy, sdk, host, plugin


def verify(args: argparse.Namespace) -> int:
    """One cacheable action owns both version and complete registry verification."""
    args.output.unlink(missing_ok=True)
    policy, sdk = _policy(args.policy), _sdk(args.sdk)
    host, plugin = _tools(args, sdk)
    args.log.parent.mkdir(parents=True, exist_ok=True)
    with args.log.open("w", encoding="utf-8") as log:
        version = _invoke([host, "--version"])
        log.write(f"=== version ===\n{version.stdout}")
        log.flush()
        pattern = rf"\b(?:LLVM|clang) version\s+{re.escape(sdk['version'])}(?![0-9.])"
        if version.returncode != 0 or re.search(pattern, version.stdout, re.IGNORECASE) is None:
            raise ValueError("clang-tidy version does not match the governed SDK")
        registry = _invoke([host, *plugin, "-checks=-*,aobus-*", "-list-checks"])
        log.write(f"=== registry ===\n{registry.stdout}")
        registered = [line.strip() for line in registry.stdout.splitlines() if line.strip().startswith("aobus-")]
        if (
            registry.returncode != 0
            or len(set(registered)) != len(registered)
            or set(registered) != set(policy["expected_checks"])
        ):
            raise ValueError("clang-tidy registry differs from the working fixture registry")
    _json(args.output, {"schema": 1, "verified": True, **_verification_identity(args, policy, sdk)})
    return 0


def _compile_arguments(identity: Identity, argv: list[str]) -> list[str]:
    if len(argv) < 2 or argv.count(identity.source) != 1:
        raise ValueError("expected exactly one exact native source argument")
    for argument in argv:
        lower = argument.lower()
        # Fail closed even for split/function-like definitions and forwarding
        # encodings: native commands must not reference the analysis-only mode.
        if re.search(r"AOBUS_LINT_HEADER_MAIN(?![A-Za-z0-9_])", argument):
            raise ValueError("native context references the reserved header-main analysis mode")
        forwarded = (
            lower.removeprefix("/clang:").removeprefix("-xclang=").lstrip("-/") if lower.startswith(("-", "/")) else ""
        )
        if forwarded.startswith(
            ("ivfsoverlay", "vfsoverlay", "remap-file", "fplugin", "fpass-plugin", "iplugindir")
        ) or (
            forwarded.partition("=")[0].partition(":")[0] in ("config", "load", "plugin", "add-plugin")
            or forwarded.startswith(("config-", "plugin-", "load-pass-plugin"))
        ):
            raise ValueError(f"unsupported compiler view/config/plugin argument: {argument}")
        if (
            argument.startswith("@")
            or any(
                marker in lower
                for marker in (
                    "-fmodule",
                    "-include-pch",
                    "-emit-pch",
                    "-emit-module",
                    ".pcm",
                    ".gch",
                    ".pch",
                    "${",
                    "%{",
                    "<module",
                    "<source",
                )
            )
            or lower.startswith(("/yu", "/yc", "/reference", "/headerunit", "/ifc"))
            or (lower.startswith("/fp") and not lower.startswith("/fp:"))
        ):
            raise ValueError(f"unsupported response/module/PCH compile context: {argument}")
    header = identity.main != identity.source and not _is_fragment(identity)
    if Path(identity.source).suffix.lower() not in (".c", ".cc", ".cpp", ".cxx", ".mm"):
        raise ValueError("native input is not a supported source translation unit")
    if header and Path(identity.main).suffix.lower() not in (".h", ".hh", ".hpp", ".hxx"):
        raise ValueError("only a self-contained header can replace a native source")
    excluded = (
        {arg.casefold() for arg in tidypolicy.WINDOWS_EXCLUDED_COMPILE_ARGUMENTS}
        if identity.platform == "windows"
        else set()
    )
    rewritten = []
    for argument in argv:
        if argument.casefold() in excluded:
            continue
        if argument == identity.source:
            if header:
                language = "objective-c++-header" if identity.source.endswith(".mm") else "c++-header"
                # PCH producers may keep their framework/macro prefix while
                # leaving the selected implementation header to this main file.
                rewritten.extend(["-DAOBUS_LINT_HEADER_MAIN=1", "-x", language])
                rewritten.append(str(Path(identity.main).absolute()))
            else:
                # A fragment main is analyzed inside its consumer translation unit.
                rewritten.append(str(Path(identity.source).absolute()))
        else:
            rewritten.append(argument)
    return rewritten


def _is_fragment(identity: Identity) -> bool:
    return identity.main != identity.source and Path(identity.main).suffix.lower() == ".def"


def _nix_wrapper_arguments(argv: list[str]) -> list[str]:
    """Replay the pinned wrapper's ordered additions, never a compiler job.

    clang-tidy reads the database without executing its compiler. NIX_DEBUG=1
    exposes the producer's before/original/after arrays; -### keeps the native
    driver read-only. The raw native argv remains the report context identity.
    """
    if any(argument in ("--", "-cc1", "-fc1", "-###") for argument in argv) or argv[-1] in (
        "-o",
        "-MF",
        "-MT",
        "-MQ",
        "-MJ",
        "-x",
        "-target",
        "--target",
        "-isysroot",
        "--sysroot",
        "-I",
        "-isystem",
        "-idirafter",
        "-include",
        "-imacros",
        "-Xclang",
        "-Xpreprocessor",
        "-Xassembler",
        "-Xlinker",
    ):
        raise ValueError("native compiler arguments cannot safely accept the driver-only -### probe")
    result = _invoke([*argv, "-###"], env={**os.environ, "NIX_DEBUG": "1"})
    try:
        if result.returncode != 0:
            raise ValueError(f"native wrapper driver trace failed ({result.returncode})")
        return _traced_wrapper_arguments(argv, result.stdout)
    except ValueError:
        # A failed action discards its outputs, so keep the trace in Bazel's output.
        print(f"native wrapper driver trace tail:\n{_log_tail(result.stdout)}", file=sys.stderr)
        raise


def _traced_wrapper_arguments(argv: list[str], trace: str) -> list[str]:
    names = ("extra flags before", "original flags", "extra flags after")
    sections: list[list[str]] = []
    compiler: str | None = None
    in_section = False
    for line in trace.splitlines():
        if line.startswith(("extra flags", "original flags")):
            match = re.fullmatch(r"(extra flags before|original flags|extra flags after) to (\S+):", line)
            if match is None or len(sections) >= 3 or match[1] != names[len(sections)]:
                raise ValueError("unexpected native wrapper trace sections")
            if re.fullmatch(r"/nix/store/[a-z0-9]{32}-[A-Za-z0-9+._=-]+/bin/clang(?:\+\+)?", match[2]) is None or (
                compiler is not None and match[2] != compiler
            ):
                raise ValueError("native wrapper trace has inconsistent/non-Nix compilers")
            compiler = match[2]
            sections.append([])
            in_section = True
        elif line.startswith("  ") and in_section:
            encoded = line[2:]
            if (
                "$'" in encoded
                or '$"' in encoded
                or any(ord(character) < 32 or ord(character) == 127 for character in encoded)
            ):
                raise ValueError("unsupported native wrapper shell quoting")
            decoded = shlex.split(encoded, posix=True)
            if len(decoded) != 1:
                raise ValueError("native wrapper trace needs exactly one quoted argument per line")
            sections[-1].append(decoded[0])
        else:
            if in_section and len(sections) < 3:
                raise ValueError("interrupted native wrapper trace section")
            in_section = False
    if len(sections) != 3 or any(not section for section in sections):
        raise ValueError("missing native wrapper trace sections")
    sections = [[] if section == [""] else section for section in sections]
    if any("" in section for section in sections) or sections[1] != [*argv[1:], "-###"]:
        raise ValueError("native wrapper transformed the original compile arguments")
    return [argv[0], *sections[0], *argv[1:], *sections[2]]


_SEPARATOR = r"[/\\]"


def _spelling(path: str) -> str:
    return re.escape(path.replace("\\", "/").rstrip("/")).replace("/", _SEPARATOR)


def _virtual_headers(context: dict[str, Any]) -> dict[str, str | None]:
    """Validate the aspect's map from virtual include inputs to checkout headers."""
    value = context.get("virtual_headers")
    if not isinstance(value, dict):
        raise ValueError("context requires the virtual include origin map")
    result: dict[str, str | None] = {}
    for virtual, original in value.items():
        if "/_virtual_includes/" not in exec_path(virtual) or (original is not None and _virtual_key(original)):
            raise ValueError(f"invalid virtual include origin: {virtual}")
        result[virtual] = None if original is None else exec_path(original)
    return result


def _virtual_key(path: str) -> str | None:
    """Return the ``.../_virtual_includes/<key>/`` directory that publishes a header."""
    directory, marker, rest = path.partition("/_virtual_includes/")
    key = rest.partition("/")[0]
    return f"{directory}{marker}{key}/" if marker and key else None


def _header_filter(
    identity: Identity, policy: dict[str, Any], roots: tuple[Path, ...], virtual_headers: dict[str, str | None]
) -> str:
    """Match project headers under each spelling the native include paths produce.

    Bazel include paths are execution-relative, so clang names a header as
    ``include/...``, ``./lib/...`` or a ``_virtual_includes`` symlink, while
    the roots cover absolute spellings. A virtual header qualifies through the
    folder of its checkout original; generated headers never qualify. A
    header main reports only itself. LLVM compiles the filter as a POSIX
    extended expression, so groups must not use Python-only syntax.
    """
    if policy["header_filter"] is not None:
        return str(policy["header_filter"])
    if _is_fragment(identity):
        # Clang keeps lexical ../ components in included-file locations. The
        # fragment's basename line filter limits candidates; after the complete
        # attempt is validated, normalized primary locations select the file.
        return ".*"
    roots_expression = "|".join(_spelling(str(root)) + _SEPARATOR for root in roots)
    prefix = rf"({roots_expression}|\.{_SEPARATOR})?"
    if identity.main != identity.source:
        names = [identity.main, *sorted(v for v, original in virtual_headers.items() if original == identity.main)]
        return "^" + prefix + "(" + "|".join(_spelling(name) for name in names) + ")$"
    folders = tidypolicy.RELAXED_HEADER_FOLDERS if identity.mode == "RELAXED" else tidypolicy.STRICT_HEADER_FOLDERS
    qualifies: dict[str, list[tuple[str, bool]]] = {}
    for virtual, original in sorted(virtual_headers.items()):
        directory = _virtual_key(virtual)
        if directory is not None:
            accepted = original is not None and original.partition("/")[0] in folders
            qualifies.setdefault(directory, []).append((virtual, accepted))
    alternatives = ["(" + "|".join(folders) + ")" + _SEPARATOR]
    for directory, entries in qualifies.items():
        # One directory alternative keeps large header sets within Windows limits.
        if all(accepted for _, accepted in entries):
            alternatives.append(_spelling(directory) + _SEPARATOR)
        else:
            alternatives.extend(_spelling(virtual) + "$" for virtual, accepted in entries if accepted)
    return "^" + prefix + "(" + "|".join(alternatives) + ")"


def _report_path(path: str, roots: tuple[Path, ...], virtual_headers: dict[str, str | None], *, windows: bool) -> str:
    """Spell a diagnostic location relative to the execution root, through virtual includes."""
    spelled = path.replace("\\", "/")
    folded = spelled.casefold() if windows else spelled
    for root in roots:
        prefix = root.as_posix().rstrip("/") + "/"
        if folded.startswith(prefix.casefold() if windows else prefix):
            spelled = spelled[len(prefix) :]
            break
    else:
        if Path(spelled).is_absolute() or re.match(r"^[A-Za-z]:/", spelled):
            return path
    relative = posixpath.normpath(spelled)
    return virtual_headers.get(relative) or relative


def _report_output(text: str, roots: tuple[Path, ...], virtual_headers: dict[str, str | None], *, windows: bool) -> str:
    """Keep reports independent of the sandbox that produced them.

    Analysis runs below a per-action execution root and may reach a checkout
    header through its virtual include symlink. Reports name both through the
    execution-relative checkout path, which the gate maps to the checkout.
    """
    lines = []
    for line in text.splitlines(keepends=True):
        content = line.rstrip("\r\n")
        # Echoed source lines may quote locations; only real headers move.
        if (header := parse_diagnostic_header(content)) is not None and header.path is not None:
            path = _report_path(header.path, roots, virtual_headers, windows=windows)
            line = path + line[len(header.path) :]
        lines.append(line)
    return "".join(lines)


def _fragment_output(text: str, main: str, *, windows: bool) -> str:
    """Keep selected-fragment warning blocks after path normalization.

    Errors and unlocated messages remain part of the consumer attempt. The
    caller validates the complete unfiltered log first so discarded warning
    continuations cannot hide an infrastructure failure.
    """
    expected = main.casefold() if windows else main
    skipped = False
    output = []
    for line in text.splitlines(keepends=True):
        header = parse_diagnostic_header(line)
        if header is not None and header.severity != "note":
            path = header.path
            if path is not None and windows:
                path = path.casefold()
            skipped = header.severity == "warning" and path is not None and path != expected
        if not skipped:
            output.append(line)
    return "".join(output)


def _line_filter(spellings: list[str]) -> str:
    # LLVM matches line-filter names as path suffixes, in their native spelling.
    names = dict.fromkeys(name for path in spellings for name in (path, path.replace("/", "\\")))
    return json.dumps([{"name": name, "lines": [[1, 2_147_483_647]]} for name in names], separators=(",", ":"))


def _includes(
    path: Path, roots: tuple[Path, ...], virtual_headers: dict[str, str | None], *, windows: bool
) -> list[str]:
    """Return the checkout files a completed analysis included, as consumer evidence."""
    if not path.is_file():
        return []
    included: set[str] = set()
    for line in path.read_text(encoding="utf-8", errors="surrogateescape").splitlines():
        # The frontend escapes each listed path like a string literal, so a
        # Windows path arrives with doubled backslashes.
        if spelled := re.sub(r"\\(.)", r"\1", line.strip()):
            relative = _report_path(spelled, roots, virtual_headers, windows=windows)
            # Generated, external and absolute system headers are not checkout files.
            if not relative.startswith(("bazel-out/", "external/", "/", "../")) and ":" not in relative:
                included.add(relative)
    return sorted(included)


def _log_tail(text: str, lines: int = 40) -> str:
    return "".join(text.splitlines(keepends=True)[-lines:])


def analyze(args: argparse.Namespace) -> int:
    """Cache completed positive/negative reports; fail without a report otherwise."""
    args.report.unlink(missing_ok=True)
    policy, sdk, host, plugin = load_verified_tools(args)
    context = load_json(args.context)
    identity = Identity.from_json(
        {key: value for key, value in context.items() if key not in ("schema", "argv", "virtual_headers")}
    )
    if identity.platform != sdk["platform"]:
        raise ValueError("context platform differs from the verified SDK")
    for path in (identity.source, identity.main):
        if not Path(path).is_file():
            raise ValueError(f"missing declared analysis input: {path}")
    virtual_headers = _virtual_headers(context)
    native_argv = _arguments(context.get("argv"), "native argv")
    argv = _compile_arguments(identity, native_argv)
    windows = identity.platform == "windows"
    roots = tuple(dict.fromkeys((Path.cwd(), Path(policy["source_root"]))))
    frontend = tidypolicy.frontend_policy(
        identity.source, winui=tidypolicy.winui_compile_context(identity.source), windows=windows
    )
    checks = tidyconfig.checks_for(identity.mode, policy["check"]) + frontend.check_suffix
    config = tidyconfig.config_for(checks, source_roots=roots)
    extra = [f"--extra-arg-before={argument}" for argument in (*sdk["system_arguments"], *sdk["analysis_arguments"])]
    if not windows:
        # Native -Werror would make a header-main-only compiler warning a
        # hard error; compiler diagnostics are disabled lint policy.
        extra.append("--extra-arg=-Wno-error")
    extra.extend(frontend.arguments)
    fragment = _is_fragment(identity)
    if fragment:
        # The consumer stays the main file, which only the line filter keeps
        # out. Windows can spell an included fragment with mixed separators,
        # so the filter names the file alone. Normalized primary locations
        # select the exact fragment after validating the complete attempt.
        extra.append(f"-line-filter={_line_filter([Path(identity.main).name])}")
    elif frontend.selected_path_only:
        aliases = sorted(virtual for virtual, original in virtual_headers.items() if original == identity.main)
        extra.append(f"-line-filter={_line_filter([identity.main, *aliases])}")
    main = str(Path(identity.source if fragment else identity.main).absolute())
    with tempfile.TemporaryDirectory(prefix=".db-", dir=args.report.parent) as temporary:
        work = Path(temporary)
        if identity.platform == "linux":
            argv = _compile_arguments(identity, _nix_wrapper_arguments(native_argv))
        _json(work / "compile_commands.json", [{"directory": str(Path.cwd()), "file": main, "arguments": argv}])
        # The frontend lists every file it includes; ClangTool strips -MD/-MF.
        listing = work / "includes.txt"
        extra.extend(f"--extra-arg={argument}" for argument in ("-Xclang", "-header-include-file", "-Xclang", listing))
        command = [
            host,
            *plugin,
            "--quiet",
            "--use-color=false",
            *policy["clang_tidy_args"],
            f"-config={config}",
            f"-header-filter={_header_filter(identity, policy, roots, virtual_headers)}",
            *extra,
            "-p",
            str(work.absolute()),
            main,
        ]
        result = _invoke(command)
        includes = _includes(listing, roots, virtual_headers, windows=windows)
    if fragment and result.returncode == 0 and identity.main not in includes:
        raise ValueError(f"consumer {identity.source} did not include fragment {identity.main}")
    buckets: dict[str, list[Path]] = {"STRICT": [], "RELAXED": []}
    buckets[identity.mode].append(Path(identity.main))
    stale_suppressions = tidypolicy.find_stale_nolint_suppressions(buckets)
    output = _report_output(result.stdout, roots, virtual_headers, windows=windows)
    if stale_suppressions and output and not output.endswith("\n"):
        output += "\n"
    output += "".join(
        f"{identity.main}:{issue.line}:1: warning: NOLINT names disabled check {issue.check} "
        "[aobus-readability-unused-suppression-style]\n"
        for issue in stale_suppressions
    )
    args.log.parent.mkdir(parents=True, exist_ok=True)
    args.log.write_text(output, encoding="utf-8")
    try:
        outcome = completed_outcome(args.log, result.returncode)
        if fragment:
            scoped = _fragment_output(output, identity.main, windows=windows)
            if scoped != output:
                output = scoped
                args.log.write_text(output, encoding="utf-8")
                outcome = completed_outcome(args.log, result.returncode)
    except ValueError:
        # A failed action discards its log, so keep the evidence in Bazel's output.
        print(f"clang-tidy exit {result.returncode}; analyzer output tail:\n{_log_tail(output)}", file=sys.stderr)
        raise
    _json(
        args.report,
        {
            "schema": 1,
            "identity": identity.as_json(),
            "outcome": outcome,
            "tool_exit": result.returncode,
            "includes": includes,
        },
    )
    return 0


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    preparation = commands.add_parser("prepare")
    for name in ("output", "configuration"):
        preparation.add_argument(f"--{name}", type=Path, required=True)
    preparation.set_defaults(run=prepare)
    verification = commands.add_parser("verify")
    analysis = commands.add_parser("analyze")
    for command in (verification, analysis):
        for name in ("policy", "sdk", "tidy", "log"):
            command.add_argument(f"--{name}", type=Path, required=True)
        command.add_argument("--plugin", type=Path)
    verification.add_argument("--output", type=Path, required=True)
    verification.set_defaults(run=verify)
    for name in ("context", "verification", "report"):
        analysis.add_argument(f"--{name}", type=Path, required=True)
    analysis.set_defaults(run=analyze)
    args = parser.parse_args(argv)
    try:
        return int(args.run(args))
    except (OSError, ValueError, RuntimeError, KeyError, TypeError) as error:
        print(f"Bazel lint {args.command} failed: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
