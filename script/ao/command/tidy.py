"""ao tidy — project clang-tidy with the Aobus plugin, strict/relaxed modes, and dedup.

Each file is classified as STRICT (lib/app/include/tool) or RELAXED (test/). Diagnostics
are de-duplicated across translation units before being printed or written to a file.
"""

import argparse
import contextlib
import fnmatch
import json
import re
import shutil
import subprocess
import sys
from dataclasses import dataclass
from pathlib import Path

from ..core import builddir, gitfiles, pythoncheck, tidyengine
from ..core.dedup import deduplicate
from ..core.paths import PROJECT_ROOT, absolute_path
from ..core.proc import die
from ..core.tidyconfig import CONFIG_BASE
from . import build

HELP = "Run C++ clang-tidy and Python Ruff/mypy checks"
NAME = "tidy"
# True when ao.bat must initialize the MSVC/vcpkg build environment first.
REQUIRES_BUILD_ENV = True


EPILOG = """\
scope: with no arguments, checks files changed against local main + working tree +
staged + untracked. Explicit files, --folder, --commit, or --all override that.

examples:
  ./ao tidy                                  # changed files
  ./ao tidy --all                            # whole repository
  ./ao tidy --folder lib --folder script     # selected folders
  ./ao tidy --commit HEAD~3 -o report.txt    # changes since a commit, to a file
  ./ao tidy --check readability-implicit-bool-conversion
  ./ao tidy --fix lib/audio/Foo.cpp          # apply exported fixes (use with caution)
"""

ALL_FOLDERS = ["lib", "app", "include", "script", "test", "tool"]

# Check groups: start from nothing, enable curated groups, then disable known false
# positives or project-preference conflicts.
STRICT_CHECKS = ",".join(
    [
        "-*",
        # === enabled check groups ===
        "aobus-*",  # Aobus custom checks (naming, spacing, etc.)
        "bugprone-*",  # bug-prone pattern detection
        "performance-*",  # performance issues
        "cppcoreguidelines-*",  # C++ Core Guidelines
        "misc-*",  # miscellaneous useful checks
        "modernize-*",  # modern C++ usage
        "readability-*",  # readability improvements
        "portability-*",  # portability concerns
        "google-build-namespaces",
        "google-readability-casting",
        "google-readability-namespace-comments",
        # === disabled: false positives or project preference ===
        "-bugprone-easily-swappable-parameters",  # frequent false positives
        # CRTP customization deliberately refines non-virtual fallback methods
        # (RecursiveASTVisitor and std::ranges::view_interface).
        "-bugprone-derived-method-shadowing-base-method",
        "-bugprone-exception-escape",  # dominated by allocation paths and MSVC container moves
        "-bugprone-throwing-static-initialization",  # dominated by STL startup-allocation details
        "-cppcoreguidelines-avoid-magic-numbers",  # handled by aobus readability check
        "-cppcoreguidelines-avoid-const-or-ref-data-members",  # common pattern for views
        "-cppcoreguidelines-pro-bounds-avoid-unchecked-container-access",  # .at() is not a universal hot-path policy
        "-cppcoreguidelines-pro-bounds-constant-array-index",  # table dispatch is fine
        "-cppcoreguidelines-pro-bounds-pointer-arithmetic",  # common in layout/audio code
        "-misc-no-recursion",  # recursion is idiomatic in some modules
        "-misc-multiple-inheritance",  # renamed Fuchsia policy conflicts with GTK/framework composition
        "-misc-non-private-member-variables-in-classes",  # impl structs use this pattern
        "-modernize-use-trailing-return-type",  # style choice: prefer leading type
        "-modernize-use-nodiscard",  # not enforced globally
        "-modernize-use-auto",  # handled by aobus custom check
        "-performance-unnecessary-value-param",  # too noisy for interface types
        "-performance-move-const-arg",  # conflicts with const-correctness
        "-readability-redundant-member-init",  # explicit init is intentional
        "-readability-convert-member-functions-to-static",  # prefers explicit design intent
        "-portability-avoid-pragma-once",  # project standard is #pragma once
        "-clang-diagnostic-*",  # compiler diagnostics not our domain
    ]
)

