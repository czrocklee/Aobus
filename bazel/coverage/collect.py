"""Strict LLVM gcov collection inside one Bazel C++ test action.

The public run-under launcher runs inside the stock outer coverage runner,
from test runfiles. ROOT anchors declared metadata while test argv/cwd and the
outer counter layout remain unchanged. Checked pairs and independent LCOV stay
in undeclared test outputs; the stock merger is shell-launched separately with
declared Java runfiles and unchanged arguments.
"""

from __future__ import annotations

import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
from collections.abc import Mapping, Sequence
from pathlib import Path
from typing import Any


def digest(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def save(path: Path, value: object) -> None:
    path.write_text(json.dumps(value, indent=2, sort_keys=True) + "\n", encoding="utf-8")


def absolute(root: Path, value: str) -> Path:
    path = Path(value)
    return Path(os.path.abspath(root / path))


def source_mappings(path: Path | None) -> dict[str, str]:
    """Bazel's reported-to-actual source metadata uses reported:actual rows."""
    result: dict[str, str] = {}
    if path is not None:
        for row in path.read_text(encoding="utf-8").splitlines():
            fields = row.split(":")
            if (
                len(fields) != 2
                or not all(fields)
                or fields[0] in result
                or any(ord(char) < 32 or ord(char) == 127 for field in fields for char in field)
            ):
                raise ValueError(f"malformed/duplicate source mapping: {row!r}")
            result[fields[0]] = fields[1]
    return result


def source_name(
    root: Path,
    value: str,
    mappings: Mapping[str, str],
    authored_root: Path | None = None,
) -> tuple[str, Path | None]:
    if not value or any(char in value for char in "\r\n\x00") or ".." in Path(value).parts:
        raise ValueError("missing or malformed report source")
    path = absolute(root, value)
    lexical = path.relative_to(root).as_posix() if path.is_relative_to(root) else str(path)
    mapped = mappings.get(value, mappings.get(lexical))
    if mapped is not None:
        if not mapped or ".." in Path(mapped).parts or any(char in mapped for char in "\r\n\x00"):
            raise ValueError("malformed authored-source mapping target")
        path = absolute(root, mapped)
    elif "_virtual_includes" in path.parts:
        raise ValueError(f"virtual source lacks authored-source metadata: {value}")
    # Classify lexical authored views before resolving source symlinks. A
    # generated path containing app/ is not an authored app/ source. Exclusion
    # depends on namespace, never on whether a source happens to be missing.
    for view in (root, authored_root or root):
        if path.is_relative_to(view):
            relative = path.relative_to(view)
            if relative.parts and relative.parts[0] in {"app", "lib", "include"}:
                # Execroot paths identify authored names, not readable runtime
                # source inputs. Always read the frozen original source view;
                # never prefer or fall back to an execroot copy's contents.
                return relative.as_posix(), (authored_root or root) / relative
    if path.is_relative_to(root):
        relative = path.relative_to(root)
        if not relative.parts or relative.parts[0] not in {"test", "external", "bazel-out"}:
            raise ValueError(f"unknown relative coverage source namespace: {value}")
        return relative.as_posix(), None
    # Absolute sources outside the declared authored views are foreign inputs,
    # such as immutable SDK headers. Their records still require full parsing.
    return str(path), None


def intermediate(
    text: str,
    root: Path,
    mappings: Mapping[str, str],
    *,
    authored_root: Path | None = None,
) -> tuple[dict[str, dict[int, int]], dict[str, str]]:
    """Parse every record; source-byte identity/bounds cover authored production only."""
    reports: dict[str, dict[int, int]] = {}
    identities: dict[str, str] = {}
    current: str | None = None
    lengths: dict[str, int] = {}
    for row in text.splitlines():
        if row.startswith("file:"):
            current, path = source_name(root, row[5:], mappings, authored_root)
            if path is not None:
                identities[current] = digest(path)
                lengths[current] = len(path.read_text(encoding="utf-8").splitlines())
            reports.setdefault(current, {})
        elif row.startswith("lcount:"):
            match = re.fullmatch(r"lcount:([1-9]\d*),(\d+)(?:,[01])?", row)
            if current is None or match is None:
                raise ValueError(f"malformed line record: {row!r}")
            number, hits = map(int, match.groups()[:2])
            if current in lengths and number > lengths[current]:
                raise ValueError(f"source line outside current source: {current}:{number}")
            lines = reports[current]
            lines[number] = lines.get(number, 0) + hits
        elif row.startswith("function:"):
            # Location zero denotes compiler-generated code with no source
            # location. Function metadata never contributes executable lines.
            if current is None or re.fullmatch(r"function:(?:0|[1-9]\d*),\d+,.+", row) is None:
                raise ValueError(f"malformed function record: {row!r}")
        else:
            raise ValueError(f"unknown or malformed gcov record: {row!r}")
    if not reports or not any(reports.values()):
        raise ValueError("empty gcov report; no executable source lines")
    return reports, identities


def lcov(reports: Mapping[str, Mapping[int, int]]) -> str:
    rows: list[str] = []
    for source, lines in sorted(reports.items()):
        if not lines:
            continue
        rows.extend(["TN:", f"SF:{source}"])
        rows.extend(f"DA:{number},{hits}" for number, hits in sorted(lines.items()))
        rows.extend([f"LF:{len(lines)}", f"LH:{sum(hits > 0 for hits in lines.values())}", "end_of_record"])
    if not rows:
        raise ValueError("no executable coverage records")
    return "\n".join(rows) + "\n"


def check_pair(notes: Path, profile: Path) -> None:
    left, right = notes.read_bytes(), profile.read_bytes()
    if (
        len(left) < 12
        or len(right) < 12
        or (left[:4], right[:4]) not in {(b"oncg", b"adcg"), (b"gcno", b"gcda")}
        or left[4:12] != right[4:12]
    ):
        raise ValueError(f"empty, stale or incompatible gcno/gcda pair: {notes}, {profile}")


def _prove_zero_function_notes(path: Path) -> None:
    """Prove the known LLVM B11* zero-record GCNO layout, not a compiler version.

    Any records (including FUNCTION), unsupported format or malformed bytes
    reject an empty extraction. Normal nonempty reports do not use this proof.
    """
    data = path.read_bytes()
    if len(data) < 12 or data[:8] != b"oncg*11B":
        raise ValueError("empty extraction has unsupported GCNO format for zero-function proof")
    offset = 12

    def word() -> int:
        nonlocal offset
        if offset + 4 > len(data):
            raise ValueError("truncated GCNO zero-function proof")
        value = int.from_bytes(data[offset : offset + 4], "little")
        offset += 4
        return value

    length = word()
    if length == 0 or length > (len(data) - offset) // 4:
        raise ValueError("malformed GCNO working-directory length")
    directory = data[offset : offset + length * 4]
    offset += length * 4
    name, terminator, padding = directory.partition(b"\0")
    if not name or not terminator or any(padding):
        raise ValueError("malformed GCNO working-directory string")
    name.decode("utf-8")
    unexecuted_blocks, tag, record_length = word(), word(), word()
    if unexecuted_blocks not in {0, 1} or tag != 0 or record_length != 0 or offset != len(data):
        raise ValueError("empty extraction lacks an exact zero-function GCNO terminator")


def collect(
    root: Path, coverage: Path, manifest: Path, tool: Path, output: Path, environment: Mapping[str, str]
) -> None:
    """Consume every observed profile once; unlinked notes need no counter."""
    receipt_path = output / "receipt.json"
    receipt: dict[str, Any] = {
        "schema": 2,
        "source_mapping_contract": "declared-standard",
        "attempt": environment["AOBUS_COVERAGE_ATTEMPT"],
        "test": environment.get("TEST_TARGET"),
        "manifest": str(manifest),
        "manifest_sha256": digest(manifest),
        "root": str(root),
        "coverage_dir": str(coverage),
        "gcov": str(tool),
        "gcov_sha256": digest(tool),
        "status": "collecting",
        "report_role": "independent strict per-profile LCOV, not the stock test or combined report",
        "profiles": [],
        "sources": {},
        "source_identity_scope": "authored app/, lib/, include/ only; all raw records remain syntax-checked",
    }
    save(receipt_path, receipt)
    try:
        authored_root = Path(environment.get("AOBUS_COVERAGE_SOURCE_ROOT", str(root)))
        if not authored_root.is_absolute():
            raise ValueError("authored coverage source root must be absolute")
        receipt["authored_root"] = str(authored_root)
        shutil.copyfile(manifest, output / "instrumented-files.txt")
        mappings_path = environment.get("COVERAGE_REPORTED_TO_ACTUAL_SOURCES_FILE")
        mapping_file = absolute(root, mappings_path) if mappings_path else None
        mappings = source_mappings(mapping_file)
        standard_file = output / "source-mappings.txt"
        if mapping_file is not None:
            shutil.copyfile(mapping_file, standard_file)
        receipt["standard_source_mappings"] = {
            "path": str(mapping_file) if mapping_file is not None else None,
            "sha256": digest(standard_file) if mapping_file is not None else None,
            "mappings": dict(mappings),
        }
        receipt["source_mappings"] = dict(mappings)
        if tool.name != "llvm-cov":
            raise ValueError(f"coverage requires the selected llvm-cov, not an ambient gcov: {tool}")
        version = subprocess.run([str(tool), "gcov", "--version"], capture_output=True, text=True, check=True)
        receipt["gcov_version"] = version.stdout.strip()
        if re.search(r"LLVM version [0-9]+\.", version.stdout) is None:
            raise ValueError("coverage requires the selected LLVM gcov reader's version capability")
        notes: dict[str, Path] = {}
        for row in manifest.read_text(encoding="utf-8").splitlines():
            if row.endswith(".gcno"):
                path = absolute(root, row)
                if not path.is_relative_to(root) or ".." in Path(row).parts:
                    raise ValueError(f"unsafe notes manifest path: {row}")
                key = path.relative_to(root).with_suffix(".gcda").as_posix()
                if key in notes:
                    raise ValueError(f"duplicate notes manifest entry: {row}")
                notes[key] = path
        profiles = sorted(coverage.rglob("*.gcda"))
        receipt["observed_profiles"] = [str(path) for path in profiles]
        if not profiles:
            raise ValueError("no current-test gcda profiles")
        if list(coverage.rglob("*.profraw")):
            raise ValueError("unexpected LLVM-map profiles in gcov coverage")
        merged: dict[str, dict[int, int]] = {}
        consumed: set[str] = set()
        for index, profile in enumerate(profiles):
            if profile.is_symlink() or not profile.is_file():
                raise ValueError(f"invalid counter file: {profile}")
            relative = profile.relative_to(coverage).as_posix()
            matches = [key for key in notes if relative == key or relative.endswith("/" + key)]
            if len(matches) != 1 or matches[0] in consumed:
                raise ValueError(f"missing/ambiguous/duplicate gcno for observed profile: {relative}")
            key = matches[0]
            consumed.add(key)
            pair = output / f"profile-{index:05d}"
            pair.mkdir()
            record: dict[str, Any] = {"profile": str(profile), "notes": str(notes[key]), "status": "extracting"}
            receipt["profiles"].append(record)
            save(receipt_path, receipt)
            local_profile, local_notes = pair / "object.gcda", pair / "object.gcno"
            shutil.copyfile(profile, local_profile)
            shutil.copyfile(notes[key], local_notes)
            check_pair(local_notes, local_profile)
            record.update(profile_sha256=digest(local_profile), notes_sha256=digest(local_notes))
            argv = [str(tool), "gcov", "-i", "-o", str(pair), str(local_profile)]
            result = subprocess.run(argv, cwd=pair, capture_output=True, text=True, check=False)
            (pair / "stdout.log").write_text(result.stdout, encoding="utf-8")
            (pair / "stderr.log").write_text(result.stderr, encoding="utf-8")
            record.update(argv=argv, exit=result.returncode)
            if result.returncode or result.stderr.strip():
                raise ValueError(f"gcov extraction failed ({result.returncode}): {profile}: {result.stderr.strip()}")
            reports = sorted(pair.glob("*.gcov"))
            if not reports:
                raise ValueError(f"gcov produced no reports: {profile}")
            texts = [report.read_text(encoding="utf-8") for report in reports]
            zero_functions = any(text == "" for text in texts)
            if zero_functions:
                if any(text != "" for text in texts):
                    raise ValueError("mixed empty/nonempty gcov reports cannot prove a zero-function translation unit")
                _prove_zero_function_notes(local_notes)
                record.update(executable_functions=0, empty_report_reason="proved zero-function translation unit")
            record["reports"] = []
            for report, text in zip(reports, texts, strict=True):
                if zero_functions:
                    data: dict[str, dict[int, int]] = {}
                    identities: dict[str, str] = {}
                else:
                    data, identities = intermediate(text, root, mappings, authored_root=authored_root)
                for source, identity in identities.items():
                    previous = receipt["sources"].setdefault(source, identity)
                    if previous != identity:
                        raise ValueError(f"source changed during collection: {source}")
                for source, lines in data.items():
                    target = merged.setdefault(source, {})
                    for number, hits in lines.items():
                        target[number] = target.get(number, 0) + hits
                record["reports"].append({"path": str(report.relative_to(output)), "sha256": digest(report)})
            record["status"] = "passed"
            save(receipt_path, receipt)
        if sorted(coverage.rglob("*.gcda")) != profiles or any(
            digest(profile) != record["profile_sha256"]
            for profile, record in zip(profiles, receipt["profiles"], strict=True)
        ):
            raise ValueError("profiles changed or appeared during collection")
        report_path = output / "coverage.lcov"
        report_path.write_text(lcov(merged), encoding="utf-8")
        receipt.update(status="passed", consumed_profiles=len(consumed), lcov_sha256=digest(report_path))
    except BaseException as error:
        receipt.update(status="failed", error=str(error) or type(error).__name__)
        raise
    finally:
        save(receipt_path, receipt)


def exit_status(returncode: int) -> int:
    """Report a signal-terminated child with the shell's 128+N convention."""
    return returncode if returncode >= 0 else 128 - returncode


def main(argv: Sequence[str] | None = None) -> int:
    environment = dict(os.environ)
    cwd = Path.cwd()
    output: Path | None = None
    status: int | None = None
    try:
        if environment.get("SPLIT_COVERAGE_POST_PROCESSING", "0") != "0":
            raise ValueError("split coverage processing is unsupported")
        if environment.get("IGNORE_COVERAGE_COLLECTION_FAILURES"):
            raise ValueError("coverage collection failures cannot be ignored")
        root = Path(environment["ROOT"])
        if not root.is_absolute() or not root.is_dir():
            raise ValueError("run-under coverage requires the stock runner's absolute ROOT")
        coverage = absolute(root, environment["COVERAGE_DIR"])
        prefix = Path(environment["GCOV_PREFIX"])
        strip = environment["GCOV_PREFIX_STRIP"]
        if not prefix.is_absolute() or prefix != coverage or re.fullmatch(r"[0-9]+", strip) is None:
            raise ValueError("run-under coverage requires the stock runner's owned counter layout")
        manifest = absolute(root, environment["COVERAGE_MANIFEST"])
        output = absolute(root, environment["TEST_UNDECLARED_OUTPUTS_DIR"]) / "aobus-coverage"
        output.mkdir(exist_ok=False)
        coverage.mkdir(parents=True, exist_ok=True)
        if list(coverage.rglob("*.gcda")) or list(coverage.rglob("*.profraw")):
            raise ValueError("coverage directory already contains stale profiles")
        arguments = list(sys.argv[1:] if argv is None else argv)
        if not arguments:
            raise ValueError("coverage run-under requires test argv")
        status = exit_status(subprocess.run(arguments, cwd=cwd, env=environment, check=False).returncode)
        save(
            output / "test.json",
            {
                "argv": arguments,
                "cwd": str(cwd),
                "root": str(root),
                "exit": status,
                "attempt": environment["AOBUS_COVERAGE_ATTEMPT"],
            },
        )
        tool = absolute(root, environment["COVERAGE_GCOV_PATH"])
        collect(root, coverage, manifest, tool, output, environment)
        # The stock outer runner owns COVERAGE_OUTPUT_FILE. Publishing this
        # independent LCOV there would count the same profiles twice.
        return status if status else 0
    except (OSError, ValueError, KeyError, subprocess.SubprocessError) as error:
        print(f"strict coverage: {error}", file=sys.stderr)
        if output is not None:
            save(output / "failure.json", {"error": str(error), "test_exit": status})
        return status if status else 1


if __name__ == "__main__":
    raise SystemExit(main())
