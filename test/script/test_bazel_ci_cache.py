"""Linux CI persists only trusted-main Bazel disk actions without weakening gates."""

import os
import re
import subprocess
import sys
import tempfile
import textwrap
import time
import unittest
from pathlib import Path

from ao.command import bazel
from ao.core.paths import PROJECT_ROOT

WORKFLOW = PROJECT_ROOT / ".github/workflows/ci.yml"
CACHE_PIN = "55cc8345863c7cc4c66a329aec7e433d2d1c52a9"
STATE_ROOT = "${{ runner.temp }}/aobus-state"
CACHE_DIR = f"{STATE_ROOT}/cache/bazel"
MARKER_FILE = "${{ runner.temp }}/aobus-bazel-cache-restored.marker"
MARKER_BASENAME = "aobus-bazel-cache-restored.marker"
RESTORE = "Restore Bazel disk cache"
MARKER = "Mark restored Bazel disk cache"
TEST = "Check Bazel portable slice"
VERIFY = "Verify Bazel compile contracts"
LINT = "Check Bazel lint reports and fixtures"
LINT_ID = "bazel-lint"
HYGIENE = "Check Bazel hygiene gate"
TRIM = "Trim restored Bazel disk cache"
TRIM_ID = "bazel-cache-trim"
SAVE = "Save Bazel disk cache"
CMAKE = "Build and test with private PipeWire"


def _linux() -> str:
    return WORKFLOW.read_text(encoding="utf-8").split("  linux:\n", 1)[1].split("\n  macos:\n", 1)[0]


def _step(name: str) -> str:
    remainder = _linux().split(f"      - name: {name}\n", 1)[1]
    return re.split(r"\n(?=      (?:- name:|#))", remainder, maxsplit=1)[0]


def _cache_inputs(name: str) -> dict[str, str]:
    """Extract scalar/folded cache inputs from these two workflow steps only."""
    inputs = _step(name).split("        with:\n", 1)[1]
    return {
        key: " ".join(value.removeprefix(">-").split())
        for key, value in re.findall(r"^          ([\w-]+): (.*(?:\n            .*)*)", inputs, re.MULTILINE)
    }


def _env(step: str) -> dict[str, str]:
    """Extract scalar environment assignments from one step's env block."""
    env = step.split("        env:\n", 1)[1]
    env = re.split(r"\n(?=        \S)", env, maxsplit=1)[0]
    return dict(re.findall(r"^          (\w+): (.*)$", env, re.MULTILINE))


def _inline_run(step: str) -> str:
    """Return the single-line script of a scalar (`run: x`) step."""
    return step.split("        run: ", 1)[1].split("\n", 1)[0].strip()


def _run_body(step: str) -> str:
    """Return the script of a literal-block (`run: |`) step."""
    return textwrap.dedent(step.split("        run: |\n", 1)[1])