RELAXED_CHECKS = ",".join(
    [
        STRICT_CHECKS,
        # === additionally disabled for test code ===
        "-bugprone-unchecked-optional-access",  # test code uses .value() liberally
        "-bugprone-unused-return-value",  # tests often discard return values
        "-cppcoreguidelines-avoid-c-arrays",  # Catch2 API and test data arrays
        "-cppcoreguidelines-avoid-magic-numbers",  # not enforced in tests
        "-cppcoreguidelines-pro-type-reinterpret-cast",  # common pattern in test mocks
        "-cppcoreguidelines-pro-type-vararg",  # debug helpers use printf-style
        "-portability-template-virtual-member-function",  # test fixtures use vfunc_* pattern
        "-modernize-use-designated-initializers",  # positional expected-data tables are clearer in tests
        "-readability-function-cognitive-complexity",  # test bodies are inherently complex
        "-readability-identifier-length",  # test locals (id, v, it) are fine
        "-readability-magic-numbers",  # not enforced in tests
    ]
)

EXPECTED_AOBUS_CHECKS = frozenset(
    {
        "aobus-async-cancellation-guard",
        "aobus-implicit-bool-conversion-in-init",
        "aobus-include-convention",
        "aobus-license-header",
        "aobus-modernize-braced-initialization",
        "aobus-modernize-concrete-final",
        "aobus-modernize-forbid-trailing-return",
        "aobus-modernize-lambda-params",
        "aobus-modernize-local-initialization-style",
        "aobus-modernize-nodiscard-usage",
        "aobus-modernize-use-ctad",
        "aobus-modernize-use-erase-if",
        "aobus-modernize-use-ranges-any-of",
        "aobus-modernize-use-ranges-contains",
        "aobus-modernize-use-ranges-min-max",
        "aobus-modernize-use-ranges-projection",
        "aobus-modernize-use-starts-with",
        "aobus-modernize-use-std-numbers",
        "aobus-modernize-use-std-to-array",
        "aobus-readability-c-api-global-qualification",
        "aobus-readability-chrono-naming-convention",
        "aobus-readability-control-block-spacing",
        "aobus-readability-forbid-raw-throw",
        "aobus-readability-identifier-naming-extensions",
        "aobus-readability-member-order",
        "aobus-readability-optional-naming-and-usage",
        "aobus-readability-pointer-naming-convention",
        "aobus-readability-redundant-namespace-qualification",
        "aobus-readability-redundant-using-directive",
        "aobus-readability-std-c-library-qualification",
        "aobus-readability-unused-suppression-style",
        "aobus-readability-use-if-init-statement",
        "aobus-strict-forward-declaration",
        "aobus-threading-policy",
    }
)

_PATH_SEPARATOR_RE = r"[/\\]"


def project_header_filter(folders: tuple[str, ...]) -> str:
    """Return a project-root filter that accepts native and POSIX separators."""
    root = re.escape(absolute_path(PROJECT_ROOT).as_posix().rstrip("/")).replace("/", _PATH_SEPARATOR_RE)
    return f"{root}{_PATH_SEPARATOR_RE}({'|'.join(folders)}){_PATH_SEPARATOR_RE}.*"


STRICT_HEADER_FILTER = project_header_filter(("lib", "app", "include", "tool"))
RELAXED_HEADER_FILTER = project_header_filter(("test", "include"))


def exact_header_filter(headers: list[Path]) -> str:
    """Return an anchored filter for mapped headers with either path separator."""
    alternatives = [re.escape(absolute_path(path).as_posix()).replace("/", _PATH_SEPARATOR_RE) for path in headers]
    return f"^({'|'.join(alternatives)})$"


def path_line_filter(paths: list[Path]) -> str:
    """Limit diagnostics and exported fixes to the explicitly selected paths."""
    entries = [{"name": absolute_path(path).as_posix(), "lines": [[1, 2_147_483_647]]} for path in paths]
    return json.dumps(entries, separators=(",", ":"))


@dataclass(frozen=True)
class TidyInvocation:
    """One selected file and the native command supplying its compiler flags."""

    selected: Path
    compile_command_source: Path

    @property
    def is_header(self) -> bool:
        return self.selected.suffix.lower() in {".h", ".hh", ".hpp", ".hxx"}


