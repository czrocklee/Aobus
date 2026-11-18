"""Configured concept cohorts and the reports built from compiler observations."""

from __future__ import annotations

import hashlib
import json
import os
import tempfile
import unittest
from contextlib import ExitStack
from pathlib import Path
from unittest.mock import Mock, patch

from ao.policy import concept_scope
from ao.portal import bazel_concepts as concepts
from ao.portal.dependency_checks import Request

CHECKSUM = "a" * 64


def _sha(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def _write(root: Path, name: str, text: str) -> Path:
    path = root / name
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text, encoding="utf-8")
    return path


def _make_word(value: str) -> str:
    return value.replace("$", "$$").replace("#", "\\#").replace(" ", "\\ ")


def _action(owner, mnemonic, inputs, outputs, **fields):
    return dict(owner=owner, mnemonic=mnemonic, inputs=inputs, outputs=outputs, **fields)


def _compile(*, source="lib/Use.cpp", output="bazel-out/target/bin/Use.o", owner="//lib:consumer", inputs=(), **fields):
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
    return _action(owner, "CppCompile", [source, *inputs], [output, output + ".d"], **({"arguments": argv} | fields))


def _document(actions, *, tree_artifacts=()):
    """Encode small hand-authored actions in Bazel's aquery transport."""
    document = {
        name: [] for name in ("targets", "configuration", "pathFragments", "artifacts", "depSetOfFiles", "actions")
    }
    paths, fragments, targets, configurations = {}, {}, {}, {}

    def add(table, **row):
        identifier = len(document[table]) + 1
        document[table].append(dict(id=identifier, **row))
        return identifier

    def fragment(path):
        if path not in fragments:
            parent, _, name = path.rpartition("/")
            fragments[path] = add("pathFragments", label=name, parentId=fragment(parent) if parent else 0)
        return fragments[path]

    def artifact(path):
        if path not in paths:
            paths[path] = add("artifacts", pathFragmentId=fragment(path), isTreeArtifact=path in tree_artifacts)
        return paths[path]

    for specification in actions:
        row = dict(specification)
        owner = row.pop("owner")
        tool = row.pop("tool", False)
        if owner not in targets:
            targets[owner] = add("targets", label=owner)
        if tool not in configurations:
            configurations[tool] = add("configuration", checksum="b" * 64 if tool else CHECKSUM, isTool=tool)
        depset = add("depSetOfFiles", directArtifactIds=[artifact(path) for path in row.pop("inputs")])
        outputs = [artifact(path) for path in row.pop("outputs")]
        document["actions"].append(
            dict(
                targetId=targets[owner],
                configurationId=configurations[tool],
                inputDepSetIds=[depset],
                outputIds=outputs,
                **row,
            )
        )
    return document


def _standalone_compile(argv=None, *, environment=()):
    action = _compile()
    return concepts.CompileAction(
        "//lib:consumer",
        CHECKSUM,
        False,
        "CppCompile",
        "lib/Use.cpp",
        "lib/Use.cpp",
        tuple(argv if argv is not None else action["arguments"]),
        tuple(environment),
        frozenset(action["inputs"]),
        tuple(action["outputs"]),
    )