class BazelCiCacheTest(unittest.TestCase):
    def test_transport_archives_only_disk_actions_under_both_explicit_bazel_state_roots(self):
        linux = _linux()
        state_root_steps = (TEST, VERIFY, LINT, HYGIENE)
        for name in state_root_steps:
            with self.subTest(step=name):
                self.assertIn(f"        env:\n          AOBUS_STATE_ROOT: {STATE_ROOT}\n", _step(name))
        # The explicit state root belongs to the Bazel steps only; the CMake
        # check keeps its own state policy and the cache steps address the
        # state root through the cache path, not the environment.
        self.assertEqual(linux.count("AOBUS_STATE_ROOT:"), len(state_root_steps))
        for name, keys in ((RESTORE, {"path", "key", "restore-keys"}), (SAVE, {"path", "key"})):
            with self.subTest(step=name):
                inputs = _cache_inputs(name)
                self.assertEqual(set(inputs), keys)
                self.assertEqual(inputs["path"], CACHE_DIR)
                self.assertIn("        continue-on-error: true\n", _step(name))
        self.assertEqual(linux.count("uses: actions/cache/"), 2)
        self.assertNotIn("BUILD_DIR", linux)
        self.assertNotIn("AOBUS_STATE_ROOT", _step(CMAKE))
        activation = _step("Enable compiler cache")
        self.assertIn('--state-root "$RUNNER_TEMP/aobus-state"', activation)

    def test_cache_actions_reuse_the_existing_workflow_pin(self):
        workflow = WORKFLOW.read_text(encoding="utf-8")
        pins = re.findall(r"uses: actions/cache/(?:restore|save)@([^\s]+)", workflow)
        self.assertGreater(len(pins), 2)
        self.assertEqual(set(pins), {CACHE_PIN})
        for name, action in ((RESTORE, "restore"), (SAVE, "save")):
            with self.subTest(step=name):
                self.assertIn(f"uses: actions/cache/{action}@{CACHE_PIN}  # v6.1.0\n", _step(name))

    def test_commit_key_and_only_fallback_share_the_versioned_pin_contract(self):
        inputs = _cache_inputs(RESTORE)
        key = re.sub(r"\s+", "", inputs["key"])
        fallback = re.sub(r"\s+", "", inputs["restore-keys"])
        self.assertEqual(key, fallback + "${{github.sha}}")
        self.assertRegex(
            fallback,
            r"^bazel-disk-v1-trusted-main-\$\{\{runner.os\}\}-\$\{\{runner.arch\}\}-"
            r"\$\{\{hashFiles\([^()]+\)\}\}-$",
        )
        contract_paths = re.findall(r"'([^']+)'", fallback)
        self.assertEqual(
            contract_paths,
            [
                ".bazelversion",
                "script/ao/bazel-toolchain.json",
                "nixpkgs.json",
                "shell.nix",
                "MODULE.bazel",
            ],
        )
        for path in contract_paths:
            with self.subTest(contract=path):
                self.assertTrue((PROJECT_ROOT / path).is_file())
        self.assertEqual(_cache_inputs(SAVE)["key"], "${{ steps.bazel-cache.outputs.cache-primary-key }}")
        self.assertIn("        id: bazel-cache\n", _step(RESTORE))

    def test_only_successful_trusted_main_pushes_publish_after_both_bazel_gates(self):
        save = _step(SAVE)
        condition = " ".join(save.split("        if: >-\n", 1)[1].split("        continue-on-error:", 1)[0].split())
        self.assertEqual(
            condition,
            "${{ success() && github.event_name == 'push' && github.ref == 'refs/heads/main' && "
            "steps.bazel-test.outcome == 'success' && steps.bazel-verify.outcome == 'success' && "
            "steps.bazel-lint.outcome == 'success' && "
            "steps.bazel-cache.outcome == 'success' && steps.bazel-cache.outputs.cache-hit != 'true' && "
            "steps.bazel-cache-trim.outcome == 'success' }}",
        )
        linux = _linux()
        positions = [linux.index(f"      - name: {name}\n") for name in (RESTORE, TEST, VERIFY, LINT, SAVE, CMAKE)]
        self.assertEqual(positions, sorted(positions))
        workflow = WORKFLOW.read_text(encoding="utf-8")
        for name in (RESTORE, SAVE):
            self.assertEqual(workflow.count(f"      - name: {name}\n"), 1)

    def test_marker_and_trim_steps_bound_the_saved_archive_without_new_actions(self):
        linux = _linux()
        positions = [
            linux.index(f"      - name: {name}\n") for name in (RESTORE, MARKER, TEST, VERIFY, LINT, TRIM, SAVE, CMAKE)
        ]
        self.assertEqual(positions, sorted(positions))
        workflow = WORKFLOW.read_text(encoding="utf-8")
        marker = _step(MARKER)
        trim = _step(TRIM)
        for name, step in ((MARKER, marker), (TRIM, trim)):
            with self.subTest(step=name):
                self.assertEqual(workflow.count(f"      - name: {name}\n"), 1)
                self.assertIn("        shell: bash\n", step)
                self.assertNotIn("        uses:", step)
        # Marking is optional for correctness gates, but failed marking must
        # prevent trimming and publication even if a stale marker exists.
        self.assertIn("        id: bazel-cache-marker\n", marker)
        self.assertIn("        continue-on-error: true\n", marker)
        self.assertNotIn("\n        if:", marker)
        self.assertEqual(_env(marker), {"AOBUS_BAZEL_CACHE_MARKER": MARKER_FILE})
        self.assertEqual(_inline_run(marker), 'touch "$AOBUS_BAZEL_CACHE_MARKER"')
        # The prune runs only for a restored prefix cache (never an exact hit or
        # a failed optional restore) and stays optional for the job itself.
        self.assertIn(f"        id: {TRIM_ID}\n", trim)
        self.assertIn("        continue-on-error: true\n", trim)
        self.assertEqual(
            " ".join(trim.split("        if: ", 1)[1].split("\n", 1)[0].split()),
            "${{ steps.bazel-cache-marker.outcome == 'success' && "
            "steps.bazel-cache.outcome == 'success' && steps.bazel-cache.outputs.cache-hit != 'true' }}",
        )
        self.assertEqual(_env(trim), {"AOBUS_BAZEL_CACHE_DIR": CACHE_DIR, "AOBUS_BAZEL_CACHE_MARKER": MARKER_FILE})
        # The marker lives under the runner temp root, outside the persisted
        # cache directory, at a stable basename.
        self.assertTrue(MARKER_FILE.startswith("${{ runner.temp }}/"))
        self.assertFalse(MARKER_FILE.startswith(CACHE_DIR))
        body = _run_body(trim)
        self.assertIn('find "$AOBUS_BAZEL_CACHE_DIR" -type f ! -newer "$AOBUS_BAZEL_CACHE_MARKER" -delete', body)
        # Both scripts address the cache and marker only through their exported
        # environment, never through inline runner paths.
        self.assertNotIn("runner.temp", body)
        self.assertNotIn("runner.temp", _inline_run(marker))

    def test_bazel_lint_step_runs_the_known_lint_consumers_before_trim(self):
        linux = _linux()
        lint = _step(LINT)
        self.assertIn(f"        id: {LINT_ID}\n", lint)
        self.assertIn("        continue-on-error: true\n", lint)
        self.assertNotIn("\n        if:", lint)
        self.assertIn(f"        env:\n          AOBUS_STATE_ROOT: {STATE_ROOT}\n", lint)
        self.assertEqual(
            lint.split("        run: |\n", 1)[1].strip().splitlines(),
            [
                './ao bazel test --path "$RUNNER_TEMP/aobus-bazel-debug" --jobs "$CMAKE_BUILD_PARALLEL_LEVEL" '
                "-- //test:lint_test",
                '          ./ao bazel lint --path "$RUNNER_TEMP/aobus-bazel-debug" --jobs '
                '"$CMAKE_BUILD_PARALLEL_LEVEL" --check aobus-modernize-use-starts-with lib/utility/Base64.cpp',
                '          ./ao bazel lint --config release --path "$RUNNER_TEMP/aobus-bazel-release" --jobs '
                '"$CMAKE_BUILD_PARALLEL_LEVEL" --target //test/lint:cache_clean '
                "--check aobus-modernize-use-starts-with",
            ],
        )
        # The lint step sits between the compile-contract verification and the
        # cache trim, so its outcomes gate the same trusted-main archive.
        positions = [linux.index(f"      - name: {name}\n") for name in (VERIFY, LINT, TRIM)]
        self.assertEqual(positions, sorted(positions))

    def test_bazel_hygiene_gate_is_optional_and_never_gates_the_cache_or_aggregation(self):
        step = _step(HYGIENE)
        self.assertIn("        continue-on-error: true\n", step)
        self.assertNotIn("\n        if:", step)
        self.assertEqual(
            _inline_run(step),
            './ao bazel hygiene --commit "$BASE_SHA" --path "$RUNNER_TEMP/aobus-bazel-debug" '
            '--jobs "$CMAKE_BUILD_PARALLEL_LEVEL"',
        )
        self.assertEqual(_env(step)["BASE_SHA"], "${{ needs.changes.outputs.base }}")
        # Its cache writes precede the trim, yet the save and aggregation ignore its outcome.
        positions = [_linux().index(f"      - name: {name}\n") for name in (LINT, HYGIENE, TRIM)]
        self.assertEqual(positions, sorted(positions))
        for name in (SAVE, "Require successful Bazel checks"):
            self.assertNotIn("bazel-hygiene", _step(name))

    def test_bazel_correctness_and_full_cmake_check_remain_mandatory(self):
        for name in (TEST, VERIFY, LINT):
            with self.subTest(step=name):
                self.assertIn("        continue-on-error: true\n", _step(name))
                self.assertNotIn("\n        if:", _step(name))
        for name in (CMAKE, "Run hygiene"):
            with self.subTest(step=name):
                self.assertNotIn("continue-on-error:", _step(name))
                self.assertNotIn("\n        if:", _step(name))
        self.assertIn("        id: bazel-test\n", _step(TEST))
        self.assertIn("        id: bazel-verify\n", _step(VERIFY))
        self.assertIn(f"        id: {LINT_ID}\n", _step(LINT))
        self.assertEqual(
            _step(TEST).split("        run: ", 1)[1].strip(),
            './ao bazel test --path "$RUNNER_TEMP/aobus-bazel-debug" --jobs "$CMAKE_BUILD_PARALLEL_LEVEL"',
        )
        self.assertEqual(
            _step(VERIFY).split("        run: |\n", 1)[1].strip().splitlines(),
            [
                './ao bazel verify --path "$RUNNER_TEMP/aobus-bazel-debug" --jobs "$CMAKE_BUILD_PARALLEL_LEVEL"',
                '          ./ao bazel verify --config release --path "$RUNNER_TEMP/aobus-bazel-release" '
                '--jobs "$CMAKE_BUILD_PARALLEL_LEVEL"',
            ],
        )
        self.assertIn("          ./ao check\n", _step(CMAKE))
        self.assertLess(_linux().index(f"- name: {CMAKE}\n"), _linux().index("- name: Run hygiene\n"))
        gate_name = "Require successful Bazel checks"
        gate = _step(gate_name)
        self.assertNotIn("continue-on-error:", gate)
        self.assertIn("        if: ${{ !cancelled() }}\n", gate)
        self.assertIn("        shell: bash\n", gate)
        self.assertEqual(
            _env(gate),
            {
                "BAZEL_TEST_OUTCOME": "${{ steps.bazel-test.outcome }}",
                "BAZEL_VERIFY_OUTCOME": "${{ steps.bazel-verify.outcome }}",
                "BAZEL_LINT_OUTCOME": "${{ steps.bazel-lint.outcome }}",
            },
        )
        self.assertLess(_linux().index("- name: Run hygiene\n"), _linux().index(f"- name: {gate_name}\n"))
        if sys.platform == "linux":
            for test, verify, lint, expected in (
                ("success", "success", "success", 0),
                ("failure", "success", "success", 1),
                ("success", "failure", "success", 1),
                ("skipped", "success", "success", 1),
                ("success", "skipped", "success", 1),
                ("success", "success", "failure", 1),
            ):
                with self.subTest(test=test, verify=verify, lint=lint):
                    result = subprocess.run(
                        ["bash", "-e", "-o", "pipefail", "-c", _run_body(gate)],
                        env={
                            **os.environ,
                            "BAZEL_TEST_OUTCOME": test,
                            "BAZEL_VERIFY_OUTCOME": verify,
                            "BAZEL_LINT_OUTCOME": lint,
                        },
                        capture_output=True,
                        text=True,
                        check=False,
                    )
                    self.assertEqual(result.returncode, expected, result.stderr)


