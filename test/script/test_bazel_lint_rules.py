"""Small consumer contracts for native lint inputs and the fixture result launcher."""

import ast
import json
import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from types import SimpleNamespace

from ao.core.paths import PROJECT_ROOT


def _declarations(path, names):
    class ListKeys(ast.NodeTransformer):
        def visit_Call(self, node):
            self.generic_visit(node)
            # Starlark dict.keys() returns a list, including for concatenation.
            if isinstance(node.func, ast.Attribute) and node.func.attr == "keys":
                return ast.Call(func=ast.Name(id="list", ctx=ast.Load()), args=[node], keywords=[])
            return node

    tree = ListKeys().visit(ast.parse(path.read_text(encoding="utf-8")))
    declarations = [
        node
        for node in tree.body
        if (isinstance(node, ast.FunctionDef) and node.name in names)
        or (isinstance(node, ast.Assign) and any(target.id in names for target in node.targets))
    ]
    return compile(ast.fix_missing_locations(ast.Module(body=declarations, type_ignores=[])), str(path), "exec")


class _Inputs:
    def __init__(self, direct=(), *, transitive=()):
        self.direct = tuple(direct)
        self.transitive = tuple(transitive)

    def to_list(self):
        return [*self.direct, *(file for group in self.transitive for file in group.to_list())]


def _file(name, *, path=None):
    return SimpleNamespace(
        path=path or name,
        short_path=name,
        basename=Path(name).name,
        extension=Path(name).suffix.removeprefix("."),
        is_source=True,
    )


class LintInputConsumerTest(unittest.TestCase):
    def setUp(self):
        self.provider = object()
        self.functions = {"CcInfo": self.provider, "depset": _Inputs}
        names = {
            "_HEADER_SUFFIXES",
            "_SOURCE_SUFFIXES",
            "_COMPILE_MNEMONICS",
            "_header_inputs",
            "_paired",
            "_component",
        }
        exec(_declarations(PROJECT_ROOT / "bazel/lint/lint.bzl", names), self.functions)

    def test_private_implementation_headers_and_additional_files_remain_inputs_not_default_archives(self):
        public, private = _file("include/ao/utility/Public.h"), _file("lib/utility/Private.h")
        extra = _file("generated/extra.def")
        owner_headers, implementation_headers = _Inputs([public]), _Inputs([private])

        class Dependency(dict):
            actions = (SimpleNamespace(mnemonic="CppCompile"),)

        dependency = Dependency(
            {self.provider: SimpleNamespace(compilation_context=SimpleNamespace(headers=implementation_headers))}
        )
        dependency.default_archive = _file("not-a-lint-input.a")
        target = {self.provider: SimpleNamespace(compilation_context=SimpleNamespace(headers=owner_headers))}
        ctx = SimpleNamespace(
            rule=SimpleNamespace(
                files=SimpleNamespace(srcs=[_file("lib/utility/Example.cpp")], additional_compiler_inputs=[extra]),
                attr=SimpleNamespace(deps=[], implementation_deps=[dependency]),
            )
        )
        inputs, _own = self.functions["_header_inputs"](target, ctx)
        self.assertIn(extra, inputs.direct)
        self.assertIn(owner_headers, inputs.transitive)
        self.assertIn(implementation_headers, inputs.transitive)
        self.assertNotIn(dependency.default_archive, inputs.to_list())
        self.assertFalse(any(file.extension == "cpp" for file in inputs.to_list()))

    def test_header_companion_uses_public_component_and_short_path_not_the_exec_prefix(self):
        header = _file("include/ao/utility/Storage.h", path="mapped/include/Storage.h")
        companion = _file("lib/utility/StoragePosix.cpp", path="different/StoragePosix.cpp")
        foreign = _file("app/utility/Storage.cpp")
        self.assertTrue(self.functions["_paired"](header, companion))
        self.assertFalse(self.functions["_paired"](header, foreign))


