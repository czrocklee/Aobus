"""Cache the real native lint fixture suite without preparing CMake or Bazel."""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path
from typing import Any

# Supports both the portal package and a declared Python script action.
if __package__ in (None, ""):
    sys.path.insert(0, str(Path(__file__).absolute().parents[2]))

from ao.core import linttest, tidyengine
from ao.core.bazel_lint_action import load_verified_tools


def _fixture_tools(args: argparse.Namespace, sdk: dict[str, Any], host: str) -> linttest.FixtureTools:
    if sdk["platform"] == "windows":
        toolchain = tidyengine.TidyToolchain(
            clang_tidy=host,
            plugin=None,
            resource_dir=Path(sdk["resource_dir"]),
            expected_llvm_version=sdk["version"],
            sdk_root=Path(sdk["sdk_root"]),
        )
        return linttest._windows_fixture_tools(args.work, toolchain)
    # These are driver flags, not product TU/PCH options or clang-tidy extras.
    # macOS's prepared fixture profile owns SDK/deployment/libc++ shim selection.
    fixture_arguments = sdk.get("fixture_arguments", sdk["system_arguments"])
    if sdk["platform"] == "macos" and "fixture_arguments" not in sdk:
        raise ValueError("macOS fixture execution requires the prepared SDK/deployment/shim arguments")
    if not isinstance(fixture_arguments, list) or any(
        not isinstance(arg, str) or not arg or "\x00" in arg or arg.startswith("@") for arg in fixture_arguments
    ):
        raise ValueError("SDK fixture_arguments must be an array of nonempty driver arguments")
    return linttest.FixtureTools(
        tidy=host,
        flags=tuple([*fixture_arguments, *sdk["analysis_arguments"]]),
        windows=False,
        plugin=args.plugin.absolute(),
    )


def run(args: argparse.Namespace) -> int:
    """Completed assertion failures are cacheable; setup failures are not."""
    args.report.unlink(missing_ok=True)
    if args.jobs < 1:
        raise ValueError("fixture execution requires at least one worker")
    _policy, sdk, host, _plugin = load_verified_tools(args)
    tools = _fixture_tools(args, sdk, host)
    args.work.mkdir(parents=True, exist_ok=True)
    args.log.parent.mkdir(parents=True, exist_ok=True)
    args.log.write_text("", encoding="utf-8")
    status = linttest.run(args.work, tools=tools, work=args.work, log=args.log, jobs=args.jobs)
    args.report.parent.mkdir(parents=True, exist_ok=True)
    args.report.write_text(
        json.dumps({"schema": 1, "status": status, "platform": sdk["platform"]}, indent=2) + "\n",
        encoding="utf-8",
    )
    return 0


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("policy", "sdk", "verification", "tidy", "work", "report", "log"):
        parser.add_argument(f"--{name}", type=Path, required=True)
    parser.add_argument("--plugin", type=Path)
    parser.add_argument("--jobs", type=int, default=2)
    args = parser.parse_args(argv)
    try:
        return run(args)
    except (OSError, ValueError, RuntimeError, KeyError, TypeError) as error:
        print(f"Bazel lint fixtures failed: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
