"""Standalone native CI selection and explicit R2 proof boundaries."""

import ntpath
import re
import unittest

from ao.core.paths import PROJECT_ROOT

WORKFLOW = PROJECT_ROOT / ".github/workflows/ci.yml"


def job(name: str) -> str:
    source = WORKFLOW.read_text(encoding="utf-8")
    return re.split(r"\n(?=  \w+:\n)", source.split(f"\n  {name}:\n", 1)[1], maxsplit=1)[0]


def step(source: str, name: str) -> str:
    return source.split(f"      - name: {name}\n", 1)[1].split("      - name:", 1)[0]


class StandaloneBazelCiTest(unittest.TestCase):
    def test_manual_bazel_mode_skips_only_the_cmake_project_jobs(self):
        source = WORKFLOW.read_text(encoding="utf-8")
        self.assertIn("options: [full, bazel]", source)
        self.assertIn("default: full", source)
        for name in ("linux", "macos", "windows"):
            with self.subTest(job=name):
                condition = job(name).split("runs-on:", 1)[0]
                self.assertIn("github.event_name != 'workflow_dispatch' || inputs.mode != 'bazel'", condition)
                self.assertIn("!cancelled()", condition)
        self.assertNotIn("inputs.mode", job("changes").split("steps:", 1)[0])
        self.assertIn("inputs.r2 && inputs.mode != 'bazel'", step(job("changes"), "Validate manual selection"))

    def test_runner_local_state_is_selected_after_the_job_starts(self):
        source = WORKFLOW.read_text(encoding="utf-8")
        for name in re.findall(r"^  (\w+):$", source.split("\njobs:\n", 1)[1], re.MULTILINE):
            with self.subTest(job=name):
                # GitHub rejects runner context in job-level env before dispatch.
                self.assertNotRegex(job(name).split("steps:", 1)[0], r"\$\{\{\s*runner\.")
        linux = job("linux_bazel")
        select = step(linux, "Select runner-local state")
        self.assertIn('"AOBUS_STATE_ROOT=$RUNNER_TEMP/aobus-state"', select)
        self.assertIn('>> "$GITHUB_ENV"', select)
        self.assertLess(linux.index("Select runner-local state"), linux.index("Provision Bazel tree"))

    def test_manual_native_jobs_fail_instead_of_masking_product_or_test_failures(self):
        self.assertNotIn("continue-on-error:", job("linux_bazel").split("steps:", 1)[0])
        for name in ("macos_bazel", "windows_bazel"):
            with self.subTest(job=name):
                header = job(name).split("steps:", 1)[0]
                self.assertIn(
                    "continue-on-error: ${{ github.event_name != 'workflow_dispatch' || inputs.mode != 'bazel' }}",
                    header,
                )
        for name in ("linux_bazel", "macos_bazel", "windows_bazel"):
            with self.subTest(job=name):
                source = job(name)
                for operation in (
                    "Build Bazel products",
                    "Test Bazel graph",
                    "Verify Bazel compile contracts",
                    "Test Bazel lint fixtures",
                    "Check Bazel hygiene gate",
                ):
                    self.assertNotIn("continue-on-error:", step(source, operation))
                self.assertNotRegex(source, r"(?:\./ao|\.\\ao\.bat) (?:check|build|hygiene)\b")
                self.assertNotRegex(source, r"(?:run:|^\s+)cmake (?:-S|--build)\b")

    def test_native_jobs_build_their_shipping_frontend_not_just_the_default_test_aggregate(self):
        for name, product in (
            ("linux_bazel", "//app/linux-gtk:aobus-gtk"),
            ("macos_bazel", "//app/macos-appkit:aobus-appkit"),
            ("windows_bazel", "//app/windows-winui:aobus-winui"),
        ):
            with self.subTest(job=name):
                source = job(name)
                build = step(source, "Build Bazel products")
                self.assertIn(product, build)
                self.assertRegex(build, r"//app/cli:aobus(?:\s|$)")
                self.assertIn("//app/tui:aobus-tui", build)
                self.assertLess(source.index("Build Bazel products"), source.index("Test Bazel graph"))
                self.assertIn("//test:lint_test", step(source, "Test Bazel lint fixtures"))
        self.assertIn("--headless", step(job("linux_bazel"), "Test headless GTK"))
        self.assertIn("test_bazel_remote_cache.py", step(job("macos_bazel"), "Test Bazel remote-cache lifecycle"))
        for name in ("linux_bazel", "windows_bazel"):
            self.assertIn("test --tooling", step(job(name), "Test portal tooling"))

    def test_windows_tooling_fails_before_expensive_product_work(self):
        windows = job("windows_bazel")
        self.assertLess(windows.index("Provision Bazel tree"), windows.index("Test portal tooling"))
        self.assertLess(windows.index("Test portal tooling"), windows.index("Build Bazel products"))
        self.assertNotIn("continue-on-error:", step(windows, "Test portal tooling"))

    def test_r2_proof_is_explicit_manual_and_cannot_be_satisfied_by_actions_cache_restore(self):
        source = job("r2_bazel")
        header = source.split("steps:", 1)[0]
        self.assertIn("github.event_name == 'workflow_dispatch' && inputs.mode == 'bazel' && inputs.r2", header)
        self.assertNotIn("continue-on-error:", header)
        self.assertNotIn("actions/cache/", source)
        proof = step(source, "Verify R2 producer and fresh consumer")
        self.assertNotIn("continue-on-error:", proof)
        self.assertIn("-m ao.core.bazel_cache_probe", proof)
        self.assertIn("bazel-ci-validation/$GITHUB_RUN_ID/$GITHUB_RUN_ATTEMPT/proof", proof)
        self.assertIn("${{ secrets.R2_SCCACHE_ENDPOINT }}", proof)
        self.assertIn("${{ secrets.R2_SCCACHE_ACCESS_KEY_ID }}", proof)
        self.assertIn("${{ secrets.R2_SCCACHE_SECRET_ACCESS_KEY }}", proof)
        self.assertNotIn("secrets.", proof.split("        run:", 1)[1])

    def test_windows_artifact_groups_keep_a_common_directory_below_the_drive_root(self):
        windows = job("windows_bazel")
        # The pinned action joins an LCA of only "D:" into drive-relative "D:.",
        # which resolves against the checkout rather than the external build tree.
        for title in ("Preserve Bazel evidence", "Preserve portal tooling evidence"):
            with self.subTest(step=title):
                evidence = step(windows, title)
                self.assertIn("if: ${{ always() }}", evidence)
                value = evidence.split("          path:", 1)[1].split("          if-no-files-found:", 1)[0]
                paths = [line.strip() for line in value.strip().splitlines() if line.strip() != "|"]
                drive, directory = ntpath.splitdrive(ntpath.commonpath(paths))
                self.assertTrue(drive)
                self.assertNotIn(directory, ("", "\\"))
        self.assertNotIn("aobus-bazel-tooling", step(windows, "Preserve Bazel evidence"))
        self.assertIn("aobus-bazel-tooling", step(windows, "Preserve portal tooling evidence"))

    def test_failed_runs_preserve_structured_evidence_without_uploading_proxy_logs_or_caches(self):
        for name, title in (
            ("linux_bazel", "Preserve Bazel evidence"),
            ("macos_bazel", "Preserve Bazel evidence"),
            ("windows_bazel", "Preserve Bazel evidence"),
            ("windows_bazel", "Preserve portal tooling evidence"),
            ("r2_bazel", "Preserve R2 proof"),
        ):
            with self.subTest(job=name):
                evidence = step(job(name), title)
                self.assertIn("if: ${{ always() }}", evidence)
                self.assertIn("actions/upload-artifact@ea165f8d65b6e75b540449e92b4886f43607fa02", evidence)
                self.assertNotIn("bazel-remote.log", evidence)
                self.assertNotIn("/proxy/", evidence)
                self.assertNotIn("cache/bazel", evidence)
        proof = step(job("r2_bazel"), "Preserve R2 proof")
        self.assertIn("/receipt.json", proof)
        self.assertIn("/build/build.log", proof)