REMOTE_START = "Start Bazel remote cache"
REMOTE_STOP = "Stop Bazel remote cache"
REMOTE_URL = "${{ steps.bazel-remote.outputs.url }}"
TRUSTED_MAIN_PUSH = "${{ github.event_name == 'push' && github.ref == 'refs/heads/main' }}"


def _job(name: str) -> str:
    workflow = WORKFLOW.read_text(encoding="utf-8")
    return re.split(r"\n(?=  \w+:\n)", workflow.split(f"\n  {name}:\n", 1)[1], maxsplit=1)[0]


def _steps(job: str) -> list[tuple[str, str]]:
    return [(step.split("\n", 1)[0], step) for step in re.split(r"\n      - name: ", job)[1:]]


class BazelRemoteCacheWorkflowTest(unittest.TestCase):
    """Rollout-gated R2 cache: explicit consumers and a draining stop."""

    def test_only_jobs_with_a_pinned_proxy_start_it(self):
        workflow = WORKFLOW.read_text(encoding="utf-8")
        starters = [name for name in re.findall(r"\n  (\w+):\n", workflow) if f"- name: {REMOTE_START}\n" in _job(name)]
        # bazel-remote refuses Windows builds upstream.
        self.assertEqual(starters, ["linux", "macos_bazel"])

    def test_remote_cache_rollout_requires_explicit_repository_opt_in(self):
        for job_name in ("linux", "macos_bazel"):
            with self.subTest(job=job_name):
                start = dict(_steps(_job(job_name)))[REMOTE_START]
                condition = start.split("        if: ", 1)[1].split("\n", 1)[0]
                # Missing repository variables evaluate to an empty string, not
                # true. This switch is rollout-only, never backend authorization.
                self.assertEqual(condition, "${{ vars.AOBUS_BAZEL_R2_CACHE_AUTHORIZED == 'true' }}")

    def test_start_selects_read_only_secret_names_without_falling_back_to_writer_values(self):
        for job_name in ("linux", "macos_bazel"):
            with self.subTest(job=job_name):
                steps = dict(_steps(_job(job_name)))
                start = steps[REMOTE_START]
                self.assertIn("        id: bazel-remote\n", start)
                self.assertIn("        continue-on-error: true\n", start)
                self.assertEqual(
                    {key: value for key, value in _env(start).items() if key.startswith("R2_")},
                    {
                        "R2_ENDPOINT": "${{ secrets[github.event_name == 'push' && "
                        "github.ref == 'refs/heads/main' && 'R2_SCCACHE_ENDPOINT' || 'R2_BAZEL_READ_ENDPOINT'] }}",
                        "R2_ACCESS_KEY_ID": "${{ secrets[github.event_name == 'push' && "
                        "github.ref == 'refs/heads/main' && "
                        "'R2_SCCACHE_ACCESS_KEY_ID' || 'R2_BAZEL_READ_ACCESS_KEY_ID'] }}",
                        "R2_SECRET_ACCESS_KEY": "${{ secrets[github.event_name == 'push' && "
                        "github.ref == 'refs/heads/main' && "
                        "'R2_SCCACHE_SECRET_ACCESS_KEY' || 'R2_BAZEL_READ_SECRET_ACCESS_KEY'] }}",
                    },
                )
                run = " ".join(start.split("        run: >-\n", 1)[1].split())
                self.assertNotIn("secrets.", run)
                self.assertTrue(run.startswith("python3 script/ao/core/bazel_remote_cache.py start "), run)
                self.assertIn(f'--trusted-writer "{TRUSTED_MAIN_PUSH}"', run)
                # Secret names, not values, are selected before lookup. Empty
                # read credentials cannot select a writer value through ||.
                self.assertNotIn("|| secrets", start)
                # No extra secret bindings for consumers/stop. Repository writer
                # secrets still need migration: editable conditions are not auth.
                for name, step in steps.items():
                    if name == REMOTE_STOP or "./ao bazel " in step:
                        self.assertNotIn("secrets.", step, name)

    def test_every_cached_bazel_step_maps_the_url_and_the_stop_drains_after_the_last(self):
        for job_name in ("linux", "macos_bazel"):
            with self.subTest(job=job_name):
                steps = _steps(_job(job_name))
                names = [name for name, _ in steps]
                consumers = [
                    name
                    for name, step in steps
                    if re.search(r"\./ao bazel (test|lint|hygiene)\b", step.split("        run:", 1)[-1])
                ]
                mapped = [name for name, step in steps if f"{bazel.REMOTE_CACHE_ENVIRONMENT}: {REMOTE_URL}" in step]
                self.assertTrue(consumers)
                self.assertEqual(mapped, consumers)
                self.assertEqual(sum(bazel.REMOTE_CACHE_ENVIRONMENT in step for _, step in steps), len(consumers))
                self.assertLess(names.index(REMOTE_START), names.index(consumers[0]))
                stop = dict(steps)[REMOTE_STOP]
                self.assertEqual(names.index(REMOTE_STOP), names.index(consumers[-1]) + 1)
                self.assertIn("        if: ${{ always() && steps.bazel-remote.outcome == 'success' }}\n", stop)
                self.assertIn("        continue-on-error: true\n", stop)
                state = '--state "$RUNNER_TEMP/aobus-bazel-remote"'
                self.assertEqual(_inline_run(stop), f"python3 script/ao/core/bazel_remote_cache.py stop {state}")
                self.assertIn(state, dict(steps)[REMOTE_START])


