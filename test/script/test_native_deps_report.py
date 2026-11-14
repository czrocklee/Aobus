"""Focused contracts for the native SDK dependency report's manifest admission.

``_manifest_file`` must resolve the declared source through the configured
DefaultInfo capture and the exact source-file location query, never through a
guessed physical execroot link created by a build. ``_verify_manifest`` checks
literal producer/configuration identity and every recorded input digest.
Expectations here are hand-written literals and real temporary files; no SDK
payload is copied and no producer output is used as the oracle.
"""

import hashlib
import json
import tempfile
import unittest
from pathlib import Path

from ao.command import deps
from ao.core import bazel_native as native

# Independent literal canonical repository names that really satisfy the
# declared-source contract: the canonical name must end with "+" + the target
# repository name (e.g. "fixture+aobus_native_deps" for @aobus_native_deps).
NATIVE_CANONICAL = "fixture+aobus_native_deps"
TEST_CANONICAL = "fixture+aobus_test_deps"

# The exact configured DefaultInfo capture expression the report query carries,
# written here as an independent expected literal (never derived from the
# production module at test time).
MANIFEST_FILES_EXPRESSION = (
    "json.encode([{'owner': str(f.owner), 'path': f.path, 'source': f.is_source} "
    "for f in providers(target)['DefaultInfo'].files.to_list()])"
)


class ManifestFileFixture(unittest.TestCase):
    """A repository output base with the declared source materialized in place.

    No execroot or ``execroot/external`` symlink exists in these fixtures: the
    source file lives directly under the selected output base, which is exactly
    the shape an analysis-only cquery leaves behind.
    """

    CANONICAL = NATIVE_CANONICAL

    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix="aobus-manifest-file-")
        self.addCleanup(temporary.cleanup)
        self.base = Path(temporary.name).resolve()
        self.queries = []

    def source_path(self, canonical: str, config: str) -> Path:
        return self.base / "external" / canonical / config / "manifest.json"

    def write_source(self, canonical: str, config: str) -> Path:
        source = self.source_path(canonical, config)
        source.parent.mkdir(parents=True, exist_ok=True)
        source.write_text("{}", encoding="utf-8")
        return source

    def query(self, rows, lines):
        def implementation(action, arguments):
            self.queries.append((action, list(arguments)))
            if action == "cquery":
                return json.dumps(rows)
            return "\n".join(lines)

        return implementation

    def manifest_file(self, target, query, *, config="release", workers=7):
        return deps._manifest_file(target, config, workers, query)


class ManifestFileSourceResolutionTest(ManifestFileFixture):
    def test_the_declared_source_location_resolves_without_an_execroot_link(self):
        for target, canonical in (
            ("@aobus_native_deps//:manifest.json", NATIVE_CANONICAL),
            ("@aobus_test_deps//:manifest.json", TEST_CANONICAL),
        ):
            with self.subTest(target=target):
                owner = f"@@{canonical}//release:manifest.json"
                declared = f"external/{canonical}/release/manifest.json"
                source = self.write_source(canonical, "release")
                row = {"owner": owner, "path": declared, "source": True}
                query = self.query(
                    [row],
                    [f"{source}:1:1: source file {owner}"],
                )

                resolved, captured = self.manifest_file(target, query)

                self.assertEqual(resolved, source)
                # The exact configured DefaultInfo capture is returned as declared.
                self.assertEqual(captured, {"owner": owner, "path": declared, "source": True})
                self.assertFalse((self.base / "execroot").exists())

    def test_the_two_queries_carry_their_exact_arguments(self):
        source = self.write_source(self.CANONICAL, "release")
        owner = f"@@{self.CANONICAL}//release:manifest.json"
        query = self.query(
            [{"owner": owner, "path": f"external/{self.CANONICAL}/release/manifest.json", "source": True}],
            [f"{source}:1:1: source file {owner}"],
        )
        self.manifest_file("@aobus_native_deps//:manifest.json", query, workers=5)
        self.assertEqual(
            self.queries,
            [
                (
                    "cquery",
                    [
                        "@aobus_native_deps//:manifest.json",
                        "--output=starlark",
                        f"--starlark:expr={MANIFEST_FILES_EXPRESSION}",
                        "--lockfile_mode=error",
                        "--jobs=5",
                    ],
                ),
                (
                    "query",
                    [f"'{owner}'", "--output=location", "--lockfile_mode=error"],
                ),
            ],
        )


class VerifyManifestTest(unittest.TestCase):
    HOST = "linux"
    CONFIG = "release"

    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix="aobus-verify-manifest-")
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name).resolve()
        self.payload = b"native input payload"
        self.input = self.root / "native" / "input.lib"
        self.input.parent.mkdir()
        self.input.write_bytes(self.payload)
        self.digest = hashlib.sha256(self.payload).hexdigest()

    def manifest(self, **overrides):
        value = {
            "generator": "aobus-bazel-artifact-adapter-v1",
            "configuration": {"platform": self.HOST, "config": "Release"},
            "packages": [{"name": "lmdb"}, {"name": "boost"}],
            "inputs": [{"path": str(self.input), "sha256": self.digest, "roles": ["native-library", "link-interface"]}],
        }
        value.update(overrides)
        return value

    def verify(self, value, *, host=HOST, config=CONFIG, workers=4):
        return deps._verify_manifest(value, host=host, config=config, workers=workers)

    def test_a_literal_valid_manifest_with_real_input_digests_verifies(self):
        value = self.manifest()
        self.assertIs(self.verify(value), value)

    def test_verification_covers_every_input_with_a_single_worker(self):
        second = self.root / "native" / "second.dll"
        second.write_bytes(b"second payload")
        value = self.manifest(
            inputs=[
                {"path": str(self.input), "sha256": self.digest, "roles": ["native-library"]},
                {"path": str(second), "sha256": hashlib.sha256(b"second payload").hexdigest(), "roles": ["runtime"]},
            ]
        )
        self.assertIs(self.verify(value, workers=1), value)

    def test_a_changed_input_fails_with_its_exact_path(self):
        self.input.write_bytes(b"changed payload")
        with self.assertRaisesRegex(native.BazelWorkspaceError, "changed or disappeared"):
            self.verify(self.manifest())

    def test_a_missing_input_fails(self):
        self.input.unlink()
        with self.assertRaisesRegex(native.BazelWorkspaceError, "changed or disappeared"):
            self.verify(self.manifest())

    def test_the_configuration_must_match_the_actual_request(self):
        for overrides in (
            {"configuration": {"platform": "windows", "config": "Release"}},
            {"configuration": {"platform": self.HOST, "config": "Debug"}},
        ):
            with self.subTest(overrides=overrides):
                with self.assertRaisesRegex(
                    native.BazelWorkspaceError, "configuration differs from the actual request"
                ):
                    self.verify(self.manifest(**overrides))


if __name__ == "__main__":
    unittest.main()
