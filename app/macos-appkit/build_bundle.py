# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Aobus Contributors
"""Assemble the declared AppKit bundle tree inside one Bazel action."""

from __future__ import annotations

import argparse
import json
import os
import plistlib
import re
import shutil
import sys
from pathlib import Path

_UNRESOLVED = re.compile(r"\$\{[^}]*\}|@[A-Za-z0-9_]+@")


def bundle_localizations(inventory: Path) -> list[str]:
    """Derive the declared bundle localizations from the sole catalog inventory.

    Root projects to English, regional underscores follow the CMake hyphen tag
    rule, pseudo localization stays excluded, English stays first, and the
    remaining tags keep stable sorted order.
    """
    tags: set[str] = set()
    for entry in inventory.read_text(encoding="utf-8").splitlines():
        if not entry.endswith(".res"):
            raise ValueError(f"Invalid catalog inventory member: {entry!r}")
        locale = entry[:-4]
        if locale == "qps_Ploc":
            continue
        tags.add("en" if locale == "root" else locale.replace("_", "-"))
    return ["en", *sorted(tags - {"en"})]


def deployment_target(macos_toolchain: Path) -> str:
    """Read the minimum system version from its single macOS authority."""
    document = json.loads(macos_toolchain.read_text(encoding="utf-8"))
    if document.get("schemaVersion") != 1:
        raise ValueError(f"Unsupported macOS toolchain schema: {document.get('schemaVersion')!r}")
    target = document.get("deploymentTarget")
    if not isinstance(target, str) or not target:
        raise ValueError("macOS toolchain declares no deployment target")
    return target


def expand_plist(template: Path, substitutions: dict[str, str]) -> str:
    text = template.read_text(encoding="utf-8")
    for placeholder, value in substitutions.items():
        text = text.replace("${" + placeholder + "}", value)
    unresolved = _UNRESOLVED.search(text)
    if unresolved:
        raise ValueError(f"Unresolved Info.plist substitution: {unresolved.group(0)!r}")
    return text


def override_plist_fields(text: str, fields: dict[str, str]) -> str:
    """Apply explicit second-consumer roles without replacing the shared template."""
    if not fields:
        return text
    plist = plistlib.loads(text.encode("utf-8"))
    if not isinstance(plist, dict):
        raise ValueError("Info.plist root must be a dictionary")
    plist.update(fields)
    return plistlib.dumps(plist, fmt=plistlib.FMT_XML, sort_keys=False).decode("utf-8")


def validate_plist(
    text: str,
    *,
    executable_name: str,
    version: str,
    deployment: str,
    localizations: list[str],
    icon_name: str,
    bundle_identifier: str | None = None,
    display_name: str | None = None,
) -> None:
    """Check that the expanded template carries the declared bundle inputs."""
    plist = plistlib.loads(text.encode("utf-8"))
    expected = {
        "CFBundleExecutable": executable_name,
        "CFBundleShortVersionString": version,
        "LSMinimumSystemVersion": deployment,
        "CFBundleLocalizations": localizations,
        "CFBundleIconFile": icon_name,
    }
    if bundle_identifier is not None:
        expected["CFBundleIdentifier"] = bundle_identifier
    if display_name is not None:
        expected["CFBundleName"] = display_name
    mismatched = {key: value for key, value in expected.items() if plist.get(key) != value}
    if mismatched:
        raise ValueError(f"Info.plist fields do not match the declared bundle inputs: {mismatched}")


def write_launcher(launcher: Path, *, tree_name: str, executable_name: str) -> None:
    """Write the run launcher; it locates the sibling tree and adds no arguments."""
    script = "\n".join(
        [
            "#!/bin/sh",
            "# Generated Aobus bundle launcher; do not edit.",
            f'exec "$(dirname "$0")/{tree_name}/Contents/MacOS/{executable_name}" "$@"',
            "",
        ]
    )
    launcher.write_text(script, encoding="utf-8", newline="\n")
    launcher.chmod(0o755)


def absolute(path: str) -> Path:
    return Path(os.path.abspath(path))


def parse_args(argv: list[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    for option in ("exe", "plist", "icon", "license", "inventory", "macos_toolchain", "app", "launcher"):
        parser.add_argument("--" + option.replace("_", "-"), dest=option, required=True, type=absolute)
    parser.add_argument("--version", required=True)
    parser.add_argument("--executable-name", required=True)
    parser.add_argument("--bundle-identifier")
    parser.add_argument("--display-name")
    return parser.parse_args(argv)


def run(options: argparse.Namespace) -> None:
    localizations = bundle_localizations(options.inventory)
    deployment = deployment_target(options.macos_toolchain)
    # A second consumer may change only these plist roles; everything else
    # stays on the shared expanded template.
    overrides = {}
    if options.bundle_identifier is not None:
        overrides["CFBundleIdentifier"] = options.bundle_identifier
    if options.display_name is not None:
        overrides["CFBundleName"] = options.display_name
    substitutions = {
        "MACOSX_BUNDLE_EXECUTABLE_NAME": options.executable_name,
        "AOBUS_APPKIT_BUNDLE_LOCALIZATIONS": "".join(f"<string>{tag}</string>" for tag in localizations),
        "PROJECT_VERSION": options.version,
        "CMAKE_OSX_DEPLOYMENT_TARGET": deployment,
    }
    plist_text = override_plist_fields(expand_plist(options.plist, substitutions), overrides)
    validate_plist(
        plist_text,
        executable_name=options.executable_name,
        version=options.version,
        deployment=deployment,
        localizations=localizations,
        icon_name=options.icon.name,
        bundle_identifier=options.bundle_identifier,
        display_name=options.display_name,
    )

    contents = options.app / "Contents"
    resources = contents / "Resources"
    (contents / "MacOS").mkdir(parents=True, exist_ok=True)
    (resources / "Brand").mkdir(parents=True, exist_ok=True)
    executable = contents / "MacOS" / options.executable_name
    executable.write_bytes(options.exe.read_bytes())
    executable.chmod(0o755)
    (contents / "Info.plist").write_text(plist_text, encoding="utf-8", newline="\n")
    shutil.copyfile(options.icon, resources / options.icon.name)
    shutil.copyfile(options.license, resources / "Brand" / options.license.name)
    write_launcher(options.launcher, tree_name=options.app.name, executable_name=options.executable_name)


def main() -> int:
    try:
        run(parse_args())
    except (OSError, ValueError, plistlib.InvalidFileException) as error:
        print(f"AppKit bundle assembly failed: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
