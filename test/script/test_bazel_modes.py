"""Consumer compilation-mode selection over prepared Debug and Release SDKs."""

import json
import tempfile
import unittest
from pathlib import Path

from ao.core import bazel_deps as deps
from ao.core import bazel_workspace as workspace
from ao.core.bazel_resolver import configured_repository


class ConfiguredRepositoryTest(unittest.TestCase):
    """The both-variant projection consumers import with -c dbg or -c opt."""

    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix="aobus-configured-repository-")
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name).resolve()
        provider = self.root / "provider"
        include = provider / "include"
        include.mkdir(parents=True)
        (include / "fixture.h").write_text("declared header\n", encoding="utf-8")
        self.provider = provider
        self.include = include

    def render(self, config, output, plan):
        spec = {
            "schema": 1,
            "platform": "linux",
            "config": config,
            "linkage": "static",
            "provider": {"name": "fixture", "version": "reviewed"},
            "allowed_roots": [str(self.provider)],
            "packages": [{"name": "fixture", "modules": []}],
        }
        interface = {"header_dirs": [str(self.include)], "copts": []}
        adapter = deps._Adapter(spec, {"schema": 1, "patches": [workspace._manual_patch(spec, "fixture", interface)]})
        adapter.prepare()
        adapter.header_files["fixture"] = [self.include / "fixture.h"]
        manifest = adapter.build(output, plan=plan)
        (output / "manifest.json").write_text(
            json.dumps(manifest, indent=2, sort_keys=True) + "\n", encoding="utf-8", newline="\n"
        )
        return manifest

    def prepared(self):
        plans = {}
        raw = {}
        for config in ("Debug", "Release"):
            output = self.root / (config.lower() + "-repository")
            output.mkdir()
            plan: dict[Path, tuple[Path, str]] = {}
            manifest = self.render(config, output, plan)
            raw[config.lower()] = plan
            plans[config.lower()] = {
                "targets": manifest["targets"],
                "links": [
                    {"source": str(source), "destination": path.as_posix()} for path, (source, _) in plan.items()
                ],
                "files": {
                    name: (output / name).read_text(encoding="utf-8") for name in ("BUILD.bazel", "manifest.json")
                },
            }
        return plans, raw

    def test_public_targets_are_the_only_mode_selected_consumer_labels(self):
        plans, _ = self.prepared()
        self.assertEqual(
            set(plans["debug"]["targets"]),
            {"fixture", "fixture_runtime", "fixture_runtime_extra", "manifest.json"},
        )
        projection = configured_repository(plans)
        build = projection["files"]["BUILD.bazel"]
        # The mode mapping is semantic: dbg selects the prepared Debug package.
        self.assertIn('config_setting(name = "_debug", values = {"compilation_mode": "dbg"})', build)
        self.assertIn('config_setting(name = "_release", values = {"compilation_mode": "opt"})', build)
        for target in plans["debug"]["targets"]:
            self.assertIn("alias(name = " + json.dumps(target) + ", actual = select({", build)
            self.assertIn(json.dumps("//debug:" + target), build)
            self.assertIn(json.dumps("//release:" + target), build)
        self.assertIn('no_match_error = "Native SDK imports support dbg or opt; fastbuild is not supported"', build)
        self.assertNotIn("//conditions:default", build)
        # The root surface only selects; prepared rules stay in their config packages.
        self.assertEqual(build.count("alias(name = "), len(plans["debug"]["targets"]))
        for kind in ("cc_library(", "cc_import(", "filegroup("):
            self.assertNotIn(kind, build)

    def test_debug_and_release_interfaces_must_match_exactly(self):
        plans, _ = self.prepared()
        mismatched = {**plans, "release": {**plans["release"], "targets": plans["release"]["targets"][:-1]}}
        with self.assertRaisesRegex(ValueError, "different consumer interfaces"):
            configured_repository(mismatched)
        for incomplete in ({"debug": plans["debug"]}, {**plans, "profile": plans["debug"]}):
            with self.subTest(plans=set(incomplete)):
                with self.assertRaisesRegex(ValueError, "require Debug and Release interfaces"):
                    configured_repository(incomplete)

    def test_projection_prefixes_artifacts_by_configuration_and_preserves_bytes(self):
        plans, _ = self.prepared()
        projection = configured_repository(plans)
        self.assertEqual(
            set(projection["files"]),
            {"BUILD.bazel", "debug/BUILD.bazel", "debug/manifest.json", "release/BUILD.bazel", "release/manifest.json"},
        )
        for config in ("debug", "release"):
            for name in ("BUILD.bazel", "manifest.json"):
                self.assertEqual(projection["files"][config + "/" + name], plans[config]["files"][name])
            expected = [
                {"source": link["source"], "destination": config + "/" + link["destination"]}
                for link in plans[config]["links"]
            ]
            selected = [link for link in projection["links"] if link["destination"].startswith(config + "/")]
            self.assertEqual(selected, expected)
            for link in selected:
                self.assertFalse(link["destination"].startswith("/"))
                self.assertNotIn("..", link["destination"].split("/"))

    def test_header_artifacts_keep_identical_roles_in_both_modes(self):
        plans, raw = self.prepared()
        headers = {
            config: {path.as_posix() for path, (_, role) in plan.items() if role == "header"}
            for config, plan in raw.items()
        }
        self.assertTrue(headers["debug"])
        self.assertEqual(headers["debug"], headers["release"])
        projection = configured_repository(plans)
        destinations = {link["destination"] for link in projection["links"]}
        for header in headers["debug"]:
            self.assertIn("debug/" + header, destinations)
            self.assertIn("release/" + header, destinations)


if __name__ == "__main__":
    unittest.main()
