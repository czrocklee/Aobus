"""Prepared Debug and Release SDKs retain their admitted configuration payloads."""

import tempfile
import unittest
from pathlib import Path

from ao.core import bazel_dependencies as dependencies
from ao.core import bazel_deps as deps


class ConfiguredAdmissionTest(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix="aobus-configured-admission-")
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name).resolve()
        self.provider = self.root / "provider"
        self.include = self.provider / "include"
        self.include.mkdir(parents=True)
        (self.include / "fixture.h").write_text("declared header\n", encoding="utf-8")
        (self.include / "unowned.h").write_text("another installed port\n", encoding="utf-8")
        for config in ("debug", "release"):
            selected = self.provider / config
            selected.mkdir()
            (selected / "fixture.lib").write_bytes(config.encode())
            (selected / "fixture.dll").write_bytes(config.encode())

    def resolve(self, config):
        selected = self.provider / config.lower()
        spec = {
            "schema": 1,
            "platform": "windows",
            "config": config,
            "linkage": "shared",
            "provider": {"name": "fixture", "version": "reviewed"},
            "allowed_roots": [str(self.provider)],
            "library_roots": [str(selected)],
            "runtime_roots": [str(selected)],
            "packages": [{"name": "fixture", "modules": []}],
        }
        interface = {
            "header_dirs": [str(self.include)],
            "libraries": [
                {
                    "name": "fixture",
                    "kind": "shared",
                    "file": str(selected / "fixture.lib"),
                    "runtime": str(selected / "fixture.dll"),
                }
            ],
        }
        adapter = deps._Adapter(
            spec, {"schema": 1, "patches": [dependencies._manual_patch(spec, "fixture", interface)]}
        )
        adapter.prepare()
        adapter.header_files["fixture"] = [self.include / "fixture.h"]
        return adapter.resolve()

    def test_configurations_keep_their_selected_archive_and_runtime_without_writing_repositories(self):
        for config in ("Debug", "Release"):
            with self.subTest(config=config):
                resolution = self.resolve(config)
                self.assertEqual(resolution["manifest"]["configuration"]["config"], config)
                self.assertNotIn("targets", resolution["manifest"])
                self.assertEqual(
                    resolution["packages"][0]["libraries"],
                    [
                        {
                            "index": 0,
                            "kind": "shared",
                            "source": str(self.provider / config.lower() / "fixture.lib"),
                            "basename": "fixture.lib",
                            "runtime": str(self.provider / config.lower() / "fixture.dll"),
                        }
                    ],
                )
        self.assertEqual(list(self.root.iterdir()), [self.provider])

    def test_both_configurations_keep_the_same_sparse_header_interface(self):
        debug = self.resolve("Debug")["packages"][0]
        release = self.resolve("Release")["packages"][0]
        self.assertEqual(
            debug["headers"],
            [
                {
                    "root": str(self.include),
                    "index": 0,
                    "files": [{"source": str(self.include / "fixture.h"), "path": "fixture.h"}],
                }
            ],
        )
        self.assertEqual(release["headers"], debug["headers"])
        self.assertEqual(debug["includes"], [{"tree": 0, "path": "."}])
        self.assertEqual(release["includes"], debug["includes"])


if __name__ == "__main__":
    unittest.main()
