"""Process-level WSL cleanup regressions through production ``_wsl``.

``remote.subprocess.run`` is redirected only at the ``wsl.exe`` executable
boundary to a local Python child. It never invokes Windows ``wsl.exe``.
The native Windows distribution boundary remains unvalidated.
"""

import json
import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

from ao.core import bazel_remote_cache as remote

# Local Python stand-in. Native Windows wsl.exe is not invoked.
SHIM = r"""
import json
import os
import sys
import threading
from pathlib import Path

state_path = Path(os.environ["AOBUS_WSL_SHIM_STATE"])
state = json.loads(state_path.read_text(encoding="utf-8"))
argv = sys.argv[1:]
# Block before any journal write so a short parent timeout can kill the child first.
if state.get("unregister_timeout") and argv[:1] == ["--unregister"]:
    threading.Event().wait()

state.setdefault("commands", []).append(argv)
state_path.write_text(json.dumps(state), encoding="utf-8")

TRANSITIONAL = {"Installing", "Uninstalling", "Converting", "Exporting", "Compacting"}


def save() -> None:
    state_path.write_text(json.dumps(state), encoding="utf-8")


def emit(text: str) -> None:
    encoding = state.get("encoding", "utf-8")
    if encoding == "utf-16":
        sys.stdout.buffer.write(text.encode("utf-16"))
    elif encoding == "utf-16-le":
        sys.stdout.buffer.write(text.encode("utf-16-le"))
    elif encoding == "utf-8-sig":
        sys.stdout.buffer.write(text.encode("utf-8-sig"))
    else:
        sys.stdout.buffer.write(text.encode("utf-8"))


if argv[:2] == ["--list", "--quiet"] and state.get("list_bytes") is not None:
    sys.stdout.buffer.write(bytes.fromhex(state["list_bytes"]))
    raise SystemExit(int(state.get("list_code", 0)))

if argv[:1] == ["--unregister"] and len(argv) == 2:
    name = argv[1]
    if not state.get("sticky"):
        registered = state.get("registered", [])
        kept = [item for item in registered if item["name"].casefold() != name.casefold()]
        removed = len(kept) != len(registered)
        state["registered"] = kept
        save()
    else:
        removed = False
    code = state.get("unregister_code")
    raise SystemExit(int(code) if code is not None else (0 if removed else 1))

if argv in (["--list", "--quiet"], ["--list", "--quiet", "--all"]):
    include_all = argv == ["--list", "--quiet", "--all"]
    names = [
        item["name"]
        for item in state.get("registered", [])
        if include_all or item["state"] not in TRANSITIONAL
    ]
    if names:
        emit("".join(f"{name}\r\n" for name in names))
    raise SystemExit(0)

if argv[:1] == ["--import"] and len(argv) >= 4:
    disk = Path(argv[2])
    disk.mkdir(parents=True, exist_ok=True)
    (disk / "ext4.vhdx").write_bytes(b"partial-import")
    state.setdefault("registered", []).append({"name": argv[1], "state": state.get("import_state", "Installing")})
    save()
    code = int(state.get("import_code", 0))
    if code:
        sys.stdout.buffer.write(b"import failed\n")
        raise SystemExit(code)
    raise SystemExit(0)

sys.stderr.write("unexpected wsl shim command\n")
raise SystemExit(99)
"""

_HOST_ENV_KEYS = ("SystemRoot", "WINDIR", "TEMP", "TMP")


