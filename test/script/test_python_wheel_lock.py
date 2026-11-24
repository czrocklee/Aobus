"""Check the authored wheel matrix and its generated standalone pip inputs."""

from __future__ import annotations

import copy
import hashlib
import json
import tempfile
import unittest
from pathlib import Path

from ao import generate_python_requirements as generator

_POLICY = {"python": "3.14.10", "markdown-it-py": "4.2.0", "mdurl": "0.1.2", "mypy": "2.3.1", "ruff": "0.17.4"}
_MAC = ["macos-arm64", "macos-x86_64"]
_NATIVE = [*_MAC, "windows-x86_64"]
_URL_PREFIX = "https://files.pythonhosted.org/packages/00/00/" + "0" * 60 + "/"


def _artifact(filename: str, *targets: str) -> dict[str, str | list[str]]:
    return {
        "filename": filename,
        "url": _URL_PREFIX + filename,
        "sha256": hashlib.sha256(filename.encode()).hexdigest(),
        "targets": list(targets),
    }


class PythonWheelLockTests(unittest.TestCase):
    def setUp(self) -> None:
        self.docs = _artifact("markdown_it_py-4.2.0-py3-none-any.whl", "docs")
        self.arm = _artifact("mypy-2.3.1-cp314-cp314-macosx_11_0_arm64.whl", _MAC[0])
        self.intel = _artifact("mypy-2.3.1-cp314-cp314-macosx_10_15_x86_64.whl", _MAC[1])
        self.win = _artifact("mypy-2.3.1-cp314-cp314-win_amd64.whl", _NATIVE[2])
        self.common = _artifact("support_lib-1.0-py3-none-any.whl", *_NATIVE)
        self.artifacts = [
            self.docs,
            _artifact("mdurl-0.1.2-py3-none-any.whl", "docs"),
            self.arm,
            self.intel,
            self.win,
            _artifact("ruff-0.17.4-py3-none-macosx_11_0_universal2.whl", *_MAC),
            _artifact("ruff-0.17.4-py3-none-win_amd64.whl", _NATIVE[2]),
            self.common,
        ]
        self.manifest = {"schema": 1, "artifacts": self.artifacts}

    def test_checked_in_locks_match_the_reviewed_artifacts(self) -> None:
        generator.sync_requirements(Path(generator.__file__).parent, check=True)

    def test_generated_native_lock_combines_hashes_and_includes_docs(self) -> None:
        locks = generator.render_requirements(self.manifest, _POLICY)
        docs = locks["docs-requirements.txt"].decode()
        mac = locks["macos-requirements.txt"].decode()
        win = locks["windows-requirements.txt"].decode()
        self.assertIn("markdown-it-py==4.2.0 \\\n" + f"    --hash=sha256:{self.docs['sha256']}\n", docs)
        hashes = sorted([str(self.arm["sha256"]), str(self.intel["sha256"])])
        self.assertIn("mypy==2.3.1 \\\n" + f"    --hash=sha256:{hashes[0]} \\\n    --hash=sha256:{hashes[1]}\n", mac)
        self.assertEqual(mac.count(str(self.common["sha256"])), 1)
        self.assertIn(f"    --hash=sha256:{self.win['sha256']}\n", win)
        for native in (mac, win):
            self.assertIn("\n-r docs-requirements.txt\n", native)
            self.assertNotIn("markdown-it-py==", native)
        self.artifacts.reverse()
        self.assertEqual(generator.render_requirements(self.manifest, _POLICY), locks)

    def test_rejects_wrong_host_and_unsupported_abi(self) -> None:
        for filename in (
            "mypy-2.3.1-cp314-cp314-macosx_12_0_arm64.whl",
            "mypy-2.3.1-cp313-cp313-win_amd64.whl",
            "mypy-2.3.1-cp314-cp314t-win_amd64.whl",
            "mypy-2.3.1-cp314-cp314-win_arm64.whl",
        ):
            with self.subTest(filename=filename):
                self.win.update(_artifact(filename, _NATIVE[2]))
                with self.assertRaisesRegex(ValueError, "wheel platform, architecture or ABI does not match"):
                    generator.render_requirements(self.manifest, _POLICY)
        self.win.update(_artifact("mypy-2.3.1-cp310-abi3-win_amd64.whl", _NATIVE[2]))
        generator.render_requirements(self.manifest, _POLICY)

    def test_rejects_pin_drift_duplicates_and_incomplete_targets(self) -> None:
        for description, mutate, error in (
            (
                "equivalent version with a different spelling",
                [*self.artifacts[:-1], _artifact("ruff-0.17.4.0-py3-none-any.whl", *_NATIVE)],
                r"'0\.17\.4\.0' differs from toolchain\.json pin '0\.17\.4'; use the exact wheel filename",
            ),
            (
                "normalized name collision",
                [*self.artifacts, _artifact("Support__Lib-1.0-py3-none-any.whl", *_NATIVE)],
                "normalized pin",
            ),
            ("duplicate identity", [*self.artifacts, copy.copy(self.common)], "wheel identity"),
            ("missing root", [item for item in self.artifacts if item != self.win], "missing wheels: mypy"),
            (
                "transitive pin missing from one architecture",
                [*self.artifacts[:-1], _artifact("support_lib-1.0-py3-none-any.whl", _MAC[0])],
                "package names differ",
            ),
            (
                "transitive version differs across architectures",
                [
                    *self.artifacts[:-1],
                    _artifact("support_lib-1.0-py3-none-any.whl", _MAC[0]),
                    _artifact("support_lib-2.0-py3-none-any.whl", _MAC[1]),
                ],
                r"macos-x86_64: support-lib wheel version '2\.0' differs from macos-arm64 wheel version '1\.0'",
            ),
        ):
            with self.subTest(description=description), self.assertRaisesRegex(ValueError, error):
                generator.render_requirements({"schema": 1, "artifacts": mutate}, _POLICY)

    def test_rejects_invalid_artifact_metadata(self) -> None:
        for mutation, error in (
            ({"url": str(self.docs["url"]).replace("files.pythonhosted.org", "example.invalid")}, "official PyPI URL"),
            ({"url": str(self.docs["url"]) + "?suffix=1"}, "official PyPI URL"),
            ({"sha256": "g" * 64}, "invalid SHA-256"),
            ({"sha256": "0" * 63}, "invalid SHA-256"),
            ({"targets": ["linux"]}, "invalid or duplicate target"),
            ({"targets": ["docs", "docs"]}, "invalid or duplicate target"),
            ({"targets": ["docs", _NATIVE[2]]}, "incorrect docs/native wheel group"),
        ):
            with self.subTest(mutation=mutation), self.assertRaisesRegex(ValueError, error):
                generator.render_requirements(
                    {"schema": 1, "artifacts": [self.docs | mutation, *self.artifacts[1:]]}, _POLICY
                )

    def test_accepts_post_and_release_candidate_wheel_versions(self) -> None:
        self.docs.update(_artifact("markdown_it_py-4.2.0rc1-py3-none-any.whl", "docs"))
        self.common.update(_artifact("support_lib-1.0.post1-py3-none-any.whl", *_NATIVE))
        policy = _POLICY | {"markdown-it-py": "4.2.0rc1"}

        locks = generator.render_requirements(self.manifest, policy)

        self.assertIn(b"markdown-it-py==4.2.0rc1 ", locks["docs-requirements.txt"])
        self.assertIn(b"support-lib==1.0.post1 ", locks["macos-requirements.txt"])
        self.assertIn("markdown-it-py==4.2.0rc1\n", generator.uv_root_pins(policy, native=False))

    def test_rejects_unsafe_versions_before_rendering_pip_input(self) -> None:
        for version in ("1.0.*", "1.0;os_name", "1.0 --index-url=https://example.invalid"):
            with self.subTest(version=version):
                self.docs.update(_artifact(f"markdown_it_py-{version}-py3-none-any.whl", "docs"))
                policy = _POLICY | {"markdown-it-py": version}
                with self.assertRaisesRegex(ValueError, "invalid wheel filename"):
                    generator.render_requirements(self.manifest, policy)
                with self.assertRaisesRegex(ValueError, "invalid version pin"):
                    generator.uv_root_pins(policy, native=False)

    def test_uv_input_contains_governed_roots_only(self) -> None:
        policy = _POLICY | {"support-lib": "1.0"}
        docs = "markdown-it-py==4.2.0\nmdurl==0.1.2\n"
        self.assertEqual(generator.uv_root_pins(policy, native=False), docs)
        self.assertEqual(generator.uv_root_pins(policy, native=True), docs + "mypy==2.3.1\nruff==0.17.4\n")

    def test_matches_only_one_official_filename_and_byte_digest(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            wheel = Path(directory) / str(self.docs["filename"])
            wheel.write_bytes(wheel.name.encode())  # The fixture's digest is derived from the filename.
            record = {"filename": wheel.name, "hashes": {"sha256": self.docs["sha256"]}, "url": self.docs["url"]}
            other = record | {"filename": "another.whl"}
            matched = generator.match_official_wheel(wheel, {"files": [other, record]})
            self.assertEqual(matched, {key: self.docs[key] for key in ("filename", "url", "sha256")})
            for files, reason in (
                ([other], "found 0"),
                ([record, record], "found 2"),
                ([record | {"hashes": {"sha256": "0" * 64}}], "found 0"),
                ([record | {"url": "https://example.invalid/archive.whl"}], "official PyPI URL"),
            ):
                with self.subTest(files=files), self.assertRaisesRegex(ValueError, reason):
                    generator.match_official_wheel(wheel, {"files": files})

    def test_offline_report_must_select_exactly_the_reviewed_wheels(self) -> None:
        first = '[[packages]]\nname = "mypy"\nwheels = [{url = "file:///tmp/mypy.whl"}]\n'
        second = '[[packages]]\nname = "ruff"\nwheels = [{url = "file:///tmp/ruff.whl"}]\n'
        expected = {"mypy.whl", "ruff.whl"}
        generator.verify_uv_selection(first + second, expected, "windows-x86_64")
        for report, reason in (
            (first, r"missing \['ruff\.whl'\]"),
            (first + second.replace("ruff.whl", "other.whl"), r"unexpected \['other\.whl'\]"),
            (first + second.replace('"file:///tmp/ruff.whl"', '"https://example.invalid/ruff.whl"'), "local wheel"),
            (
                first + second.replace("}]", '}, {url = "file:///tmp/ruff-other.whl"}]'),
                r"expected one wheel for ruff, found \['ruff\.whl', 'ruff-other\.whl'\]",
            ),
        ):
            with self.subTest(reason=reason), self.assertRaisesRegex(ValueError, reason):
                generator.verify_uv_selection(report, expected, "windows-x86_64")

    def test_check_rejects_stale_outputs_without_rewriting(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "toolchain.json").write_text(json.dumps(_POLICY), encoding="utf-8")
            (root / "python-wheels.json").write_text(json.dumps(self.manifest), encoding="utf-8")
            generator.sync_requirements(root, check=False)
            path = root / "macos-requirements.txt"
            path.write_bytes(b"stale")
            with self.assertRaisesRegex(ValueError, "stale generated locks: macos-requirements.txt"):
                generator.sync_requirements(root, check=True)
            self.assertEqual(path.read_bytes(), b"stale")