class HeaderOriginTest(unittest.TestCase):
    def setUp(self):
        self.cc_info = object()

        class HeaderOriginInfo:
            def __init__(self, origins):
                self.origins = origins

        def fail(message):
            raise AssertionError(message)

        self.info = HeaderOriginInfo
        self.functions = {"CcInfo": self.cc_info, "HeaderOriginInfo": HeaderOriginInfo, "depset": _Inputs, "fail": fail}
        names = {"_header_origin_impl"}
        exec(_declarations(PROJECT_ROOT / "bazel/lint/lint.bzl", names), self.functions)

    def origins(self, published, declared, deps=(), *, pairs=(), actions=None):
        class Target(dict):
            pass

        target = Target(
            {
                self.cc_info: SimpleNamespace(
                    compilation_context=SimpleNamespace(direct_public_headers=published, direct_textual_headers=[])
                )
            }
        )
        target.actions = (
            [
                SimpleNamespace(mnemonic="Symlink", inputs=_Inputs([original]), outputs=_Inputs([virtual]))
                for virtual, original in pairs
            ]
            if actions is None
            else actions
        )
        ctx = SimpleNamespace(
            label=SimpleNamespace(repo_name=""),
            rule=SimpleNamespace(files=SimpleNamespace(hdrs=declared), attr=SimpleNamespace(deps=list(deps))),
        )
        return self.functions["_header_origin_impl"](target, ctx)[0]

    def virtual(self, path):
        return SimpleNamespace(**{**vars(_file(path)), "is_source": False})

    def test_virtual_headers_follow_native_symlink_inputs_and_preserve_transitive_origins(self):
        nested, flat = _file("app/runtime/detail/State.h"), _file("app/runtime/State.h")
        generated = SimpleNamespace(**{**vars(_file("app/i18n/catalog_codegen/Tags.h")), "is_source": False})
        published = [
            self.virtual("bazel-out/k8-dbg/bin/app/runtime/_virtual_includes/private/runtime/detail/State.h"),
            nested,
            self.virtual("bazel-out/k8-dbg/bin/_virtual_includes/3f2a/runtime/State.h"),
            flat,
            self.virtual("bazel-out/k8-dbg/bin/_virtual_includes/77/Tags.h"),
            generated,
        ]
        dependency = {self.info: self.info(_Inputs(["dependency/_virtual_includes/k/A.h\ninclude/A.h"]))}
        info = self.origins(
            published,
            [nested, flat, generated],
            [dependency],
            pairs=[(published[0], nested), (published[2], flat), (published[4], generated)],
        )
        self.assertEqual(
            sorted(info.origins.to_list()),
            [
                "bazel-out/k8-dbg/bin/_virtual_includes/3f2a/runtime/State.h\napp/runtime/State.h",
                "bazel-out/k8-dbg/bin/app/runtime/_virtual_includes/private/runtime/detail/State.h\n"
                "app/runtime/detail/State.h",
                "dependency/_virtual_includes/k/A.h\ninclude/A.h",
            ],
        )

    def test_include_prefix_that_matches_another_header_suffix_does_not_change_its_origin(self):
        flat = _file("lib/sample/lib/Header.h")
        nested = _file("lib/sample/lib/prefix/Header.h")
        first = self.virtual("bazel-out/k8-dbg/bin/lib/sample/_virtual_includes/headers/prefix/Header.h")
        second = self.virtual("bazel-out/k8-dbg/bin/lib/sample/_virtual_includes/headers/prefix/prefix/Header.h")
        info = self.origins([first, second], [flat, nested], pairs=[(first, flat), (second, nested)])
        self.assertEqual(
            info.origins.to_list(),
            [first.path + "\n" + flat.path, second.path + "\n" + nested.path],
        )

    def test_virtual_headers_without_one_declared_native_symlink_input_fail_analysis(self):
        original = _file("x/Header.h")
        virtual = self.virtual("bazel-out/k8-dbg/bin/x/_virtual_includes/headers/Header.h")
        for mnemonic, inputs in (
            ("FileWrite", [original]),
            ("Symlink", []),
            ("Symlink", [original, _file("x/Other.h")]),
            ("Symlink", [_file("foreign/Header.h")]),
        ):
            with self.subTest(mnemonic=mnemonic, inputs=inputs), self.assertRaises(AssertionError):
                self.origins(
                    [virtual],
                    [original],
                    actions=[SimpleNamespace(mnemonic=mnemonic, inputs=_Inputs(inputs), outputs=_Inputs([virtual]))],
                )

    def test_an_unattributable_virtual_header_fails_analysis(self):
        published = [self.virtual("bazel-out/k8-dbg/bin/x/_virtual_includes/k/prefix/Same.h")]
        with self.assertRaises(AssertionError):
            self.origins(published, [_file("x/a/Same.h"), _file("x/b/Same.h")])
        with self.assertRaises(AssertionError):
            self.origins(published, [_file("x/Other.h")])