class WslProcessBoundaryTest(unittest.TestCase):
    """Real ``_wsl`` subprocess boundary. Native Windows ``wsl.exe`` stays unvalidated."""

    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix="wsl shim ")
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        self.shim = self.root / "wsl_shim.py"
        self.shim.write_text(SHIM, encoding="utf-8")
        self.shim_state = self.root / "shim-state.json"
        self.bundle = self.root / "git" / "ca-bundle.crt"
        self.bundle.parent.mkdir()
        self.bundle.write_text("trust", encoding="utf-8")
        self.host = {key: os.environ[key] for key in _HOST_ENV_KEYS if key in os.environ}
        self.invocations: list[list[str]] = []
        self.real_run = subprocess.run
        patcher = mock.patch.object(remote.subprocess, "run", side_effect=self.redirect_wsl)
        patcher.start()
        self.addCleanup(patcher.stop)
        self.write_state()

    def redirect_wsl(self, argv, **kwargs):
        self.assertEqual(argv[0], "wsl.exe")
        self.invocations.append(list(argv))
        return self.real_run([sys.executable, "-I", "-B", str(self.shim), *argv[1:]], **kwargs)

    def write_state(self, **overrides) -> None:
        self.invocations.clear()
        state = {
            "registered": [],
            "encoding": "utf-8",
            "sticky": False,
            "unregister_code": None,
            "import_code": 0,
            "import_state": "Installing",
            "unregister_timeout": False,
            "list_bytes": None,
            "commands": [],
        }
        state.update(overrides)
        self.shim_state.write_text(json.dumps(state), encoding="utf-8")

    @property
    def environ(self) -> dict[str, str]:
        return {**self.host, "AOBUS_WSL_SHIM_STATE": str(self.shim_state)}

    def commands(self) -> list[list[str]]:
        return json.loads(self.shim_state.read_text(encoding="utf-8"))["commands"]

    def start_in_wsl(self):
        with (
            mock.patch.object(
                remote,
                "install",
                side_effect=lambda selected, directory, name="bazel-remote": directory / name,
            ),
            mock.patch.object(remote, "_git_ca_bundle", return_value=self.bundle),
        ):
            return remote._start_in_wsl(
                self.root,
                self.root / "bazel-remote",
                {"BAZEL_REMOTE_S3_SECRET_ACCESS_KEY": "secret-key"},
                upload=False,
                key_prefix="validation.ci/42",
                environ={**self.environ, "BAZEL_REMOTE_S3_SECRET_ACCESS_KEY": "secret-key"},
            )

    def test_encoded_absence_accepts_a_nonzero_unregister(self):
        self.write_state(
            encoding="utf-16",
            unregister_code=1,
            registered=[{"name": "Ubuntu", "state": "Running"}],
        )
        remote._remove_wsl(self.environ)
        self.assertEqual(
            self.invocations,
            [
                ["wsl.exe", "--unregister", remote.WSL_DISTRO],
                ["wsl.exe", "--list", "--quiet", "--all"],
            ],
        )
        self.assertEqual(
            self.commands(),
            [["--unregister", remote.WSL_DISTRO], ["--list", "--quiet", "--all"]],
        )

    def test_encoded_names_still_count_as_registered(self):
        samples = (
            ("utf-16-le", "AOBUS-BAZEL-REMOTE"),
            ("utf-8-sig", remote.WSL_DISTRO),
        )
        for encoding, name in samples:
            with self.subTest(encoding=encoding):
                self.write_state(
                    encoding=encoding,
                    sticky=True,
                    unregister_code=0,
                    registered=[{"name": name, "state": "Running"}],
                )
                with self.assertRaisesRegex(remote.RemoteCacheError, "remains registered after unregister"):
                    remote._remove_wsl(self.environ)
                self.assertIn(["wsl.exe", "--list", "--quiet", "--all"], self.invocations)
                self.assertIn(["--list", "--quiet", "--all"], self.commands())

    def test_transitional_registration_is_hidden_without_all_but_not_from_production(self):
        self.write_state(
            encoding="utf-16",
            sticky=True,
            registered=[{"name": remote.WSL_DISTRO, "state": "Installing"}],
        )
        hidden = self.real_run(
            [sys.executable, "-I", "-B", str(self.shim), "--list", "--quiet"],
            env=self.environ,
            capture_output=True,
            check=False,
        )
        self.assertEqual((hidden.returncode, hidden.stdout), (0, b""))
        with self.assertRaisesRegex(remote.RemoteCacheError, "remains registered"):
            remote._remove_wsl(self.environ)
        self.assertIn(["wsl.exe", "--list", "--quiet", "--all"], self.invocations)
        self.assertIn(["--list", "--quiet", "--all"], self.commands())

    def test_partial_import_rollback_removes_the_marker_and_keeps_the_original_error(self):
        self.write_state(import_code=1, encoding="utf-8-sig")
        with self.assertRaisesRegex(remote.RemoteCacheError, "WSL import failed with exit code 1"):
            self.start_in_wsl()
        self.assertFalse((self.root / remote.SERVER_NAME).exists())
        self.assertEqual((self.root / "wsl" / "ext4.vhdx").read_bytes(), b"partial-import")
        self.assertEqual(json.loads(self.shim_state.read_text(encoding="utf-8"))["registered"], [])

    def test_unregister_timeout_preserves_the_disk_sentinel(self):
        disk = self.root / "wsl"
        disk.mkdir()
        sentinel = disk / "sentinel"
        sentinel.write_bytes(b"keep")
        self.write_state(unregister_timeout=True)
        with (
            mock.patch.object(remote, "_WSL_SECONDS", 0.3),
            self.assertRaisesRegex(remote.RemoteCacheError, "unregister did not finish"),
        ):
            self.start_in_wsl()
        self.assertEqual(sentinel.read_bytes(), b"keep")
        self.assertFalse((self.root / remote.SERVER_NAME).exists())
        self.assertEqual(self.invocations, [["wsl.exe", "--unregister", remote.WSL_DISTRO]])
        self.assertNotIn("--import", [argument for command in self.invocations for argument in command])


if __name__ == "__main__":
    unittest.main()
