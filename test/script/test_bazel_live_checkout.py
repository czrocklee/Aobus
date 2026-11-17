"""Live readers resolve the main repository, not a copied snapshot or cwd."""

import contextlib
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

    def test_without_runfiles_the_working_directory_cannot_supplant_the_module_checkout(self):
        with tempfile.TemporaryDirectory(prefix="ao-live-cwd-") as directory:
            unrelated = Path(directory)
            (unrelated / "VERSION.txt").write_text("1.2.3\n", encoding="utf-8")
            with contextlib.chdir(unrelated):
                self.assertEqual(live.project_root({}), Path(live.__file__).resolve().parents[3])


if __name__ == "__main__":
    unittest.main()
