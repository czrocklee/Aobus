"""Configured cohort, exact native observations and concept consumer boundaries."""

from __future__ import annotations

import argparse
import gc
import hashlib
import json
import os
import tempfile
import unittest
import weakref
from contextlib import ExitStack, contextmanager
from pathlib import Path
from unittest.mock import Mock, patch

from ao.core import bazel_concepts as concepts
from ao.core import concept_scope

CHECKSUM = "a" * 64
TOOL_CHECKSUM = "b" * 64


def _sha(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def _make_word(value: str) -> str:
    return value.replace("$", "$$").replace("#", "\\#").replace(" ", "\\ ")


def _compile(
    *,
    source="lib/Use.cpp",
    output="bazel-out/target/bin/Use.o",
    owner="//lib:consumer",
    checksum=CHECKSUM,
    tool=False,
    inputs=(),
):
    argv = [
        "clang++",
        "-Iinclude",
        "-Iapp/include",
        "-std=c++26",
        "-Werror",
        "-flto=thin",
        "-MD",
        "-MF",
        output + ".d",
        "-c",
        source,
        "-o",
        output,
    ]
    return dict(
        owner=owner,
        checksum=checksum,
        tool=tool,
        mnemonic="CppCompile",
        source=source,
        argv=argv,
        inputs=[source, *inputs],
        outputs=[output, output + ".d"],
    )


def _document(specifications):
    """Encode tiny hand-authored action fixtures, not expected measurement results."""
    document = {
        name: [] for name in ("targets", "configuration", "pathFragments", "artifacts", "depSetOfFiles", "actions")
    }
    fragments, artifacts, targets, configurations = {}, {}, {}, {}

    def artifact(path):
        if path in artifacts:
            return artifacts[path]
        parent = None
        prefix = ""
        for component in path.split("/"):
            prefix = prefix + "/" + component if prefix else component
            if prefix not in fragments:
                identifier = str(len(fragments) + 1)
                row = {"id": identifier, "label": component}
                if parent is not None:
                    row["parentId"] = parent
                fragments[prefix] = identifier
                document["pathFragments"].append(row)
            parent = fragments[prefix]
        identifier = str(len(artifacts) + 1)
        artifacts[path] = identifier
        document["artifacts"].append({"id": identifier, "pathFragmentId": parent})
        return identifier

    for specification in specifications:
        owner = specification["owner"]
        if owner not in targets:
            targets[owner] = str(len(targets) + 1)
            document["targets"].append({"id": targets[owner], "label": owner})
        checksum, tool = specification.get("checksum", CHECKSUM), specification.get("tool", False)
        if (checksum, tool) not in configurations:
            identifier = str(len(configurations) + 1)
            configurations[checksum, tool] = identifier
            document["configuration"].append({"id": identifier, "checksum": checksum, "isTool": tool})
        depset = str(len(document["depSetOfFiles"]) + 1)
        document["depSetOfFiles"].append(
            {"id": depset, "directArtifactIds": [artifact(path) for path in specification["inputs"]]}
        )
        row = {
            "targetId": targets[owner],
            "configurationId": configurations[checksum, tool],
            "mnemonic": specification["mnemonic"],
            "inputDepSetIds": [depset],
            "outputIds": [artifact(path) for path in specification["outputs"]],
            "arguments": specification.get("argv", ["fixture-tool"]),
            "actionKey": "fixture-key",
        }
        for name in ("paramFiles", "fileContents"):
            if name in specification:
                row[name] = specification[name]
        row["environmentVariables"] = [
            {"key": key, "value": value}
            for key, value in specification.get("environment", {"POLICY": "native"}).items()
        ]
        document["actions"].append(row)
    return document


def _action(argv=None, environment=()):
    specification = _compile()
    return concepts.CompileAction(
        "//lib:consumer",
        CHECKSUM,
        False,
        "CppCompile",
        "lib/Use.cpp",
        "lib/Use.cpp",
        tuple(argv or specification["argv"]),
        tuple(environment),
        frozenset(specification["inputs"]),
        tuple(specification["outputs"]),
    )


class ConfiguredConceptCohortTest(unittest.TestCase):
    def test_default_output_reachability_excludes_unused_pic_and_retains_exec_identity(self):
        native = _compile(inputs=("bazel-out/exec/bin/generator", "include/ao/Used.h"))
        pic = _compile(output="bazel-out/target/bin/Use.pic.o", inputs=("include/ao/Unused.h",))
        tool = _compile(
            source="tool/Generate.cpp",
            output="bazel-out/exec/bin/Generate.o",
            owner="//tool:generator",
            checksum=TOOL_CHECKSUM,
            tool=True,
        )
        tool_link = dict(
            owner="//tool:generator",
            checksum=TOOL_CHECKSUM,
            tool=True,
            mnemonic="CppLink",
            inputs=[tool["outputs"][0]],
            outputs=["bazel-out/exec/bin/generator"],
        )
        link = dict(
            owner="//app:product",
            mnemonic="CppLink",
            inputs=[native["outputs"][0]],
            outputs=["bazel-out/target/bin/product"],
        )
        graph = concepts.ActionGraph(_document([native, pic, tool, tool_link, link]), "linux")
        selected = graph.reachable(["bazel-out/target/bin/product"])
        self.assertEqual(
            [(action.owner, action.configuration, action.is_tool) for action in selected],
            [
                ("//lib:consumer", CHECKSUM, False),
                ("//tool:generator", TOOL_CHECKSUM, True),
            ],
        )
        self.assertEqual(selected[0].outputs, ("bazel-out/target/bin/Use.o", "bazel-out/target/bin/Use.o.d"))
        self.assertNotIn("include/ao/Unused.h", graph.required_inputs)

    def test_inline_response_expands_without_reading_native_param_file(self):
        specification = _compile()
        expanded = specification["argv"]
        specification["argv"] = ["clang++", "@native.params"]
        specification["paramFiles"] = [{"execPath": "native.params", "arguments": expanded[1:]}]
        graph = concepts.ActionGraph(_document([specification]), "linux")
        self.assertEqual(graph.reachable([specification["outputs"][0]])[0].argv, tuple(expanded))

    def test_missing_generated_producer_is_not_an_unbuilt_zero_observation(self):
        graph = concepts.ActionGraph(_document([_compile(inputs=("bazel-out/target/bin/Missing.h",))]), "linux")
        with self.assertRaisesRegex(ValueError, "no declared producer"):
            graph.reachable(["bazel-out/target/bin/Use.o"])


class NativeDependencyTransportTest(unittest.TestCase):
    def test_clang_transform_preserves_diagnostics_lto_order_and_source(self):
        original = _action()
        attempt = Path("/attempt")
        argv, outputs = concepts.dependency_command(original, "linux", attempt)
        self.assertEqual(
            argv,
            [
                "clang++",
                "-Iinclude",
                "-Iapp/include",
                "-std=c++26",
                "-Werror",
                "-flto=thin",
                "-MD",
                "-MF",
                str(attempt / "dependencies.d"),
                "-E",
                "lib/Use.cpp",
                "-o",
                str(attempt / "preprocessed.ii"),
            ],
        )
        self.assertEqual(outputs, ["bazel-out/target/bin/Use.o", "bazel-out/target/bin/Use.o.d"])
        self.assertNotIn("-w", argv)
        self.assertEqual(original.argv[-1], "bazel-out/target/bin/Use.o")

    def test_msvc_keeps_forced_include_distinct_from_preprocess_output(self):
        native = [
            "cl.exe",
            "/c",
            "lib/Use.cpp",
            "/W4",
            "/WX",
            "/GL",
            "/Z7",
            "/FIpch.h",
            "/fp:precise",
            "/Fooriginal.obj",
            "/Fd",
            "original.pdb",
            "/showIncludes",
        ]
        action = _action(
            native, (("VSLANG", "1033"), ("INCLUDE", "selected-native-sdk"), ("SYSTEMROOT", "C:/captured/Windows"))
        )
        argv, outputs = concepts.dependency_command(action, "windows", Path("/attempt"))
        self.assertIn("/FIpch.h", argv)
        self.assertIn("/fp:precise", argv)
        self.assertIn("/GL", argv)
        self.assertIn("/WX", argv)
        self.assertIn("/P", argv)
        self.assertIn("/Fi" + str(Path("/attempt") / "preprocessed.i"), argv)
        self.assertNotIn("original.obj", " ".join(argv))
        self.assertNotIn("original.pdb", argv)
        self.assertEqual(outputs, ["original.obj", "original.pdb"])
        self.assertEqual(
            action.environment,
            (("VSLANG", "1033"), ("INCLUDE", "selected-native-sdk"), ("SYSTEMROOT", "C:/captured/Windows")),
        )

    def test_windows_completion_uses_only_same_client_pinned_fields(self):
        declared = (
            ("VSLANG", "1033"),
            ("TMP", "old-repository-temp"),
            ("TEMP", "old-temp"),
            ("SystemRoot", "C:/declared/Windows"),
            ("PATH", "declared-tools"),
        )
        action = _action(environment=declared)
        client = {
            "SYSTEMROOT": "C:/client/Windows",
            "SYSTEMDRIVE": "C:",
            "TMP": "C:/client/tmp",
            "TEMP": "C:/other/tmp",
            "COMSPEC": "must-not-merge",
            "USERPROFILE": "must-not-merge",
            "PATH": "must-not-merge",
        }
        environment = concepts.observation_environment(action, "windows", client)
        self.assertEqual(
            environment,
            {
                "VSLANG": "1033",
                "TMP": "C:\\client\\tmp",
                "TEMP": "C:\\client\\tmp",
                "SystemRoot": "C:/declared/Windows",
                "SYSTEMDRIVE": "C:",
                "PATH": "declared-tools",
            },
        )
        self.assertEqual(action.environment, declared)

    def test_missing_windows_client_temp_or_system_fields_never_uses_ambient_fallback(self):
        action = _action(environment=(("VSLANG", "1033"),))
        for client in (None, {"SYSTEMROOT": "C:/Windows", "SYSTEMDRIVE": "C:"}, {"TMP": "C:/tmp"}):
            with (
                self.subTest(client=client),
                patch.dict(os.environ, {"SYSTEMROOT": "C:/ambient/Windows", "TMP": "C:/ambient/tmp"}),
                self.assertRaises(ValueError),
            ):
                concepts.observation_environment(action, "windows", client)

    def test_make_escaping_is_not_shell_quoting(self):
        text = "out\\ file.o: source.cpp path\\ with\\ space.h hash\\#name.h cash$$name.h \\\n C:\\sdk\\Header.h\n"
        target, dependencies = concepts.parse_dependencies(text)
        self.assertEqual(target, ["out file.o"])
        self.assertEqual(
            dependencies, ["source.cpp", "path with space.h", "hash#name.h", "cash$name.h", "C:\\sdk\\Header.h"]
        )

    def test_unicode_separators_inside_make_names_are_not_ascii_whitespace(self):
        target, dependencies = concepts.parse_dependencies(
            "out\u00a0dir.ii: lib/Source.cpp include/ao/No\u2028Break.h\n"
        )
        self.assertEqual(target, ["out\u00a0dir.ii"])
        self.assertEqual(dependencies, ["lib/Source.cpp", "include/ao/No\u2028Break.h"])


class NativeObservationTest(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.base = Path(temporary.name)
        self.root = self.base / "source"
        for name, contents in (
            ("lib/Use.cpp", b"#include <ao/Used.h>\n"),
            ("include/ao/Used.h", b"// used\n"),
            ("include/ao/Unused.h", b"// unused\n"),
            ("include/ao/MandatoryUnused.h", b"// declared only\n"),
            ("clang++", b"fixture compiler"),
            ("bazel-out/target/bin/Use.o", b"original-object"),
            ("bazel-out/target/bin/Use.o.d", b"original-dependencies"),
        ):
            path = self.root / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(contents)
        self.locations = concepts._Locations(self.root, self.root, {})
        self.action = _action(environment=(("ONLY_DECLARED", "kept"),))
        self.guard = concepts._Guard()
        for name in ("include/ao/Used.h", "lib/Use.cpp", "clang++"):
            self.guard.add(self.root / name)
        self.calls = []

    def process(self, argv, *, cwd, environment, stdout, stderr):
        self.assertEqual(cwd, self.root)
        self.assertEqual(environment, {"ONLY_DECLARED": "kept"})
        self.assertEqual(argv[0], "clang++")
        self.assertIn("-Werror", argv)
        self.calls.append(list(argv))
        preprocessed = Path(argv[argv.index("-o") + 1])
        dependency = Path(argv[argv.index("-MF") + 1])
        preprocessed.write_bytes(b"// preprocessed\n")
        dependency.write_text(
            _make_word(str(preprocessed)) + ": lib/Use.cpp include/ao/Used.h include/ao/Used.h\n", encoding="utf-8"
        )
        stdout.write_bytes(b"")
        stderr.write_bytes(b"")
        return 0

    def test_fresh_native_observation_excludes_declared_but_unincluded_headers(self):
        with patch.object(concepts, "_process", side_effect=self.process):
            observed = concepts._native_observation(
                self.action, "linux", self.base / "attempt", self.root, self.locations, self.guard
            )
        self.assertEqual(observed, {"include/ao/Used.h"})
        self.assertEqual(len(self.calls), 2)
        self.assertEqual((self.root / "bazel-out/target/bin/Use.o").read_bytes(), b"original-object")
        self.assertEqual((self.root / "bazel-out/target/bin/Use.o.d").read_bytes(), b"original-dependencies")
        self.guard.verify()
        for phase in ("read-inventory", "measurement"):
            receipt = json.loads((self.base / "attempt" / phase / "receipt.json").read_text(encoding="utf-8"))
            body = receipt["preprocessed"]
            self.assertFalse(body["retained"])
            self.assertEqual(body["bytes"], len(b"// preprocessed\n"))
            self.assertEqual(body["sha256"], hashlib.sha256(b"// preprocessed\n").hexdigest())
            self.assertFalse(Path(body["path"]).exists())
            self.assertTrue((self.base / "attempt" / phase / "dependencies.d").is_file())

    def test_nonzero_second_pass_does_not_publish_partial_observations(self):
        def failing(*args, **kwargs):
            status = self.process(*args, **kwargs)
            return status if len(self.calls) == 1 else 9

        with (
            patch.object(concepts, "_process", side_effect=failing),
            self.assertRaisesRegex(ValueError, "native dependency pass failed"),
        ):
            concepts._native_observation(
                self.action, "linux", self.base / "attempt", self.root, self.locations, self.guard
            )
        self.assertTrue((self.base / "attempt/read-inventory/preprocessed.ii").is_file())
        self.assertTrue((self.base / "attempt/measurement/preprocessed.ii").is_file())


class ConceptSourceAndLifecycleTest(unittest.TestCase):
    def test_location_cache_does_not_retain_an_ended_report(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            header = root / "Header.h"
            header.write_bytes(b"// observed input\n")
            locations = concepts._Locations(root, root, {})
            self.assertEqual(locations.physical(str(header)), header.resolve())
            reference = weakref.ref(locations)
            del locations
            gc.collect()
            self.assertIsNone(reference())

    def test_metadata_source_location_does_not_require_execroot_external_link(self):
        with tempfile.TemporaryDirectory() as directory:
            tree = Path(directory) / "tree"
            physical = tree / "state/output/external/+dependencies+aobus_test_deps/release/manifest.json"
            physical.parent.mkdir(parents=True)
            physical.write_bytes(b"{}")
            owner = "@@+dependencies+aobus_test_deps//release:manifest.json"
            record = {
                "owner": owner,
                "path": "external/+dependencies+aobus_test_deps/release/manifest.json",
                "source": True,
                "root": "",
            }
            attempt = object.__new__(concepts._Attempt)
            attempt.tree, attempt.guard = tree, concepts._Guard()
            attempt.invoke = Mock(return_value=f"{physical}:1:1: source file @aobus_test_deps//release:manifest.json\n")
            path = attempt.source_file(record, "@aobus_test_deps//:manifest.json")
            self.assertEqual(path, physical)
            self.assertFalse((tree / "state/output/execroot/_main/external").exists())
            attempt.invoke.assert_called_once_with("query", [json.dumps(owner), "--output=location"])

    def test_kill_failure_does_not_skip_child_wait(self):
        events = []
        child = Mock()
        waits = iter((KeyboardInterrupt(), 0))

        def wait():
            events.append("wait")
            value = next(waits)
            if isinstance(value, BaseException):
                raise value
            return value

        def kill():
            events.append("kill")
            raise OSError("kill denied")

        child.wait.side_effect, child.kill.side_effect = wait, kill
        with tempfile.TemporaryDirectory() as directory, patch.object(concepts.subprocess, "Popen", return_value=child):
            with self.assertRaises(BaseExceptionGroup) as raised:
                concepts._process(
                    ["native"],
                    cwd=Path(directory),
                    environment={},
                    stdout=Path(directory) / "out",
                    stderr=Path(directory) / "err",
                )
        self.assertEqual(events, ["wait", "kill", "wait"])
        self.assertIn("reaped", str(raised.exception))


class ConceptConsumerTest(unittest.TestCase):
    """Keep selection/metrics/validation real; fake only subprocess and portal setup I/O."""

    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.base = Path(temporary.name)
        self.platform = "linux"
        self.root, self.tree = self.base / "source", self.base / "tree"
        self.output_base = self.tree / "state/output"
        self.execution_root = self.output_base / "execroot/_main"
        self.source = "lib/Use.cpp"
        self.core, self.application = "include/ao/Used.h", "app/include/ao/Unused.h"
        self.binary = "bazel-out/target/bin/fixture"
        self.compiler_relative = "external/+dependencies+aobus_toolchain/tools/clang++"
        self.compiler = self.output_base / "external/+dependencies+aobus_toolchain/tools/clang++"
        self.compiler.parent.mkdir(parents=True)
        self.compiler.write_bytes(b"fixture compiler, never executed")
        self.resource = self.base / "resource"
        self.resource.mkdir()
        for name in (
            "MODULE.bazel",
            "REPO.bazel",
            ".bazelrc",
            ".bazelversion",
            "BUILD.bazel",
            "script/ao/bazel-toolchain.json",
            self.source,
            self.core,
            self.application,
        ):
            path = self.root / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(b"// fixture input\n")
        (self.root / self.core).write_bytes(b"namespace ao { struct A {}; }\n")
        (self.root / self.application).write_bytes(b"#include <ao/Used.h>\nnamespace ao { struct B { A value; }; }\n")
        (self.root / self.source).write_bytes(b"#include <ao/Used.h>\nint main() { return 0; }\n")
        (self.root / ".bazelversion").write_bytes(b"9.2.0\n")
        self.bazel = self.base / "bazel"
        self.bazel.write_bytes(b"fixture Bazel, never executed")
        self.metadata = dict(
            schema=1,
            platform=self.platform,
            version="22.1.8",
            clang=str(self.compiler),
            resource_dir=str(self.resource),
            system_arguments=[],
            analysis_arguments=[],
        )
        self.declarations, self.metadata_paths = {}, {}
        self.sdk_facts = dict(schema=1, platform=self.platform, config="release", tree=str(self.tree), manifests=[])
        provider_input = self.base / "provider.h"
        provider_input.write_bytes(b"// provider\n")
        for repository in ("aobus_native_deps", "aobus_test_deps"):
            canonical = "+dependencies+" + repository
            owner = "@@" + canonical + "//release:manifest.json"
            target = "@" + repository + "//:manifest.json"
            path = self.output_base / "external" / canonical / "release/manifest.json"
            path.parent.mkdir(parents=True)
            facts = {
                "generator": "aobus-bazel-artifact-adapter-v1",
                "configuration": {"platform": self.platform, "config": "Release"},
                "packages": [{"name": "fixture"}],
                "inputs": [{"path": str(provider_input), "sha256": _sha(provider_input), "roles": ["header"]}],
            }
            path.write_text(json.dumps(facts), encoding="utf-8")
            record = {"owner": owner, "path": f"external/{canonical}/release/manifest.json", "source": True}
            self.sdk_facts["manifests"].append(
                dict(target=target, path=str(path), declared_source=record, sha256=_sha(path), facts=facts)
            )
            self.declarations[target], self.metadata_paths[target] = record, path
        self._metadata_source(
            "@aobus_lint_sdk//:metadata.json", "+lint_sdk+aobus_lint_sdk", "metadata.json", self.metadata
        )
        self._metadata_source(
            "@aobus_toolchain//:identity.json",
            "+dependencies+aobus_toolchain",
            "identity.json",
            {
                "schema": 1,
                "context": {"platform": "linux", "builtin_roots": json.dumps([str(self.compiler.parent)])},
                "files": [{"path": str(self.compiler), "sha256": _sha(self.compiler)}],
            },
        )
        specification = _compile(inputs=(self.core, self.application, self.compiler_relative))
        specification["argv"][0] = self.compiler_relative
        specification["environment"] = {"ONLY_DECLARED": "native"}
        link = dict(
            owner="//test:fixture", mnemonic="CppLink", inputs=[specification["outputs"][0]], outputs=[self.binary]
        )
        self.graph = _document([specification, link])
        self.labels = ["//:check_products", "//:check_native_tests", "//:check_runs", "//test:fixture"]
        self.depth, self.native_calls = 0, []
        self.fail_native = False
        self.mutate_on_build = None
        self.chain = concept_scope.ConstructionChain("fixture", self.source, ("construct A", "construct B"))

    def _metadata_source(self, target, canonical, filename, facts):
        path = self.output_base / "external" / canonical / filename
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(json.dumps(facts), encoding="utf-8")
        self.declarations[target] = {
            "owner": "@@" + canonical + "//:" + filename,
            "path": "external/" + canonical + "/" + filename,
            "source": True,
        }
        self.metadata_paths[target] = path

    @contextmanager
    def lock(self, tree):
        self.assertEqual(tree, self.tree)
        self.depth += 1
        try:
            yield
        finally:
            self.depth -= 1

    def prepare(self, tree, config, *, prefix):
        self.assertGreater(self.depth, 0)
        self.assertEqual(config, "release")
        path = tree / "configuration"
        path.mkdir(parents=True, exist_ok=True)
        (path / "aobus.bazelrc").write_text("# frozen\n", encoding="utf-8")
        (path / "request.json").write_text(
            json.dumps({"host": self.platform, "source": str(self.root)}), encoding="utf-8"
        )
        return path

    def provider(self, label, files):
        return {"label": label, "incompatible": False, "files": files}

    def root_files(self):
        return [
            {
                "path": self.binary,
                "root": "bazel-out/target/bin",
                "source": False,
                "directory": False,
                "owner": "//test:fixture",
            }
        ]

    def process(self, argv, *, cwd, environment, stdout, stderr):
        self.assertGreater(self.depth, 0)
        if argv[0] == "BAZEL":
            action = argv[1]
            expression = next((arg for arg in argv[2:] if not arg.startswith("--")), "")
            if action == "build":
                self.build(self.bazel, self.tree, None, "build", "release", argv[2:], jobs=2, isolate_rc=True)
                stdout.write_bytes(b"fixture governed build\n")
                stderr.write_bytes(b"")
                return 0
            if action == "cquery" and expression in self.declarations:
                record = {**self.declarations[expression], "root": "", "directory": False}
                payload = json.dumps(self.provider(expression, [record])) + "\n"
            elif action == "query" and "--output=location" in argv:
                owner = json.loads(expression)
                selected = next(target for target, record in self.declarations.items() if record["owner"] == owner)
                payload = f"{self.metadata_paths[selected]}:1:1: source file {owner}\n"
            elif action == "query" and expression.startswith("buildfiles("):
                payload = '<query><source-file name="//:BUILD.bazel"/></query>'
            elif action == "cquery" and "tests(//:check_tests)" in expression:
                payload = json.dumps(self.provider("//test:fixture", self.root_files())) + "\n"
            elif action == "cquery" and "--output=jsonproto" in argv:
                payload = json.dumps(
                    {
                        "configurations": [{"id": 1, "checksum": CHECKSUM}],
                        "results": [
                            {
                                "configurationId": 1,
                                "target": {"type": "RULE", "rule": {"name": label, "ruleClass": "filegroup"}},
                            }
                            for label in self.labels
                        ],
                    }
                )
            elif action == "cquery" and expression.startswith("config(set("):
                payload = "\n".join(
                    json.dumps(
                        self.provider(
                            label, self.root_files() if label in {"//:check_products", "//test:fixture"} else []
                        )
                    )
                    for label in self.labels
                )
            elif action == "aquery" and expression.startswith("deps(set("):
                payload = json.dumps(self.graph)
            elif action == "info" and expression == "execution_root":
                payload = str(self.execution_root) + "\n"
            else:
                raise AssertionError(f"unexpected governed query: {argv}")
            stdout.write_text(payload, encoding="utf-8")
            stderr.write_bytes(b"")
            return 0
        self.assertEqual(cwd, self.execution_root)
        self.assertEqual(environment, {"ONLY_DECLARED": "native"})
        stdout.write_bytes(b"")
        stderr.write_bytes(b"")
        if "-E" in argv:
            self.assertEqual(argv[0], self.compiler_relative)
            self.assertIn(self.source, argv)
            self.assertIn("-Werror", argv)
            self.native_calls.append(list(argv))
            if self.fail_native:
                return 8
            output = Path(argv[argv.index("-o") + 1])
            dependency = Path(argv[argv.index("-MF") + 1])
            output.write_bytes(b"// fresh preprocess\n")
            dependency.write_text(
                _make_word(str(output)) + ": " + self.source + " " + self.core + "\n", encoding="utf-8"
            )
            return 0
        self.assertEqual(argv[0], str(self.compiler))
        self.assertIn("-fsyntax-only", argv)
        header = next(
            Path(arg) for arg in argv if arg in {str(self.root / self.core), str(self.root / self.application)}
        )
        application = header == self.root / self.application
        main_record = {
            "kind": "CXXRecordDecl",
            "tagUsed": "struct",
            "name": "B" if application else "A",
            "completeDefinition": True,
            "loc": {"file": str(header), "line": 1},
        }
        inner = [main_record]
        if application:
            main_record["inner"] = [
                {"kind": "FieldDecl", "name": "value", "type": {"qualType": "ao::A"}, "loc": {"file": str(header)}}
            ]
            inner.append({"kind": "CXXRecordDecl", "name": "A", "loc": {"file": str(self.root / self.core)}})
        if "-ast-dump=json" in argv:
            stdout.write_text(
                json.dumps({"kind": "NamespaceDecl", "name": "ao", "loc": {"file": str(header)}, "inner": inner}),
                encoding="utf-8",
            )
        dependency = Path(argv[argv.index("-MF") + 1])
        names = [str(header), *([str(self.root / self.core)] if application else [])]
        dependency.write_text("ast.o: " + " ".join(_make_word(name) for name in names) + "\n", encoding="utf-8")
        return 0

    def build(self, executable, tree, prepared, action, config, arguments, *, jobs, isolate_rc):
        self.assertGreater(self.depth, 0)
        self.assertEqual(action, "build")
        self.assertTrue(isolate_rc)
        self.assertIn("--remote_download_outputs=all", arguments)
        if self.mutate_on_build is not None:
            self.mutate_on_build()
        for name in (self.source, self.core, self.application):
            path = self.execution_root / name
            path.parent.mkdir(parents=True, exist_ok=True)
            os.link(self.root / name, path)
        compiler_view = self.execution_root / self.compiler_relative
        compiler_view.parent.mkdir(parents=True, exist_ok=True)
        os.link(self.compiler, compiler_view)
        for name in (self.binary, "bazel-out/target/bin/Use.o", "bazel-out/target/bin/Use.o.d"):
            path = self.execution_root / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(b"original native output")
        bep = Path(next(arg.partition("=")[2] for arg in arguments if arg.startswith("--build_event_json_file=")))
        events = [
            {
                "id": {"targetCompleted": {"label": label, "configuration": {"id": CHECKSUM}}},
                "completed": {"success": True},
            }
            for label in self.labels
        ]
        events.append({"finished": {"exitCode": {"name": "SUCCESS"}}, "lastMessage": True})
        bep.write_text("\n".join(json.dumps(event) for event in events), encoding="utf-8")
        (tree / "build.log").write_bytes(b"fixture build, no native process executed")
        return 0

    def run_consumer(self):
        args = argparse.Namespace(config="release", path=str(self.tree), jobs=2, vcpkg_prefix=None)
        with ExitStack() as stack:
            if os.name == "nt":
                # Emulate the native source-forest path resolution at the I/O
                # boundary without requiring Windows symlink privileges.
                realpath = os.path.realpath
                aliases = {
                    os.path.normcase(os.path.abspath(self.execution_root / name)): self.root / name
                    for name in (self.source, self.core, self.application)
                }

                def resolve(value, *positional, **keywords):
                    selected = aliases.get(os.path.normcase(os.path.abspath(value)), value)
                    return realpath(selected, *positional, **keywords)

                stack.enter_context(patch.object(os.path, "realpath", side_effect=resolve))
            stack.enter_context(patch.object(concepts, "build_tree_lock", self.lock))
            stack.enter_context(patch.object(concepts.native, "native_platform", return_value=self.platform))
            stack.enter_context(patch.object(concepts.native, "require_supported_source"))
            stack.enter_context(patch.object(concepts.bazel_workspace, "workspace_path", return_value=self.tree))
            stack.enter_context(patch.object(concepts.bazel_workspace, "bazel_executable", return_value=self.bazel))
            stack.enter_context(patch.object(concepts.bazel_workspace, "prepare_workspace", side_effect=self.prepare))
            stack.enter_context(patch.object(concepts.bazel_workspace, "invocation", return_value=["BAZEL"]))
            stack.enter_context(patch.object(concepts.bazel_workspace, "execute", side_effect=self.build))
            stack.enter_context(patch.object(concepts, "_process", side_effect=self.process))
            stack.enter_context(patch.object(concept_scope, "CONSTRUCTION_CHAINS", (self.chain,)))
            return concepts.write_bazel_report(args, sdk_facts=self.sdk_facts, root=self.root)

    def test_public_consumer_writes_real_complete_metric_report_and_preserves_unused_header(self):
        report = self.run_consumer()
        self.assertEqual(self.depth, 0)
        self.assertEqual(len(self.native_calls), 2)
        self.assertEqual(report["schemaVersion"], 4)
        self.assertEqual(report["primary"]["core"]["declarations"], 1)
        self.assertEqual(report["primary"]["application"]["declarations"], 1)
        self.assertEqual(report["primary"]["application"]["dependencyEdges"], 1)
        self.assertEqual(report["primary"]["constructionHops"][0]["hops"], 2)
        self.assertEqual(
            report["headerFanOut"],
            [
                {"header": self.core, "area": "core", "translationUnits": 1},
                {"header": self.application, "area": "application", "translationUnits": 0},
            ],
        )
        self.assertEqual(json.loads((self.tree / "concept-report.json").read_text(encoding="utf-8")), report)
        self.assertEqual((self.execution_root / "bazel-out/target/bin/Use.o").read_bytes(), b"original native output")

    def test_native_failure_keeps_old_report_historical_and_does_not_publish_zero_success(self):
        self.tree.mkdir(parents=True, exist_ok=True)
        previous = self.tree / "concept-report.json"
        previous.write_bytes(b"historical-report")
        self.fail_native = True
        with self.assertRaisesRegex(concepts.native.BazelWorkspaceError, "native dependency pass failed"):
            self.run_consumer()
        self.assertEqual(previous.read_bytes(), b"historical-report")
        self.assertEqual(self.depth, 0)

    def _macos_identity(self, **overrides):
        """Only the report consumer's selection contract, not a compiler fixture."""
        facts = {
            "schema": 2,
            "platform": "macos",
            "sdk": {
                "sdk": str(self.base / "MacOS.sdk"),
                "sdk_version": "26.5",
                "sdk_build_version": "25F70",
            },
        }
        facts.update(overrides)
        return facts

    def _admit_macos(self, identity=None):
        """Rebind every declared fixture selection to a macOS host before the run."""
        self.platform = "macos"
        self.metadata["platform"] = "macos"
        self._metadata_source(
            "@aobus_lint_sdk//:metadata.json", "+lint_sdk+aobus_lint_sdk", "metadata.json", self.metadata
        )
        self.sdk_facts["platform"] = "macos"
        for manifest in self.sdk_facts["manifests"]:
            manifest["facts"]["configuration"]["platform"] = "macos"
            path = Path(manifest["path"])
            path.write_text(json.dumps(manifest["facts"]), encoding="utf-8")
            manifest["sha256"] = _sha(path)
        self._metadata_source(
            "@aobus_toolchain//:identity.json",
            "+dependencies+aobus_toolchain",
            "identity.json",
            self._macos_identity() if identity is None else identity,
        )

    def test_macos_schema_two_identity_metadata_is_admitted_without_files_ledger(self):
        self._admit_macos()
        report = self.run_consumer()
        self.assertEqual(self.depth, 0)
        self.assertEqual(report["schemaVersion"], 4)
        identity = self.metadata_paths["@aobus_toolchain//:identity.json"]
        receipts = list((self.tree / "concepts").glob("report-*/receipt.json"))
        self.assertEqual(len(receipts), 1)
        receipt = json.loads(receipts[0].read_text(encoding="utf-8"))
        self.assertEqual(receipt["nativeToolchain"], {"path": str(identity), "sha256": _sha(identity)})