def build_invocations(
    plan: tidyengine.CompileCommandPlan,
    buckets: dict[str, list[Path]],
) -> dict[str, list[TidyInvocation]]:
    """Create one native invocation for every covered selected file."""
    mode_by_path = {absolute_path(path): mode for mode, paths in buckets.items() for path in paths}
    invocations: dict[str, list[TidyInvocation]] = {"STRICT": [], "RELAXED": []}
    for target in plan.targets:
        mode = mode_by_path[absolute_path(target.selected)]
        invocations[mode].append(
            TidyInvocation(
                selected=absolute_path(target.selected),
                compile_command_source=absolute_path(target.translation_unit),
            )
        )
    return invocations


def mode_disabled_checks(mode: str) -> list[str]:
    """The explicit disable directives a mode applies, e.g. ['-bugprone-...', ...]."""
    source = RELAXED_CHECKS if mode == "RELAXED" else STRICT_CHECKS
    return [token for token in source.split(",") if token.startswith("-") and token != "-*"]


def checks_for(mode: str, selected: str | None) -> str:
    """Resolve the clang-tidy check list for a mode.

    With --check, run only the selected check(s) but still honour the mode's disable
    list, so a check disabled for tests (e.g. readability-magic-numbers) stays off there.
    """
    if selected:
        return ",".join(["-*", selected, *mode_disabled_checks(mode)])
    return RELAXED_CHECKS if mode == "RELAXED" else STRICT_CHECKS


_NOLINT_OPEN_RE = re.compile(r"\bNOLINT(?:NEXTLINE|BEGIN)?\(([^)]*)\)")


@dataclass(frozen=True)
class StaleNolintSuppression:
    path: Path
    line: int
    check: str


def find_stale_nolint_suppressions(buckets: dict[str, list[Path]]) -> list[StaleNolintSuppression]:
    """Find NOLINT entries naming checks that the file's tidy mode never enables."""
    issues: list[StaleNolintSuppression] = []
    for mode, paths in buckets.items():
        disabled_patterns = [token.removeprefix("-") for token in mode_disabled_checks(mode)]
        for path in paths:
            try:
                lines = path.read_text(encoding="utf-8", errors="replace").splitlines()
            except OSError:
                continue
            for line_number, line in enumerate(lines, start=1):
                for match in _NOLINT_OPEN_RE.finditer(line):
                    checks = (check.strip() for check in match.group(1).split(","))
                    for check in checks:
                        if check and any(fnmatch.fnmatchcase(check, pattern) for pattern in disabled_patterns):
                            issues.append(StaleNolintSuppression(path=path, line=line_number, check=check))
    return issues


FIX_REPLACEMENT_RE = re.compile(r"^\s+- FilePath:")