class PairingParityTest(unittest.TestCase):
    def test_aspect_and_portal_pair_every_repository_header_identically(self):
        from ao.core import bazel_lint

        functions = {}
        exec(_declarations(PROJECT_ROOT / "bazel/lint/lint.bzl", {"_component", "_paired"}), functions)
        tracked = subprocess.run(
            ["git", "ls-files", "--", "app", "include", "lib", "test", "tool"],
            cwd=PROJECT_ROOT,
            capture_output=True,
            text=True,
            check=True,
        ).stdout.split()
        headers = [path for path in tracked if Path(path).suffix.lower() in (".h", ".hh", ".hpp", ".hxx")]
        sources = [path for path in tracked if Path(path).suffix.lower() in (".c", ".cc", ".cpp", ".cxx", ".mm")]
        by_stem = {}
        for source in sources:
            by_stem.setdefault(Path(source).stem.casefold()[:4], []).append(source)
        compared = 0
        for header in headers:
            # Pairing requires the source stem to extend the header stem.
            for source in by_stem.get(Path(header).stem.casefold()[:4], []):
                with self.subTest(header=header, source=source):
                    self.assertEqual(
                        functions["_paired"](_file(header), _file(source)), bazel_lint._paired(header, source)
                    )
                compared += 1
        self.assertGreater(compared, len(headers))


class ComponentParityTest(unittest.TestCase):
    def test_aspect_and_portal_map_every_repository_directory_to_one_component(self):
        from ao.core import bazel_lint

        functions = {}
        exec(_declarations(PROJECT_ROOT / "bazel/lint/lint.bzl", {"_component"}), functions)
        tracked = subprocess.run(
            ["git", "ls-files", "--", "app", "include", "lib", "test", "tool"],
            cwd=PROJECT_ROOT,
            capture_output=True,
            text=True,
            check=True,
        ).stdout.split()
        for path in tracked:
            portal = bazel_lint._component(path)
            with self.subTest(path=path):
                self.assertEqual(functions["_component"](_file(path)), None if portal is None else "/".join(portal))
        for path, component in (
            ("include/ao/utility/Span.h", "lib/utility"),
            ("app/include/ao/rt/Session.h", "app/runtime"),
            ("app/include/ao/i18n/MessageInventory.def", "app/i18n"),
            ("app/windows-winui/include/ao/winui/Shell.h", "app/windows-winui"),
            ("app/windows-winui/include/ao/winui/detail/Bits.h", "app/windows-winui/detail"),
            ("app/linux-gtk/Window.cpp", "app/linux-gtk"),
            ("doc/Example.h", None),
        ):
            with self.subTest(path=path):
                self.assertEqual(functions["_component"](_file(path)), component)