class ConfiguredConceptCohortTest(unittest.TestCase):
    def test_default_output_reaches_only_the_linked_variant_and_retains_tool_identity(self):
        builder = _compile(
            source="tool/Generate.cpp", output="bazel-out/exec/Generate.o", owner="//tool:generator", tool=True
        )
        native = _compile(inputs=(builder["outputs"][0],))
        pic = _compile(output="bazel-out/target/bin/Use.pic.o", inputs=("include/ao/Unused.h",))
        link = _action("//app:product", "CppLink", native["outputs"][:1], ["bazel-out/target/bin/product"])
        graph = concepts.ActionGraph(_document([native, pic, builder, link]), "linux")
        selected = graph.reachable(link["outputs"])
        self.assertEqual(
            [(action.outputs[0], action.is_tool) for action in selected],
            [(native["outputs"][0], False), (builder["outputs"][0], True)],
        )
        self.assertNotIn("include/ao/Unused.h", graph.required_inputs)

    def test_shared_python_bytecode_does_not_drop_the_native_consumer(self):
        # rules_python legitimately emits identical precompilation actions under two owners.
        native = _compile()
        bytecode = _action(
            "//py:first",
            "PyCompile",
            native["outputs"][:1],
            ["bazel-out/target/bin/shared.pyc"],
            actionKey="c" * 64,
            executionPlatform="@@platforms//host:host",
        )
        graph = concepts.ActionGraph(_document([native, bytecode, {**bytecode, "owner": "//py:second"}]), "linux")
        self.assertEqual([action.source for action in graph.reachable(bytecode["outputs"])], ["lib/Use.cpp"])

    def test_tree_child_reaches_the_declared_tree_producer(self):
        native = _compile()
        tree = "bazel-out/target/bin/generated"
        generate = _action("//lib:tree", "GenerateTree", native["outputs"][:1], [tree])
        graph = concepts.ActionGraph(_document([native, generate], tree_artifacts=(tree,)), "linux")
        self.assertEqual([action.source for action in graph.reachable([tree + "/nested/config.h"])], ["lib/Use.cpp"])

    def test_inline_response_arguments_recover_the_compile_source(self):
        action = _compile()
        arguments = action["arguments"]
        action["arguments"] = ["clang++", "@native.params"]
        action["paramFiles"] = [{"execPath": "native.params", "arguments": arguments[1:]}]
        graph = concepts.ActionGraph(_document([action]), "linux")
        self.assertEqual(graph.reachable(action["outputs"])[0].argv, tuple(arguments))


class ConceptGraphQueryTest(unittest.TestCase):
    def test_windows_opaque_response_uses_the_scoped_compile_context_write(self):
        # Bazel 9.2 fails when the broad query asks for all FileWrite bytes.
        native = _compile(arguments=["cl.exe", "@opaque.params"])
        expanded = ["cl.exe", "/Iinclude", "/c", "lib/Use.cpp", "/Fo" + native["outputs"][0]]
        context = _action(
            native["owner"], "FileWrite", [], ["bazel-out/target/bin/consumer.aobus-compile-context.json"]
        )
        write = {
            **context,
            "fileContents": json.dumps(
                {"quote_includes": ["include"], "compile_argv": [{"outputs": native["outputs"], "argv": expanded}]}
            ),
        }
        expression = 'deps(set("//lib:consumer"))'

        def query(action, arguments):
            self.assertEqual(action, "aquery")
            if arguments[0] == expression:
                self.assertNotIn("--include_file_write_contents", arguments)
                return json.dumps(_document([native, context]))
            self.assertEqual(arguments[0], f'mnemonic("FileWrite", {expression})')
            self.assertIn("--include_file_write_contents", arguments)
            return json.dumps(_document([write]))

        attempt = object.__new__(concepts._Attempt)
        attempt.invoke = query
        graph = attempt.graph(["//lib:consumer"], "windows")
        self.assertEqual(graph.reachable(native["outputs"])[0].argv, tuple(expanded))