def register(subparsers: "argparse._SubParsersAction[argparse.ArgumentParser]") -> None:
    parser = subparsers.add_parser(
        NAME, help=HELP, description=HELP, epilog=EPILOG, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    tidyengine.add_scope_arguments(parser, verb="check")
    parser.add_argument("--fix", action="store_true", help="apply exported fixes automatically (use with caution)")
    parser.add_argument("-n", "--no-build", action="store_true", help="use an existing plugin and compile database")
    parser.add_argument(
        "--tidy-arg",
        action="append",
        default=[],
        metavar="<arg>",
        help="extra argument passed to clang-tidy verbatim (repeatable)",
    )
    parser.add_argument("--header-filter", metavar="<regex>", help="override the per-mode header filter")
    parser.set_defaults(func=run_command)


def classify(path: Path, explicit: bool) -> str:
    """STRICT for production code, RELAXED for tests, IGNORE for lint fixtures and main."""
    try:
        rel = absolute_path(path).relative_to(absolute_path(PROJECT_ROOT)).as_posix()
    except ValueError:
        rel = absolute_path(path).as_posix()

    rel_slash = "/" + rel

    if "/test/integration/lint/fixture/" in rel_slash:
        # Fixture files: check when explicitly requested, skip in batch scans.
        return "STRICT" if explicit else "IGNORE"
    if "/test/integration/lint/" in rel_slash:
        return "IGNORE"
    if rel.endswith("/test/main.cpp") or rel == "test/main.cpp":
        return "IGNORE"
    if "/tool/lint/" in rel_slash:
        return "STRICT"
    if "/test/" in rel_slash:
        return "RELAXED"
    return "STRICT"


@dataclass(frozen=True)
class TidyToolchain:
    clang_tidy: str
    plugin: Path | None
    resource_dir: Path | None
    expected_llvm_version: str | None = None


def expected_lint_artifact_path(build_dir: Path, *, os_name: str | None = None) -> Path:
    """Return the native artifact that carries the Aobus checks."""
    filename = "AobusClangTidy.exe" if builddir.platform_profile(os_name).name == "windows" else "libAobusLintPlugin.so"
    return build_dir / "tool" / "lint" / filename


def find_lint_artifact(build_dir: Path, *, os_name: str | None = None) -> Path | None:
    """Find the native lint artifact, including multi-config output folders."""
    profile = builddir.platform_profile(os_name)
    expected = expected_lint_artifact_path(build_dir, os_name=os_name)
    if expected.is_file():
        return expected
    pattern = "**/AobusClangTidy.exe" if profile.name == "windows" else "**/libAobusLintPlugin.so"
    matches = sorted((build_dir / "tool" / "lint").glob(pattern))
    return matches[0] if len(matches) == 1 else None


def verify_tidy_toolchain(toolchain: TidyToolchain) -> None:
    """Fail closed unless clang-tidy has the expected ABI and complete Aobus registry."""
    if toolchain.expected_llvm_version is not None:
        version_command = [toolchain.clang_tidy, "--version"]
        try:
            version_result = subprocess.run(
                version_command,
                cwd=PROJECT_ROOT,
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
                text=True,
            )
        except FileNotFoundError as error:
            raise die(f"clang-tidy tool not found: {toolchain.clang_tidy}") from error
        version_pattern = rf"(?<![0-9.]){re.escape(toolchain.expected_llvm_version)}(?![0-9.])"
        if version_result.returncode != 0 or re.search(version_pattern, version_result.stdout) is None:
            raise die(
                f"the selected clang-tidy is not LLVM {toolchain.expected_llvm_version}; "
                f"command exited {version_result.returncode}: {' '.join(version_command)}"
            )

    command = [toolchain.clang_tidy]
    if toolchain.plugin is not None:
        command.append(f"-load={toolchain.plugin}")
    command.extend(["-checks=-*,aobus-*", "-list-checks"])
    try:
        result = subprocess.run(
            command,
            cwd=PROJECT_ROOT,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
        )
    except FileNotFoundError as error:
        raise die(f"clang-tidy tool not found: {toolchain.clang_tidy}") from error
    registered_checks = {line.strip() for line in result.stdout.splitlines() if line.strip().startswith("aobus-")}
    if result.returncode != 0 or registered_checks != EXPECTED_AOBUS_CHECKS:
        missing = sorted(EXPECTED_AOBUS_CHECKS - registered_checks)
        unexpected = sorted(registered_checks - EXPECTED_AOBUS_CHECKS)
        details = []
        if missing:
            details.append(f"missing {', '.join(missing)}")
        if unexpected:
            details.append(f"unexpected {', '.join(unexpected)}")
        detail = f" ({'; '.join(details)})" if details else ""
        raise die(
            "the selected clang-tidy toolchain did not expose the complete Aobus check registry"
            f"{detail}; command exited {result.returncode}: {' '.join(command)}"
        )


def prepare_toolchain(
    build_dir: Path,
    *,
    no_build: bool,
    reconfigure_preset: bool = False,
) -> TidyToolchain:
    profile = builddir.platform_profile()
    expected = expected_lint_artifact_path(build_dir)

    if no_build:
        if not (build_dir / "compile_commands.json").is_file():
            raise die(f"compile_commands.json not found in {build_dir}")
    else:
        tidyengine.ensure_compile_db(
            build_dir,
            ["-DAOBUS_BUILD_LINT_PLUGIN=ON"],
            preset=builddir.tidy_preset(),
            reconfigure_preset=reconfigure_preset,
        )
        target = "AobusClangTidy" if profile.name == "windows" else "AobusLintPlugin"
        print(f"Building {target} (incremental)...")
        tool_build = subprocess.run(
            ["cmake", "--build", str(build_dir), "--target", target, *build.parallel_build_arguments()],
            cwd=PROJECT_ROOT,
            stdout=subprocess.DEVNULL,
        )
        if tool_build.returncode != 0:
            raise die(f"failed to build {target}.")

    artifact = find_lint_artifact(build_dir)
    if artifact is None:
        raise die(f"native lint artifact was not found at {expected}")
    toolchain = TidyToolchain(
        clang_tidy=tidyengine.clang_tool(build_dir, "clang-tidy"),
        plugin=None if profile.name == "windows" else artifact,
        resource_dir=tidyengine.clang_resource_dir(build_dir) if profile.name == "windows" else None,
        expected_llvm_version=(tidyengine.llvm_sdk_version(build_dir) if profile.name == "windows" else None),
    )
    verify_tidy_toolchain(toolchain)
    return toolchain


def classify_existing(files: list[str], explicit: bool) -> dict[str, list[Path]]:
    """Resolve, dedupe, and bucket the candidate files."""
    buckets: dict[str, list[Path]] = {"STRICT": [], "RELAXED": []}
    seen: set[Path] = set()
    for name in files:
        path = Path(name)
        if not path.is_absolute():
            path = PROJECT_ROOT / path
        if not path.is_file():
            continue
        path = absolute_path(path)
        if path in seen:
            continue
        seen.add(path)
        mode = classify(path, explicit)
        if mode != "IGNORE":
            buckets[mode].append(path)
    return buckets


def split_existing(files: list[str]) -> tuple[list[str], list[str]]:
    cpp_files: list[str] = []
    python_files: list[str] = []
    seen: set[Path] = set()
    for name in files:
        path = Path(name)
        if not path.is_absolute():
            path = PROJECT_ROOT / path
        if not path.is_file():
            continue
        resolved = absolute_path(path)
        if resolved in seen:
            continue
        seen.add(resolved)
        try:
            rel = resolved.relative_to(absolute_path(PROJECT_ROOT)).as_posix()
        except ValueError:
            rel = resolved.as_posix()
        if resolved.suffix in gitfiles.CPP_SUFFIXES:
            cpp_files.append(rel)
        elif resolved.suffix in gitfiles.PYTHON_SUFFIXES:
            python_files.append(rel)
    return cpp_files, python_files


def missing_explicit_files(files: list[str]) -> list[str]:
    """Return explicit paths that do not name regular files."""
    missing: list[str] = []
    for name in files:
        path = Path(name)
        if not path.is_absolute():
            path = PROJECT_ROOT / path
        if not path.is_file():
            missing.append(name)
    return missing


def filter_fixes_yaml(text: str) -> str:
    """Drop identifier-naming diagnostics from exported fixes; renames are never auto-applied."""
    output: list[str] = []
    skip = False
    for line in text.splitlines(keepends=True):
        if line.startswith("  - DiagnosticName:"):
            skip = "readability-identifier-naming" in line
            if not skip:
                output.append(line)
        elif line.startswith("    "):
            if not skip:
                output.append(line)
        else:
            skip = False
            output.append(line)
    return "".join(output)


def parse_diagnostics_from_yaml(text: str) -> list[str]:
    blocks: list[str] = []
    current_block: list[str] = []
    in_diagnostics = False

    for line in text.splitlines(keepends=True):
        if line.startswith("Diagnostics:"):
            in_diagnostics = True
            continue
        if not in_diagnostics:
            continue

        if line.startswith("  - DiagnosticName:"):
            if current_block:
                blocks.append("".join(current_block))
            current_block = [line]
        elif line.startswith("    ") or line.strip() == "":
            if current_block:
                current_block.append(line)
        else:
            if current_block:
                blocks.append("".join(current_block))
                current_block = []
            in_diagnostics = False

    if current_block:
        blocks.append("".join(current_block))

    return blocks


def normalize_block_paths(block: str) -> str:
    lines = []
    for line in block.splitlines(keepends=True):
        stripped = line.strip()
        if stripped.startswith("FilePath:"):
            indent_len = line.find("FilePath:")
            indent = line[:indent_len]
            path_str = stripped.split(":", 1)[1].strip().strip("'\"")
            canonical_path = absolute_path(Path(path_str)).as_posix()
            lines.append(f"{indent}FilePath:        '{canonical_path}'\n")
        else:
            lines.append(line)
    return "".join(lines)


def extract_replacement_key(block: str) -> str:
    key_parts = []
    for line in block.splitlines():
        line_stripped = line.strip()
        if (
            line_stripped.startswith("FilePath:")
            or line_stripped.startswith("Offset:")
            or line_stripped.startswith("Length:")
            or line_stripped.startswith("ReplacementText:")
        ):
            key_parts.append(line_stripped)
    return "\n".join(key_parts)


def deduplicate_fixes(tmpdir: Path) -> None:
    yaml_files = list(tmpdir.glob("*.yaml"))
    if not yaml_files:
        return

    unique_diagnostics: list[str] = []
    seen: set[str] = set()

    for yaml_file in yaml_files:
        content = yaml_file.read_text(encoding="utf-8")
        blocks = parse_diagnostics_from_yaml(content)
        for block in blocks:
            normalized_block = normalize_block_paths(block)
            rep_key = extract_replacement_key(normalized_block)
            key = rep_key if rep_key else normalized_block.strip()
            if key not in seen:
                seen.add(key)
                unique_diagnostics.append(normalized_block)

    # Use MainSourceFile from the first file found
    main_source = "PROJECT_ROOT"
    for yaml_file in yaml_files:
        content = yaml_file.read_text(encoding="utf-8")
        for line in content.splitlines():
            if line.startswith("MainSourceFile:"):
                main_source = line.split(":", 1)[1].strip().strip("'\"")
                break
        if main_source != "PROJECT_ROOT":
            break

    # Unlink all original yaml files
    for yaml_file in yaml_files:
        yaml_file.unlink()

    # Write a single consolidated fixes file
    consolidated_file = tmpdir / "consolidated_fixes.yaml"
    output = []
    output.append("---")
    output.append(f"MainSourceFile:  '{main_source}'")
    if unique_diagnostics:
        output.append("Diagnostics:")
        for diag in unique_diagnostics:
            output.append(diag.rstrip("\n"))
    else:
        output.append("Diagnostics:     []")
    output.append("...")

    consolidated_file.write_text("\n".join(output) + "\n", encoding="utf-8")


def apply_fixes(tmpdir: Path, clang_apply_replacements: str = "clang-apply-replacements") -> bool:
    deduplicate_fixes(tmpdir)
    yaml_files = sorted(tmpdir.glob("*.yaml"))
    for yaml_file in yaml_files:
        yaml_file.write_text(filter_fixes_yaml(yaml_file.read_text(encoding="utf-8")), encoding="utf-8")

    replacements = sum(
        1
        for yaml_file in yaml_files
        for line in yaml_file.read_text(encoding="utf-8").splitlines()
        if FIX_REPLACEMENT_RE.match(line)
    )
    if replacements == 0:
        if not yaml_files:
            print("  No automatic fixes were exported.")
        else:
            print(
                f"  No automatic replacements were exported ({len(yaml_files)} diagnostic fix file(s) "
                "contained no replacements)."
            )
        return True

    print(f"  Applying {replacements} replacement(s) from {len(yaml_files)} fix file(s) safely...")
    try:
        status = subprocess.call([clang_apply_replacements, "--ignore-insert-conflict", str(tmpdir)])
    except FileNotFoundError:
        print(f"ERROR: clang-apply-replacements not found: {clang_apply_replacements}", file=sys.stderr)
        return False
    if status != 0:
        print("ERROR: clang-apply-replacements failed.", file=sys.stderr)
        return False
    return True


def run_command(args: argparse.Namespace) -> int:
    build_dir = Path(args.path) if args.path else builddir.tidy_dir()
    files, explicit = tidyengine.resolve_scope(args, ALL_FOLDERS, "Checking", suffixes=gitfiles.SOURCE_SUFFIXES)
    if explicit:
        missing_files = missing_explicit_files(files)
        if missing_files:
            print("ERROR: explicitly selected paths do not exist or are not files:", file=sys.stderr)
            for missing_name in missing_files:
                print(f"  {missing_name}", file=sys.stderr)
            return 1
    cpp_files, python_files = split_existing(files)

    out = open(args.output, "w", encoding="utf-8") if args.output else sys.stdout
    with contextlib.ExitStack() as stack:
        if args.output:
            stack.callback(out.close)

        overall_failed = False

        if python_files:
            if args.check:
                print("Skipping Python checks because --check selects a clang-tidy check.", file=sys.stderr)
            elif args.fix:
                print("Skipping Python checks because --fix only applies clang-tidy fixes.", file=sys.stderr)
            else:
                print(f"=== Python hygiene — {len(python_files)} file(s) ===")
                if pythoncheck.run_paths(python_files, sink=out if args.output else None) != 0:
                    overall_failed = True

        if not cpp_files:
            if not python_files:
                print("No .cpp/.h/.hpp/.py files found.", file=sys.stderr)
            return 1 if overall_failed else 0

        toolchain = prepare_toolchain(
            build_dir,
            no_build=args.no_build,
            reconfigure_preset=args.path is None,
        )

        buckets = classify_existing(cpp_files, explicit)
        if not buckets["STRICT"] and not buckets["RELAXED"]:
            return 1 if overall_failed else 0

        stale_suppressions = find_stale_nolint_suppressions(buckets)
        if stale_suppressions:
            print("ERROR: stale NOLINT suppressions name checks disabled for their file mode:", file=sys.stderr)
            for issue in stale_suppressions:
                try:
                    display_path = issue.path.relative_to(absolute_path(PROJECT_ROOT)).as_posix()
                except ValueError:
                    display_path = issue.path.as_posix()
                print(f"  {display_path}:{issue.line}: {issue.check}", file=sys.stderr)
            print("Remove these suppressions; the configured check cannot emit there.", file=sys.stderr)
            return 1

        selected = [*buckets["STRICT"], *buckets["RELAXED"]]
        coverage_plan = tidyengine.compile_command_plan(build_dir, selected)
        if coverage_plan.deferred:
            label = "ERROR: explicitly selected files" if explicit else "Deferred files"
            print(f"{label} without a compile command on this platform:", file=sys.stderr)
            for path in coverage_plan.deferred:
                print(f"  {path}", file=sys.stderr)
            if explicit:
                print(
                    "Run those files on the platform that builds them, or use a build directory "
                    "whose compile_commands.json contains them.",
                    file=sys.stderr,
                )
                return 1
            print(
                "  These files were not checked here; the platform that builds them must cover them.",
                file=sys.stderr,
            )

        invocations = build_invocations(coverage_plan, buckets)
        if not invocations["STRICT"] and not invocations["RELAXED"]:
            print(
                "ERROR: clang-tidy coverage is incomplete: no selected C++ file has a native compile command.",
                file=sys.stderr,
            )
            return 1

        mapped_headers = [target for target in coverage_plan.targets if target.is_header]
        native_database_dir = build_dir
        if toolchain.resource_dir is not None:
            native_database_dir = tidyengine.make_tmpdir("tidy-windows-db-")
            stack.callback(shutil.rmtree, native_database_dir, ignore_errors=True)
            tidyengine.write_filtered_compile_database(
                build_dir,
                native_database_dir,
                ("/Zc:preprocessor",),
            )
        header_database_dir: Path | None = None
        if mapped_headers:
            translation_units = {absolute_path(target.translation_unit) for target in mapped_headers}
            print(
                f"Header coverage: {len(mapped_headers)} header(s) mapped to "
                f"{len(translation_units)} exact native translation unit(s).",
                file=sys.stderr,
            )
            print(
                "  Each header is checked as the main file with compiler flags copied from its native implementation.",
                file=sys.stderr,
            )
            header_database_dir = tidyengine.make_tmpdir("tidy-header-db-")
            stack.callback(shutil.rmtree, header_database_dir, ignore_errors=True)
            tidyengine.write_header_compile_database(
                native_database_dir,
                mapped_headers,
                header_database_dir,
                excluded_arguments=("/TP",) if toolchain.resource_dir is not None else (),
            )

        clang_tidy = toolchain.clang_tidy
        clang_apply_replacements = (
            tidyengine.clang_tool(build_dir, "clang-apply-replacements") if args.fix else "clang-apply-replacements"
        )
        isystem = tidyengine.system_include_args()
        if args.debug:
            print(f"DEBUG ISYSTEM_ARGS: {isystem}", file=sys.stderr)

        def run_one(mode: str, invocation: TidyInvocation, log: Path) -> int:
            checks = checks_for(mode, args.check)
            header_filter = args.header_filter or (
                exact_header_filter([invocation.selected])
                if invocation.is_header
                else (RELAXED_HEADER_FILTER if mode == "RELAXED" else STRICT_HEADER_FILTER)
            )
            extra: list[str] = list(isystem)
            if toolchain.resource_dir is not None:
                extra.append(f"--extra-arg-before=-resource-dir={toolchain.resource_dir}")
                # VS 18's STL lets Clang classify three-byte records as SIMD-find
                # candidates, but its vectorized helper supports only 1/2/4/8-byte
                # elements. clang-tidy does not link or execute the TU, so disabling
                # that optional STL implementation path preserves the analyzed API.
                extra.append("--extra-arg-before=-D_USE_STD_VECTOR_ALGORITHMS=0")
            if invocation.is_header:
                extra.append(f"-line-filter={path_line_filter([invocation.selected])}")
                extra.append("--extra-arg-before=-x")
                extra.append("--extra-arg-before=c++-header")
            if "linux-gtk/" in invocation.compile_command_source.as_posix():
                # GTK framework patterns: gtkmm widget trees own children, printf-style APIs.
                checks += ",-cppcoreguidelines-owning-memory"
                extra.append("--extra-arg=-Wno-format-nonliteral")
            if toolchain.plugin is not None:
                extra.append(f"-load={toolchain.plugin}")
            if args.fix:
                extra.append(f"-export-fixes={log.with_suffix('.yaml')}")
            config = " ".join(CONFIG_BASE.replace("PLACEHOLDER", checks).split())
            compile_database = header_database_dir if invocation.is_header else native_database_dir
            if compile_database is None:
                raise die(f"synthetic compile database was not created for {invocation.selected}")
            command = [
                clang_tidy,
                "--quiet",
                "-p",
                str(compile_database),
                *args.tidy_arg,
                f"-config={config}",
                f"-header-filter={header_filter}",
                *extra,
                str(invocation.selected),
            ]
            with open(log, "w", encoding="utf-8") as sink:
                if args.debug:
                    sink.write(f"DEBUG CONFIG: {config}\n")
                status = subprocess.call(command, cwd=PROJECT_ROOT, stdout=sink, stderr=subprocess.STDOUT)
            if tidyengine.log_has_compile_command_error(log):
                return status or 1
            return status

        for mode in ("STRICT", "RELAXED"):
            batch = invocations[mode]
            if not batch:
                continue
            print(f"=== clang-tidy [{mode}] — {len(batch)} selected file(s), {len(batch)} native invocation(s) ===")
            tmpdir = tidyengine.make_tmpdir("tidy-")

            def run_file(invocation: TidyInvocation, log: Path, current_mode: str = mode) -> int:
                return run_one(current_mode, invocation, log)

            try:
                result = tidyengine.run_parallel(batch, args.jobs, tmpdir, run_file)
                if result.failed:
                    overall_failed = True
                    print("=== Failed logs ===", file=sys.stderr)
                    for log in result.failed_logs:
                        print(f"--- Log: {log} ---", file=sys.stderr)
                        sys.stderr.write(log.read_text(encoding="utf-8", errors="replace"))

                noisy = tidyengine.logs_with_diagnostics(result.logs)
                if noisy:
                    deduplicate(noisy, out, PROJECT_ROOT)
                    out.flush()
                    overall_failed = True
                    if args.output:
                        print(f"  Output appended to {args.output}")
                else:
                    print("  No warnings found.")

                if args.fix:
                    if result.failed:
                        print("  Skipping automatic fixes because clang-tidy failed for at least one file.")
                    elif not apply_fixes(tmpdir, clang_apply_replacements):
                        overall_failed = True
            finally:
                shutil.rmtree(tmpdir, ignore_errors=True)

        return 1 if overall_failed else 0
