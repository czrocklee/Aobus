"""Observe one ready executable tool for Bazel's live repository checks."""

from __future__ import annotations

import argparse
import json
import os
import sys
from pathlib import Path

# Bazel runs this entry with isolated Python from the selected host environment.
_ROOT = Path(__file__).resolve().parents[3]


def resolve(name: str) -> dict[str, object]:
    """Reuse provider admission without downloading or projecting host files."""
    from ao.core.paths import PROJECT_ROOT
    from ao.portal.ready_tool import ReadyTool, ToolMissing

    selection: dict[str, object] = {"name": name}
    try:
        # The resolver starts in a fresh interpreter. Import only the selected
        # provider so its watch inventory excludes unrelated tool code.
        if name == "actionlint":
            if sys.platform != "linux":
                raise ValueError("The pinned actionlint repository is only available on Linux")
            from ao.portal import source_lint

            tool = source_lint.ready_actionlint()
        elif name == "buildifier":
            from ao.portal import buildifier

            tool = buildifier.ready_tool()
        elif name == "clang-format":
            from ao.portal import source_format

            tool = source_format.clang_format_selection()
        elif name == "prettier":
            from ao.portal import prettier

            tool = prettier.ready_tool()
        else:
            raise ValueError(f"Unknown check tool: {name}")
        selection["command"] = tool.command
    except ToolMissing as error:
        # A successful repository fetch can describe an absent optional tool.
        # Its consumer must fail, and setup changes the watched missing input.
        selection["unavailable"] = str(error)
        tool = ReadyTool((), error.files)
    nix_pin = (PROJECT_ROOT / "nixpkgs.json",) if sys.platform == "linux" else ()
    imports = {
        str(path)
        for module in tuple(sys.modules.values())
        if isinstance(filename := getattr(module, "__file__", None), str)
        and (path := Path(filename)).is_relative_to(PROJECT_ROOT / "script/ao")
    }
    return {
        "selection": selection,
        "watch_files": [str(path) for path in (*tool.files, *nix_pin)],
        "directories": [str(directory) for directory in tool.directories],
        "trees": [str(tree) for tree in tool.trees],
        "imports": sorted(imports),
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("tool")
    parser.add_argument("output", type=Path)
    arguments = parser.parse_args()
    output = arguments.output.absolute()
    # A relative managed state root means the checkout, as in ./ao and the
    # live check child, not Bazel's external repository scratch directory.
    os.chdir(_ROOT)
    sys.path.insert(0, str(_ROOT / "script"))
    output.write_text(json.dumps(resolve(arguments.tool)), encoding="utf-8")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