class NativeDependencyTransportTest(unittest.TestCase):
    def test_native_gcc_redirects_preprocessing_without_changing_compile_semantics(self):
        output = "bazel-out/target/bin/Use.o"
        native = _standalone_compile(
            [
                "g++",
                "-Iinclude",
                "-Werror",
                "-flto=auto",
                "-fno-canonical-system-headers",
                "-MD",
                "-MF",
                output + ".d",
                "-c",
                "lib/Use.cpp",
                "-o",
                output,
            ]
        )
        directory = Path("/attempt space$hash#")
        argv, outputs = concepts.dependency_command(native, "linux", directory)
        preprocessed = str(directory / "preprocessed.ii")
        self.assertEqual(argv[0], "g++")
        self.assertIn("-E", argv)
        self.assertNotIn("-c", argv)
        self.assertIn("-Werror", argv)
        self.assertIn("-flto=auto", argv)
        self.assertIn("-fno-canonical-system-headers", argv)
        self.assertEqual(argv[argv.index("-o") + 1], preprocessed)
        self.assertEqual(argv[argv.index("-MQ") + 1], preprocessed)
        self.assertEqual(argv[argv.index("-MF") + 1], str(directory / "dependencies.d"))
        self.assertEqual(outputs, [output, output + ".d"])

    def test_clang_ast_keeps_gcc_context_defines_but_not_gcc_only_flags(self):
        original = _compile()["arguments"]
        native = _standalone_compile(
            [
                "g++",
                "-DBOOST_PFR_USE_CPP26=0",
                "-fno-canonical-system-headers",
                "-fno-fat-lto-objects",
                *[arg for arg in original[1:] if arg != "-flto=thin"],
                "-flto=auto",
            ]
        )
        metadata = {
            "clang": "/selected/sdk/clang++",
            "system_arguments": ["-isystem/nix/store/include"],
            "analysis_arguments": [],
        }
        header, depfile = Path("/main/Used.h"), Path("/work/ast.d")
        argv = concepts._ast_command(native, "linux", metadata, header, depfile)
        self.assertEqual(argv[0], metadata["clang"])
        self.assertIn("-DBOOST_PFR_USE_CPP26=0", argv)
        self.assertIn("-Iinclude", argv)
        self.assertIn("-isystem/nix/store/include", argv)
        self.assertTrue(
            {"-flto=auto", "-fno-fat-lto-objects", "-fno-canonical-system-headers", "lib/Use.cpp"}.isdisjoint(argv)
        )
        self.assertEqual(argv[-1], str(header))

    def test_windows_preprocessing_keeps_the_forced_include_not_the_object_output(self):
        native = _standalone_compile(
            [
                "cl.exe",
                "/c",
                "lib/Use.cpp",
                "/WX",
                "/GL",
                "/FIpch.h",
                "/Fooriginal.obj",
                "/Fd",
                "original.pdb",
                "/showIncludes",
            ],
            environment=(("VSLANG", "1033"),),
        )
        root = Path("/attempt")
        argv, outputs = concepts.dependency_command(native, "windows", root)
        self.assertIn("/FIpch.h", argv)
        self.assertIn("/Fi" + str(root / "preprocessed.i"), argv)
        self.assertNotIn("/Fooriginal.obj", argv)
        self.assertNotIn("original.pdb", argv)
        self.assertEqual(outputs, ["original.obj", "original.pdb"])

    def test_clang_cl_ast_addresses_a_header_with_spaces_as_one_argument(self):
        native = _standalone_compile(
            ["cl.exe", "/c", "lib/Use.cpp", "/WX", "/FIpch.h", "/Fooriginal.obj", "/showIncludes"],
            environment=(("VSLANG", "1033"),),
        )
        metadata = {"clang": "clang-cl", "system_arguments": [], "analysis_arguments": []}
        header, depfile = Path("main header.h").absolute(), Path("work/ast.d").absolute()
        argv = concepts._ast_command(native, "windows", metadata, header, depfile)
        self.assertEqual(argv[argv.index("/clang:-x") + 1], "/clang:c++-header")
        self.assertEqual(argv[-4:], ["/clang:-MD", "/clang:-MF", "/clang:" + str(depfile), "/clang:" + str(header)])

    def test_windows_observation_uses_captured_temp_and_keeps_declared_tools(self):
        action = _standalone_compile(
            environment=(
                ("TMP", "old-repository-temp"),
                ("SystemRoot", "C:/declared/Windows"),
                ("PATH", "declared-tools"),
            )
        )
        client = {
            "SYSTEMROOT": "C:/client/Windows",
            "SYSTEMDRIVE": "C:",
            "TMP": "C:/client/tmp",
            "TEMP": "C:/other/tmp",
            "USERPROFILE": "must-not-merge",
            "PATH": "must-not-merge",
        }
        self.assertEqual(
            concepts.observation_environment(action, "windows", client),
            {
                "TMP": "C:\\client\\tmp",
                "TEMP": "C:\\client\\tmp",
                "SystemRoot": "C:/declared/Windows",
                "SYSTEMDRIVE": "C:",
                "PATH": "declared-tools",
            },
        )

    def test_make_depfile_preserves_make_escapes_and_unicode_in_paths(self):
        text = (
            "out\\ file.o: lib/Source.cpp path\\ with\\ space.h hash\\#name.h cash$$name.h \\\n"
            " include/ao/No\u2028Break.h\n"
        )
        target, dependencies = concepts.parse_dependencies(text)
        self.assertEqual(target, ["out file.o"])
        self.assertEqual(
            dependencies,
            ["lib/Source.cpp", "path with space.h", "hash#name.h", "cash$name.h", "include/ao/No\u2028Break.h"],
        )


