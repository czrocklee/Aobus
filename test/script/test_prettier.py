"""Prettier admission, provisioning, and process-boundary contracts, without native downloads.

Subprocess and download fakes prove argv, isolation, extraction, and failure
handling against tiny real tar/zip fixtures, not acceptance by a native
Node.js runtime. Native control and binary smoke probes own that evidence.
"""

import contextlib
import hashlib
import io
import json
import os
import shutil
import subprocess
import tarfile
import tempfile
import unittest
import zipfile
from pathlib import Path
from types import SimpleNamespace
from unittest import mock

from ao.core import prettier

RECEIPT = ".aobus-prettier-receipt.json"


def _tar(entries: list[tuple[tarfile.TarInfo, bytes | None]]) -> bytes:
    buffer = io.BytesIO()
    with tarfile.open(fileobj=buffer, mode="w:gz") as bundle:
        for info, payload in entries:
            bundle.addfile(info, io.BytesIO(payload) if payload is not None else None)
    return buffer.getvalue()


def _file_entry(name: str, payload: bytes) -> tuple[tarfile.TarInfo, bytes]:
    info = tarfile.TarInfo(name)
    info.size = len(payload)
    return info, payload


def _package_entries(files: dict[str, bytes]) -> list[tuple[tarfile.TarInfo, bytes]]:
    return [_file_entry(name, payload) for name, payload in files.items()]


def _node_tar(distribution: str, payload: bytes) -> bytes:
    return _tar([_file_entry(f"{distribution}/bin/node", payload)])


def _windows_node_zip(member: str, payload: bytes) -> bytes:
    buffer = io.BytesIO()
    with zipfile.ZipFile(buffer, "w") as bundle:
        bundle.writestr(member, payload)
    return buffer.getvalue()


