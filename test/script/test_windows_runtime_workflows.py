"""Windows runfiles copy-route admission for declared synthetic files.

Regular-file copies admit only through the configured manifest route. Directory
copies stay rejected. Deployment authority is not part of this contract.
"""

import shutil
import tempfile
import unittest
from pathlib import Path
from unittest import mock

from ao.core import bazel_native as native
from ao.core import bazel_workflows as workflows


def _path_key(path):
    """Lexical path identity for an I/O fake. Not Path equality or Unicode casefold."""
    return str(path)


def _resolve_as(mapping):
    """Stub Path.resolve for specific lexical paths. Every other path stays real."""
    original = Path.resolve

    def resolve(path, *args, **kwargs):
        replacement = mapping.get(_path_key(path))
        if replacement is not None:
            return replacement
        return original(path, *args, **kwargs)

    return resolve


def _report_symlink(paths):
    """Stub Path.is_symlink for specific paths. Every other path stays real."""
    original = Path.is_symlink
    keys = {_path_key(path) for path in paths}

    def is_symlink(path):
        if _path_key(path) in keys:
            return True
        return original(path)

    return is_symlink


class WindowsRunfilesWorkflowTest(unittest.TestCase):
    def setUp(self):
        self.root = Path(self.enterContext(tempfile.TemporaryDirectory(prefix="aobus-windows-runfiles-")))
        self.tree = self.root / "tree"
        self.execution_root = self.tree / "state/output/execroot/_main"
        self.execution_root.mkdir(parents=True)
        self.binary = "bazel-out/target-dbg/bin/app/application.exe"
        self.source = "external/sdk/debug/bin/codec.dll"
        self.deployed = "bazel-out/target-dbg/bin/app/codec.dll"
        self.extra_source = "external/sdk/debug/bin/data.dll"
        self.extra_deployed = "bazel-out/target-dbg/bin/app/data.dll"
        self.write(self.binary, b"existing PE fixture").chmod(0o755)
        self.write(self.source, b"current canonical SDK bytes")
        self.write(self.deployed, b"current canonical SDK bytes")
        self.write(self.extra_source, b"current indirect SDK bytes")
        self.write(self.extra_deployed, b"current indirect SDK bytes")
        paths = [
            self.binary,
            self.source,
            self.deployed,
            self.extra_source,
            self.extra_deployed,
        ]
        self.runfiles_manifest = self.binary + ".runfiles_manifest"
        self.write(
            self.runfiles_manifest,
            "".join("_main/" + path + " " + str(self.execution_root / path) + "\n" for path in paths).encode(),
        )
        self.row = {
            "label": "//app:application",
            "incompatible": False,
            "executable": self.binary,
            "manifest": self.runfiles_manifest,
            "files": [{"path": self.binary, "directory": False}],
            "runfiles": [{"path": path, "short_path": path, "directory": False} for path in paths],
            "environment": {},
        }

    def directory_runfiles(self, *, copies=False):
        directory = Path(str(self.execution_root / self.binary) + ".runfiles")
        for item in self.row["runfiles"]:
            destination = directory / "_main" / item["path"]
            destination.parent.mkdir(parents=True, exist_ok=True)
            if destination.exists():
                destination.unlink()
            source = self.execution_root / item["path"]
            if copies:
                shutil.copy(source, destination)
            else:
                destination.hardlink_to(source)
        return directory

    def write(self, relative, data):
        path = self.execution_root / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(data)
        return path

    def admit(self, *, windows=True):
        return workflows.parse_artifact(
            self.row,
            target="//app:application",
            checksum="a" * 64,
            config="debug",
            tree=self.tree,
            execution_root=self.execution_root,
            windows=windows,
        )

    def test_directory_and_manifest_runfiles_membership_are_independently_checked(self):
        directory = self.directory_runfiles()
        artifact = self.admit()
        self.assertEqual(artifact.runfiles_directory, directory)
        wrong = directory / "_main" / self.deployed
        wrong.unlink()
        wrong.write_bytes(b"unrelated logical runfile")
        with self.assertRaisesRegex(native.BazelWorkspaceError, "configured declared input"):
            self.admit()

    def test_windows_copied_files_admit_when_the_manifest_binds_the_declared_route(self):
        directory = self.directory_runfiles(copies=True)
        manifest = (self.execution_root / self.runfiles_manifest).read_text(encoding="utf-8")
        self.assertIn(f"_main/{self.deployed} {self.execution_root / self.deployed}\n", manifest)
        artifact = self.admit()
        self.assertEqual(artifact.runfiles_directory, directory)
        self.assertEqual(artifact.runfiles_manifest, self.execution_root / self.runfiles_manifest)
        self.assertEqual(artifact.runfiles, tuple(item["path"] for item in self.row["runfiles"]))
        self.assertEqual(artifact.executable_sha256, native.sha256(self.execution_root / self.binary))
        self.assertNotIn("runtime_deployments", artifact.provenance())
        for item in self.row["runfiles"]:
            copied = directory / "_main" / item["path"]
            declared = self.execution_root / item["path"]
            self.assertFalse(copied.is_symlink())
            self.assertFalse(copied.samefile(declared))
            self.assertEqual(copied.read_bytes(), declared.read_bytes())

    def test_direct_symlink_to_the_declared_file_remains_admitted(self):
        directory = self.directory_runfiles()
        declared = self.execution_root / self.deployed
        supplied = directory / "_main" / self.deployed
        self.assertTrue(supplied.samefile(declared))
        with mock.patch.object(Path, "is_symlink", _report_symlink((supplied,))):
            self.assertTrue(supplied.is_symlink())
            self.assertFalse(declared.is_symlink())
            self.assertTrue(supplied.samefile(declared))
            artifact = self.admit()
        self.assertEqual(artifact.runfiles_directory, directory)
        self.assertTrue(supplied.samefile(declared))
        self.assertEqual(supplied.read_bytes(), declared.read_bytes())

    def test_windows_copy_admits_escaped_spaced_manifest_path(self):
        spaced = "bazel-out/target-dbg/bin/app/spaced name.dll"
        payload = b"spaced payload bytes"
        self.write(spaced, payload)
        self.row["runfiles"].append({"path": spaced, "short_path": spaced, "directory": False})
        directory = self.directory_runfiles(copies=True)

        def encode(value):
            return value.replace("\\", "\\b").replace(" ", "\\s").replace("\n", "\\n")

        lines = []
        for item in self.row["runfiles"]:
            key = "_main/" + item["path"]
            physical = str(self.execution_root / item["path"])
            if " " in key or " " in physical or "\\" in physical:
                lines.append(" " + encode(key) + " " + encode(physical) + "\n")
            else:
                lines.append(f"{key} {physical}\n")
        self.write(self.runfiles_manifest, "".join(lines).encode())
        manifest = (self.execution_root / self.runfiles_manifest).read_text(encoding="utf-8")
        self.assertIn("\\s", manifest)
        self.assertTrue(any(line.startswith(" ") for line in manifest.splitlines()))
        artifact = self.admit()
        copied = directory / "_main" / spaced
        declared = self.execution_root / spaced
        self.assertIn(spaced, artifact.runfiles)
        self.assertFalse(copied.is_symlink())
        self.assertFalse(copied.samefile(declared))
        self.assertEqual(copied.read_bytes(), payload)

    def test_windows_copy_admits_ancestor_directory_manifest_stub(self):
        directory = self.directory_runfiles(copies=True)
        prefix = "_main/bazel-out/target-dbg/bin/app"
        prefix_target = self.execution_root / "bazel-out/target-dbg/bin/app"
        lines = [
            f"_main/{item['path']} {self.execution_root / item['path']}\n"
            for item in self.row["runfiles"]
            if item["path"] != self.deployed
        ]
        lines.append(f"{prefix} {prefix_target}\n")
        self.write(self.runfiles_manifest, "".join(lines).encode())
        manifest = (self.execution_root / self.runfiles_manifest).read_text(encoding="utf-8")
        self.assertNotIn(f"_main/{self.deployed} ", manifest)
        self.assertIn(f"{prefix} {prefix_target}\n", manifest)
        artifact = self.admit()
        copied = directory / "_main" / self.deployed
        declared = self.execution_root / self.deployed
        self.assertIn(self.deployed, artifact.runfiles)
        self.assertFalse(copied.is_symlink())
        self.assertFalse(copied.samefile(declared))
        self.assertEqual(copied.read_bytes(), declared.read_bytes())

    def test_windows_copy_admits_external_files_at_canonical_output_base(self):
        # I/O fake only: Path.resolve models the execroot/external junction and repository
        # symlink. The manifest names the lexical output-base route, not the resolved provider.
        output_base = self.tree / "state/output"
        provider_root = output_base / "provider"
        resolved = {}
        lines = []
        for item in self.row["runfiles"]:
            expected = self.execution_root / item["path"]
            if item["path"].startswith("external/"):
                canonical = output_base / item["path"]
                provider = provider_root / item["path"]
                payload = expected.read_bytes()
                canonical.parent.mkdir(parents=True, exist_ok=True)
                provider.parent.mkdir(parents=True, exist_ok=True)
                canonical.write_bytes(payload)
                provider.write_bytes(payload)
                self.assertNotEqual(str(canonical), str(expected))
                self.assertNotEqual(str(canonical), str(provider))
                self.assertEqual(canonical.read_bytes(), payload)
                self.assertEqual(provider.read_bytes(), payload)
                resolved[_path_key(expected)] = provider
                resolved[_path_key(canonical)] = provider
                target = canonical
            else:
                target = expected
            lines.append(f"_main/{item['path']} {target}\n")
        self.write(self.runfiles_manifest, "".join(lines).encode())
        manifest = (self.execution_root / self.runfiles_manifest).read_text(encoding="utf-8")
        source_canonical = output_base / self.source
        source_provider = provider_root / self.source
        self.assertIn(f"_main/{self.source} {source_canonical}\n", manifest)
        self.assertNotIn(str(source_provider), manifest)
        directory = self.directory_runfiles(copies=True)
        declared = self.execution_root / self.source
        with mock.patch.object(Path, "resolve", _resolve_as(resolved)):
            self.assertEqual(declared.resolve(), source_provider)
            self.assertEqual(source_canonical.resolve(), source_provider)
            self.assertNotEqual(str(source_canonical), str(declared.resolve()))
            artifact = self.admit()
        copied = directory / "_main" / self.source
        self.assertFalse(copied.samefile(declared))
        self.assertEqual(copied.read_bytes(), declared.read_bytes())
        self.assertEqual(artifact.runfiles_manifest, self.execution_root / self.runfiles_manifest)
        self.assertEqual(artifact.executable_sha256, native.sha256(self.execution_root / self.binary))


if __name__ == "__main__":
    unittest.main()
