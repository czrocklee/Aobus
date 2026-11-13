# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Aobus Contributors
"""Run the bounded native ICU catalog pipeline inside one Bazel action."""

from __future__ import annotations

import argparse
import os
import re
import subprocess
import sys
from pathlib import Path

_LOCALE = re.compile(r"[a-z]{2,8}(?:_[A-Za-z0-9]{2,8})*\Z", re.ASCII)
_RESERVED = {"con", "prn", "aux", "nul", *(f"com{i}" for i in range(10)), *(f"lpt{i}" for i in range(10))}
_PLACEHOLDER = "@AOBUS_CATALOG_LOCALE_TAG_LITERALS@"


def read_inventory(inventory: Path, sources: list[Path]) -> list[tuple[str, str]]:
    """Validate resource basenames and the exact declared authored-source set."""
    entries = inventory.read_text(encoding="utf-8").splitlines()
    locales: list[tuple[str, str]] = []
    resources: set[str] = set()
    projections: set[str] = set()
    for entry in entries:
        if not entry.endswith(".res"):
            raise ValueError(f"Invalid catalog inventory member: {entry!r}")
        locale = entry[:-4]
        if not _LOCALE.fullmatch(locale) or locale.casefold() in _RESERVED:
            raise ValueError(f"Unsafe catalog locale: {locale!r}")
        tag = {"root": "en", "qps_Ploc": "qps-ploc"}.get(locale, locale.replace("_", "-"))
        if entry.casefold() in resources or tag.casefold() in projections:
            raise ValueError(f"Duplicate catalog resource or projection: {entry!r}")
        resources.add(entry.casefold())
        projections.add(tag.casefold())
        locales.append((locale, tag))
    if not {"root.res", "qps_ploc.res"} <= resources or not {"root", "qps_Ploc"} <= {locale for locale, _ in locales}:
        raise ValueError("Catalog inventory requires root.res and qps_Ploc.res")

    expected = {locale + ".txt" for locale, _ in locales if locale != "qps_Ploc"}
    actual: set[str] = set()
    for source in sources:
        if source.name in actual or source.parent.resolve() != inventory.parent.resolve():
            raise ValueError(f"Duplicate or misplaced authored catalog: {source}")
        if not source.is_file():
            raise ValueError(f"Missing authored catalog: {source}")
        actual.add(source.name)
    if actual != expected:
        raise ValueError(
            f"Authored catalog membership mismatch: missing={sorted(expected - actual)}, "
            f"unexpected={sorted(actual - expected)}"
        )
    return locales


def write_locale_header(template: Path, output: Path, locales: list[tuple[str, str]]) -> None:
    text = template.read_text(encoding="utf-8")
    if text.count(_PLACEHOLDER) != 1:
        raise ValueError("Locale header template must contain exactly one catalog placeholder")
    tags = ["en", *sorted(tag for locale, tag in locales if locale not in {"root", "qps_Ploc"})]
    text = text.replace(_PLACEHOLDER, "".join(f'    "{tag}",\n' for tag in tags))
    if "@" in text:
        raise ValueError("Unresolved locale header template substitution")
    output.write_text(text, encoding="utf-8", newline="\n")


def embed_data(data: Path, output: Path) -> None:
    payload = data.read_bytes()
    if not payload:
        raise ValueError("Refusing to embed an empty ICU common-data package")
    lines = [
        "// SPDX-License-Identifier: MIT",
        "// Copyright (c) 2026 Aobus Contributors",
        "// Generated from the governed ICU common-data package. Do not edit.",
        "",
        '#include "app/i18n/EmbeddedCatalogData.h"',
        "",
        "#include <cstddef>",
        "#include <span>",
        "",
        "namespace ao::i18n::detail",
        "{",
        "  namespace",
        "  {",
        "    alignas(16) constexpr std::byte kEmbeddedCatalogData[] = {",
    ]
    for offset in range(0, len(payload), 8):
        lines.append("      " + " ".join(f"std::byte{{0x{byte:02x}}}," for byte in payload[offset : offset + 8]))
    lines.extend(
        [
            "    };",
            "  } // namespace",
            "",
            "  std::span<std::byte const> embeddedCatalogData() noexcept",
            "  {",
            "    return kEmbeddedCatalogData;",
            "  }",
            "} // namespace ao::i18n::detail",
            "",
        ]
    )
    output.write_text("\n".join(lines), encoding="utf-8", newline="\n")


