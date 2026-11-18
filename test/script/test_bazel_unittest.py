"""Exercise the unittest entry from the runfiles actually visible to it."""

import os
import subprocess
import sys
import tempfile
import textwrap
import unittest
from pathlib import Path

from ao.graph import bazel_live_checkout, bazel_unittest

_ENTRY = "script/ao/graph/bazel_unittest.py"


def _write(root, name, source):
    path = root / name
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(textwrap.dedent(source), encoding="utf-8")


class UnittestEntryTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix="ao-bazel-unittest-")
        self.addCleanup(temporary.cleanup)
        self.runfiles = Path(temporary.name) / "runfiles"
        self.entry_source = Path(bazel_unittest.__file__).read_text(encoding="utf-8")
        _write(self.runfiles, _ENTRY, self.entry_source)

    def run_entry(self, *arguments, search=()):
        env = dict(os.environ, PYTHONDONTWRITEBYTECODE="1")
        env["PYTHONPATH"] = os.pathsep.join(str(path) for path in (self.runfiles / "test/script", *search))
        env.pop("PYTHONHOME", None)
        env.pop("RUNFILES_DIR", None)
        env.pop("RUNFILES_MANIFEST_FILE", None)
        return subprocess.run(
            [sys.executable, "-S", str(self.runfiles / _ENTRY), "fixture_suite", *arguments],
            cwd=self.runfiles,
            env=env,
            capture_output=True,
            text=True,
            timeout=15,
        )

    def test_a_module_with_no_tests_is_not_a_passing_target(self):
        _write(self.runfiles, "test/script/fixture_suite.py", "VALUE = 1\n")
        result = self.run_entry()
        self.assertEqual(result.returncode, 1, result.stderr)

    def test_a_declared_module_and_its_child_need_only_the_projected_runfiles(self):
        # No live-checkout companion or portal packages are present. The test's
        # child must see the real data in the projected tree, not in the checkout.
        _write(self.runfiles, "script/fixture_payload.py", "DATA = 'declared runfiles payload'\n")
        if os.name != "nt":
            # POSIX runfiles link to the source but give it a different import closure.
            entry = self.runfiles / _ENTRY
            physical = self.runfiles.parent / "physical" / _ENTRY
            physical.parent.mkdir(parents=True)
            entry.rename(physical)
            entry.symlink_to(physical)
        _write(
            self.runfiles,
            "test/script/fixture_suite.py",
            """
            import subprocess
            import sys
            import unittest

            class Probe(unittest.TestCase):
                def test_child_reads_the_payload(self):
                    output = subprocess.check_output(
                        [sys.executable, '-S', '-c', 'from fixture_payload import DATA; print(DATA)'],
                        text=True,
                    )
                    self.assertEqual(output.strip(), 'declared runfiles payload')
            """,
        )
        result = self.run_entry()
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_live_target_runs_the_physical_module_not_a_passing_runfiles_copy(self):
        checkout = self.runfiles.parent / "checkout"
        _write(checkout, "VERSION.txt", "1.2.3\n")
        _write(checkout, _ENTRY, self.entry_source)
        _write(
            self.runfiles,
            "script/ao/graph/bazel_live_checkout.py",
            Path(bazel_live_checkout.__file__).read_text(encoding="utf-8"),
        )
        suite = """
            import unittest

            class Probe(unittest.TestCase):
                def test_checkout(self):
                    {}
            """
        _write(self.runfiles, "test/script/fixture_suite.py", suite.format("pass"))
        _write(checkout, "test/script/fixture_suite.py", suite.format("self.fail('live fixture failure')"))

        search = (self.runfiles / "script",)
        result = self.run_entry("--live-checkout", str(checkout / "VERSION.txt"), search=search)
        self.assertEqual(result.returncode, 1, result.stderr)
        self.assertIn("live fixture failure", result.stderr)
        result = self.run_entry("--live-checkout", str(checkout / "missing.txt"), search=search)
        self.assertEqual(result.returncode, 1, result.stderr)


if __name__ == "__main__":
    unittest.main()