@unittest.skipUnless(sys.platform == "linux", "The CI cache steps require Linux Bash and GNU find.")
class BazelCacheTrimBashTest(unittest.TestCase):
    """Run the workflow's own marker and trim scripts against a fixture cache."""

    def setUp(self) -> None:
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)

    def _bash(self, script: str, **env: str) -> subprocess.CompletedProcess[str]:
        return subprocess.run(
            ["bash", "-e", "-o", "pipefail", "-c", script],
            env={**os.environ, **env},
            capture_output=True,
            text=True,
            check=False,
        )

    def _touch_marker(self, marker: Path) -> None:
        process = self._bash(_inline_run(_step(MARKER)), AOBUS_BAZEL_CACHE_MARKER=str(marker))
        self.assertEqual(process.returncode, 0, process.stderr)

    @staticmethod
    def _write(path: Path, mtime: float) -> None:
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(f"entry {path.name}", encoding="utf-8")
        os.utime(path, (mtime, mtime))

    def test_prune_removes_only_entries_not_newer_than_the_restore_marker(self):
        cache = self.root / "aobus-state" / "cache" / "bazel"
        marker = self.root / MARKER_BASENAME
        restored = [
            cache / "ac" / "old" / "action",
            cache / "cas" / "blobs" / "old" / "entry",
            cache / "cas" / "dir with space" / "old blob",
        ]
        refreshed = cache / "ac" / "refreshed" / "action"
        created = [cache / "cas" / "created" / "entry", cache / "cas" / "dir with space" / "new blob"]
        sentinel = self.root / "aobus-state" / "sentinel"

        # Restored archive entries and the outside-cache sentinel predate the
        # marker, like files extracted by the restore step.
        restored_mtime = time.time() - 3600
        for path in [*restored, refreshed, sentinel]:
            self._write(path, restored_mtime)

        # The workflow's own marker step runs after the restore, so the marker
        # postdates every restored entry; the prune boundary follows from that
        # ordering, with explicit mtimes instead of sleeps below.
        self._touch_marker(marker)
        marker_mtime = marker.stat().st_mtime
        self.assertGreater(marker_mtime, restored_mtime)
        boundary = cache / "cas" / "marker-time"
        self._write(boundary, marker_mtime)
        marker_ns = marker.stat().st_mtime_ns
        os.utime(boundary, ns=(marker_ns, marker_ns))
        restored.append(boundary)

        # This run's Bazel work refreshes one restored entry and creates new
        # entries, all newer than the marker.
        run_mtime = marker_mtime + 1
        os.utime(refreshed, (run_mtime, run_mtime))
        for path in created:
            self._write(path, run_mtime)
        sentinel_identity = (sentinel.read_bytes(), sentinel.stat().st_mtime_ns)

        process = self._bash(
            _run_body(_step(TRIM)),
            AOBUS_BAZEL_CACHE_DIR=str(cache),
            AOBUS_BAZEL_CACHE_MARKER=str(marker),
        )

        self.assertEqual(process.returncode, 0, process.stderr)
        for path in restored:
            with self.subTest(removed=path):
                self.assertFalse(path.exists())
        for path in (refreshed, *created):
            with self.subTest(kept=path):
                self.assertTrue(path.is_file())
        # The prune never leaves the cache directory: the sentinel and the
        # marker itself stay untouched.
        self.assertEqual((sentinel.read_bytes(), sentinel.stat().st_mtime_ns), sentinel_identity)
        self.assertTrue(marker.is_file())

    def test_missing_marker_fails_closed_and_keeps_restored_entries(self):
        cache = self.root / "aobus-state" / "cache" / "bazel"
        marker = self.root / MARKER_BASENAME
        restored = [cache / "ac" / "old" / "action", cache / "cas" / "blobs" / "old" / "entry"]
        for path in restored:
            self._write(path, time.time() - 3600)

        process = self._bash(
            _run_body(_step(TRIM)),
            AOBUS_BAZEL_CACHE_DIR=str(cache),
            AOBUS_BAZEL_CACHE_MARKER=str(marker),
        )

        self.assertNotEqual(process.returncode, 0)
        for path in restored:
            with self.subTest(intact=path):
                self.assertTrue(path.is_file())

    def test_absent_or_empty_cache_directory_has_nothing_to_trim(self):
        cache = self.root / "aobus-state" / "cache" / "bazel"
        marker = self.root / MARKER_BASENAME
        self._touch_marker(marker)

        absent = self._bash(
            _run_body(_step(TRIM)),
            AOBUS_BAZEL_CACHE_DIR=str(cache),
            AOBUS_BAZEL_CACHE_MARKER=str(marker),
        )
        self.assertEqual(absent.returncode, 0, absent.stderr)

        cache.mkdir(parents=True)
        empty = self._bash(
            _run_body(_step(TRIM)),
            AOBUS_BAZEL_CACHE_DIR=str(cache),
            AOBUS_BAZEL_CACHE_MARKER=str(marker),
        )
        self.assertEqual(empty.returncode, 0, empty.stderr)


if __name__ == "__main__":
    unittest.main()
