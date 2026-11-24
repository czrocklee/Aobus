"""Check the ready-tool resolver's watch scope and offline repository invalidation."""

import json
import os
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

from ao.core.paths import PROJECT_ROOT
from ao.graph.bazel_live_checkout import declared_file
from ao.portal import prettier

# Bazel's implicit module metadata is resolved even for a repository-only query.
# When upgrading the Bazel pin, rerun this offline test and reconcile the stubs
# with the newly bundled bazel_tools/MODULE.bazel if the query cannot load.
# The fixture does not load any targets or extensions from these modules.
_BUILTIN_DEPENDENCIES = (
    "apple_support",
    "buildozer",
    "platforms",
    "protobuf",
    "rules_cc",
    "rules_java",
    "rules_license",
    "rules_python",
    "rules_shell",
    "zlib",
)


@unittest.skipUnless(sys.platform == "linux", "offline repository fixture uses the pinned Linux Bazel")
class CheckToolRepositoryTest(unittest.TestCase):
    def test_buildifier_and_prettier_watches_are_independent_in_fresh_processes(self):
        probe = (
            # Load the standard library's host-specific proxy support before selecting the fixture host.
            "import json, sys, urllib.request\n"
            "sys.path.insert(0, sys.argv[1])\n"
            "from ao import portal\n"
            "from ao.portal import bazel_check_tools\n"
            "portal.__path__.insert(0, sys.argv[2])\n"
            "tool, other = sys.argv[3:]\n"
            # Use the managed provider without requiring a real Nix tool or native SDK.
            "sys.platform = 'darwin'\n"
            "facts = bazel_check_tools.resolve(tool)\n"
            "assert 'ao.portal.' + other not in sys.modules, f'imported unrelated {other} provider'\n"
            "print(json.dumps(facts))\n"
        )
        for tool, other in (("prettier", "buildifier"), ("buildifier", "prettier")):
            with self.subTest(tool=tool), tempfile.TemporaryDirectory(prefix="ao-resolver-imports-") as temporary:
                root = Path(temporary)
                provider = root / f"{other}.py"
                env = {**os.environ, "AOBUS_STATE_ROOT": str(root / "state")}
                command = [sys.executable, "-I", "-S", "-B", "-c", probe, str(PROJECT_ROOT / "script")]
                observations = []
                for revision in ("before", "after"):
                    provider.write_text(
                        f"def ready_tool():\n    raise AssertionError({revision!r})\n", encoding="utf-8"
                    )
                    result = subprocess.run(
                        [*command, str(root), tool, other],
                        env=env,
                        capture_output=True,
                        text=True,
                        timeout=30,
                    )
                    self.assertEqual(result.returncode, 0, result.stderr)
                    observations.append(json.loads(result.stdout))

                before, after = observations
                self.assertEqual(before, after)
                self.assertIn(f"run ao setup {tool}", before["selection"]["unavailable"])
                self.assertIn(str(PROJECT_ROOT / f"script/ao/{tool}.json"), before["watch_files"])
                self.assertIn(str(PROJECT_ROOT / "script/ao/core/build_lock.py"), before["imports"])
                watched = {*before["watch_files"], *before["imports"]}
                self.assertNotIn(str(PROJECT_ROOT / f"script/ao/portal/{other}.py"), watched)
                self.assertNotIn(str(PROJECT_ROOT / f"script/ao/{other}.json"), watched)

    def test_warm_prettier_repository_admits_setup_and_rechecks_receipt_and_cache_membership(self):
        manifest = declared_file("aobus_environment/check-tool-test-launcher.json", os.environ)
        self.assertIsNotNone(manifest, "The fixture requires the declared Bazel launcher runfile")
        selection = json.loads(manifest.read_text(encoding="utf-8"))
        self.assertEqual(selection["schema"], 1)
        if "unavailable" in selection:
            self.fail(selection["unavailable"])
        bazel = Path(selection["bazel"])
        javabase = Path(selection["server_javabase"])
        for tool in (bazel, javabase / "bin/java"):
            self.assertTrue(tool.is_absolute() and tool.resolve().is_relative_to("/nix/store") and tool.is_file(), tool)
        with tempfile.TemporaryDirectory(prefix="ao-check-tool-repo-") as temporary:
            root = Path(temporary)
            workspace = root / "workspace"
            workspace.mkdir()
            self.exercise(workspace, root, bazel, javabase)

    def exercise(self, workspace, root, bazel, javabase):
        state = root / "state"
        key = prettier._platform_key(sys.platform)
        cache = state / "tools/prettier/1.0.0/2.0.0" / key
        bundle = root / "staged"
        package = bundle / "package"
        entry = package / "bin/prettier.cjs"
        entry.parent.mkdir(parents=True)
        entry.write_text("// inert package entry\n", encoding="utf-8")
        plugin = package / "plugins/markdown.mjs"
        plugin.parent.mkdir()
        plugin.write_text("// inert Markdown plugin\n", encoding="utf-8")
        node = bundle / "node"
        node.write_text("#!/bin/sh\nprintf '1.0.0\\n'\n", encoding="utf-8")
        node.chmod(0o755)
        archive_hash = "a" * 64
        package_hash = "b" * 64
        node_hash = prettier._sha256(node)
        tree_hash = prettier._tree_digest(package)
        receipt = prettier._receipt(
            version="1.0.0",
            node_version="2.0.0",
            key=key,
            node_archive=archive_hash,
            node_executable=node_hash,
            package_archive=package_hash,
            package_tree=tree_hash,
        )
        prettier.cache_receipt_path(bundle).write_text(json.dumps(receipt), encoding="utf-8")
        policy = workspace / "prettier.json"
        policy.write_text(
            json.dumps(
                {
                    "version": "1.0.0",
                    "package": {"url": "https://invalid.example/prettier.tgz", "sha256": package_hash},
                    "packageTreeSha256": tree_hash,
                    "node": {
                        "version": "2.0.0",
                        "artifacts": {
                            key: {
                                "url": "https://invalid.example/node.tar.xz",
                                "sha256": archive_hash,
                                "nodeExecutableSha256": node_hash,
                            },
                        },
                    },
                }
            ),
            encoding="utf-8",
        )
        (workspace / "BUILD.bazel").touch()
        (workspace / "bazel").mkdir()
        (workspace / "bazel/BUILD.bazel").touch()
        for name in ("check_tools.bzl", "environment.bzl"):
            source = declared_file(f"_main/bazel/{name}", os.environ)
            self.assertIsNotNone(source, f"The fixture requires the declared {name} runfile")
            shutil.copyfile(source, workspace / "bazel" / name)
        resolver = workspace / "script/ao/portal/bazel_check_tools.py"
        resolver.parent.mkdir(parents=True)
        resolver.write_text(
            f"import sys\nsys.path.insert(0, {str(PROJECT_ROOT / 'script')!r})\n"
            "from pathlib import Path\nfrom ao.portal import bazel_check_tools, prettier\n"
            f"prettier.MANIFEST_PATH = Path({str(policy)!r})\nraise SystemExit(bazel_check_tools.main())\n",
            encoding="utf-8",
        )
        environment = workspace / "environment"
        environment.mkdir()
        (environment / "MODULE.bazel").write_text('module(name = "aobus_environment")\n', encoding="utf-8")
        (environment / "BUILD.bazel").write_text('exports_files(["environment.json"])\n', encoding="utf-8")
        (environment / "environment.json").write_text(
            json.dumps(
                {
                    "schema": 1,
                    "host": "linux",
                    "python": sys.executable,
                    "environment": {"AOBUS_STATE_ROOT": str(state)},
                }
            ),
            encoding="utf-8",
        )
        module = (
            'module(name = "test_ready_tools")\n'
            'bazel_dep(name = "aobus_environment", version = "1.0")\n'
            'local_path_override(module_name = "aobus_environment", path = "environment")\n'
            'tool = use_repo_rule("//bazel:check_tools.bzl", "check_tool_repository")\n'
            'tool(name = "fixture_prettier", tool = "prettier")\n'
        )
        for name in _BUILTIN_DEPENDENCIES:
            stub = workspace / "offline" / name
            stub.mkdir(parents=True)
            (stub / "MODULE.bazel").write_text(f'module(name = "{name}")\n', encoding="utf-8")
            module += f'local_path_override(module_name = "{name}", path = "offline/{name}")\n'
        (workspace / "MODULE.bazel").write_text(module, encoding="utf-8")
        registry = root / "registry"
        registry.mkdir()
        output = root / "output"
        startup = [
            str(bazel),
            "--ignore_all_rc_files",
            f"--server_javabase={javabase}",
            "--host_jvm_args=-Xmx512m",
            # Bound an idle orphan if the test process cannot run its finally block.
            "--max_idle_secs=120",
            f"--output_user_root={root / 'user'}",
            f"--output_base={output}",
        ]
        query = [
            *startup,
            "query",
            "@fixture_prettier//:command.json",
            "--lockfile_mode=off",
            "--repository_disable_download",
            f"--registry={registry.as_uri()}",
        ]

        # The pinned versioned wrapper provides its own helper PATH. Drop
        # ambient Java and PATH so successful startup proves the declared input.
        child_env = {"HOME": str(root), "LANG": "C.UTF-8", "PATH": "", "TMPDIR": str(root)}

        def read_selection():
            result = subprocess.run(query, cwd=workspace, env=child_env, capture_output=True, text=True, timeout=60)
            self.assertEqual(result.returncode, 0, result.stderr)
            (selection,) = (output / "external").glob("*fixture_prettier/command.json")
            return json.loads(selection.read_text(encoding="utf-8"))

        def check_rejected(message):
            result = subprocess.run(query, cwd=workspace, env=child_env, capture_output=True, text=True, timeout=60)
            self.assertNotEqual(result.returncode, 0, result.stderr)
            self.assertIn(message, result.stderr)

        try:
            self.assertIn("run ao setup prettier", read_selection()["unavailable"])
            cache.parent.mkdir(parents=True)
            bundle.rename(cache)
            self.assertEqual(
                read_selection()["command"], [str(cache / "node"), str(cache / "package/bin/prettier.cjs")]
            )

            receipt_file = prettier.cache_receipt_path(cache)
            receipt_file.write_text(json.dumps({**receipt, "prettierVersion": "1.0.1"}), encoding="utf-8")
            check_rejected("receipt mismatch")
            receipt_file.write_text(json.dumps(receipt), encoding="utf-8")
            self.assertIn("command", read_selection())

            unrelated = cache / "unrelated.txt"
            unrelated.touch()
            check_rejected("unexpected root entry: unrelated.txt")
            unrelated.unlink()
            self.assertIn("command", read_selection())

            cached_plugin = cache / plugin.relative_to(bundle)
            cached_plugin.write_text("// changed Markdown plugin\n", encoding="utf-8")
            check_rejected("pinned decoded digests")
        finally:
            subprocess.run(
                [*startup, "shutdown"], cwd=workspace, env=child_env, capture_output=True, timeout=30, check=True
            )
