"""CI managed-tools cache promotion gates and Windows R2 receipt availability.

The managed tools caches are saved only after every setup step that populates
their paths succeeded, so a failed setup never promotes a partial tools tree;
unrelated product validation stays excluded. The Windows R2 stop receipt
follows the stop step's availability, including a failed start.
"""

import re
import unittest

from ao.core.paths import PROJECT_ROOT

WORKFLOW = PROJECT_ROOT / ".github" / "workflows" / "ci.yml"


def _job(name: str) -> str:
    """Return the named job's YAML block, bounded by the following job header."""
    remainder = WORKFLOW.read_text(encoding="utf-8").split(f"  {name}:\n", 1)[1]
    return re.split(r"\n(?=  \w+:\n)", remainder, maxsplit=1)[0]


def _step(job: str, name: str) -> str:
    """Return one step's YAML block from a job block."""
    return job.split(f"      - name: {name}\n", 1)[1].split("      - name:", 1)[0]


def _folded_condition(step: str) -> str:
    """Return a folded (`if: >-`) step condition as one comparable line."""
    return " ".join(step.split("        if: >-\n", 1)[1].split("        continue-on-error:", 1)[0].split())


def _inline_condition(step: str) -> str:
    return re.search(r"^        if: \$\{\{ (.*) \}\}$", step, re.MULTILINE).group(1)


class WindowsManagedToolsKeyTest(unittest.TestCase):
    def test_the_key_covers_the_pinned_bazel_artifact(self):
        step = _step(_job("windows"), "Restore managed Windows tools")
        key_inputs = set(re.findall(r"'([^']+)'", step.partition("hashFiles(")[2]))
        # The restored tools tree contains tools\bazel\<.bazelversion>; without
        # the Bazel pins in the key, a pin change would restore a stale Bazel
        # or never stage the requested one.
        self.assertLessEqual({".bazelversion", "script/ao/bazel-toolchain.json"}, key_inputs)


class ManagedToolsSaveGateTest(unittest.TestCase):
    WINDOWS_POPULATING_STEPS = (
        ("Check Windows build prerequisites", "windows-prerequisites"),
        ("Set up Windows App Runtime", "windows-app-runtime"),
        ("Prepare vcpkg binary cache", "vcpkg-prepare"),
        ("Set up Buildifier", "buildifier"),
    )
    MACOS_POPULATING_STEPS = (
        ("Prepare vcpkg binary cache", "vcpkg-prepare"),
        ("Set up Buildifier", "buildifier"),
    )

    def test_windows_save_requires_every_cache_populating_setup_step(self):
        job = _job("windows")
        for name, step_id in self.WINDOWS_POPULATING_STEPS:
            with self.subTest(step=name):
                self.assertIn(f"        id: {step_id}\n", _step(job, name))
        condition = _folded_condition(_step(job, "Save managed Windows tools"))
        self.assertTrue(condition.startswith("${{ !cancelled()"))
        for _, step_id in self.WINDOWS_POPULATING_STEPS:
            with self.subTest(gate=step_id):
                self.assertIn(f"steps.{step_id}.outcome == 'success'", condition)
        self.assertIn("steps.windows-state.outcome == 'success'", condition)
        self.assertIn("steps.windows-state.outputs.cache-hit != 'true'", condition)
        # Product completion is not a cache-populating setup step.
        self.assertNotIn("steps.build.outcome", condition)

    def test_windows_save_runs_after_every_populating_setup_step(self):
        job = _job("windows")
        positions = [
            job.index(f"      - name: {name}\n")
            for name, _ in (*self.WINDOWS_POPULATING_STEPS, ("Save managed Windows tools", ""))
        ]
        self.assertEqual(positions, sorted(positions))

    def test_macos_save_requires_every_cache_populating_setup_step(self):
        job = _job("macos")
        for name, step_id in self.MACOS_POPULATING_STEPS:
            with self.subTest(step=name):
                self.assertIn(f"        id: {step_id}\n", _step(job, name))
        condition = _folded_condition(_step(job, "Save managed macOS tools"))
        self.assertTrue(condition.startswith("${{ !cancelled()"))
        for _, step_id in self.MACOS_POPULATING_STEPS:
            with self.subTest(gate=step_id):
                self.assertIn(f"steps.{step_id}.outcome == 'success'", condition)
        self.assertIn("steps.macos-state.outcome == 'success'", condition)
        self.assertIn("steps.macos-state.outputs.cache-hit != 'true'", condition)
        self.assertNotIn("steps.build.outcome", condition)

    def test_macos_save_runs_after_every_populating_setup_step(self):
        job = _job("macos")
        positions = [
            job.index(f"      - name: {name}\n")
            for name, _ in (*self.MACOS_POPULATING_STEPS, ("Save managed macOS tools", ""))
        ]
        self.assertEqual(positions, sorted(positions))


class WindowsR2ReceiptAvailabilityTest(unittest.TestCase):
    def test_the_receipt_is_preserved_whenever_stop_ran(self):
        job = _job("windows")
        stop = _inline_condition(_step(job, "Stop Bazel remote cache"))
        receipt = _inline_condition(_step(job, "Preserve R2 stop receipt"))
        # A failed start still runs stop; the receipt must follow stop's
        # availability instead of a success-only start.
        self.assertEqual(stop, "always() && steps.bazel-remote.outcome != 'skipped'")
        self.assertEqual(receipt, stop)


if __name__ == "__main__":
    unittest.main()
