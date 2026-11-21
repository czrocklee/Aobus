"""CI cache promotion boundaries that a successful build cannot verify."""

import re
import unittest

from ao.core.paths import PROJECT_ROOT

WORKFLOW = PROJECT_ROOT / ".github" / "workflows" / "ci.yml"


def _step(job: str, name: str) -> str:
    remainder = WORKFLOW.read_text(encoding="utf-8").split(f"  {job}:\n", 1)[1]
    block = re.split(r"\n(?=  \w+:\n)", remainder, maxsplit=1)[0]
    step = block.split(f"      - name: {name}\n", 1)[1]
    return re.split(r"(?m)^      - ", step, maxsplit=1)[0]


def _required_conditions(step: str) -> set[str]:
    condition = re.search(r"(?ms)^        if:\s*(?:[|>]-?\s*)?\$\{\{(.*?)\}\}", step)[1]
    if "||" in condition:
        raise AssertionError("Cache promotion must not bypass the required conditions")
    return {" ".join(term.split()) for term in condition.split("&&")}


class CachePromotionTest(unittest.TestCase):
    def test_linux_archive_contains_only_ac_and_cas(self):
        root = "${{ env.AOBUS_BAZEL_DISK_CACHE }}"
        for name in ("Restore Bazel disk cache", "Save Bazel disk cache"):
            with self.subTest(step=name):
                step = _step("linux", name)
                paths = re.search(r"(?m)^          path: \|\n((?:            .*\n|\n)+)", step)[1]
                self.assertEqual(
                    {line.strip() for line in paths.splitlines() if line.strip()}, {root + "/ac", root + "/cas"}
                )

    def test_linux_disk_archive_only_promotes_a_successful_main_push_after_trim(self):
        conditions = _required_conditions(_step("linux", "Save Bazel disk cache"))
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
                step = _step(name, "Start Bazel remote cache")
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
                conditions = _required_conditions(_step(job, f"Save managed {platform} tools"))
                self.assertLessEqual({f"steps.{producer}.outcome == 'success'" for producer in producers}, conditions)


if __name__ == "__main__":
    unittest.main()
