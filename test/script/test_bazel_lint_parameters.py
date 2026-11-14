"""Wire contracts for the generated consumer aspects-parameters bazelrc line.

These tests cover the Python wire protocol only: how the consumer mapping is
encoded into the named config line of the real generated ``aobus.bazelrc``.
The native BazelRC semantics (config selection, option parsing) are owned by
the portal's native proof, not by this module.
"""

import json
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

from ao.core import bazel_lint, bazel_lint_reports

OWNER = "@@//lib/util:util"
SOURCE = "lib/util/Clock.cpp"
SPAN = "include/ao/util/Span.h"
CONFIG_PREFIX = "build:aobus_lint_consumers "
PARAMETER_OPTION = "--aspects_parameters=aobus_lint_consumers="


def canonical(mapping):
    """Return the canonical compact sorted ASCII JSON the producer must write."""
    return json.dumps(mapping, sort_keys=True, separators=(",", ":"), ensure_ascii=True)


def consumer_line(parameters):
    """Return the exact named config line for one encoded mapping."""
    return CONFIG_PREFIX + json.dumps(PARAMETER_OPTION + parameters)


class WriteConsumerParametersTest(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix="ao-lint-consumers-")
        self.addCleanup(temporary.cleanup)
        self.prepared = Path(temporary.name) / "configuration"
        self.prepared.mkdir(parents=True)
        self.rc = self.prepared / "aobus.bazelrc"

    def baseline(self, *lines):
        self.rc.write_bytes(("\n".join(lines) + "\n").encode("utf-8"))

    def consumer_lines(self):
        return [line for line in self.rc.read_text(encoding="utf-8").splitlines() if line.startswith(CONFIG_PREFIX)]

    def decode(self, line):
        option = json.loads(line[len(CONFIG_PREFIX) :])
        self.assertTrue(option.startswith(PARAMETER_OPTION), option)
        return json.loads(option[len(PARAMETER_OPTION) :])

    def write(self, consumers):
        return bazel_lint._write_consumer_parameters(self.prepared, consumers)

    def test_appends_one_named_config_line_and_preserves_every_baseline_line(self):
        self.baseline(
            "# Generated launch configuration; repository rules own dependency refresh.",
            "build --config=debug",
            "common --repo_env=AOBUS_BAZEL_PYTHON=/usr/bin/python",
        )
        parameters = self.write({SPAN: (OWNER, SOURCE)})
        self.assertEqual(parameters, canonical({OWNER: [[SOURCE, SPAN]]}))
        self.assertEqual(
            self.rc.read_text(encoding="utf-8").splitlines(),
            [
                "# Generated launch configuration; repository rules own dependency refresh.",
                "build --config=debug",
                "common --repo_env=AOBUS_BAZEL_PYTHON=/usr/bin/python",
                consumer_line(parameters),
            ],
        )
        # The canonical encoding is exactly the parameterized aspect identity.
        self.assertEqual(
            bazel_lint_reports.lint_aspect_id(parameters),
            "//bazel/lint:lint.bzl%lint_aspect[aobus_lint_consumers=" + json.dumps(parameters) + "]",
        )

    def test_empty_consumers_encode_the_default_empty_mapping(self):
        self.baseline("build --config=debug")
        parameters = self.write({})
        self.assertEqual(parameters, "{}")
        self.assertEqual(self.consumer_lines(), [consumer_line("{}")])
        self.assertEqual(self.decode(self.consumer_lines()[0]), {})

    def test_replaces_the_previous_named_config_line_and_collapses_duplicates(self):
        stale = canonical({"@@//old:owner": [["old/Main.cpp", "old/Old.h"]]})
        self.baseline("build --config=debug", consumer_line(stale))
        parameters = self.write({SPAN: (OWNER, SOURCE)})
        lines = self.rc.read_text(encoding="utf-8").splitlines()
        self.assertEqual(lines, ["build --config=debug", consumer_line(parameters)])
        self.assertEqual(self.decode(lines[-1]), {OWNER: [[SOURCE, SPAN]]})

        self.baseline("build --config=debug", consumer_line(stale), "build --norun_validations", consumer_line(stale))
        self.write({})
        lines = self.rc.read_text(encoding="utf-8").splitlines()
        self.assertEqual(lines, ["build --config=debug", "build --norun_validations", consumer_line("{}")])

    def test_encoding_is_canonical_compact_sorted_ascii_with_json_escaping(self):
        self.baseline("build --config=debug")
        exotic = 'lib/ünïcode/Qu"ote\\Header.h=x#y%z\n'
        consumers = {
            exotic: (OWNER, SOURCE),
            SPAN: (OWNER, SOURCE),
            "include/ao/util/Bits.h": (OWNER, "lib/util/Bits.cpp"),
        }
        parameters = self.write(consumers)
        self.assertEqual(parameters, canonical(bazel_lint._consumer_selection(consumers)))
        self.assertIn("\\u00fc", parameters)  # non-ASCII escapes to \uXXXX
        self.assertIn('\\"', parameters)  # quotes escape
        self.assertIn("\\\\", parameters)  # backslashes escape
        self.assertIn("\\n", parameters)  # control newline escapes
        self.assertIn("=x#y%z", parameters)  # equals, hash and percent stay literal
        self.assertEqual(self.consumer_lines(), [consumer_line(parameters)])
        self.assertEqual(self.decode(self.consumer_lines()[0]), bazel_lint._consumer_selection(consumers))
        # The escaped line stays one physical rc line even with a newline in a path.
        self.assertEqual(len(self.rc.read_text(encoding="utf-8").splitlines()), 2)

    def test_repeating_an_identical_mapping_does_not_rewrite_the_file(self):
        self.baseline("build --config=debug")
        original = Path.write_bytes
        writes = []

        def spy(path, data, *arguments, **keywords):
            writes.append(path)
            return original(path, data, *arguments, **keywords)

        with patch.object(Path, "write_bytes", spy):
            first = self.write({SPAN: (OWNER, SOURCE)})
            repeated = self.write({SPAN: (OWNER, SOURCE)})
            changed = self.write({"include/ao/util/Bits.h": (OWNER, "lib/util/Bits.cpp")})
        self.assertEqual(first, repeated)
        self.assertEqual(changed, canonical({OWNER: [["lib/util/Bits.cpp", "include/ao/util/Bits.h"]]}))
        self.assertEqual(writes, [self.rc, self.rc])

    def test_unicode_line_separators_in_other_lines_keep_their_exact_bytes(self):
        # U+2028, U+0085 and U+2029 are legal inside quoted repo_env values;
        # only "\n" may ever separate rc lines, so the rewrite must preserve
        # those bytes exactly instead of splitting on them.
        separators = ("\u2028", "\u0085", "\u2029")
        quoted = ["common " + json.dumps("--repo_env=AOBUS_NOTE=" + value, ensure_ascii=False) for value in separators]
        for line in quoted:
            self.assertTrue(any(character in line for character in separators), line)
        self.baseline(*quoted)
        original = self.rc.read_bytes()
        parameters = self.write({SPAN: (OWNER, SOURCE)})
        result = self.rc.read_bytes()
        self.assertTrue(result.startswith(original), result)
        self.assertEqual(result[len(original) :].decode("utf-8"), consumer_line(parameters) + "\n")
        for line in quoted:
            self.assertIn(line.encode("utf-8"), result)

    def test_written_rc_stays_canonical_utf8_lf_without_carriage_returns(self):
        # Deliberately a byte assertion, not an os.linesep fake: on a real
        # Windows host text-mode writes would inject "\r\n" and fail here.
        self.baseline("build --config=debug")
        self.write({SPAN: (OWNER, SOURCE)})
        data = self.rc.read_bytes()
        self.assertNotIn(b"\r", data)
        self.assertTrue(data.endswith(b"\n"))

    def test_repeated_writes_do_not_accumulate_blank_lines(self):
        self.baseline("build --config=debug", "")
        self.write({SPAN: (OWNER, SOURCE)})
        after_first = self.rc.read_bytes()
        self.write({SPAN: (OWNER, SOURCE)})
        self.assertEqual(self.rc.read_bytes(), after_first)
        self.write({"include/ao/util/Bits.h": (OWNER, "lib/util/Bits.cpp")})
        data = self.rc.read_bytes()
        self.assertEqual(data.count(b"\n\n"), 1)  # only the baseline's own blank line remains
        self.assertEqual(
            self.consumer_lines(),
            [consumer_line(canonical({OWNER: [["lib/util/Bits.cpp", "include/ao/util/Bits.h"]]}))],
        )


if __name__ == "__main__":
    unittest.main()