class PrettierRunTest(unittest.TestCase):
    def setUp(self):
        stack = contextlib.ExitStack()
        self.addCleanup(stack.close)
        self.root = Path(stack.enter_context(tempfile.TemporaryDirectory(prefix="aobus-prettier-test-")))
        (self.root / ".prettierrc.json").write_text("{}\n", encoding="utf-8")
        (self.root / ".gitignore").write_text("", encoding="utf-8")
        (self.root / ".prettierignore").write_text("", encoding="utf-8")
        (self.root / "README.md").write_text("# Aobus\n", encoding="utf-8")
        self.tool = ("/managed/node", "/managed/package/bin/prettier.cjs")
        self.returncode = 0
        self.stdout = ""
        self.stderr = ""
        stack.enter_context(mock.patch.object(prettier, "PROJECT_ROOT", self.root))
        self.selection = stack.enter_context(mock.patch.object(prettier, "command", return_value=self.tool))
        self.process = stack.enter_context(mock.patch.object(prettier.subprocess, "run", side_effect=self._process))

    def _mode_index(self, argv):
        indices = [index for index, argument in enumerate(argv) if argument in ("--check", "--write")]
        self.assertEqual(len(indices), 1, argv)
        return indices[0]

    def _mode(self, argv):
        return argv[self._mode_index(argv)]

    def _files(self, argv):
        return argv[self._mode_index(argv) + 1 :]

    def _process(self, argv, **kwargs):
        self.assertEqual(argv[:2], list(self.tool))
        self.assertEqual(
            argv[2:9],
            [
                "--config",
                str(self.root / ".prettierrc.json"),
                "--no-editorconfig",
                "--ignore-path",
                str(self.root / ".gitignore"),
                "--ignore-path",
                str(self.root / ".prettierignore"),
            ],
        )
        self.assertEqual(argv[9:11], ["--log-level", "warn"])
        self.assertEqual(kwargs["cwd"], self.root)
        self.assertEqual(kwargs["stdin"], subprocess.DEVNULL)
        self.assertEqual(kwargs["encoding"], "utf-8")
        self.assertEqual(kwargs["errors"], "strict")
        self.assertNotIn("NODE_OPTIONS", {key.upper() for key in kwargs["env"]})
        self.assertNotIn("NODE_PATH", {key.upper() for key in kwargs["env"]})
        # Every invocation carries exactly its intended mode flag, never both.
        flag = self._mode(argv)
        self.assertEqual(argv.count("--check"), 1 if flag == "--check" else 0)
        self.assertEqual(argv.count("--write"), 1 if flag == "--write" else 0)
        files = self._files(argv)
        self.assertTrue(files)
        self.assertTrue(all(Path(filename).is_absolute() for filename in files))
        return subprocess.CompletedProcess(argv, self.returncode, self.stdout, self.stderr)

    def _run(self, files=None, *, check=True):
        output, error = io.StringIO(), io.StringIO()
        with contextlib.redirect_stdout(output), contextlib.redirect_stderr(error):
            code = prettier.run(["README.md"] if files is None else files, check=check)
        return code, output.getvalue(), error.getvalue()

    def test_empty_selection_does_not_select_or_execute_any_tool(self):
        for check in (False, True):
            self.assertEqual(self._run([], check=check)[0], 0)
        self.selection.assert_not_called()
        self.process.assert_not_called()

    def test_check_mode_passes_explicit_config_and_both_repository_ignore_files(self):
        self.assertEqual(self._run(), (0, "", ""))
        argv = self.process.call_args.args[0]
        self.assertEqual(self._mode(argv), "--check")
        self.assertNotIn("--write", argv)
        self.assertEqual(argv[-1], str(self.root / "README.md"))
        self.selection.assert_called_once()

    def test_write_mode_rewrites_explicitly_and_never_requests_the_check_flag(self):
        self.assertEqual(self._run(check=False), (0, "", ""))
        argv = self.process.call_args.args[0]
        self.assertEqual(self._mode(argv), "--write")
        self.assertNotIn("--check", argv)

    def test_mode_flags_are_exclusive_and_always_present(self):
        # Without --write Prettier prints to stdout instead of rewriting, so a
        # missing mode flag is a defect even when the process succeeds.
        for check in (True, False):
            with self.subTest(check=check):
                self.assertEqual(self._run(check=check)[0], 0)
                argv = self.process.call_args.args[0]
                expected = "--check" if check else "--write"
                self.assertEqual(argv.count(expected), 1)
                self.assertNotIn("--write" if check else "--check", argv)
                self.process.reset_mock()

    def test_quiet_mode_keeps_warnings_visible_in_both_modes(self):
        # --log-level warn suppresses routine chatter without silencing the
        # file findings or the fail-closed stderr handling below.
        for check in (True, False):
            with self.subTest(check=check):
                self.assertEqual(self._run(check=check)[0], 0)
                argv = self.process.call_args.args[0]
                self.assertEqual(argv[argv.index("--log-level") + 1], "warn")
                self.assertNotIn("silent", argv)
                self.assertNotIn("error", argv)
                self.process.reset_mock()
        self.returncode, self.stdout = 1, "[warn] README.md\n"
        code, output, error = self._run()
        self.assertEqual(code, 1)
        self.assertEqual(output, "[warn] README.md\n")
        self.assertEqual(error, "Prettier exited with status 1\n")

    def test_child_environment_cannot_inject_node_selectors(self):
        with mock.patch.dict(
            os.environ,
            {"NODE_OPTIONS": "--require evil", "NODE_PATH": "/ambient", "node_options": "--require evil"},
        ):
            self.assertEqual(self._run()[0], 0)
            # The caller's ambient environment stays intact; only the child is filtered.
            self.assertEqual(os.environ["NODE_OPTIONS"], "--require evil")
        environment = self.process.call_args.kwargs["env"]
        self.assertNotIn("NODE_OPTIONS", environment)
        self.assertNotIn("node_options", environment)
        self.assertNotIn("NODE_PATH", environment)

    def test_paths_with_spaces_and_unicode_keep_their_caller_spelling(self):
        path = self.root / "directory with spaces" / "café 文件.md"
        path.parent.mkdir()
        path.write_text("content\n", encoding="utf-8")
        logical = str(Path("directory with spaces") / path.name)
        self.assertEqual(self._run([logical])[0], 0)
        self.assertEqual(self.process.call_args.args[0][-1], str(path))

    def test_duplicate_empty_or_nul_paths_fail_before_tool_selection(self):
        for files in (["README.md", str(self.root / "README.md")], [""], ["bad\0.md"]):
            with self.subTest(files=files), self.assertRaises(prettier.PrettierError):
                self._run(files)
        self.selection.assert_not_called()
        self.process.assert_not_called()

    def test_explicit_paths_outside_the_checkout_are_rejected_before_writes(self):
        outside = self.root.parent / "outside.md"
        for filename in ("../outside.md", str(outside), "sub/../../outside.md"):
            with self.subTest(filename=filename), self.assertRaises(prettier.PrettierError):
                self._run([filename])
        self.selection.assert_not_called()
        self.process.assert_not_called()
        self.assertFalse(outside.exists())

    @unittest.skipIf(os.name == "nt", "native symlink creation needs separate Windows privileges")
    def test_symlink_escape_is_rejected_but_interior_links_keep_their_spelling(self):
        target = self.root.parent / "linked-outside.md"
        target.write_text("outside\n", encoding="utf-8")
        escaping = self.root / "escape.md"
        escaping.symlink_to(target)
        with self.assertRaisesRegex(prettier.PrettierError, "inside the checkout"):
            self._run(["escape.md"])
        self.selection.assert_not_called()
        self.process.assert_not_called()
        interior = self.root / "interior target.md"
        interior.write_text("inside\n", encoding="utf-8")
        link = self.root / "interior link.md"
        link.symlink_to(interior)
        self.assertEqual(self._run(["interior link.md"])[0], 0)
        self.assertEqual(self.process.call_args.args[0][-1], str(link))

    def test_missing_repository_configuration_fails_before_tool_selection(self):
        for name in (".prettierrc.json", ".gitignore", ".prettierignore"):
            with self.subTest(name=name):
                (self.root / name).unlink()
                with self.assertRaisesRegex(prettier.PrettierError, name):
                    self._run()
                self.selection.assert_not_called()
                self.process.assert_not_called()
                (self.root / name).write_text("{}\n", encoding="utf-8")

    def test_formatting_findings_and_tool_errors_both_fail_the_gate(self):
        # Exit 1 from --check is a formatting difference; exit 2 is a tool
        # error. Both fail, and neither is silently treated as the other.
        self.returncode, self.stdout = 1, "[warn] README.md\n"
        self.assertEqual(self._run(), (1, "[warn] README.md\n", "Prettier exited with status 1\n"))
        self.returncode, self.stdout, self.stderr = 2, "", "README.md: Unexpected token (1:1)\n"
        code, output, error = self._run()
        self.assertEqual(code, 1)
        self.assertEqual(output, "")
        self.assertEqual(error, "README.md: Unexpected token (1:1)\nPrettier exited with status 2\n")
        self.returncode, self.stdout, self.stderr = 0, "", "unexplained child stderr\n"
        code, _, error = self._run()
        self.assertEqual(code, 1)
        self.assertIn("unexplained child stderr", error)

    def test_successful_child_output_is_forwarded_without_extra_diagnostics(self):
        self.stdout = "All matched files use Prettier code style!\n"
        self.assertEqual(self._run(), (0, self.stdout, ""))
        self.stdout = "formatted output\n"
        self.assertEqual(self._run(check=False), (0, self.stdout, ""))

    def test_process_launch_and_decoding_errors_have_a_local_prettier_exception(self):
        for failure in (FileNotFoundError("missing"), UnicodeDecodeError("utf-8", b"\xff", 0, 1, "invalid")):
            with self.subTest(failure=failure):
                self.process.side_effect = failure
                with self.assertRaisesRegex(prettier.PrettierError, "cannot execute Prettier"):
                    self._run()

    def test_large_selections_are_bounded_and_a_later_batch_failure_is_retained(self):
        files = [f"directory with spaces/file {index}.md" for index in range(205)]
        self.assertEqual(self._run(files), (0, "", ""))
        self.selection.assert_called_once()
        commands = [call.args[0] for call in self.process.call_args_list]
        self.assertEqual([len(self._files(command)) for command in commands], [100, 100, 5])
        self.assertEqual(
            [filename for command in commands for filename in self._files(command)],
            [str(self.root / filename) for filename in files],
        )
        self.process.reset_mock()

        def fail_second_batch(argv, **kwargs):
            result = self._process(argv, **kwargs)
            if argv[-1] == str(self.root / "directory with spaces/file 199.md"):
                result.returncode = 4
            return result

        self.process.side_effect = fail_second_batch
        self.assertEqual(self._run(files)[0], 1)
        self.assertEqual(self.process.call_count, 3)

    def test_command_length_bounds_split_long_unicode_paths(self):
        files = [f"{'文件' * 120}/file{index}.md" for index in range(100)]
        self.assertEqual(self._run(files)[0], 0)
        self.assertGreater(self.process.call_count, 1)
        for call in self.process.call_args_list:
            length = len(subprocess.list2cmdline(call.args[0]).encode("utf-16-le")) // 2
            self.assertLessEqual(length, 24000)


