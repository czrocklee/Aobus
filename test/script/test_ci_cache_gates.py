"""CI cache promotion boundaries that a successful build cannot verify."""

import re
import unittest

from ao.core.paths import PROJECT_ROOT

WORKFLOW = PROJECT_ROOT / ".github" / "workflows" / "ci.yml"


def _job(name):
    remainder = WORKFLOW.read_text(encoding="utf-8").split(f"  {name}:\n", 1)[1]
    return re.split(r"\n(?=  \w+:\n)", remainder, maxsplit=1)[0]


def _step(job, name):
    return job.split(f"      - name: {name}\n", 1)[1].split("      - name:", 1)[0]


def _required_conditions(step):
    # These promotion gates must be conjunctions, not alternatives.
    condition = re.search(r"(?ms)^        if:\s*(?:[|>]-?\s*)?\$\{\{(.*?)\}\}", step)
    return {" ".join(term.split()) for term in condition[1].split("&&")}


class CachePromotionTest(unittest.TestCase):
    def test_linux_disk_archive_only_promotes_a_successful_main_push_after_trim(self):
        conditions = _required_conditions(_step(_job("linux"), "Save Bazel disk cache"))
        self.assertLessEqual(
            {
                "github.event_name == 'push'",
                "github.ref == 'refs/heads/main'",
                "steps.bazel-check.outcome == 'success'",
                "steps.bazel-cache-trim.outcome == 'success'",
            },
            conditions,
        )

    def test_pull_requests_cannot_start_a_remote_cache_writer(self):
        for name, writer in (
            ("linux", '"$CACHE_UPLOAD"'),
            ("macos", '"$CACHE_UPLOAD"'),
            ("windows", "$env:CACHE_UPLOAD"),
        ):
            with self.subTest(job=name):
                step = _step(_job(name), "Start Bazel remote cache")
                self.assertIn("CACHE_UPLOAD: ${{ github.event_name != 'pull_request' }}", step)
                self.assertIn(f"--trusted-writer {writer} ", step)

    def test_managed_tools_save_requires_successful_cache_population(self):
        for job, platform, producers in (
            ("macos", "macOS", ("vcpkg-prepare", "buildifier", "prettier")),
            (
                "windows",
                "Windows",
                ("windows-prerequisites", "windows-app-runtime", "vcpkg-prepare", "buildifier", "prettier"),
            ),
        ):
            with self.subTest(job=job):
                before_save = _job(job).split(f"      - name: Save managed {platform} tools\n", 1)[0]
                conditions = _required_conditions(_step(_job(job), f"Save managed {platform} tools"))
                for producer in producers:
                    self.assertIn(f"        id: {producer}\n", before_save)
                    self.assertIn(f"steps.{producer}.outcome == 'success'", conditions)


if __name__ == "__main__":
    unittest.main()