class ConsumerMainTest(unittest.TestCase):
    def setUp(self):
        def fail(message):
            raise AssertionError(message)

        self.functions = {"fail": fail}
        exec(_declarations(PROJECT_ROOT / "bazel/lint/lint.bzl", {"_consumer_mains"}), self.functions)
        self.source = _file("lib/util/Clock.cpp")
        self.span = _file("include/ao/util/Span.h")
        self.fragment = _file("app/include/ao/i18n/Inventory.def")
        generated = SimpleNamespace(**{**vars(_file("bazel-out/k8-dbg/bin/lib/util/Generated.h")), "is_source": False})
        self.headers = _Inputs([self.span, self.fragment, generated])

    def mains(self, selected, pairs=None):
        found = {self.source.path: self.source}
        return self.functions["_consumer_mains"]("//lib/util:util", selected, found, self.headers, pairs or {})

    def test_selected_consumers_bind_their_source_and_checkout_header_files(self):
        selected = [[self.source.path, self.span.path], [self.source.path, self.fragment.path]]
        self.assertEqual(
            self.mains(selected),
            {self.span.path: (self.source.path, self.span), self.fragment.path: (self.source.path, self.fragment)},
        )
        self.assertEqual(self.mains([]), {})

    def test_foreign_sources_unreachable_or_paired_mains_and_duplicates_fail_analysis(self):
        for selected, pairs in (
            ([["lib/util/Other.cpp", self.span.path]], None),
            ([[self.source.path, "include/ao/util/Undeclared.h"]], None),
            ([[self.source.path, "bazel-out/k8-dbg/bin/lib/util/Generated.h"]], None),
            ([[self.source.path, self.span.path]], {self.span.path: self.source.path}),
            ([[self.source.path, self.span.path], [self.source.path, self.span.path]], None),
        ):
            with self.subTest(selected=selected, pairs=pairs), self.assertRaises(AssertionError):
                self.mains(selected, pairs)