class PrettierProvisionTest(unittest.TestCase):
    def setUp(self):
        stack = contextlib.ExitStack()
        self.addCleanup(stack.close)
        self.root = Path(stack.enter_context(tempfile.TemporaryDirectory(prefix="aobus-prettier-cache-test-")))
        self.state = self.root / "state with spaces café"
        self.manifest = self.root / "prettier.json"
        # Each platform carries a distinct executable so per-platform pins and
        # cache layouts cannot be confused with each other.
        self.node_payloads = {
            key: f"#!/bin/sh\n# pinned {key} Node.js executable fixture\n".encode()
            for key in ("linux-x86_64", "darwin-x86_64", "darwin-arm64", "windows-x86_64")
        }
        self.package_files = {
            "package/package.json": b'{"name": "prettier"}\n',
            "package/bin/prettier.cjs": b"// prettier entry fixture\n",
            "package/index.js": b"// prettier library fixture\n",
        }
        self.archives = {
            "linux-x86_64": _node_tar("node-v24.22.2-linux-x64", self.node_payloads["linux-x86_64"]),
            "darwin-x86_64": _node_tar("node-v24.22.2-darwin-x64", self.node_payloads["darwin-x86_64"]),
            "darwin-arm64": _node_tar("node-v24.22.2-darwin-arm64", self.node_payloads["darwin-arm64"]),
            "windows-x86_64": _windows_node_zip("node-v24.22.2-win-x64/node.exe", self.node_payloads["windows-x86_64"]),
        }
        self.package_archive = _tar(_package_entries(self.package_files))
        # The decoded-content pins come from the implementation's own safe
        # extraction, exactly like the real manifest was produced.
        decoded = self.root / "decoded"
        decoded_package = decoded / "package"
        decoded_package.mkdir(parents=True)
        package_file = decoded / "prettier-download"
        package_file.write_bytes(self.package_archive)
        prettier._extract_package(package_file, decoded_package)
        self.document = {
            "version": "3.9.10",
            "package": {
                "url": "https://registry.npmjs.org/prettier/-/prettier-3.9.10.tgz",
                "sha256": hashlib.sha256(self.package_archive).hexdigest(),
            },
            "packageTreeSha256": prettier._tree_digest(decoded_package),
            "node": {
                "version": "24.22.2",
                "artifacts": {
                    key: {
                        "url": f"https://nodejs.org/dist/v24.22.2/node-{key}.fixture",
                        "sha256": hashlib.sha256(archive).hexdigest(),
                        "nodeExecutableSha256": hashlib.sha256(self.node_payloads[key]).hexdigest(),
                    }
                    for key, archive in self.archives.items()
                },
            },
        }
        self._save_manifest()
        self.host = SimpleNamespace(platform="darwin")
        self.machine = SimpleNamespace(machine=lambda: "arm64")
        self.key = "darwin-arm64"
        self.version_output = "3.9.10\n"
        self.version_status = 0
        self.version_stderr = ""
        self.locked = False
        stack.enter_context(mock.patch.object(prettier, "MANIFEST_PATH", self.manifest))
        stack.enter_context(mock.patch.object(prettier, "sys", self.host))
        stack.enter_context(mock.patch.object(prettier, "platform", self.machine))
        # The pinned Nix shell exports its selection; managed admission, which
        # the lightweight Linux docs CI host also uses, must stay selectable.
        stack.enter_context(mock.patch.dict(os.environ, {"AOBUS_STATE_ROOT": str(self.state), "AO_NIX_PRETTIER": ""}))
        self.download = stack.enter_context(mock.patch.object(prettier, "_download", side_effect=self._download))
        self.process = stack.enter_context(mock.patch.object(prettier.subprocess, "run", side_effect=self._process))
        real_lock = prettier.shared_file_lock

        @contextlib.contextmanager
        def locked(path):
            self.assertTrue(path.is_relative_to(self.state))
            with real_lock(path):
                self.locked = True
                try:
                    yield
                finally:
                    self.locked = False

        stack.enter_context(mock.patch.object(prettier, "shared_file_lock", side_effect=locked))

    @property
    def node_payload(self):
        return self.node_payloads[self.key]

    def _save_manifest(self):
        self.manifest.write_text(json.dumps(self.document), encoding="utf-8")

    def _repin(self, key):
        self.document["node"]["artifacts"][key]["sha256"] = hashlib.sha256(self.archives[key]).hexdigest()
        self._save_manifest()

    def _select(self, host, machine, key):
        self.host.platform = host
        self.machine.machine = lambda machine=machine: machine
        self.key = key

    def _cache(self, key=None):
        return self.state / "tools/prettier/3.9.10/24.22.2" / (self.key if key is None else key)

    def _node(self, key=None):
        cache = self._cache(key)
        return cache / ("node.exe" if cache.name.startswith("windows-") else "node")

    def _download(self, url, destination):
        self.assertTrue(self.locked)
        self.assertEqual(Path(destination).parent.name[:16], ".prettier-stage-")
        for key, archive in self.archives.items():
            if url == self.document["node"]["artifacts"][key]["url"]:
                Path(destination).write_bytes(archive)
                return
        if url == self.document["package"]["url"]:
            Path(destination).write_bytes(self.package_archive)
            return
        raise AssertionError(f"unexpected download: {url}")

    def _process(self, argv, **kwargs):
        # The probe runs against the staged pair before publication and the
        # published pair afterwards; both must be consistent local closures.
        self.assertTrue(self.locked)
        self.assertEqual(len(argv), 3)
        self.assertEqual(argv[2], "--version")
        node = Path(argv[0])
        self.assertIn(node.name, ("node", "node.exe"))
        self.assertEqual(node.parent.parent, self._cache().parent)
        self.assertTrue(node.parent.name == self._cache().name or node.parent.name.startswith(".prettier-stage-"))
        self.assertEqual(argv[1], str(node.parent / "package/bin/prettier.cjs"))
        self.assertNotIn("NODE_OPTIONS", {name.upper() for name in kwargs["env"]})
        self.assertEqual(node.read_bytes(), self.node_payload)
        return subprocess.CompletedProcess(argv, self.version_status, self.version_output, self.version_stderr)

    def test_missing_cache_provisions_verified_artifacts_and_publishes_atomically(self):
        real_replace = os.replace
        published = []

        def publish(staged, target):
            self.assertTrue(self.locked)
            self.assertEqual(target, self._cache())
            self.assertEqual(staged.parent, target.parent)
            self.assertNotEqual(staged, target)
            self.assertEqual((staged / "node").read_bytes(), self.node_payload)
            self.assertTrue((staged / RECEIPT).is_file())
            self.assertTrue((staged / "package/bin/prettier.cjs").is_file())
            # Raw archives are staging inputs only and must be gone before the
            # atomic publication, which carries exactly the execution closure.
            self.assertFalse((staged / "node-download").exists())
            self.assertFalse((staged / "package-download").exists())
            self.assertEqual(sorted(path.name for path in staged.iterdir()), [RECEIPT, "node", "package"])
            published.append(target)
            real_replace(staged, target)

        with mock.patch.object(prettier.os, "replace", side_effect=publish):
            self.assertEqual(prettier.command(), (str(self._node()), str(self._cache() / "package/bin/prettier.cjs")))
        self.assertEqual(published, [self._cache()])
        self.assertEqual(self._node().read_bytes(), self.node_payload)
        self.assertEqual(
            (self._cache() / "package/bin/prettier.cjs").read_bytes(),
            self.package_files["package/bin/prettier.cjs"],
        )
        self.assertEqual(sorted(path.name for path in self._cache().iterdir()), [RECEIPT, "node", "package"])
        self.assertEqual(
            [call.args[0] for call in self.download.call_args_list],
            [self.document["node"]["artifacts"]["darwin-arm64"]["url"], self.document["package"]["url"]],
        )
        receipt = json.loads((self._cache() / RECEIPT).read_text(encoding="utf-8"))
        self.assertEqual(receipt["prettierVersion"], "3.9.10")
        self.assertEqual(receipt["nodeVersion"], "24.22.2")
        self.assertEqual(receipt["platform"], "darwin-arm64")
        self.assertEqual(receipt["nodeArchiveSha256"], self.document["node"]["artifacts"]["darwin-arm64"]["sha256"])
        self.assertEqual(
            receipt["nodeExecutableSha256"], self.document["node"]["artifacts"]["darwin-arm64"]["nodeExecutableSha256"]
        )
        self.assertEqual(receipt["packageArchiveSha256"], self.document["package"]["sha256"])
        self.assertEqual(receipt["packageTreeSha256"], self.document["packageTreeSha256"])
        self.assertEqual(list(self._cache().parent.glob(".prettier-stage-*")), [])

    def test_existing_cache_is_reverified_and_reused_without_any_download(self):
        expected = (str(self._node()), str(self._cache() / "package/bin/prettier.cjs"))
        self.assertEqual(prettier.command(), expected)
        # The producer publishes exactly the three execution entries.
        self.assertEqual(sorted(path.name for path in self._cache().iterdir()), [RECEIPT, "node", "package"])
        self.download.reset_mock()
        self.process.reset_mock()
        self.assertEqual(prettier.command(), expected)
        self.download.assert_not_called()
        self.process.assert_called_once()
        self.assertEqual(list(self._cache().parent.glob(".prettier-stage-*")), [])

    def test_legacy_cache_with_raw_download_archives_still_verifies(self):
        # Older caches retained the raw archives beside the execution closure.
        # Those legacy entries stay admissible and are never repaired or
        # deleted, so an existing valid cache is not declared corrupt.
        prettier.command()
        self.process.reset_mock()
        self.download.reset_mock()
        (self._cache() / "node-download").write_bytes(b"legacy raw Node.js archive\n")
        (self._cache() / "package-download").write_bytes(b"legacy raw Prettier archive\n")
        self.assertEqual(
            sorted(path.name for path in self._cache().iterdir()),
            [RECEIPT, "node", "node-download", "package", "package-download"],
        )
        self.assertEqual(prettier.command(), (str(self._node()), str(self._cache() / "package/bin/prettier.cjs")))
        self.process.assert_called_once()
        self.download.assert_not_called()
        self.assertEqual((self._cache() / "node-download").read_bytes(), b"legacy raw Node.js archive\n")
        self.assertEqual((self._cache() / "package-download").read_bytes(), b"legacy raw Prettier archive\n")

    def test_windows_cache_rejects_dll_planted_beside_the_verified_runtime(self):
        # node.exe statically imports dbghelp and friends, which are not all
        # KnownDLLs: a planted library would load before any digest of it is
        # taken, so the root-entry guard must reject it before the probe.
        self._select("win32", "AMD64", "windows-x86_64")
        prettier.command()
        self.process.reset_mock()
        self.download.reset_mock()
        (self._cache() / "dbghelp.dll").write_bytes(b"malicious library\n")
        with self.assertRaisesRegex(prettier.PrettierError, "unexpected root entry: dbghelp.dll"):
            prettier.command()
        self.process.assert_not_called()
        self.download.assert_not_called()

    @unittest.skipIf(os.name == "nt", "native symlink creation needs separate Windows privileges")
    def test_cache_root_entry_symlinks_are_rejected_before_probe(self):
        prettier.command()
        self.process.reset_mock()
        (self._cache() / "node-download").symlink_to(self.root / "outside-download")
        with self.assertRaisesRegex(prettier.PrettierError, "symlink at the cache root: node-download"):
            prettier.command()
        self.process.assert_not_called()

    @unittest.skipIf(os.name == "nt", "native symlink creation needs separate Windows privileges")
    def test_cache_root_symlink_is_rejected_before_probe(self):
        prettier.command()
        self.process.reset_mock()
        real = self._cache().parent / "real-cache"
        self._cache().rename(real)
        self._cache().symlink_to(real)
        with self.assertRaisesRegex(prettier.PrettierError, "cache root is a symlink"):
            prettier.command()
        self.process.assert_not_called()

    def test_damaged_cache_fails_explicitly_without_silent_repair(self):
        for damage in (self._damage_node, self._damage_package, self._damage_receipt, self._remove_receipt):
            with self.subTest(damage=damage.__name__):
                shutil.rmtree(self._cache(), ignore_errors=True)
                prettier.command()
                self.download.reset_mock()
                self.process.reset_mock()
                damage()
                with self.assertRaisesRegex(prettier.PrettierError, "damaged; remove it explicitly"):
                    prettier.command()
                self.download.assert_not_called()
                self.process.assert_not_called()

    def _damage_node(self):
        self._node().write_bytes(b"corrupted executable\n")

    def _damage_package(self):
        (self._cache() / "package/bin/prettier.cjs").write_bytes(b"corrupted package\n")

    def _damage_receipt(self):
        receipt = self._cache() / RECEIPT
        document = json.loads(receipt.read_text(encoding="utf-8"))
        document["prettierVersion"] = "3.9.11"
        receipt.write_text(json.dumps(document), encoding="utf-8")

    def _remove_receipt(self):
        (self._cache() / RECEIPT).unlink()

    def test_tampered_cache_with_matching_receipt_is_rejected_before_probe(self):
        # Tripwire: an attacker who rewrites both the runtime and the receipt
        # to agree with each other still fails, because the independent
        # manifest pins decide; no cached runtime may execute first.
        prettier.command()
        self.process.reset_mock()
        self._node().write_bytes(b"#!/bin/sh\n# tampered executable\n")
        receipt = self._cache() / RECEIPT
        document = json.loads(receipt.read_text(encoding="utf-8"))
        document["nodeExecutableSha256"] = hashlib.sha256(b"#!/bin/sh\n# tampered executable\n").hexdigest()
        receipt.write_text(json.dumps(document), encoding="utf-8")
        with self.assertRaisesRegex(prettier.PrettierError, "pinned decoded digests"):
            prettier.command()
        self.process.assert_not_called()

        shutil.rmtree(self._cache())
        prettier.command()
        self.process.reset_mock()
        (self._cache() / "package/bin/prettier.cjs").write_bytes(b"// tampered entry\n")
        document = json.loads(receipt.read_text(encoding="utf-8"))
        document["packageTreeSha256"] = prettier._tree_digest(self._cache() / "package")
        receipt.write_text(json.dumps(document), encoding="utf-8")
        with self.assertRaisesRegex(prettier.PrettierError, "pinned decoded digests"):
            prettier.command()
        self.process.assert_not_called()

    def test_provision_checks_extracted_content_against_pins_before_probe(self):
        original = json.loads(json.dumps(self.document))
        for field, mutate, pattern in (
            ("nodeExecutableSha256", self._wrong_executable_pin, "extracted Node.js executable"),
            ("packageTreeSha256", self._wrong_package_pin, "extracted Prettier package tree"),
        ):
            with self.subTest(field=field):
                mutate()
                with self.assertRaisesRegex(prettier.PrettierError, pattern):
                    prettier.command()
                self.process.assert_not_called()
                self.assertFalse(self._cache().exists())
                self.assertEqual(list(self._cache().parent.glob(".prettier-stage-*")), [])
                self.document = json.loads(json.dumps(original))
                self._save_manifest()

    def _wrong_executable_pin(self):
        self.document["node"]["artifacts"]["darwin-arm64"]["nodeExecutableSha256"] = "0" * 64
        self._save_manifest()

    def _wrong_package_pin(self):
        self.document["packageTreeSha256"] = "0" * 64
        self._save_manifest()

    def test_archive_identity_drift_between_manifest_and_cache_fails(self):
        prettier.command()
        self.download.reset_mock()
        self.process.reset_mock()
        self.document["package"]["sha256"] = "0" * 64
        self._save_manifest()
        with self.assertRaisesRegex(prettier.PrettierError, "packageArchiveSha256"):
            prettier.command()
        self.download.assert_not_called()

    def test_download_hash_mismatches_leave_no_published_cache(self):
        self.document["node"]["artifacts"]["darwin-arm64"]["sha256"] = "0" * 64
        self._save_manifest()
        with self.assertRaisesRegex(prettier.PrettierError, "Node.js SHA-256"):
            prettier.command()
        self.assertFalse(self._cache().exists())
        self.assertEqual(list(self._cache().parent.glob(".prettier-stage-*")), [])
        self.process.assert_not_called()

        self.document["node"]["artifacts"]["darwin-arm64"]["sha256"] = hashlib.sha256(
            self.archives["darwin-arm64"]
        ).hexdigest()
        self.document["package"]["sha256"] = "0" * 64
        self._save_manifest()
        with self.assertRaisesRegex(prettier.PrettierError, "Prettier package SHA-256"):
            prettier.command()
        self.assertFalse(self._cache().exists())
        self.assertEqual(list(self._cache().parent.glob(".prettier-stage-*")), [])

    def test_download_errors_remain_local_failures_without_a_partial_cache(self):
        self.download.side_effect = OSError("download interrupted")
        with self.assertRaisesRegex(prettier.PrettierError, "download interrupted"):
            prettier.command()
        self.assertFalse(self._cache().exists())
        self.assertEqual(list(self._cache().parent.glob(".prettier-stage-*")), [])

    def test_failed_atomic_publication_leaves_no_partial_cache(self):
        with mock.patch.object(prettier.os, "replace", side_effect=OSError("publication failed")):
            with self.assertRaisesRegex(prettier.PrettierError, "publication failed"):
                prettier.command()
        self.assertFalse(self._cache().exists())
        self.assertEqual(list(self._cache().parent.glob(".prettier-stage-*")), [])

    def test_exact_version_probe_accepts_only_the_pinned_banner(self):
        for output in ("3.9.10\n", "3.9.10 \n", "3.9.10"):
            with self.subTest(output=output):
                self.version_output = output
                self.assertEqual(prettier.command()[0], str(self._node()))

    def test_probe_failures_prevent_publication(self):
        for output, status, error in (
            ("3.9.11\n", 0, ""),
            ("prettier 3.9.10\n", 0, ""),
            ("3.9.10\nextra output\n", 0, ""),
            ("3.9.10\n", 1, ""),
            ("3.9.10\n", 0, "warning noise\n"),
        ):
            with self.subTest(output=output, status=status, error=error):
                self.version_output, self.version_status, self.version_stderr = output, status, error
                with self.assertRaisesRegex(prettier.PrettierError, "3.9.10"):
                    prettier.command()
                self.assertFalse(self._cache().exists())
                self.assertEqual(list(self._cache().parent.glob(".prettier-stage-*")), [])

    def test_probe_failures_retain_bounded_stream_diagnostics(self):
        self.version_status = 127
        self.version_stderr = (
            "Could not start dynamically linked executable: ./node\n"
            "NixOS cannot run dynamically linked executables intended for generic\n"
            "linux environments out of the box.\n"
        )
        with self.assertRaisesRegex(prettier.PrettierError, "exit status 127; stderr: Could not start") as raised:
            prettier.command()
        self.assertIn("NixOS cannot run", str(raised.exception))
        self.version_status, self.version_stderr = 0, ""
        self.version_output = "prettier 3.9.10\n"
        with self.assertRaisesRegex(prettier.PrettierError, "stdout: prettier 3.9.10"):
            prettier.command()
        self.version_output = "3.9.10\n"
        self.version_stderr = "noise " * 400
        with self.assertRaisesRegex(prettier.PrettierError, r"stderr: noise.*\.\.\.") as bounded:
            prettier.command()
        self.assertLess(len(str(bounded.exception)), 700)

    def test_unsafe_package_archives_are_rejected_without_publication(self):
        escape = tarfile.TarInfo("../escape.txt")
        escape.size = 1
        link = tarfile.TarInfo("package/link")
        link.type = tarfile.SYMTYPE
        link.linkname = "/etc/passwd"
        absolute = tarfile.TarInfo("/etc/passwd")
        absolute.size = 1
        backslash = tarfile.TarInfo("package/..\\escape.txt")
        backslash.size = 1
        for info, payload in (
            (escape, b"x"),
            (link, None),
            (absolute, b"x"),
            (backslash, b"x"),
        ):
            with self.subTest(member=info.name):
                self.package_archive = _tar([*_package_entries(self.package_files), (info, payload)])
                self.document["package"]["sha256"] = hashlib.sha256(self.package_archive).hexdigest()
                self._save_manifest()
                with self.assertRaisesRegex(prettier.PrettierError, "unsafe|unsupported"):
                    prettier.command()
                self.assertFalse(self._cache().exists())
                self.assertEqual(list(self._cache().parent.glob(".prettier-stage-*")), [])

    def test_platform_selections_use_separate_caches_and_the_windows_zip_layout(self):
        self._select("linux", "x86_64", "linux-x86_64")
        self.assertEqual(
            prettier.command(),
            (str(self._node("linux-x86_64")), str(self._cache("linux-x86_64") / "package/bin/prettier.cjs")),
        )
        self.assertEqual(self._node("linux-x86_64").read_bytes(), self.node_payloads["linux-x86_64"])
        self._select("win32", "AMD64", "windows-x86_64")
        self.assertEqual(
            prettier.command(),
            (str(self._node("windows-x86_64")), str(self._cache("windows-x86_64") / "package/bin/prettier.cjs")),
        )
        self.assertEqual(self._node("windows-x86_64").read_bytes(), self.node_payloads["windows-x86_64"])
        self._select("darwin", "AMD64", "darwin-x86_64")
        self.assertEqual(prettier.command()[0], str(self._node("darwin-x86_64")))
        self.assertEqual(self._node("darwin-x86_64").read_bytes(), self.node_payloads["darwin-x86_64"])
        self._select("darwin", "aarch64", "darwin-arm64")
        self.assertEqual(prettier.command()[0], str(self._node("darwin-arm64")))
        self.assertEqual(self._node("darwin-arm64").read_bytes(), self.node_payloads["darwin-arm64"])
        # Four separate platform caches, each provisioned from two archives.
        self.assertEqual(self.download.call_count, 8)

    def test_unsupported_hosts_and_machines_fail_before_cache_creation(self):
        for host, machine in (("darwin", "riscv64"), ("win32", "ARM64"), ("freebsd14", "x86_64")):
            with self.subTest(host=host, machine=machine):
                self._select(host, machine, f"{host}-{machine}")
                with self.assertRaisesRegex(prettier.PrettierError, "artifact|platform"):
                    prettier.command()
        self.assertFalse(self.state.exists())
        self.download.assert_not_called()
        self.process.assert_not_called()

    def test_missing_platform_artifact_fails_before_download(self):
        del self.document["node"]["artifacts"]["darwin-arm64"]
        self._save_manifest()
        with self.assertRaisesRegex(prettier.PrettierError, "darwin-arm64"):
            prettier.command()
        self.assertFalse(self.state.exists())
        self.download.assert_not_called()

    def test_malformed_manifests_fail_before_any_cache_or_download(self):
        cases = (
            {"version": "3.9.10"},
            {"version": "3.9.10", "package": {"url": "https://x", "sha256": "0" * 63}},
            {
                "version": "3.9.10",
                "package": {"url": "https://x", "sha256": "A" * 64},
                "node": {"version": "24.22.2", "artifacts": {}},
            },
            {"version": "3.9.10", "package": {"url": "https://x", "sha256": "0" * 64}, "node": {"version": "24.22.2"}},
            {
                "version": "3.9.10",
                "package": {"url": "https://x", "sha256": "0" * 64},
                "node": {
                    "version": "24.22.2",
                    "artifacts": {
                        "darwin-arm64": {"url": "https://x", "sha256": "0" * 64, "nodeExecutableSha256": "0" * 64}
                    },
                },
            },
            {
                "version": "3.9.10",
                "package": {"url": "https://x", "sha256": "0" * 64},
                "packageTreeSha256": "A" * 64,
                "node": {
                    "version": "24.22.2",
                    "artifacts": {"darwin-arm64": {"url": "https://x", "sha256": "0" * 64}},
                },
            },
            {
                "version": "3.9.10",
                "package": {"url": "https://x", "sha256": "0" * 64},
                "packageTreeSha256": "0" * 64,
                "node": {
                    "version": "24.22.2",
                    "artifacts": {
                        "darwin-arm64": {
                            "url": "https://x",
                            "sha256": "0" * 64,
                            "nodeExecutableSha256": "0" * 63,
                        }
                    },
                },
            },
            [],
        )
        for document in cases:
            with self.subTest(document=document):
                self.manifest.write_text(json.dumps(document), encoding="utf-8")
                with self.assertRaisesRegex(prettier.PrettierError, "manifest"):
                    prettier.command()
        self.manifest.write_text("{ not json", encoding="utf-8")
        with self.assertRaisesRegex(prettier.PrettierError, "manifest"):
            prettier.command()
        self.manifest.unlink()
        with self.assertRaisesRegex(prettier.PrettierError, "manifest"):
            prettier.command()
        self.assertFalse(self.state.exists())
        self.download.assert_not_called()

    def test_node_archives_without_exactly_one_executable_are_rejected(self):
        self.archives["darwin-arm64"] = _tar(_package_entries(self.package_files))
        self.document["node"]["artifacts"]["darwin-arm64"]["sha256"] = hashlib.sha256(
            self.archives["darwin-arm64"]
        ).hexdigest()
        self._save_manifest()
        with self.assertRaisesRegex(prettier.PrettierError, "bin/node"):
            prettier.command()
        self.assertFalse(self._cache().exists())

        self._select("win32", "AMD64", "windows-x86_64")
        buffer = io.BytesIO()
        with zipfile.ZipFile(buffer, "w") as bundle:
            bundle.writestr("readme.txt", "no executable")
        self.archives["windows-x86_64"] = buffer.getvalue()
        self._repin("windows-x86_64")
        with self.assertRaisesRegex(prettier.PrettierError, "node.exe"):
            prettier.command()
        self.assertFalse(self._cache().exists())

    def test_windows_node_archives_admit_only_one_unambiguous_executable(self):
        self._select("win32", "AMD64", "windows-x86_64")
        # The official one-directory layout provisions; a flat archive does too.
        self.archives["windows-x86_64"] = _windows_node_zip("node.exe", self.node_payload)
        self._repin("windows-x86_64")
        self.assertEqual(prettier.command()[0], str(self._node("windows-x86_64")))
        shutil.rmtree(self._cache())
        self.archives["windows-x86_64"] = _windows_node_zip("node-v24.22.2-win-x64/node.exe", self.node_payload)
        self._repin("windows-x86_64")
        ambiguous = io.BytesIO()
        with zipfile.ZipFile(ambiguous, "w") as bundle:
            bundle.writestr("node.exe", b"decoy")
            bundle.writestr("node-v24.22.2-win-x64/node.exe", self.node_payload)
        nested = io.BytesIO()
        with zipfile.ZipFile(nested, "w") as bundle:
            bundle.writestr("node-v24.22.2-win-x64/tools/node.exe", self.node_payload)
        linked = io.BytesIO()
        with zipfile.ZipFile(linked, "w") as bundle:
            info = zipfile.ZipInfo("node-v24.22.2-win-x64/node.exe")
            info.external_attr = 0o120000 << 16
            bundle.writestr(info, b"/elsewhere/node")
        escaping = io.BytesIO()
        with zipfile.ZipFile(escaping, "w") as bundle:
            bundle.writestr("../node.exe", self.node_payload)
        for archive, pattern in (
            (ambiguous.getvalue(), "exactly one node.exe"),
            (nested.getvalue(), "exactly one node.exe"),
            (linked.getvalue(), "exactly one node.exe"),
            (escaping.getvalue(), "unsafe Node.js archive member"),
        ):
            with self.subTest(archive=archive[:24]):
                shutil.rmtree(self._cache(), ignore_errors=True)
                self.archives["windows-x86_64"] = archive
                self._repin("windows-x86_64")
                with self.assertRaisesRegex(prettier.PrettierError, pattern):
                    prettier.command()
                self.assertFalse(self._cache().exists())
                self.assertEqual(list(self._cache().parent.glob(".prettier-stage-*")), [])


