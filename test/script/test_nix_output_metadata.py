"""Source guards for additive Nix dependency metadata; these do not evaluate Nix."""

import re
import unittest
from pathlib import Path

PROJECT_ROOT = Path(__file__).resolve().parents[2]


class NixOutputMetadataSourceTest(unittest.TestCase):
    def setUp(self):
        self.shell = (PROJECT_ROOT / "shell.nix").read_text(encoding="utf-8")
        self.helper = self.shell.split("  dependencyEntry = package: {", 1)[1].split("  dependencyReport =", 1)[0]
        self.dependencies = self.shell.split("    dependencies = {", 1)[1].split("\n    };", 1)[0]

    def test_metadata_discards_only_extra_derivation_and_named_output_contexts(self):
        helper = " ".join(self.helper.split())
        self.assertIn("version = package.version;", helper)
        self.assertIn("storePath = toString package;", helper)
        self.assertIn("drvPath = builtins.unsafeDiscardStringContext package.drvPath;", helper)
        self.assertIn(
            "outputs = pkgs.lib.genAttrs package.outputs ( "
            "name: builtins.unsafeDiscardStringContext (toString (pkgs.lib.getOutput name package)) );",
            helper,
        )
        self.assertEqual(self.shell.count("builtins.unsafeDiscardStringContext"), 2)
        for operation in ("builtins.pathExists", "builtins.readFile", "builtins.import"):
            with self.subTest(operation=operation):
                self.assertNotIn(operation, self.helper)

    def test_dependency_entries_keep_the_selected_package_objects(self):
        # Every entry passes a package expression to the helper. Do not freeze
        # the provider inventory here; real resolution validates owned SDK paths.
        entries = re.findall(r"^\s*([\w-]+) = dependencyEntry ([\w.-]+)(?=\s*;|\s*//)", self.dependencies, re.MULTILINE)
        self.assertTrue(entries)
        self.assertEqual(len({name for name, _ in entries}), len(entries))
        self.assertEqual(self.dependencies.count("dependencyEntry "), len(entries))
        self.assertNotIn("storePath =", self.dependencies)
        self.assertNotIn("drvPath =", self.dependencies)
        self.assertNotIn("outputs =", self.dependencies)

    def test_wrapper_salts_are_reported_without_changing_hardening_authority(self):
        self.assertIn("suffixSalt = pkgs.llvmPackages_22.clang.suffixSalt;", self.shell)
        self.assertIn("gccSuffixSalt = pkgs.stdenv.cc.suffixSalt;", self.shell)
        self.assertIn("hardeningFlags = pkgs.stdenv.cc.defaultHardeningFlags;", self.shell)
        self.assertEqual(self.shell.count("hardeningEnable = hardeningFlags;"), 2)
        self.assertIn("schemaVersion = 1;", self.shell)


if __name__ == "__main__":
    unittest.main()
