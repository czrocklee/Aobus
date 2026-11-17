"""Guard lazy dependencies that can escape narrow Bazel consumers unnoticed."""

import ast
import importlib.util
import sys
import unittest
from pathlib import Path

# Use declared source data, not the physical checkout beside symlink runfiles.
_SOURCE = Path(__file__).absolute().parents[2] / "script" / "ao"
_ALLOWED = {
    "core": {"core"},
    "policy": {"core", "policy"},
    "graph": {"core", "policy", "graph"},
}
_PROCESS_OR_NETWORK = {"subprocess", "multiprocessing", "urllib", "http", "socket", "ctypes"}


def _imports(module: str, path: Path, modules: set[str]):
    package = module if path.name == "__init__.py" else module.rsplit(".", 1)[0]
    for node in ast.walk(ast.parse(path.read_text(encoding="utf-8"), filename=str(path))):
        if isinstance(node, ast.Import):
            yield from (alias.name for alias in node.names)
        elif isinstance(node, ast.ImportFrom):
            name = node.module or ""
            if node.level:
                name = importlib.util.resolve_name("." * node.level + name, package)
            yield name
            yield from (f"{name}.{alias.name}" for alias in node.names if f"{name}.{alias.name}" in modules)


class PackageBoundaryTests(unittest.TestCase):
    def test_narrow_consumers_do_not_reach_portal_or_impure_policy_dependencies(self):
        paths = {
            ("ao." + ".".join(path.relative_to(_SOURCE).with_suffix("").parts)).removesuffix(".__init__"): path
            for path in _SOURCE.rglob("*.py")
        }
        # This file is not supplied by the shared entry or foundations library.
        self.assertIn("ao.policy.bazel_contract", paths, "declared ao source data is missing")
        modules = set(paths)
        violations = []
        for module, path in paths.items():
            layer = module.removeprefix("ao.").partition(".")[0]
            if layer not in _ALLOWED:
                continue
            for dependency in _imports(module, path, modules):
                root, _, rest = dependency.partition(".")
                if root == "ao":
                    forbidden = bool(rest) and rest.split(".")[0] not in _ALLOWED[layer]
                else:
                    forbidden = layer == "policy" and (
                        root not in sys.stdlib_module_names or root in _PROCESS_OR_NETWORK
                    )
                if forbidden:
                    violations.append(f"{module} imports {dependency}")
        self.assertEqual(violations, [])


if __name__ == "__main__":
    unittest.main()