class ConceptConsumerTest(unittest.TestCase):
    """Run the real cohort, dependency, AST and report consumers over a tiny graph."""

    source = "lib/Use.cpp"
    core = "include/ao/Used.h"
    application = "app/include/ao/Unused.h"
    script = "script/smoke.py"
    wrapper = "bazel-out/target/bin/wrapper"
    inner = "bazel-out/target/bin/fixture.native"
    compiler_relative = "external/+dependencies+aobus_toolchain/clang/tools/clang++"
    test_label = "//test:fixture"

    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.base = Path(temporary.name).resolve()
        self.root, self.tree = self.base / "source", self.base / "tree"
        self.output_base = self.tree / "state/output"
        self.execution_root = self.output_base / "execroot/_main"
        self.compiler = _write(self.output_base, self.compiler_relative, "fixture compiler")
        self.resource = self.base / "resource"
        (self.resource / "include").mkdir(parents=True)
        for path in ("MODULE.bazel", "REPO.bazel", ".bazelrc", "script/ao/bazel-toolchain.json"):
            _write(self.root, path, "# fixture\n")
        _write(self.root, ".bazelversion", "9.2.0\n")
        _write(self.root, self.core, "namespace ao { struct A {}; }\n")
        _write(self.root, self.application, "#include <ao/Used.h>\nnamespace ao { struct B { A value; }; }\n")
        _write(self.root, self.source, "#include <ao/Used.h>\nint main() { return 0; }\n")
        _write(self.root, self.script, "# wrapper's direct source, not a compiler input\n")
        self.bazel = _write(self.base, "bazel", "fixture Bazel")
        self.prepared = self.base / "prepared"
        _write(self.prepared, "aobus.bazelrc", "# fixture\n")
        _write(self.prepared, "request.json", json.dumps({"host": "linux", "source": str(self.root)}))
        self.metadata = {}
        self.ast_sdk_dependency = None
        manifests = []
        provider = _write(self.base, "provider.h", "// provider\n")
        for repository in ("aobus_native_deps", "aobus_test_deps"):
            facts = {
                "generator": "aobus-bazel-artifact-adapter-v1",
                "configuration": {"platform": "linux", "config": "Debug"},
                "inputs": [{"path": str(provider), "sha256": _sha(provider), "roles": ["header"]}],
            }
            target = f"@{repository}//:manifest.json"
            record, path = self.add_metadata(target, "+dependencies+" + repository, "debug:manifest.json", facts)
            declaration = {key: record[key] for key in ("owner", "path", "source")}
            manifests.append(
                dict(target=target, path=str(path), declared_source=declaration, sha256=_sha(path), facts=facts)
            )
        self.sdk = dict(schema=1, platform="linux", config="debug", tree=str(self.tree), manifests=manifests)
        self.add_metadata(
            "@aobus_toolchain//clang:identity.json",
            "+dependencies+aobus_toolchain",
            "clang:identity.json",
            {
                "schema": 1,
                "context": {
                    "platform": "linux",
                    "compiler": "clang",
                    "version": "22.1.8",
                    "builtin_roots": json.dumps([str(self.compiler.parent)]),
                },
                "files": [{"path": str(self.compiler), "sha256": _sha(self.compiler)}],
            },
        )
        self.set_ast_metadata([])
        self.compile = _compile(inputs=(self.core, self.application, self.compiler_relative))
        self.compile["arguments"][0] = self.compiler_relative
        self.compile["environmentVariables"] = [{"key": "ONLY_DECLARED", "value": "native"}]
        self.graph = _document(
            [
                self.compile,
                _action(self.test_label, "CppLink", self.compile["outputs"][:1], [self.inner]),
                _action(self.test_label, "TemplateExpand", [], [self.wrapper]),
            ]
        )
        self.labels = [*concepts._ROOTS, self.test_label]
        self.fail_native = False

    def add_metadata(self, target, canonical, file_label, facts):
        relative = f"external/{canonical}/" + file_label.replace(":", "/").lstrip("/")
        path = _write(self.output_base, relative, json.dumps(facts))
        record = dict(owner=f"@@{canonical}//{file_label}", path=relative, source=True, root="", directory=False)
        self.metadata[target] = record, path
        return record, path

    def set_ast_metadata(self, system_arguments):
        self.add_metadata(
            "@aobus_lint_sdk//:metadata.json",
            "+lint_sdk+aobus_lint_sdk",
            ":metadata.json",
            dict(
                schema=1,
                platform="linux",
                version="22.1.8",
                clang=str(self.compiler),
                resource_dir=str(self.resource),
                system_arguments=system_arguments,
                analysis_arguments=[],
            ),
        )

    @staticmethod
    def provider(label, files):
        return json.dumps(dict(label=label, incompatible=False, files=files))

    def bazel_result(self, action, arguments):
        expression = arguments[0]
        labels = " ".join(json.dumps(label) for label in self.labels)
        compatible = " ".join(json.dumps(label) for label in sorted(self.labels))
        if action == "cquery" and "--output=starlark" in arguments:
            if expression in self.metadata:
                return self.provider(expression, [self.metadata[expression][0]])
            files = [
                dict(path=name, directory=False, root="bazel-out/target/bin", source=False, owner=self.test_label)
                for name in (self.wrapper, self.inner)
            ] + [dict(path=self.script, directory=False, root="", source=True, owner="@@//:" + self.script)]
            if expression == "config(tests(//:check_tests), target)":
                return self.provider(self.test_label, files)
            if expression == f"config(set({labels}), target)":
                return "\n".join(
                    self.provider(label, files if label == self.test_label else []) for label in self.labels
                )
        if action == "cquery" and arguments == [f"config(set({labels}), target)", "--output=jsonproto"]:
            return json.dumps(
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
        if action == "query" and arguments[1:] == ["--output=location"]:
            owner = json.loads(expression)
            candidates = [path for record, path in self.metadata.values() if record["owner"] == owner]
            self.assertEqual(len(candidates), 1)
            return f"{candidates[0]}:1:1: source file {owner}\n"
        if action == "query" and arguments == [
            f"buildfiles(deps(set({' '.join(sorted(self.labels))})))",
            "--output=xml",
        ]:
            return "<query/>"
        if action == "aquery" and expression == f"deps(set({compatible}))":
            return json.dumps(self.graph)
        if action == "info" and arguments == ["execution_root"]:
            return str(self.execution_root)
        if action == "build":
            self.assertEqual(arguments[: len(self.labels)], sorted(self.labels))
            self.materialize(arguments)
            return ""
        raise AssertionError(f"unexpected Bazel command: {action} {arguments}")

    def materialize(self, arguments):
        for name in (self.source, self.core, self.application, self.script, self.compiler_relative):
            original = self.compiler if name == self.compiler_relative else self.root / name
            alias = self.execution_root / name
            alias.parent.mkdir(parents=True, exist_ok=True)
            os.link(original, alias)
        for name in (self.wrapper, self.inner, *self.compile["outputs"]):
            _write(self.execution_root, name, "original native output")
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

    def process(self, argv, *, cwd, environment, stdout, stderr):
        stdout.write_bytes(b"")
        stderr.write_bytes(b"")
        if argv[0] == str(self.bazel):
            action, *arguments = argv[1:]
            stdout.write_text(
                self.bazel_result(action, [value for value in arguments if not value.startswith("--jobs=")]),
                encoding="utf-8",
            )
            return 0
        self.assertEqual(cwd, self.execution_root)
        self.assertEqual(environment, {"ONLY_DECLARED": "native"})
        if "-E" in argv:
            self.assertEqual(argv[0], self.compiler_relative)
            self.assertEqual(argv[argv.index("-E") + 1], self.source)
            output = Path(argv[argv.index("-o") + 1])
            output.write_bytes(b"// fresh preprocess\n")
            dependency_text = f"{_make_word(str(output))}: {self.source} {self.core}\n"
        else:
            self.assertEqual(argv[0], str(self.compiler))
            self.assertIn("-fsyntax-only", argv)
            header = Path(argv[-1])
            self.assertIn(header, (self.root / self.core, self.root / self.application))
            application = header == self.root / self.application
            record = {
                "kind": "CXXRecordDecl",
                "tagUsed": "struct",
                "name": "B" if application else "A",
                "completeDefinition": True,
                "loc": {"file": str(header), "line": 1},
            }
            declarations = [record]
            names = [str(header)]
            if application:
                record["inner"] = [
                    {"kind": "FieldDecl", "name": "value", "type": {"qualType": "ao::A"}, "loc": {"file": str(header)}}
                ]
                core = str(self.root / self.core)
                declarations.append({"kind": "CXXRecordDecl", "name": "A", "loc": {"file": core}})
                names.append(core)
            if self.ast_sdk_dependency is not None:
                self.assertIn(f"-isystem{self.resource / 'include'}", argv)
                sdk = str(self.ast_sdk_dependency)
                declarations.append({"kind": "TypedefDecl", "name": "int32_t", "loc": {"file": sdk}})
                names.append(sdk)
            if "-ast-dump=json" in argv:
                stdout.write_text(
                    json.dumps(
                        {"kind": "NamespaceDecl", "name": "ao", "loc": {"file": str(header)}, "inner": declarations}
                    ),
                    encoding="utf-8",
                )
            dependency_text = "ast.o: " + " ".join(_make_word(name) for name in names) + "\n"
        Path(argv[argv.index("-MF") + 1]).write_text(dependency_text, encoding="utf-8")
        return 8 if self.fail_native and "-E" in argv and stdout.parent.name == "measurement" else 0

    def run_consumer(self):
        with ExitStack() as stack:
            if os.name == "nt":
                # Model the Windows source forest's canonical names without symlink privilege.
                realpath = os.path.realpath
                aliases = {
                    os.path.normcase(str(self.execution_root / name)): self.root / name
                    for name in (self.source, self.core, self.application, self.script)
                }

                def resolve(path, *args, **kwargs):
                    return realpath(aliases.get(os.path.normcase(os.path.abspath(path)), path), *args, **kwargs)

                stack.enter_context(patch.object(os.path, "realpath", side_effect=resolve))
            stack.enter_context(patch.object(concepts.native, "native_platform", return_value="linux"))
            stack.enter_context(patch.object(concepts.native, "require_supported_source"))
            stack.enter_context(patch.object(concepts.bazel_workspace, "bazel_executable", return_value=self.bazel))
            stack.enter_context(patch.object(concepts.bazel_workspace, "prepare_workspace", return_value=self.prepared))
            stack.enter_context(patch.object(concepts.bazel_workspace, "invocation", return_value=[str(self.bazel)]))
            stack.enter_context(patch.object(concepts.bazel_workflows, "selected_compiler", return_value="clang"))
            stack.enter_context(patch.object(concepts, "_process", side_effect=self.process))
            chain = concept_scope.ConstructionChain("fixture", self.source, ("construct A", "construct B"))
            stack.enter_context(patch.object(concept_scope, "CONSTRUCTION_CHAINS", (chain,)))
            return concepts.write_bazel_report(
                Request(config="debug", path=str(self.tree), jobs=1), sdk_facts=self.sdk, root=self.root
            )

    def test_wrapped_cpp_output_counts_actual_includes_and_retains_the_unused_public_header(self):
        report = self.run_consumer()
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
        self.assertEqual(
            (self.execution_root / self.compile["outputs"][0]).read_text(encoding="utf-8"), "original native output"
        )

    @unittest.skipIf(os.name == "nt", "Nix resource directory links are a POSIX SDK layout")
    def test_clang_resource_include_symlink_does_not_become_project_include_weight(self):
        include = self.resource / "include"
        sdk = _write(self.base, "store/" + "b" * 32 + "-clang-lib/include/stdint.h", "// Clang SDK header\n")
        include.rmdir()
        include.symlink_to(sdk.parent, target_is_directory=True)
        self.set_ast_metadata([f"-isystem{include}"])
        self.ast_sdk_dependency = include / sdk.name
        with patch.object(concepts, "_NIX_STORE", self.base / "store"):
            report = self.run_consumer()
        self.assertEqual(report["primary"]["core"]["includeWeight"]["medianHeaders"], 1)
        self.assertEqual(report["primary"]["application"]["includeWeight"]["medianHeaders"], 2)

    def test_failed_compiler_observation_preserves_the_last_report(self):
        previous = _write(self.tree, "concept-report.json", "historical-report")
        self.fail_native = True
        with self.assertRaises(concepts.native.BazelWorkspaceError):
            self.run_consumer()
        self.assertEqual(previous.read_text(encoding="utf-8"), "historical-report")


class ConceptProcessTest(unittest.TestCase):
    def test_failed_kill_still_reaps_the_compiler(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            child = Mock()
            child.wait.side_effect = [KeyboardInterrupt(), 0]
            child.kill.side_effect = OSError("kill denied")
            with patch.object(concepts.subprocess, "Popen", return_value=child), self.assertRaises(BaseExceptionGroup):
                concepts._process(["native"], cwd=root, environment={}, stdout=root / "out", stderr=root / "err")
            child.kill.assert_called_once()
            self.assertEqual(child.wait.call_count, 2)