@unittest.skipIf(os.name == "nt", "Nix provider admission uses POSIX store paths")
class PrettierNixTest(unittest.TestCase):
    def setUp(self):
        stack = contextlib.ExitStack()
        self.addCleanup(stack.close)
        self.root = Path(stack.enter_context(tempfile.TemporaryDirectory(prefix="aobus-prettier-nix-test-")))
        manifest = self.root / "prettier.json"
        manifest.write_text(
            json.dumps(
                {
                    "version": "3.9.10",
                    "package": {"url": "https://registry.npmjs.org/prettier/-/prettier-3.9.10.tgz", "sha256": "0" * 64},
                    "packageTreeSha256": "0" * 64,
                    "node": {
                        "version": "24.22.2",
                        "artifacts": {
                            "linux-x86_64": {
                                "url": "https://nodejs.org/fixture",
                                "sha256": "0" * 64,
                                "nodeExecutableSha256": "0" * 64,
                            }
                        },
                    },
                }
            ),
            encoding="utf-8",
        )
        self.tool = Path("/nix/store/fixture-prettier-3.9.10/bin/prettier")
        self.version_output = "3.9.10\n"
        self.status = 0
        stack.enter_context(mock.patch.object(prettier, "MANIFEST_PATH", manifest))
        stack.enter_context(mock.patch.object(prettier, "sys", SimpleNamespace(platform="linux")))
        stack.enter_context(
            mock.patch.dict(
                os.environ,
                {
                    "AO_NIX_PRETTIER": str(self.tool),
                    "AOBUS_PRETTIER": str(self.root / "ambient-prettier"),
                    "PRETTIER": str(self.root / "another-ambient-prettier"),
                },
            )
        )
        real_is_file = Path.is_file
        stack.enter_context(
            mock.patch.object(
                Path, "is_file", autospec=True, side_effect=lambda path: path == self.tool or real_is_file(path)
            )
        )
        self.access = stack.enter_context(mock.patch.object(prettier.os, "access", return_value=True))
        self.process = stack.enter_context(mock.patch.object(prettier.subprocess, "run", side_effect=self._process))
        self.managed = stack.enter_context(
            mock.patch.object(
                prettier, "_managed_command", side_effect=AssertionError("unexpected managed provisioning")
            )
        )

    def _process(self, argv, **kwargs):
        self.assertEqual(argv, [str(self.tool), "--version"])
        self.assertNotIn("NODE_OPTIONS", {name.upper() for name in kwargs["env"]})
        return subprocess.CompletedProcess(argv, self.status, self.version_output, "")

    def test_exact_pinned_version_banner_is_required_and_sufficient(self):
        for output in ("3.9.10\n", "3.9.10 \n", "3.9.10"):
            with self.subTest(output=output):
                self.version_output = output
                self.assertEqual(prettier.command(), (str(self.tool),))
        for output, status in (("3.9.11\n", 0), ("prettier 3.9.10\n", 0), ("3.9.10\n3.9.10\n", 0), ("3.9.10\n", 1)):
            with self.subTest(output=output, status=status):
                self.version_output, self.status = output, status
                with self.assertRaisesRegex(prettier.PrettierError, "3.9.10"):
                    prettier.command()
        self.managed.assert_not_called()

    def test_broken_selections_fail_instead_of_falling_back(self):
        for selected in ("relative/prettier", str(self.root / "ambient-prettier"), "/nix/store/missing/bin/prettier"):
            with self.subTest(selected=selected), mock.patch.dict(os.environ, {"AO_NIX_PRETTIER": selected}):
                with self.assertRaisesRegex(prettier.PrettierError, "AO_NIX_PRETTIER"):
                    prettier.command()
        self.process.assert_not_called()
        self.managed.assert_not_called()

    def test_unset_selection_takes_the_managed_linux_docs_ci_path(self):
        with mock.patch.dict(os.environ, {"AO_NIX_PRETTIER": ""}):
            self.managed.side_effect = None
            self.managed.return_value = ("/managed/node", "/managed/package/bin/prettier.cjs")
            self.assertEqual(prettier.command(), ("/managed/node", "/managed/package/bin/prettier.cjs"))
        self.process.assert_not_called()
        self.managed.assert_called_once()


if __name__ == "__main__":
    unittest.main()
