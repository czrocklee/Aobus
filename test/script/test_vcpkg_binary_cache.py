"""Tests for the content-addressed CI vcpkg binary cache."""

import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

from ao.core import vcpkg_binary_cache

ABI_A = "a" * 64
ABI_B = "b" * 64
ABI_STALE = "c" * 64
ABI_FOREIGN = "d" * 64


def _write_status(installed: Path, text: str, *, update: str | None = None, name: str = "0000000001") -> None:
    database = installed / "vcpkg"
    database.mkdir(parents=True, exist_ok=True)
    (database / "status").write_text(text, encoding="utf-8")
    if update is not None:
        (database / "updates").mkdir(exist_ok=True)
        (database / "updates" / name).write_text(update, encoding="utf-8")


def _write_archive(cache: Path, abi: str, payload: bytes = b"zip") -> Path:
    path = cache / abi[:2] / f"{abi}.zip"
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(payload)
    return path


STATUS = f"""Package: alac
Version: 2017-11-03-c38887c5
Port-Version: 4
Architecture: arm64-aobus-osx
Multi-Arch: same
Abi: {ABI_A}
Description: The Apple Lossless Audio Codec
  continued description line
Type: Port
Status: install ok installed

Package: boost-regex
Feature: icu
Architecture: arm64-aobus-osx
Multi-Arch: same
Description: feature row without an ABI
Type: Port
Status: install ok installed

Package: boost-regex
Version: 1.89.0
Architecture: arm64-aobus-osx
Multi-Arch: same
Abi: {ABI_B}
Type: Port
Status: install ok installed

Package: fmt
Version: 11.0.0
Architecture: arm64-aobus-osx
Multi-Arch: same
Abi: {ABI_STALE}
Type: Port
Status: install ok installed
"""

UPDATE = f"""Package: fmt
Version: 11.0.0
Architecture: arm64-aobus-osx
Multi-Arch: same
Abi: {ABI_STALE}
Type: Port
Status: purge ok not-installed
"""


class InstalledAbiTest(unittest.TestCase):
    def test_reads_installed_package_abis_and_applies_later_updates(self):
        with tempfile.TemporaryDirectory() as temporary:
            installed = Path(temporary) / "vcpkg_installed"
            _write_status(installed, STATUS, update=UPDATE)

            self.assertEqual(vcpkg_binary_cache.installed_abis(installed), {ABI_A, ABI_B})

    def test_applies_updates_in_numeric_order(self):
        reinstall = UPDATE.replace("Status: purge ok not-installed", "Status: install ok installed")
        with tempfile.TemporaryDirectory() as temporary:
            installed = Path(temporary) / "vcpkg_installed"
            _write_status(installed, STATUS, update=UPDATE, name="9")
            _write_status(installed, STATUS, update=reinstall, name="10")

            self.assertEqual(vcpkg_binary_cache.installed_abis(installed), {ABI_A, ABI_B, ABI_STALE})

    def test_finds_installed_trees_without_descending_into_them(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            first = root / "Aobus" / "debug" / "vcpkg_installed"
            second = root / "Aobus" / "winui" / "vcpkg_installed"
            nested = first / "x64-windows" / "vcpkg_installed"
            for path in (first, second, nested):
                path.mkdir(parents=True)

            self.assertEqual(vcpkg_binary_cache.find_installed_trees([root]), [first, second])


class PruneTest(unittest.TestCase):
    def test_removes_only_archives_outside_the_installed_set(self):
        with tempfile.TemporaryDirectory() as temporary:
            cache = Path(temporary)
            kept = _write_archive(cache, ABI_A, b"12345")
            stale = _write_archive(cache, ABI_STALE)
            unrelated = cache / "README.txt"
            unrelated.write_text("not an archive", encoding="utf-8")

            result = vcpkg_binary_cache.prune(cache, {ABI_A, ABI_B})

            self.assertTrue(kept.is_file())
            self.assertFalse(stale.exists())
            self.assertFalse(stale.parent.exists())
            self.assertTrue(unrelated.is_file())
            self.assertEqual(result.kept, (ABI_A,))
            self.assertEqual(result.removed, 1)
            self.assertEqual(result.missing, (ABI_B,))
            self.assertEqual(result.bytes, 5)

    def test_fingerprint_depends_only_on_the_abi_set(self):
        self.assertEqual(
            vcpkg_binary_cache.content_fingerprint([ABI_A, ABI_B]),
            vcpkg_binary_cache.content_fingerprint([ABI_B, ABI_A]),
        )
        self.assertNotEqual(
            vcpkg_binary_cache.content_fingerprint([ABI_A]),
            vcpkg_binary_cache.content_fingerprint([ABI_A, ABI_B]),
        )


class SealCommandTest(unittest.TestCase):
    def _seal(self, root: Path) -> tuple[subprocess.CompletedProcess[str], dict[str, str]]:
        output = root / "github-output"
        environment = dict(os.environ)
        environment.pop("PYTHONPATH", None)
        # CI runs the helper as a standalone file before the portal environment exists.
        process = subprocess.run(
            [
                sys.executable,
                "script/ao/core/vcpkg_binary_cache.py",
                "seal",
                "--cache-dir",
                str(root / "cache"),
                "--build-root",
                str(root / "build"),
                "--key-prefix",
                "vcpkg-files-v2-test-",
                "--github-output",
                str(output),
            ],
            cwd=Path(vcpkg_binary_cache.__file__).resolve().parents[3],
            env=environment,
            capture_output=True,
            text=True,
            check=False,
        )
        fields = {}
        if output.exists():
            for line in output.read_text(encoding="utf-8").splitlines():
                name, _, value = line.partition("=")
                fields[name] = value
        return process, fields

    def test_emits_a_content_key_for_the_retained_archives(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            _write_status(root / "build" / "debug" / "vcpkg_installed", STATUS, update=UPDATE)
            _write_archive(root / "cache", ABI_A)
            _write_archive(root / "cache", ABI_B)
            _write_archive(root / "cache", ABI_STALE)

            process, fields = self._seal(root)

            self.assertEqual(process.returncode, 0, process.stderr)
            self.assertEqual(fields["ready"], "true")
            expected = vcpkg_binary_cache.content_fingerprint([ABI_A, ABI_B])
            self.assertEqual(fields["key"], f"vcpkg-files-v2-test-{expected}")
            self.assertFalse((root / "cache" / ABI_STALE[:2]).exists())

    def test_declines_to_save_without_installed_packages_or_archives(self):
        for with_status in (False, True):
            with self.subTest(with_status=with_status), tempfile.TemporaryDirectory() as temporary:
                root = Path(temporary)
                (root / "build").mkdir()
                if with_status:
                    _write_status(root / "build" / "vcpkg_installed", STATUS)
                _write_archive(root / "cache", ABI_FOREIGN if with_status else ABI_A)

                process, fields = self._seal(root)

                self.assertEqual(process.returncode, 0, process.stderr)
                self.assertEqual(fields, {"ready": "false"})


if __name__ == "__main__":
    unittest.main()
