"""Configured-root identity regressions for the ao check producer, against the real cquery extractor."""

import unittest

from ao.core import bazel_check
from ao.core.bazel_test_ownership import ConfiguredTarget

_TARGET = "a" * 64
_EXEC = "b" * 64
_OLD = "c" * 64
_TRIMMED = "d" * 64
_MISSING = "e" * 64


def _cquery(rules, ignored=()):
    """Minimal cquery jsonproto. Ignored rows are not configured-rule identities."""
    configurations = []
    identifiers = {}
    results = []
    for label, checksum in rules:
        identifier = identifiers.get(checksum)
        if identifier is None:
            identifier = len(configurations) + 1
            identifiers[checksum] = identifier
            configurations.append({"id": identifier, "checksum": checksum})
        results.append(
            {
                "target": {"type": "RULE", "rule": {"name": label, "ruleClass": "cc_binary"}},
                "configurationId": identifier,
            }
        )
    return {"results": [*ignored, *results], "configurations": configurations}


def _ignored_rows(masquerading_label=None):
    source = {"type": "SOURCE_FILE"}
    generated = {"type": "GENERATED_FILE"}
    if masquerading_label is not None:
        source["sourceFile"] = {"name": masquerading_label}
        generated["generatedFile"] = {"name": masquerading_label}
    return [
        {"target": source},
        {"target": generated},
        {"target": {"type": "RULE", "rule": {"name": "@@sdk//:external", "ruleClass": "cc_library"}}},
    ]


class ConfiguredRootIdentityTest(unittest.TestCase):
    def test_one_full_checksum_identity_per_expected_root_allows_trimmed_configurations(self):
        ignored = _ignored_rows()
        cases = {
            "shared": (("@@//:product", _TARGET), ("//test:unit", _TARGET)),
            "trimmed": (("@@//:product", _TARGET), ("//test:unit", _TRIMMED)),
        }
        for name, rules in cases.items():
            with self.subTest(name=name):
                observed = bazel_check._configured_roots(
                    _cquery(rules, ignored),
                    ("//test:unit", "//:product"),
                )
                self.assertEqual(
                    observed,
                    frozenset(ConfiguredTarget(label.removeprefix("@@"), checksum) for label, checksum in rules),
                )

    def test_expected_root_labels_must_be_nonempty_and_unique(self):
        document = _cquery((("//:product", _TARGET), ("//test:unit", _TRIMMED)))
        for labels in ((), ("//:product", "//:product"), ("//:product", "//test:unit", "//:product")):
            with self.subTest(labels=labels):
                with self.assertRaises(ValueError):
                    bazel_check._configured_roots(document, labels)

    def test_label_set_must_match_exactly_including_same_count_substitution(self):
        document = _cquery((("//:product", _TARGET), ("//test:unit", _TRIMMED)))
        for name, labels in (
            ("omitted", ("//:product", "//test:unit", "//:run")),
            ("substituted", ("//:product", "//:run")),
            ("extra", ("//:product",)),
        ):
            with self.subTest(name=name):
                with self.assertRaises(ValueError):
                    bazel_check._configured_roots(document, labels)

    def test_duplicate_configuration_variants_are_not_one_root_identity(self):
        document = _cquery((("//:product", _TARGET), ("//:product", _TRIMMED)))
        with self.assertRaises(ValueError):
            bazel_check._configured_roots(document, ("//:product",))

    def test_ignored_source_generated_and_external_rows_do_not_mask_a_missing_root(self):
        document = _cquery((("//:product", _TARGET),), _ignored_rows("//:run"))
        with self.assertRaises(ValueError):
            bazel_check._configured_roots(document, ("//:product", "//:run"))

    def test_empty_or_truncated_checksum_fails_through_the_real_extractor(self):
        for checksum in ("", "a" * 63):
            with self.subTest(checksum=checksum or "empty"):
                with self.assertRaisesRegex(ValueError, "full configuration checksum"):
                    bazel_check._configured_roots(_cquery((("//:product", checksum),)), ("//:product",))


if __name__ == "__main__":
    unittest.main()
