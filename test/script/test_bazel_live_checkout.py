"""Live readers resolve the main repository, not a copied snapshot or cwd."""

import contextlib
import json
import os
import sys
import tempfile
import unittest
from pathlib import Path
from types import SimpleNamespace
from unittest import mock

from ao.graph import bazel_live_checkout as live


class CheckoutSelectionTests(unittest.TestCase):
    def test_runfiles_anchor_prefers_the_physical_main_repository(self):
        with tempfile.TemporaryDirectory(prefix="ao-live-root-") as directory:
            root = Path(directory).resolve()
            physical = root / "checkout" / "VERSION.txt"
            snapshot = root / "snapshot" / "VERSION.txt"
            for anchor in (physical, snapshot):
                anchor.parent.mkdir()
                anchor.write_text("1.2.3\n", encoding="utf-8")

            def rlocation(location, source_repo=None):
                if source_repo is None:
                    raise ValueError("physical caller does not lie under the runfiles root")
                self.assertEqual(source_repo, "")
                return {"_main/VERSION.txt": str(physical), "aobus/VERSION.txt": str(snapshot)}.get(location)

            resolver = SimpleNamespace(Rlocation=rlocation)
            api = SimpleNamespace(Runfiles=SimpleNamespace(Create=lambda env: resolver))
            with (
                mock.patch.dict(sys.modules, {"python.runfiles": SimpleNamespace(runfiles=api)}),
                contextlib.chdir(snapshot.parent),
            ):
                self.assertEqual(live.project_root({}), physical.parent)
                self.assertEqual(live.physical_root("_main/VERSION.txt", {}), physical.parent)
                self.assertEqual(
                    live.project_root({"AOBUS_BAZEL_TEST_PROJECT_ROOT": str(snapshot.parent)}), snapshot.parent
                )

    def test_manifest_wheels_remain_importable_in_physical_checkout_child(self):
        from importlib.util import find_spec

        wheel_paths = []
        for package in ("markdown_it", "mdurl"):
            spec = find_spec(package)
            self.assertIsNotNone(spec)
            self.assertIsNotNone(spec.origin)
            wheel = Path(spec.origin).parent.parent
            self.assertEqual(wheel.suffix, ".whl")
            self.assertTrue(wheel.is_file())
            wheel_paths.append(wheel)

        with tempfile.TemporaryDirectory(prefix="ao-wheel-runfiles-") as directory:
            root = Path(directory).resolve()
            manifest = root / "MANIFEST"
            keys = ["wheels/" + wheel.name for wheel in wheel_paths]
            manifest.write_text(
                "".join(f"{key} {path}\n" for key, path in zip(keys, wheel_paths, strict=True)),
                encoding="utf-8",
                newline="\n",
            )
            receipt = root / "receipt.json"
            entry = root / "probe.py"
            entry.write_text(
                "import json, sys\n"
                "from pathlib import Path\n"
                "import markdown_it, mdurl\n"
                "assert sys.argv[1:] == ['payload']\n"
                "assert markdown_it.MarkdownIt().render('[x](<foo>)') == '<p><a href=\"foo\">x</a></p>\\n'\n"
                "sources = [markdown_it.__file__, mdurl.__file__]\n"
                "Path('receipt.json').write_text(json.dumps(sources), encoding='utf-8')\n",
                encoding="utf-8",
            )
            environment = {
                key: value
                for key, value in os.environ.items()
                if key not in {"PYTHONPATH", "RUNFILES_DIR", "RUNFILES_MANIFEST_FILE", "RUNFILES_MANIFEST_ONLY"}
            }
            environment["RUNFILES_MANIFEST_FILE"] = str(manifest)
            environment["RUNFILES_MANIFEST_ONLY"] = "1"
            arguments = [arg for key in keys for arg in (live.WHEEL_FLAG, key)] + ["payload"]
            self.assertEqual(live.reexec(root, "probe.py", arguments, environment=environment), 0)
            sources = [Path(path).parent.parent for path in json.loads(receipt.read_text(encoding="utf-8"))]
            self.assertEqual(sources, wheel_paths)

    def test_without_runfiles_the_working_directory_cannot_supplant_the_module_checkout(self):
        with tempfile.TemporaryDirectory(prefix="ao-live-cwd-") as directory:
            unrelated = Path(directory)
            (unrelated / "VERSION.txt").write_text("1.2.3\n", encoding="utf-8")
            with contextlib.chdir(unrelated):
                self.assertEqual(live.project_root({}), Path(live.__file__).resolve().parents[3])


if __name__ == "__main__":
    unittest.main()
