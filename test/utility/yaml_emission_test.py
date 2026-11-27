"""Read the governed writer's output without using RapidYAML as the parser."""

import json
import subprocess
import sys
import unittest

import yaml
from python.runfiles import runfiles


class YamlEmissionTest(unittest.TestCase):
    def emit(self, target_format: str) -> bytes:
        resolver = runfiles.Create()
        assert resolver is not None
        probe = resolver.Rlocation(sys.argv[1])
        assert probe is not None
        return subprocess.check_output([probe, target_format], timeout=10)

    def test_json_distinguishes_nul_from_literal_backslash_zero(self) -> None:
        self.assertEqual(json.loads(self.emit("json")), {"nul": "a\0b", "literal": r"a\0b"})

    def test_long_explicit_keys_keep_their_tag_and_alias_in_nested_maps(self) -> None:
        root = yaml.compose(self.emit("yaml"), Loader=yaml.SafeLoader)
        self.assertIsInstance(root, yaml.MappingNode)
        self.assertEqual([field.value for field, _ in root.value], ["block", "flow"])
        for field, mapping in root.value:
            with self.subTest(style=field.value):
                (key, payload), (alias_name, alias) = mapping.value
                self.assertEqual((key.tag, key.value, payload.value), ("!custom", "\0" * 512, "payload"))
                self.assertEqual(alias_name.value, "alias")
                self.assertIs(alias, key)


if __name__ == "__main__":
    unittest.main(argv=[sys.argv[0]])
