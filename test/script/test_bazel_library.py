"""Keep the bounded Bazel storage graph aligned with its declared contracts."""

import subprocess
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

from ao.core import bazel_dependencies as dependencies


class BazelCompileInterfacePatchTest(unittest.TestCase):
    def test_equivalent_metadata_include_paths_do_not_create_redundant_patches(self):
        with tempfile.TemporaryDirectory(prefix="bazel provider ") as directory:
            root = Path(directory).resolve()
            include = root / "include"
            include.mkdir()
            pc = root / "debug/lib/pkgconfig"
            pc.mkdir(parents=True)
            equivalent = pc / "../../../include"
            spec = {"platform": "macos", "config": "Debug", "provider": {"name": "vcpkg", "version": "fixture"}}
            for flags in (
                [f"-I{equivalent}"],
                [f"-isystem{equivalent}"],
                ["-I", str(equivalent)],
                ["-isystem", str(equivalent)],
            ):
                with self.subTest(flags=flags), patch.object(dependencies, "_metadata_flags", return_value=flags):
                    self.assertIsNone(
                        dependencies._compile_interface_patch(spec, "lmdb", {"header_dirs": [str(include)]})
                    )

    def test_metadata_flag_tokenization_matches_the_adapter_for_quoted_paths(self):
        spec = {
            "linkage": "static",
            "pkg_config": {"executable": "pkgconf", "pc_dirs": []},
            "packages": [{"name": "lmdb", "modules": ["lmdb"]}],
        }
        output = '-I"/provider with spaces/include" -DC4_CPP=17 /permissive-'
        with patch.object(dependencies.subprocess, "run", return_value=subprocess.CompletedProcess([], 0, output)):
            self.assertEqual(
                dependencies._metadata_flags(spec, "--cflags", package="lmdb"),
                ["-I/provider with spaces/include", "-DC4_CPP=17", "/permissive-"],
            )


if __name__ == "__main__":
    unittest.main()