class LintOutputIdentityTest(unittest.TestCase):
    def setUp(self):
        def fail(message):
            raise ValueError(message)

        self.cc_info, self.origins, self.verification = object(), object(), object()
        self.functions = {
            "CcInfo": self.cc_info,
            "HeaderOriginInfo": self.origins,
            "LintVerificationInfo": self.verification,
            "OutputGroupInfo": SimpleNamespace,
            "LINT_CONSUMERS": {},
            "LINT_SDK_HOST": "linux",
            "CATALOG_TOOLS": {"python": "native-python"},
            "depset": _Inputs,
            "fail": fail,
            "json": SimpleNamespace(encode=json.dumps),
            "native_environment": lambda _ctx, action, _language: action.env,
            "_arguments": lambda *_args: SimpleNamespace(add_all=lambda *_values: None),
        }
        names = {
            "_HEADER_SUFFIXES",
            "_SOURCE_SUFFIXES",
            "_COMPILE_MNEMONICS",
            "_header_inputs",
            "_paired",
            "_component",
            "_consumer_mains",
            "_action_key",
            "_output_stem",
            "_lint_impl",
        }
        exec(_declarations(PROJECT_ROOT / "bazel/lint/lint.bzl", names), self.functions)
        self.source = _file("test/lint/CacheClean.cpp", path="bazel-out/k8-opt/bin/test/lint/CacheClean.cpp")
        self.source.is_source = False
        self.paired = _file("test/lint/CacheClean.h")
        self.consumer = _file("test/lint/Shared.h")

    def action(self, *, pic=False, source=None):
        source = source or self.source
        output = "bazel-out/k8-opt/bin/test/lint/_objs/cache_clean/" + source.basename
        output += ".pic.o" if pic else ".o"
        return SimpleNamespace(
            mnemonic="CppCompile",
            inputs=_Inputs([source]),
            outputs=_Inputs([_file(output), _file(output + ".d")]),
            argv=["native-c++", "-fPIC" if pic else "-fno-PIC", "-c", source.path, "-o", output],
            env={"NATIVE_CONTEXT": "pic" if pic else "non-pic"},
        )

    def analyze(self, actions, *, headers=True):
        class Target(dict):
            pass

        class Label(str):
            name = "cache_clean"
            repo_name = ""

        label = Label("//test/lint:cache_clean")
        self.functions["LINT_CONSUMERS"] = {label: [[self.source.path, self.consumer.path]]} if headers else {}
        target = Target({self.origins: SimpleNamespace(origins=_Inputs())})
        target.actions = actions
        declared, written, runs = [], {}, []

        def declare(name):
            declared.append(name)
            return _file(name, path="bazel-out/k8-opt/bin/test/lint/" + name)

        verification = SimpleNamespace(
            policy=_file("policy.json"),
            sdk=_file("sdk.json"),
            result=_file("verified.json"),
            host=_file("native-tidy"),
            plugin=None,
            inputs=_Inputs(),
        )
        sources = {file.path: file for action in actions for file in action.inputs.to_list()}
        ctx = SimpleNamespace(
            label=label,
            bin_dir=SimpleNamespace(path="bazel-out/k8-opt/bin"),
            rule=SimpleNamespace(
                files=SimpleNamespace(
                    srcs=list(sources.values()), hdrs=[self.paired, self.consumer] if headers else []
                ),
                attr=SimpleNamespace(),
            ),
            attr=SimpleNamespace(_verification={self.verification: verification}),
            actions=SimpleNamespace(
                declare_file=declare,
                write=lambda file, text: written.update({file.path: json.loads(text)}),
                run=lambda **kwargs: runs.append(kwargs),
            ),
        )
        self.functions["_lint_impl"](target, ctx)
        self.assertEqual(len(declared), len(set(declared)))
        contexts = {path: value for path, value in written.items() if path.endswith(".context.json")}
        index = written["bazel-out/k8-opt/bin/test/lint/cache_clean.aobus-lint.index.json"]
        return contexts, runs, index

    def test_pic_and_non_pic_actions_keep_separate_source_paired_header_and_consumer_reports(self):
        actions = [self.action(), self.action(pic=True)]
        contexts, runs, index = self.analyze(actions)
        self.assertEqual(len(contexts), 6)
        self.assertEqual(len(runs), 6)
        self.assertEqual(len(index["reports"]), 6)
        for action in actions:
            captured = [context for context in contexts.values() if context["argv"] == action.argv]
            self.assertEqual(
                {context["main"] for context in captured}, {self.source.path, self.paired.path, self.consumer.path}
            )
            for context in captured:
                self.assertEqual(context["source"], self.source.path)
                self.assertEqual(context["outputs"], sorted(file.path for file in action.outputs.to_list()))
            native_runs = [run for run in runs if run["env"] == action.env]
            self.assertEqual(len(native_runs), 3)
            self.assertTrue(all(action.inputs in run["inputs"].transitive for run in native_runs))
        self.assertEqual({entry["report"] for entry in index["reports"]}, {run["outputs"][0].path for run in runs})
        for path in contexts:
            self.assertRegex(Path(path).name, r"^[0-9a-f]{1,8}\.context\.json$")

    def test_unchanged_contexts_keep_paths_and_contents_when_actions_or_outputs_reorder_and_sources_are_added(self):
        actions = [self.action(), self.action(pic=True)]
        original, _, _ = self.analyze(actions)
        for action in actions:
            action.outputs = _Inputs(reversed(action.outputs.to_list()))
        added = self.action(source=_file("test/lint/Additional.cpp"))
        expanded, _, _ = self.analyze([added, *reversed(actions)])
        self.assertEqual({path: expanded[path] for path in original}, original)
        self.assertEqual(len(expanded), len(original) + 1)

    def test_stems_distinguish_every_report_identity_field_without_embedding_paths(self):
        identity = {
            "owner": "//test/lint:cache_clean",
            "configuration": "bazel-out/k8-opt/bin",
            "source": self.source.path,
            "main": self.consumer.path,
            "outputs": ["bazel-out/k8-opt/bin/test/lint/CacheClean.o"],
            "mode": "RELAXED",
            "platform": "linux",
        }
        stem = self.functions["_output_stem"](identity)
        self.assertRegex(stem, r"^[0-9a-f]{1,8}$")
        for field, value in identity.items():
            with self.subTest(field=field):
                changed = ["different-output.o"] if field == "outputs" else value + "-different"
                self.assertNotEqual(stem, self.functions["_output_stem"](dict(identity, **{field: changed})))

    def test_hash_collision_fails_analysis_with_both_native_context_identities(self):
        self.functions["hash"] = lambda _value: 1
        with self.assertRaisesRegex(ValueError, r"Lint output stem 1 is shared") as raised:
            self.analyze([self.action(), self.action(pic=True)], headers=False)
        self.assertIn("CacheClean.cpp.o", str(raised.exception))
        self.assertIn("CacheClean.cpp.pic.o", str(raised.exception))