def absolute(path: str) -> Path:
    # Retain installed executable symlink topology; never resolve tools onto PATH.
    return Path(os.path.abspath(path))


def parse_args(argv: list[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    for option in ("inventory", "template", "compiler", "genrb", "pkgdata", "private", "cpp", "header", "dat"):
        parser.add_argument("--" + option, required=True, type=absolute)
    parser.add_argument("--source", action="append", default=[], type=absolute)
    parser.add_argument("--systemroot")
    return parser.parse_args(argv)


def run(options: argparse.Namespace) -> None:
    locales = read_inventory(options.inventory, options.source)
    for tool in (options.compiler, options.genrb, options.pkgdata):
        if not tool.is_file():
            raise ValueError(f"Missing declared catalog tool: {tool}")
    if os.name == "nt" and not options.systemroot:
        raise ValueError("Windows catalog generation requires declared SYSTEMROOT")
    outputs = [options.cpp, options.header, options.dat, options.private]
    if [path.name for path in outputs[:3]] != ["EmbeddedCatalogData.cpp", "CatalogLocaleTags.h", "aobus_messages.dat"]:
        raise ValueError("Unexpected fixed catalog output names")
    inputs = [options.inventory, options.template, *options.source, options.compiler, options.genrb, options.pkgdata]
    for index, output in enumerate(outputs):
        if output.is_symlink() or output.resolve().is_relative_to(options.inventory.parent.resolve()):
            raise ValueError(f"Unsafe catalog output path: {output}")
        for other in outputs[index + 1 :] + inputs:
            if output.resolve().is_relative_to(other.resolve()) or other.resolve().is_relative_to(output.resolve()):
                raise ValueError(f"Overlapping catalog paths: {output}, {other}")

    resources = options.private / "res"
    resw = options.private / "resw"
    temp = options.private / "temp"
    for directory in (resources, temp, options.private / "pycache", *(resw / tag for _, tag in locales)):
        directory.mkdir(parents=True, exist_ok=True)
    for output in outputs[:3]:
        output.parent.mkdir(parents=True, exist_ok=True)
    env = {
        "LC_ALL": "C" if sys.platform == "darwin" else "C.UTF-8",
        "PYTHONUTF8": "1",
        "PYTHONDONTWRITEBYTECODE": "1",
        "PYTHONPYCACHEPREFIX": str(options.private / "pycache"),
        "TEMP": str(temp),
        "TMP": str(temp),
        "TMPDIR": str(temp),
    }
    if options.systemroot:
        env["SYSTEMROOT"] = options.systemroot

    def execute(arguments: list[str | Path]) -> None:
        subprocess.run([str(argument) for argument in arguments], cwd=options.private, env=env, check=True)

    strict = ["--strict", "--encoding", "UTF-8"]
    authored = [locale for locale, _ in locales if locale != "qps_Ploc"]
    execute(
        [
            options.genrb,
            *strict,
            "--sourcedir",
            options.inventory.parent,
            "--destdir",
            resources,
            *(locale + ".txt" for locale in authored),
        ]
    )
    compiler = [
        options.compiler,
        "--root-res",
        resources / "root.res",
        "--pseudo-source",
        resources / "qps_Ploc.txt",
        "--en-resw",
        resw / "en/Resources.resw",
        "--pseudo-resw",
        resw / "qps-ploc/Resources.resw",
    ]
    for locale, tag in locales:
        if locale not in {"root", "qps_Ploc"}:
            compiler.extend(["--translation", resources / (locale + ".res"), resw / tag / "Resources.resw"])
    execute(compiler)
    execute([options.genrb, *strict, "--sourcedir", resources, "--destdir", resources, "qps_Ploc.txt"])
    execute(
        [
            options.pkgdata,
            "--quiet",
            "--force-prefix",
            "--name",
            "aobus_messages",
            "--mode",
            "common",
            "--sourcedir",
            resources,
            "--destdir",
            resources,
            "--tempdir",
            temp,
            options.inventory,
        ]
    )
    (resources / "aobus_messages.dat").replace(options.dat)
    embed_data(options.dat, options.cpp)
    write_locale_header(options.template, options.header, locales)


def main() -> int:
    try:
        run(parse_args())
    except (OSError, ValueError, subprocess.CalledProcessError) as error:
        print(f"Catalog generation failed: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