class LintRuntimeEnvironmentTest(unittest.TestCase):
    def test_windows_python_profile_is_selected_sdk_data_not_ambient_inheritance(self):
        selected = {"UserProfile": "C:/Users/selected"}
        functions = {
            "LINT_SDK_HOST": "windows",
            "CATALOG_TOOLS": selected,
            "find_cc_toolchain": lambda _ctx: object(),
            "cc_common": SimpleNamespace(
                configure_features=lambda **_kwargs: object(),
                create_compile_variables=lambda **_kwargs: object(),
                get_environment_variables=lambda **_kwargs: {"INCLUDE": "selected native includes"},
            ),
        }
        exec(
            _declarations(PROJECT_ROOT / "bazel/lint/lint.bzl", {"native_environment", "_SUPPORTED_FEATURES"}),
            functions,
        )
        original = {"SYSTEMROOT": "C:/Windows", "USERPROFILE": "C:/Users/not-selected"}
        ctx = SimpleNamespace(
            features=["aobus_warnings", "treat_warnings_as_errors", "external_include_paths"],
            disabled_features=[],
            configuration=SimpleNamespace(default_shell_env=original),
        )
        environment = functions["native_environment"](ctx, portal=True)
        self.assertEqual(environment["USERPROFILE"], selected["UserProfile"])
        self.assertEqual(environment["INCLUDE"], "selected native includes")
        # Report actions import no profile-dependent module, so the profile
        # stays out of their cache keys.
        self.assertNotIn("USERPROFILE", functions["native_environment"](ctx))
        self.assertEqual(environment["SYSTEMROOT"], original["SYSTEMROOT"])
        self.assertEqual(environment["PYTHONDONTWRITEBYTECODE"], "1")
        self.assertEqual(original["USERPROFILE"], "C:/Users/not-selected")
        ctx.features = ["external_include_paths"]
        ctx.disabled_features = ["aobus_warnings", "treat_warnings_as_errors"]
        self.assertEqual(functions["native_environment"](ctx, portal=True), environment)


class FixtureResultLauncherTest(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix="aobus-lint-result-")
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        tree = ast.parse((PROJECT_ROOT / "bazel/lint/fixtures.bzl").read_text(encoding="utf-8"))
        assignment = next(
            node for node in tree.body if isinstance(node, ast.Assign) and node.targets[0].id == "_TEST_PROGRAM"
        )
        self.program = ast.literal_eval(assignment.value)
        self.report_key, self.log_key = "_main/test/result.json", "_main/test/result.log"
        self.program = (
            self.program.replace("REPORT_KEY", repr(self.report_key))
            .replace("LOG_KEY", repr(self.log_key))
            .replace("PLATFORM_KEY", repr("linux"))
        )
        self.report, self.log = self.root / "physical result.json", self.root / "physical log.txt"
        self.log.write_text("real fixture stage output\n", encoding="utf-8")

    def invoke(self, result):
        self.report.write_text(json.dumps(result), encoding="utf-8")
        manifest = self.root / "MANIFEST"
        manifest.write_text(f"{self.report_key} {self.report}\n{self.log_key} {self.log}\n", encoding="utf-8")
        environment = dict(os.environ, RUNFILES_MANIFEST_FILE=str(manifest))
        environment.pop("TEST_SRCDIR", None)
        return subprocess.run(
            [sys.executable, "-I", "-S", "-B", "-c", self.program],
            env=environment,
            capture_output=True,
            text=True,
            check=False,
        )

    def test_manifest_only_runfiles_preserve_completed_pass_and_failure_status(self):
        for status in (0, 1):
            with self.subTest(status=status):
                result = self.invoke({"schema": 1, "status": status, "platform": "linux"})
                self.assertEqual(result.returncode, status, result.stderr)
                self.assertEqual(result.stdout, "real fixture stage output\n")

    def test_malformed_schema_or_status_and_foreign_platform_cannot_pass(self):
        for value in (
            {"schema": True, "status": 0, "platform": "linux"},
            {"schema": 1, "status": False, "platform": "linux"},
            {"schema": 1, "status": 0, "platform": "windows"},
            [],
        ):
            with self.subTest(value=value):
                result = self.invoke(value)
                self.assertEqual(result.returncode, 2, result.stderr)
                self.assertIn("Cannot read lint fixture result:", result.stderr)
