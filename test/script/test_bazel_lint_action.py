"""Contracts for the non-LLVM, cacheable lint action launcher."""

import argparse
import contextlib
import io
import json
import os
import re
import shlex
import subprocess
import sys
import tempfile
import types
import unittest
from pathlib import Path
from unittest.mock import patch

from ao.core import bazel_lint_action as action
from ao.core.paths import PROJECT_ROOT

# The native compile action's argv[0] under the patched apple_support toolchain.
_APPLE_ENTRY = "bazel-out/darwin_arm64-opt-exec/bin/external/+dependencies+aobus_toolchain/entry/wrapped_clang_pp"


class LintActionTest(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="ao-lint-action-")
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name).resolve()
        self.previous = Path.cwd()
        os.chdir(self.root)
        self.addCleanup(os.chdir, self.previous)
        self.host = self.root / "clang-tidy"
        self.host.write_text("declared host", encoding="utf-8")
        self.plugin = self.root / "plugin.so"
        self.plugin.write_text("declared plugin", encoding="utf-8")
        self.configuration = {
            "schema": 1,
            "source_root": str(PROJECT_ROOT),
            "check": None,
            "header_filter": None,
            "clang_tidy_args": [],
            "alpha": False,
        }
        # The published action policy carries no absolute checkout root.
        self.policy = {
            "schema": 1,
            "check": None,
            "header_filter": None,
            "clang_tidy_args": [],
            "alpha": False,
            "expected_checks": ["aobus-example"],
        }
        self.sdk = {
            "schema": 1,
            "version": "22.1.8",
            "platform": "linux",
            "host_kind": "plugin",
            "system_arguments": [],
            "analysis_arguments": [],
        }
        self.context = {
            "schema": 1,
            "owner": "@@//lib/sample:sample",
            "configuration": "bazel-out/k8-dbg/bin",
            "source": "lib/sample/Sample.cpp",
            "main": "lib/sample/Sample.cpp",
            "outputs": ["bazel-out/k8-dbg/bin/lib/sample/_objs/sample/Sample.o"],
            "mode": "STRICT",
            "platform": "linux",
            "argv": ["/native/clang++", "-std=c++26", "-include", "semantic.h", "-c", "lib/sample/Sample.cpp"],
            "virtual_headers": {},
        }
        for path in ("lib/sample/Sample.cpp", "include/ao/sample/Sample.h", "semantic.h"):
            target = self.root / path
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_text("// declared input\n", encoding="utf-8")
        self.analysis_output = ""
        self.analysis_status = 0
        # Lines the analyzer frontend writes to its header include listing.
        self.included: list[str] = []
        self.driver_output = None
        self.driver_status = 0
        self.driver_calls = []
        self.driver_environments = []
        self.calls = []
        self.write_inputs()

    def write_inputs(self):
        for name, data in (
            ("configuration", self.configuration),
            ("policy", self.policy),
            ("sdk", self.sdk),
            ("context", self.context),
        ):
            Path(f"{name}.json").write_text(json.dumps(data), encoding="utf-8")

    def driver_trace(self, before=(), after=(), original=None):
        compiler = "/nix/store/w3ifgwjmsvy8dwlfir4i4jz17kzxr1xb-clang-22.1.8/bin/clang++"
        original = [*self.context["argv"][1:], "-###"] if original is None else original
        text = 'HARDENING: Is active (not completely disabled with "all" flag)\n'
        for name, arguments in (
            ("extra flags before", before),
            ("original flags", original),
            ("extra flags after", after),
        ):
            text += f"{name} to {compiler}:\n"
            text += "".join(f"  {shlex.quote(argument)}\n" for argument in arguments) if arguments else "  ''\n"
        return text + "clang version 22.1.8\n"

    def fake_tool(self, command, **kwargs):
        if command[-1:] == ["-###"]:
            self.assertEqual(command, [*self.context["argv"], "-###"])
            self.assertEqual(kwargs["env"]["NIX_DEBUG"], "1")
            self.assertIs(kwargs["stderr"], subprocess.STDOUT)
            self.driver_calls.append(command)
            self.driver_environments.append(kwargs["env"])
            output = self.driver_trace() if self.driver_output is None else self.driver_output
            return subprocess.CompletedProcess(command, self.driver_status, output)
        self.assertEqual(command[0], str(self.host))
        self.calls.append(command)
        self.assertIs(kwargs["stderr"], subprocess.STDOUT)
        if command[1:] == ["--version"]:
            return subprocess.CompletedProcess(command, 0, "LLVM version 22.1.8\n")
        plugin = [] if self.sdk["platform"] == "windows" else [f"-load={self.plugin}"]
        if command[1:] == [*plugin, "-checks=-*,aobus-*", "-list-checks"]:
            return subprocess.CompletedProcess(command, 0, "Enabled checks:\n    aobus-example\n")
        self.assertEqual(command[1 : 1 + len(plugin)], plugin)
        self.assertIn("--quiet", command)
        self.assertIn("--use-color=false", command)
        database = Path(command[command.index("-p") + 1]) / "compile_commands.json"
        self.assertTrue(database.is_file())
        entries = json.loads(database.read_text(encoding="utf-8"))
        self.database = entries[0]
        self.assertEqual(len(entries), 1)
        self.assertEqual(entries[0]["file"], command[-1])
        self.assertEqual(entries[0]["directory"], str(self.root))
        listing = command.index("--extra-arg=-header-include-file")
        self.assertEqual(command[listing - 1 : listing + 2 : 2], ["--extra-arg=-Xclang", "--extra-arg=-Xclang"])
        Path(command[listing + 2].removeprefix("--extra-arg=")).write_text(
            "".join(line + "\n" for line in self.included), encoding="utf-8"
        )
        return subprocess.CompletedProcess(command, self.analysis_status, self.analysis_output)

    def run_phase(self, phase, extra=()):
        common = ["--policy", "policy.json", "--sdk", "sdk.json", "--tidy", str(self.host)]
        if self.sdk["platform"] != "windows":
            common.extend(["--plugin", str(self.plugin)])
        if phase == "verify":
            arguments = ["verify", *common, "--output", "verified.json", "--log", "verify.log"]
        else:
            arguments = [
                "analyze",
                *common,
                "--context",
                "context.json",
                "--verification",
                "verified.json",
                "--report",
                "result.report.json",
                "--log",
                "result.log",
            ]
        self.stderr = io.StringIO()
        with (
            patch.object(action.subprocess, "run", side_effect=self.fake_tool),
            contextlib.redirect_stderr(self.stderr),
        ):
            return action.main([*arguments, *extra])

    def verified(self):
        self.write_inputs()
        self.assertEqual(self.run_phase("verify"), 0)
        self.calls.clear()
        self.driver_calls.clear()
        self.driver_environments.clear()

    def report(self):
        return json.loads(Path("result.report.json").read_text(encoding="utf-8"))

    def header_filter(self):
        return next(arg for arg in self.calls[-1] if arg.startswith("-header-filter=")).removeprefix("-header-filter=")

    def use_windows_sdk(self):
        self.context["platform"] = self.sdk["platform"] = "windows"
        resource = self.root / "sdk/lib/clang/22"
        (resource / "include").mkdir(parents=True, exist_ok=True)
        self.sdk["resource_dir"] = str(resource)
        self.sdk["host_kind"] = "standalone"
        self.sdk["analysis_arguments"] = [f"-resource-dir={resource}", "-D_USE_STD_VECTOR_ALGORITHMS=0"]
        self.context["argv"] = ["cl.exe", "/c", "/Iinclude", self.context["source"]]

    def test_cacheable_actions_import_only_the_declared_module_closure(self):
        build = (PROJECT_ROOT / "BUILD.bazel").read_text(encoding="utf-8")
        block = re.search(r'name = "ao_lint_action_sources",\s*srcs = \[(.*?)\]', build, re.DOTALL)
        self.assertIsNotNone(block)
        declared = set(re.findall(r'"(script/ao/[^"]+\.py)"', block[1]))
        # Execute the action script's imports exactly as Bazel starts it.
        probe = (
            "import pathlib, runpy, sys\n"
            "runpy.run_path('script/ao/core/bazel_lint_action.py', run_name='lint_probe')\n"
            "root = pathlib.Path('script').absolute()\n"
            "for module in list(sys.modules.values()):\n"
            "    path = pathlib.Path(getattr(module, '__file__', None) or '/')\n"
            "    if path.is_relative_to(root):\n"
            "        print(path.relative_to(root.parent).as_posix())\n"
        )
        result = subprocess.run(
            [sys.executable, "-I", "-S", "-B", "-c", probe], cwd=PROJECT_ROOT, capture_output=True, text=True
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        imported = set(result.stdout.split()) - {"script/ao/core/bazel_lint_action.py"}
        self.assertEqual(imported, declared)

    def test_header_filter_matches_each_native_spelling_of_checkout_headers_only(self):
        private = "bazel-out/k8-dbg/bin/app/runtime/_virtual_includes/private_headers/runtime/"
        shortened = "bazel-out/k8-dbg/bin/_virtual_includes/1a2b/"
        self.context["virtual_headers"] = {
            private + "A.h": "app/runtime/A.h",
            private + "detail/B.h": "app/runtime/detail/B.h",
            shortened + "runtime/C.h": "app/runtime/C.h",
            shortened + "CatalogTags.h": None,
            "bazel-out/k8-dbg/bin/test/_virtual_includes/test_headers/fixture/T.h": "test/fixture/T.h",
        }
        self.verified()
        accepted = (
            "include/ao/sample/Sample.h",
            "./lib/sample/Detail.h",
            f"{self.root.as_posix()}/lib/sample/Detail.h",
            (PROJECT_ROOT / "app/runtime/A.h").as_posix(),
            "tool/lint/Probe.h",
            private + "A.h",
            private + "detail/B.h",
            f"{self.root.as_posix()}/{private}A.h",
            shortened + "runtime/C.h",
        )
        rejected = (
            "bazel-out/k8-dbg/bin/lib/sample/Generated.h",
            "external/dependency/Header.h",
            "/nix/store/w3ifgwjmsvy8dwlfir4i4jz17kzxr1xb-gcc/include/vector",
            "test/fixture/Helper.h",
            shortened + "CatalogTags.h",
            "bazel-out/k8-dbg/bin/test/_virtual_includes/test_headers/fixture/T.h",
            "script/lib/Header.h",
            r"tool\lint\Probe.h",  # POSIX backslashes are literal, not separators.
        )
        for mode, include, exclude in (
            ("STRICT", accepted, rejected),
            ("RELAXED", ("test/fixture/Helper.h", "include/ao/sample/Sample.h"), ("app/runtime/A.h", private + "A.h")),
        ):
            with self.subTest(mode=mode):
                self.context["mode"] = mode
                self.write_inputs()
                self.assertEqual(self.run_phase("analyze"), 0)
                expression = self.header_filter()
                # LLVM compiles a POSIX extended expression without Python groups.
                self.assertNotIn("(?", expression)
                for name in include:
                    self.assertIsNotNone(re.search(expression, name), name)
                for name in exclude:
                    self.assertIsNone(re.search(expression, name), name)
        self.context["mode"] = "STRICT"
        self.write_inputs()
        self.assertEqual(self.run_phase("analyze"), 0)
        # A wholly qualifying virtual directory needs one alternative, not one per header.
        self.assertNotIn("A\\.h", self.header_filter())
        self.assertIn("C\\.h", self.header_filter())
        self.context["main"] = "include/ao/sample/Sample.h"
        self.context["virtual_headers"] = {private + "Sample.h": "include/ao/sample/Sample.h"}
        self.write_inputs()
        self.assertEqual(self.run_phase("analyze"), 0)
        expression = self.header_filter()
        for name in (
            "include/ao/sample/Sample.h",
            f"{self.root.as_posix()}/include/ao/sample/Sample.h",
            private + "Sample.h",
        ):
            self.assertIsNotNone(re.search(expression, name), name)
        for name in ("include/ao/sample/Other.h", "include/ao/sample/Sample.hpp"):
            self.assertIsNone(re.search(expression, name), name)

    def test_windows_header_filter_admits_both_separator_spellings_only_for_owned_headers(self):
        self.use_windows_sdk()
        self.verified()
        self.assertEqual(self.run_phase("analyze"), 0)
        expression = self.header_filter()
        for path in ("tool/lint/Probe.h", r"tool\lint\Probe.h", r"include\ao\sample\Sample.h"):
            with self.subTest(path=path):
                self.assertIsNotNone(re.search(expression, path))
        for path in (r"external\dependency\Header.h", r"script\lib\Header.h"):
            with self.subTest(path=path):
                self.assertIsNone(re.search(expression, path))

    def test_reports_name_locations_relative_to_the_execution_root_through_virtual_includes(self):
        virtual = "bazel-out/k8-dbg/bin/app/runtime/_virtual_includes/private_headers/runtime/A.h"
        self.context["virtual_headers"] = {virtual: "app/runtime/A.h"}
        self.verified()
        self.analysis_output = (
            f"{self.root.as_posix()}/lib/sample/Sample.cpp:1:2: warning: main [aobus-example]\n"
            "   12 | int value; // ./lib/sample//Quoted.h:1:1: warning: quoted\n"
            "./lib/sample/Detail.h:3:4: note: declared here\n"
            f"{virtual}:5:6: warning: through a virtual include [aobus-example]\n"
            "In file included from ./lib/sample/Detail.h:1:\n"
            "/nix/store/w3ifgwjmsvy8dwlfir4i4jz17kzxr1xb-gcc/include/vector:7:8: note: system\n"
            "external/dependency/Header.h:9:1: note: external"
        )
        self.assertEqual(self.run_phase("analyze"), 0)
        self.assertEqual(
            Path("result.log").read_text(encoding="utf-8"),
            "lib/sample/Sample.cpp:1:2: warning: main [aobus-example]\n"
            "   12 | int value; // ./lib/sample//Quoted.h:1:1: warning: quoted\n"
            "lib/sample/Detail.h:3:4: note: declared here\n"
            "app/runtime/A.h:5:6: warning: through a virtual include [aobus-example]\n"
            "In file included from ./lib/sample/Detail.h:1:\n"
            "/nix/store/w3ifgwjmsvy8dwlfir4i4jz17kzxr1xb-gcc/include/vector:7:8: note: system\n"
            "external/dependency/Header.h:9:1: note: external",
        )
        self.assertEqual(self.report()["outcome"], "warning")
        root = Path("C:/Exec/Root")
        for spelled in ("c:\\exec\\root\\lib\\Sample.cpp", "C:/Exec/Root/lib/Sample.cpp"):
            self.assertEqual(action._report_path(spelled, (root,), {}, windows=True), "lib/Sample.cpp")
        self.assertEqual(action._report_path("D:\\other\\A.h", (root,), {}, windows=True), "D:\\other\\A.h")

    def test_native_warning_promotion_stays_a_disabled_compiler_diagnostic(self):
        self.verified()
        self.assertEqual(self.run_phase("analyze"), 0)
        self.assertIn("--extra-arg=-Wno-error", self.calls[-1])
        self.use_windows_sdk()
        self.verified()
        self.assertEqual(self.run_phase("analyze"), 0)
        self.assertFalse(any("Wno-error" in argument for argument in self.calls[-1]))

    def test_frontend_policy_matches_the_tidy_route(self):
        self.context["source"] = self.context["main"] = "app/linux-gtk/Window.cpp"
        self.context["argv"][-1] = self.context["source"]
        Path(self.context["source"]).parent.mkdir(parents=True)
        Path(self.context["source"]).write_text("// declared input\n", encoding="utf-8")
        self.verified()
        self.assertEqual(self.run_phase("analyze"), 0)
        config = next(arg for arg in self.calls[-1] if arg.startswith("-config="))
        self.assertIn(",-cppcoreguidelines-owning-memory", config)
        self.assertIn("--extra-arg=-Wno-format-nonliteral", self.calls[-1])
        self.assertFalse(any(arg.startswith("-line-filter=") for arg in self.calls[-1]))
        self.context["source"] = self.context["main"] = "app/windows-winui/MainWindow.cpp"
        Path(self.context["source"]).parent.mkdir(parents=True)
        Path(self.context["source"]).write_text("// declared input\n", encoding="utf-8")
        self.use_windows_sdk()
        self.verified()
        self.assertEqual(self.run_phase("analyze"), 0)
        self.assertIn("--extra-arg-before=-Wno-nonportable-include-path", self.calls[-1])
        line_filter = next(arg for arg in self.calls[-1] if arg.startswith("-line-filter="))
        names = {entry["name"] for entry in json.loads(line_filter.removeprefix("-line-filter="))}
        self.assertEqual(names, {"app/windows-winui/MainWindow.cpp", "app\\windows-winui\\MainWindow.cpp"})
        self.assertNotIn("owning-memory", next(arg for arg in self.calls[-1] if arg.startswith("-config=")))

    def test_incomplete_analysis_keeps_the_analyzer_output_in_the_failure(self):
        self.verified()
        self.analysis_output = "Error: no checks enabled.\n"
        self.assertEqual(self.run_phase("analyze"), 2)
        self.assertFalse(Path("result.report.json").exists())
        self.assertIn("analyzer output tail:\nError: no checks enabled.", self.stderr.getvalue())

    def test_prepare_uses_the_working_fixture_registry_without_executing_tidy(self):
        Path("configuration.json").write_text(json.dumps(self.configuration), encoding="utf-8")

        with patch.object(action.subprocess, "run", side_effect=AssertionError("unexpected policy child")):
            self.assertEqual(
                action.main(["prepare", "--configuration", "configuration.json", "--output", "rendered.json"]), 0
            )
        rendered = json.loads(Path("rendered.json").read_text(encoding="utf-8"))
        self.assertIn("aobus-modernize-use-starts-with", rendered["expected_checks"])
        self.assertNotIn("selected", rendered)
        self.assertEqual(rendered["clang_tidy_args"], [])
        self.assertNotIn("source_root", rendered)

    def test_published_policy_is_stable_across_checkouts_and_changes_only_with_semantic_fields(self):
        rendered = []
        for name in ("checkout-a", "checkout-b"):
            checkout = self.root / name
            fixture = checkout / "test/lint/fixture/aobus-example"
            fixture.mkdir(parents=True)
            (fixture / "Bad.cpp").write_text("// fixture\n", encoding="utf-8")
            Path("configuration.json").write_text(
                json.dumps({**self.configuration, "source_root": str(checkout)}), encoding="utf-8"
            )
            self.assertEqual(
                action.main(["prepare", "--configuration", "configuration.json", "--output", "rendered.json"]), 0
            )
            rendered.append(Path("rendered.json").read_bytes())
        self.assertEqual(rendered[0], rendered[1])
        policy = json.loads(rendered[0])
        self.assertNotIn("source_root", policy)
        self.assertEqual(policy["expected_checks"], ["aobus-example"])
        for field, value in (
            ("check", "aobus-example"),
            ("header_filter", "project-filter"),
            ("clang_tidy_args", ["--quiet"]),
        ):
            with self.subTest(field=field):
                Path("configuration.json").write_text(
                    json.dumps({**self.configuration, "source_root": str(self.root / "checkout-a"), field: value}),
                    encoding="utf-8",
                )
                self.assertEqual(
                    action.main(["prepare", "--configuration", "configuration.json", "--output", "changed.json"]), 0
                )
                changed = json.loads(Path("changed.json").read_text(encoding="utf-8"))
                self.assertEqual(changed[field], value)
                self.assertNotEqual(Path("changed.json").read_bytes(), rendered[0])

    def test_rootless_policy_verifies_and_maps_runtime_roots_through_the_declared_helper(self):
        # The published policy carries no absolute root, so the verification and
        # analysis actions stay checkout-stable. Runtime roots come from the
        # working directory plus the declared helper's lexical invocation alias
        # and resolved physical checkout, never from a possibly generated
        # identity source.
        generated = "bazel-out/k8-dbg/bin/lib/sample/Generated.cpp"
        Path(generated).parent.mkdir(parents=True, exist_ok=True)
        Path(generated).write_text("// generated input\n", encoding="utf-8")
        self.context["mode"] = "RELAXED"
        self.context["source"] = self.context["main"] = generated
        self.context["argv"][-1] = generated
        self.verified()
        self.assertEqual(self.run_phase("analyze"), 0)
        config = next(arg for arg in self.calls[-1] if arg.startswith("-config="))
        self.assertIn(self.root.as_posix(), config)
        self.assertIn(PROJECT_ROOT.as_posix(), config)
        # Each root beyond the working directory is a checkout the declared
        # helper really occupies: the invocation alias and its physical target.
        roots = action._runtime_roots()
        self.assertEqual(roots[0], Path.cwd())
        self.assertIn(PROJECT_ROOT, roots)
        for root in roots[1:]:
            self.assertIs(root.joinpath(*action._HELPER_LOCATION).is_file(), True)
        materialized = self.root / "other-checkout" / Path(*action._HELPER_LOCATION)
        materialized.parent.mkdir(parents=True)
        materialized.write_text("// materialized helper\n", encoding="utf-8")
        with patch.object(action, "__file__", str(materialized)):
            self.assertEqual(action._runtime_roots(), (Path.cwd(), self.root / "other-checkout"))
        with patch.object(action, "__file__", str(self.root / "elsewhere/bazel_lint_action.py")):
            with self.assertRaisesRegex(ValueError, "declared checkout path"):
                action._runtime_roots()

    @unittest.skipIf(os.name == "nt", "a distinct alias needs a POSIX symlink spelling")
    def test_runtime_roots_keep_a_distinct_lexical_alias_of_the_physical_checkout(self):
        # A mapped checkout spelling and its physical target are both
        # legitimate header spellings; the alias stays runtime-only, never an
        # action input, and the working directory stays a separate root.
        physical = self.root / "physical-checkout"
        helper = physical.joinpath(*action._HELPER_LOCATION)
        helper.parent.mkdir(parents=True)
        helper.write_text("// physical helper\n", encoding="utf-8")
        alias = self.root / "mapped-checkout"
        alias.symlink_to(physical)
        with patch.object(action, "__file__", str(alias.joinpath(*action._HELPER_LOCATION))):
            self.assertEqual(action._runtime_roots(), (Path.cwd(), alias, physical))
            expression = action._header_filter(
                action.Identity(
                    "@@//lib/sample:sample",
                    "bazel-out/k8-dbg/bin",
                    "lib/sample/Sample.cpp",
                    "lib/sample/Sample.cpp",
                    ("bazel-out/k8-dbg/bin/lib/sample/_objs/sample/Sample.o",),
                    "STRICT",
                    "linux",
                ),
                self.policy,
                action._runtime_roots(),
                {},
            )
        for spelling in (alias.as_posix() + "/app/runtime/A.h", physical.as_posix() + "/app/runtime/A.h"):
            self.assertIsNotNone(re.search(expression, spelling), spelling)

    def test_runtime_roots_admit_native_windows_helper_casing_but_only_the_declared_location(self):
        # A virtiofs checkout may resolve the helper through an uppercased
        # final path while the lexical spelling keeps the invoked case. Only
        # native Windows folds component case, admitted roots keep their
        # actual spellings, and a different suffix or parent stays rejected.
        checkout = self.root / "windows-checkout"
        windows_os = types.SimpleNamespace(name="nt")
        for tail in (
            ("SCRIPT", "AO", "CORE", "BAZEL_LINT_ACTION.PY"),
            ("ScRiPt", "Ao", "CoRe", "Bazel_Lint_Action.py"),
        ):
            with self.subTest(tail=tail):
                helper = checkout.joinpath(*tail)
                helper.parent.mkdir(parents=True, exist_ok=True)
                helper.write_text("// native helper\n", encoding="utf-8")
                with patch.object(action, "__file__", str(helper)), patch.object(action, "os", windows_os):
                    self.assertEqual(action._runtime_roots(), (Path.cwd(), checkout))
        for wrong in (
            ("SCRIPT", "AO", "CORE", "BAZEL_LINT_ACTION.PYC"),
            ("SCRIPT", "AO", "FOREIGN", "BAZEL_LINT_ACTION.PY"),
        ):
            with self.subTest(wrong=wrong):
                helper = checkout.joinpath(*wrong)
                helper.parent.mkdir(parents=True, exist_ok=True)
                helper.write_text("// wrong helper\n", encoding="utf-8")
                with patch.object(action, "__file__", str(helper)), patch.object(action, "os", windows_os):
                    with self.assertRaisesRegex(ValueError, "declared checkout path"):
                        action._runtime_roots()

    def test_diagnostic_only_validation_precedes_preparation_and_analysis_io(self):
        for argument in (
            "--fix",
            "-fix-errors=true",
            "--export-fixes=out.yaml",
            "@hidden",
            "--extra-arg=@hidden",
            "--list-checks",
        ):
            with self.subTest(argument=argument):
                self.policy["clang_tidy_args"] = [argument]
                self.configuration["clang_tidy_args"] = [argument]
                self.write_inputs()
                with (
                    patch.object(action.subprocess, "run", side_effect=AssertionError("unexpected child")),
                    contextlib.redirect_stderr(io.StringIO()),
                ):
                    self.assertEqual(
                        action.main(["prepare", "--configuration", "configuration.json", "--output", "rendered.json"]),
                        2,
                    )
                    self.assertEqual(self.run_phase("analyze"), 2)
                self.assertFalse(Path("rendered.json").exists())
                self.assertFalse(Path("result.report.json").exists())

    def test_public_configuration_validation_preserves_the_portal_contract(self):
        supplied = {
            **self.configuration,
            "check": "aobus-example",
            "header_filter": "project-filter",
            "clang_tidy_args": ["--quiet"],
        }
        self.assertEqual(
            action.validate_configuration(supplied), {key: value for key, value in supplied.items() if key != "schema"}
        )
        with self.assertRaises(ValueError):
            action.validate_configuration({**supplied, "clang_tidy_args": ["--fix"]})

    def test_raw_diagnostic_allowlist_preserves_supported_display_options(self):
        supported = (
            "--quiet",
            "--system-headers",
            "--system-headers=true",
            "--system-headers=false",
            "--enable-check-profile",
            "--use-color=false",
        )
        for argument in supported:
            with self.subTest(argument=argument):
                validated = action.validate_configuration({**self.configuration, "clang_tidy_args": [argument]})
                self.assertEqual(validated["clang_tidy_args"], [argument])
        self.policy["clang_tidy_args"] = ["--system-headers=true", "--enable-check-profile", "--use-color=false"]
        self.verified()
        self.assertEqual(self.run_phase("analyze"), 0)
        for argument in self.policy["clang_tidy_args"]:
            self.assertIn(argument, self.calls[0])
        self.assertEqual(self.report()["outcome"], "clean")

    def test_help_views_overrides_and_warning_promotion_fail_before_root_or_tool_lookup(self):
        args = argparse.Namespace(
            policy=Path("policy.json"),
            sdk=Path("absent-sdk.json"),
            verification=Path("absent-verification.json"),
            tidy=Path("absent-host"),
            plugin=None,
        )
        unsupported = (
            "-h",
            "--help",
            "--help-hidden",
            "--help-list",
            "--help-list-hidden",
            "--vfsoverlay=overlay.yaml",
            "--extra-arg=-Wno-error",
            "--extra-arg-before=-ivfsoverlay=overlay.yaml",
            "--warnings-as-errors=*",
            "--warnings-as-errors",
            "--config-file=external.yaml",
            "--use-color=true",
            "--use-color",
            "--system-headers=True",
            "--quiet=true",
            "--enable-check-profile=true",
            "--store-check-profile=external",
            "--checks=aobus-*",
        )
        for argument in unsupported:
            with self.subTest(argument=argument):
                data = {
                    **self.configuration,
                    "source_root": str(self.root / "absent-checkout"),
                    "clang_tidy_args": [argument],
                }
                with self.assertRaisesRegex(ValueError, "unsupported Bazel clang-tidy diagnostic argument"):
                    action.validate_configuration(data)
                Path("policy.json").write_text(
                    json.dumps({**self.policy, "clang_tidy_args": [argument]}), encoding="utf-8"
                )
                with patch.object(action.subprocess, "run", side_effect=AssertionError("unexpected child")):
                    with self.assertRaisesRegex(ValueError, "unsupported Bazel clang-tidy diagnostic argument"):
                        action.load_verified_tools(args)

    def test_verified_tools_can_be_loaded_by_an_independent_consumer_without_children(self):
        self.sdk["sdk_root"] = str(self.root / "sdk")
        self.sdk["fixture_arguments"] = ["-std=c++26", "-resource-dir=/native/clang/22"]
        self.verified()
        args = argparse.Namespace(
            policy=Path("policy.json"),
            sdk=Path("sdk.json"),
            verification=Path("verified.json"),
            tidy=self.host,
            plugin=self.plugin,
        )
        with patch.object(action.subprocess, "run", side_effect=AssertionError("unexpected verification child")):
            policy, sdk, host, plugin = action.load_verified_tools(args)
        self.assertEqual(policy, self.policy)
        self.assertEqual(sdk, self.sdk)
        self.assertEqual(host, str(self.host))
        self.assertEqual(plugin, [f"-load={self.plugin}"])
        self.assertEqual(sdk["fixture_arguments"], ["-std=c++26", "-resource-dir=/native/clang/22"])

    def test_verification_is_separate_and_linux_analysis_starts_one_driver_and_one_analyzer(self):
        self.verified()
        self.assertEqual(self.run_phase("analyze"), 0)
        self.assertEqual(len(self.calls), 1)
        self.assertEqual(self.report()["outcome"], "clean")
        self.assertEqual(self.report()["identity"]["source"], self.context["source"])
        command = self.calls[0]
        config = next(arg for arg in command if arg.startswith("-config="))
        self.assertIn(self.root.as_posix(), config)
        self.assertIn(PROJECT_ROOT.as_posix(), config)
        self.assertIn("bugprone-*", config)
        self.assertIn("AobusSourceRoots", config)
        self.assertEqual(self.report()["tool_exit"], 0)
        self.assertEqual(len(self.driver_calls), 1)
        # The temporary database directory never becomes an output.
        self.assertEqual(sorted(path.name for path in self.root.glob(".db-*")), [])

    def test_linux_wrapper_additions_preserve_fortify_macro_and_nonmacro_order(self):
        self.context["argv"] = [
            "/native/clang++",
            "-O2",
            "-D_FORTIFY_SOURCE=0",
            "-U_LIBCPP_HARDENING_MODE",
            "-c",
            "lib/sample/Sample.cpp",
        ]
        self.verified()
        # These section spellings/order are the pinned wrapper's NIX_DEBUG=1
        # output, not a second mapping from configuration mode to hardening.
        self.driver_output = (
            "HARDENING: enabling fortify\n"
            "extra flags before to /nix/store/w3ifgwjmsvy8dwlfir4i4jz17kzxr1xb-clang-22.1.8/bin/clang++:\n"
            "  -D_LIBCPP_HARDENING_MODE=_LIBCPP_HARDENING_MODE_EXTENSIVE\n"
            "  -fstack-protector-strong\n"
            "  --param\n"
            "  ssp-buffer-size=4\n"
            "  -O2\n"
            "  -U_FORTIFY_SOURCE\n"
            "original flags to /nix/store/w3ifgwjmsvy8dwlfir4i4jz17kzxr1xb-clang-22.1.8/bin/clang++:\n"
            "  -O2\n  -D_FORTIFY_SOURCE=0\n  -U_LIBCPP_HARDENING_MODE\n  -c\n  lib/sample/Sample.cpp\n  -###\n"
            "extra flags after to /nix/store/w3ifgwjmsvy8dwlfir4i4jz17kzxr1xb-clang-22.1.8/bin/clang++:\n"
            "  -U_FORTIFY_SOURCE\n  -D_FORTIFY_SOURCE=2\n  -nostdlibinc\n"
            "clang version 22.1.8\n"
        )
        hardening = "fortify libcxxhardeningextensive stackprotector"
        with patch.dict(os.environ, {"NIX_HARDENING_ENABLE_x86_64_unknown_linux_gnu": hardening, "NIX_DEBUG": "7"}):
            self.assertEqual(self.run_phase("analyze"), 0)
            self.assertEqual(os.environ["NIX_DEBUG"], "7")
        argv = self.database["arguments"]
        self.assertEqual(
            argv,
            [
                "/native/clang++",
                "-D_LIBCPP_HARDENING_MODE=_LIBCPP_HARDENING_MODE_EXTENSIVE",
                "-fstack-protector-strong",
                "--param",
                "ssp-buffer-size=4",
                "-O2",
                "-U_FORTIFY_SOURCE",
                "-O2",
                "-D_FORTIFY_SOURCE=0",
                "-U_LIBCPP_HARDENING_MODE",
                "-c",
                str(self.root / "lib/sample/Sample.cpp"),
                "-U_FORTIFY_SOURCE",
                "-D_FORTIFY_SOURCE=2",
                "-nostdlibinc",
            ],
        )
        self.assertEqual(json.loads(Path("context.json").read_text(encoding="utf-8"))["argv"], self.context["argv"])
        self.assertNotIn("-###", argv)
        # Windows canonicalizes os.environ keys before the driver sees them.
        key = "NIX_HARDENING_ENABLE_x86_64_unknown_linux_gnu"
        self.assertEqual(self.driver_environments[0][key.upper() if os.name == "nt" else key], hardening)
        self.assertEqual(self.driver_environments[0]["NIX_DEBUG"], "1")
        self.assertEqual(len(self.calls), 1)

    def test_linux_wrapper_shell_quoting_and_empty_sections_are_lossless(self):
        self.verified()
        self.driver_output = self.driver_trace(
            before=['-DNOTE="two words"'], after=["-isystem", "/nix/store/include path"]
        )
        self.driver_output = self.driver_output.replace(shlex.quote('-DNOTE="two words"'), r"-DNOTE=\"two\ words\"")
        self.driver_output = self.driver_output.replace(
            shlex.quote("/nix/store/include path"), r"/nix/store/include\ path"
        )
        self.assertEqual(self.run_phase("analyze"), 0)
        argv = self.database["arguments"]
        self.assertEqual(argv[1], '-DNOTE="two words"')
        self.assertEqual(argv[-2:], ["-isystem", "/nix/store/include path"])
        self.driver_output = self.driver_trace()
        self.assertEqual(self.run_phase("analyze"), 0)
        argv = self.database["arguments"]
        self.assertNotIn("", argv)
        self.assertEqual(argv[1:-1], self.context["argv"][1:-1])

    def test_invalid_or_failed_wrapper_trace_never_completes_an_analysis_report(self):
        self.verified()
        trace = self.driver_trace()
        compiler = "/nix/store/w3ifgwjmsvy8dwlfir4i4jz17kzxr1xb-clang-22.1.8/bin/clang++"
        after_header = f"extra flags after to {compiler}:"
        invalid = (
            "",
            trace.replace("extra flags before", "extra flags missing"),
            trace + trace,
            trace.replace(after_header, "extra flags after to /usr/bin/clang++:"),
            trace.replace(after_header, after_header.replace("w3if", "xxxx")),
            trace.replace("  ''", "  -O2 -fPIC", 1),
            trace.replace("  ''", "  $'unsupported\\nquoting'", 1),
            trace.replace("  ''", "  'unterminated", 1),
            self.driver_trace(original=[*self.context["argv"][1:-1], "-###"]),
            self.driver_trace(before=["", "-fPIC"]),
        )
        unresolved = self.driver_trace(after=["@unresolved.params"])
        for output, status in [*((item, 0) for item in invalid), (unresolved, 0), (trace, 1), (trace, -11)]:
            with self.subTest(output=output, status=status):
                self.driver_output, self.driver_status = output, status
                Path("result.report.json").write_text('{"outcome":"clean"}', encoding="utf-8")
                self.assertEqual(self.run_phase("analyze"), 2)
                self.assertFalse(Path("result.report.json").exists())
                if output == unresolved:
                    # A complete trace hands its additions to the compile-context guard.
                    self.assertIn("unsupported response/module/PCH", self.stderr.getvalue())
                else:
                    tail = f"native wrapper driver trace tail:\n{action._log_tail(output)}"
                    self.assertIn(tail, self.stderr.getvalue())
                self.assertEqual(self.calls, [])

    def test_policy_check_mode_header_filter_and_sdk_arguments_reach_the_actual_analysis(self):
        self.verified()
        self.policy["check"] = "aobus-example"
        self.policy["header_filter"] = "explicit-project-filter"
        self.context["mode"] = "RELAXED"
        self.write_inputs()
        self.assertEqual(self.run_phase("analyze"), 0)
        config = next(arg for arg in self.calls[-1] if arg.startswith("-config="))
        self.assertIn("-*,aobus-example,", config)
        self.assertIn("-readability-magic-numbers", config)
        self.assertNotIn("bugprone-*", config)
        self.assertIn("-header-filter=explicit-project-filter", self.calls[-1])
        self.assertEqual(self.report()["identity"]["mode"], "RELAXED")
        self.sdk["system_arguments"] = ["-isystem/native/stdlib"]
        self.sdk["analysis_arguments"] = ["-resource-dir=/native/clang/22"]
        self.verified()
        self.assertEqual(self.run_phase("analyze"), 0)
        self.assertIn("--extra-arg-before=-isystem/native/stdlib", self.calls[-1])
        self.assertIn("--extra-arg-before=-resource-dir=/native/clang/22", self.calls[-1])

    def analyzer_identity(self, main=None):
        return action.Identity(
            "@@//lib/sample:sample",
            "bazel-out/k8-dbg/bin",
            main or "lib/sample/Sample.cpp",
            main or "lib/sample/Sample.cpp",
            ("bazel-out/k8-dbg/bin/lib/sample/_objs/sample/Sample.o",),
            "ANALYZER",
            "linux",
        )

    def run_analyzer_mode(self):
        self.context["mode"] = "ANALYZER"
        self.write_inputs()
        self.verified()
        return self.run_phase("analyze")

    def analyzer_config(self):
        return next(arg for arg in self.calls[-1] if arg.startswith("-config=")).removeprefix("-config=")

    def test_analyzer_mode_uses_the_command_check_groups_in_a_minimal_config(self):
        self.assertEqual(self.run_analyzer_mode(), 0)
        expected = "{Checks: '" + ",".join(["-*", *action._ANALYZER_CHECK_GROUPS]) + "'}"
        self.assertEqual(self.analyzer_config(), expected)
        # Alpha stays opt-in: the default profile never admits the experimental
        # group, whatever the producer constant lists.
        self.assertNotIn("clang-analyzer-alpha.*", self.analyzer_config())
        # The lint naming/style options never reach the analyzer profile.
        self.assertNotIn("CheckOptions", self.analyzer_config())
        self.assertNotIn("NOLINT", Path("result.log").read_text(encoding="utf-8"))
        self.assertEqual(self.report()["identity"]["mode"], "ANALYZER")

    def test_analyzer_alpha_and_check_override_replace_the_groups(self):
        for policy, expected in (
            ({**self.policy, "alpha": True}, "clang-analyzer-alpha.*"),
            ({**self.policy, "check": "clang-analyzer-core.NullDereference"}, ""),
            # An explicit check wins over a requested alpha group.
            ({**self.policy, "alpha": True, "check": "clang-analyzer-core.NullDereference"}, ""),
        ):
            with self.subTest(policy=policy):
                self.policy.clear()
                self.policy.update(policy)
                self.context["mode"] = "ANALYZER"
                self.write_inputs()
                self.verified()
                self.assertEqual(self.run_phase("analyze"), 0)
                config = self.analyzer_config()
                if expected:
                    self.assertIn(expected, config)
                else:
                    self.assertEqual(config, "{Checks: '-*,clang-analyzer-core.NullDereference'}")

    def test_analyzer_test_main_leak_exception_applies_only_to_default_group_scopes(self):
        self.context["main"] = self.context["source"] = "test/unit/SpanTest.cpp"
        self.context["argv"][-1] = self.context["source"]
        Path(self.context["main"]).parent.mkdir(parents=True, exist_ok=True)
        Path(self.context["main"]).write_text("// declared input\n", encoding="utf-8")
        self.assertEqual(self.run_analyzer_mode(), 0)
        self.assertIn(",-clang-analyzer-cplusplus.NewDeleteLeaks", self.analyzer_config())
        self.policy["check"] = "clang-analyzer-core.NullDereference"
        self.context["mode"] = "ANALYZER"
        self.write_inputs()
        self.verified()
        self.assertEqual(self.run_phase("analyze"), 0)
        self.assertEqual(self.analyzer_config(), "{Checks: '-*,clang-analyzer-core.NullDereference'}")
        # Production mains never receive the test exception, not even in the
        # default group scope.
        self.policy["check"] = None
        self.context["main"] = self.context["source"] = "lib/sample/Sample.cpp"
        self.context["argv"][-1] = self.context["source"]
        self.assertEqual(self.run_analyzer_mode(), 0)
        self.assertNotIn("NewDeleteLeaks", self.analyzer_config())

    def test_analyzer_alpha_and_test_exception_combine_for_test_mains(self):
        self.context["main"] = self.context["source"] = "test/unit/SpanTest.cpp"
        self.context["argv"][-1] = self.context["source"]
        Path(self.context["main"]).parent.mkdir(parents=True, exist_ok=True)
        Path(self.context["main"]).write_text("// declared input\n", encoding="utf-8")
        self.policy["alpha"] = True
        self.assertEqual(self.run_analyzer_mode(), 0)
        config = self.analyzer_config()
        self.assertIn("clang-analyzer-alpha.*", config)
        self.assertIn(",-clang-analyzer-cplusplus.NewDeleteLeaks", config)

    def test_analyzer_mode_skips_the_lint_stale_suppression_rule(self):
        Path(self.context["main"]).write_text("// NOLINT(bugprone-easily-swappable-parameters)\n", encoding="utf-8")
        self.assertEqual(self.run_analyzer_mode(), 0)
        self.assertEqual(self.report()["outcome"], "clean")
        self.assertEqual(Path("result.log").read_text(encoding="utf-8"), "")

    def test_analyzer_header_filter_covers_the_command_project_folders(self):
        virtual = {
            "bazel-out/x/_virtual_includes/sample/lib/util/Span.h": "lib/util/Span.h",
            "bazel-out/x/_virtual_includes/other/tool/other/A.h": "tool/other/A.h",
            "bazel-out/x/_virtual_includes/gen/lib/generated/G.h": None,
        }
        expression = action._header_filter(self.analyzer_identity(), self.policy, (self.root,), virtual)
        accepted = (
            "lib/util/Span.h",
            "app/linux-gtk/Window.h",
            "include/ao/util/Span.h",
            "test/unit/SpanTest.h",
            "tool/lint/Runner.h",
            self.root.as_posix() + "/lib/util/Span.h",
            "bazel-out/x/_virtual_includes/sample/lib/util/Span.h",
        )
        for path in accepted:
            self.assertIsNotNone(re.search(expression, path), path)
        for path in (
            "tool/other/A.h",
            "external/dependency/Header.h",
            "bazel-out/x/_virtual_includes/other/tool/other/A.h",
            "bazel-out/x/_virtual_includes/gen/lib/generated/G.h",
        ):
            self.assertIsNone(re.search(expression, path), path)

    def test_analyzer_mode_keeps_frontend_arguments_without_the_lint_suffix_or_line_filter(self):
        self.context["source"] = self.context["main"] = "app/linux-gtk/Window.cpp"
        self.context["argv"][-1] = self.context["source"]
        Path(self.context["source"]).parent.mkdir(parents=True, exist_ok=True)
        Path(self.context["source"]).write_text("// declared input\n", encoding="utf-8")
        self.assertEqual(self.run_analyzer_mode(), 0)
        self.assertIn("--extra-arg=-Wno-format-nonliteral", self.calls[-1])
        self.assertNotIn("-cppcoreguidelines-owning-memory", self.analyzer_config())
        self.assertFalse(any(argument.startswith("-line-filter=") for argument in self.calls[-1]))

    def test_analyzer_mode_still_selects_an_include_fragment_inside_its_consumer(self):
        self.context["main"] = "lib/sample/Foo.def"
        Path(self.context["main"]).write_text("// fragment\n", encoding="utf-8")
        self.included = [self.context["main"]]
        self.assertEqual(self.run_analyzer_mode(), 0)
        self.assertTrue(any(argument.startswith("-line-filter=") for argument in self.calls[-1]))
        self.assertEqual(self.report()["identity"]["main"], "lib/sample/Foo.def")

    def test_relaxed_stale_suppression_warns_in_actual_and_generated_main_reports_after_analysis(self):
        self.context["mode"] = "RELAXED"
        self.verified()
        original_source = self.context["source"]
        for main in (original_source, "bazel-out/k8-dbg/bin/lib/sample/generated/Sample.cpp"):
            with self.subTest(main=main):
                path = Path(main)
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text("// declared main\n// NOLINT(readability-magic-numbers)\n", encoding="utf-8")
                self.context["source"] = self.context["main"] = main
                self.context["argv"][-1] = main
                self.write_inputs()
                self.calls.clear()
                self.assertEqual(self.run_phase("analyze"), 0)
                self.assertEqual(len(self.calls), 1)
                self.assertEqual(self.calls[0][-1], str(path.absolute()))
                self.assertEqual(self.report()["outcome"], "warning")
                self.assertEqual(self.report()["tool_exit"], 0)
                self.assertEqual(self.report()["identity"]["main"], main)
                self.assertEqual(
                    Path("result.log").read_text(encoding="utf-8"),
                    f"{main}:2:1: warning: NOLINT names disabled check readability-magic-numbers "
                    "[aobus-readability-unused-suppression-style]\n",
                )

    def test_strict_stale_suppression_uses_the_real_mode_disabled_checks(self):
        self.verified()
        main = Path(self.context["main"])
        for check, outcome in (("readability-magic-numbers", "clean"), ("modernize-use-auto", "warning")):
            with self.subTest(check=check):
                main.write_text(f"// NOLINT({check})\n", encoding="utf-8")
                self.calls.clear()
                self.assertEqual(self.run_phase("analyze"), 0)
                self.assertEqual(len(self.calls), 1)
                self.assertEqual(self.report()["outcome"], outcome)
                self.assertEqual(self.report()["tool_exit"], 0)
                log = Path("result.log").read_text(encoding="utf-8")
                if outcome == "clean":
                    self.assertEqual(log, "")
                else:
                    self.assertIn(f"NOLINT names disabled check {check}", log)

    def test_stale_suppression_scans_the_actual_header_main_and_preserves_raw_tool_output(self):
        self.context["mode"] = "RELAXED"
        self.context["main"] = "include/ao/sample/Sample.h"
        Path(self.context["source"]).write_text("// NOLINT(readability-magic-numbers)\n", encoding="utf-8")
        self.verified()
        self.assertEqual(self.run_phase("analyze"), 0)
        self.assertEqual(self.report()["outcome"], "clean")
        header = Path(self.context["main"])
        header.write_text("// NOLINT(readability-magic-numbers)\n", encoding="utf-8")
        self.analysis_output = "Analyzer trace without final newline"
        self.assertEqual(self.run_phase("analyze"), 0)
        self.assertEqual(self.report()["outcome"], "warning")
        self.assertEqual(
            Path("result.log").read_text(encoding="utf-8"),
            "Analyzer trace without final newline\n"
            f"{self.context['main']}:1:1: warning: NOLINT names disabled check readability-magic-numbers "
            "[aobus-readability-unused-suppression-style]\n",
        )

    def test_completed_warnings_and_ordinary_compiler_errors_are_successful_negative_actions(self):
        self.verified()
        for output, status, outcome in (
            ("include/ao/sample/Sample.h:2:3: warning: prefer prefix [aobus-example]\n", 0, "warning"),
            (
                "lib/sample/Sample.cpp:2:3: error: unknown type [clang-diagnostic-error]\n"
                "Error while processing lib/sample/Sample.cpp.\n",
                1,
                "compiler_error",
            ),
            ("lib/sample/Sample.cpp:2:3: fatal error: missing header [clang-diagnostic-error]\n", 0, "compiler_error"),
        ):
            with self.subTest(outcome=outcome, status=status):
                self.analysis_output, self.analysis_status = output, status
                self.assertEqual(self.run_phase("analyze"), 0)
                self.assertEqual(self.report()["outcome"], outcome)
                self.assertEqual(Path("result.log").read_text(encoding="utf-8"), output)

    def test_processing_summaries_are_checkout_neutral_without_rewriting_other_text(self):
        source = "lib/sample/Résumé file.cpp"
        Path(source).write_text("// declared input\n", encoding="utf-8")
        self.context["source"] = self.context["main"] = source
        self.context["argv"][-1] = source
        for windows in (False, True):
            with self.subTest(windows=windows):
                if windows:
                    self.use_windows_sdk()
                self.verified()
                root = self.root.as_posix().upper() if windows else self.root.as_posix()
                absolute = root + "/" + source
                if windows:
                    absolute = absolute.replace("/", "\\")
                foreign = self.root.parent.as_posix() + "/" + self.root.name + "-foreign/Sample.cpp"
                unchanged = (
                    f"    2 | // Error while processing {absolute}.\n"
                    f'Analyzer trace: "Error while processing {absolute}."\n'
                    f"Error while processing {foreign}.\n"
                    f"Error while processing {absolute}. trailing text"
                )
                self.analysis_status = 1
                self.analysis_output = (
                    f"Error while processing {absolute}.\n"
                    f"{absolute}:2:3: error: invalid source [clang-diagnostic-error]\n" + unchanged
                )
                self.assertEqual(self.run_phase("analyze"), 0)
                self.assertEqual(self.report()["outcome"], "compiler_error")
                self.assertEqual(self.report()["tool_exit"], 1)
                self.assertEqual(
                    Path("result.log").read_text(encoding="utf-8"),
                    f"Error while processing {source}.\n"
                    f"{source}:2:3: error: invalid source [clang-diagnostic-error]\n" + unchanged,
                )

    def test_processing_summaries_follow_the_same_virtual_header_mapping_as_diagnostics(self):
        header = "include/ao/sample/Sample.h"
        virtual = "bazel-out/k8-dbg/bin/lib/sample/_virtual_includes/headers/ao/sample/Sample.h"
        self.context["main"] = header
        self.context["virtual_headers"] = {virtual: header}
        self.verified()
        self.analysis_status = 1
        self.analysis_output = (
            f"Error while processing {self.root.as_posix()}/{virtual}.\n"
            f"{self.root.as_posix()}/{virtual}:2:3: error: invalid header [clang-diagnostic-error]\n"
        )
        self.assertEqual(self.run_phase("analyze"), 0)
        self.assertEqual(self.report()["outcome"], "compiler_error")
        self.assertEqual(
            Path("result.log").read_text(encoding="utf-8"),
            f"Error while processing {header}.\n{header}:2:3: error: invalid header [clang-diagnostic-error]\n",
        )

    def test_incomplete_or_crashed_attempt_removes_any_prior_positive_report(self):
        self.verified()
        for output, status in (
            ("", -11),
            ("", 1),
            ("PLEASE submit a bug report\n", 0),
            ("Error while trying to load a compilation database\n", 0),
            ("Skipping lib/sample/Sample.cpp. Compile command not found.\n", 0),
            ("No checks enabled.\n", 0),
            ("Error while processing lib/sample/Sample.cpp.\n", 0),
            (f"Error while processing {self.root}/lib/sample/Sample.cpp.\n", 1),
            ("error: invalid configuration\n", 1),
            ("lib/sample/Sample.cpp:2:3: error: bad [clang-diagnostic-error]\nStack dump:\n", 1),
        ):
            with self.subTest(output=output, status=status):
                Path("result.report.json").write_text('{"outcome":"clean"}', encoding="utf-8")
                self.analysis_output, self.analysis_status = output, status
                self.assertEqual(self.run_phase("analyze"), 2)
                self.assertFalse(Path("result.report.json").exists())

    def test_diagnostic_text_mentioning_skipping_or_errors_is_not_an_infrastructure_banner(self):
        self.verified()
        self.analysis_output = (
            "lib/sample/Sample.cpp:1:1: warning: skipping is dangerous [aobus-example]\n"
            "    1 | // Skipping fake.cpp\n"
            "lib/sample/Sample.cpp:1:1: note: fake.cpp:1:1: error: quoted text\n"
        )
        self.assertEqual(self.run_phase("analyze"), 0)
        self.assertEqual(self.report()["outcome"], "warning")

    def test_header_is_the_main_file_with_explicit_language_and_semantic_forced_include(self):
        self.context["main"] = "include/ao/sample/Sample.h"
        self.context["argv"].extend(["-fobjc-arc", "-isysroot", "/native/MacOS.sdk"])
        self.verified()
        self.assertEqual(self.run_phase("analyze"), 0)
        database = self.database
        argv = database["arguments"]
        self.assertIn("semantic.h", argv)
        self.assertIn("-DAOBUS_LINT_HEADER_MAIN=1", argv)
        self.assertIn("-fobjc-arc", argv)
        self.assertIn("/native/MacOS.sdk", argv)
        self.assertNotIn(self.context["source"], argv)
        self.assertEqual(
            argv[argv.index("-x") : argv.index("-x") + 3], ["-x", "c++-header", str(self.root / self.context["main"])]
        )
        self.assertEqual(self.report()["identity"]["main"], self.context["main"])
        self.assertEqual(self.calls[0][-1], str(self.root / self.context["main"]))
        self.assertNotIn(".*", next(arg for arg in self.calls[0] if arg.startswith("-header-filter=")))

    def test_reports_record_the_checkout_files_the_analysis_included(self):
        virtual = "bazel-out/k8-dbg/bin/lib/sample/_virtual_includes/sample/ao/Virtual.h"
        self.context["virtual_headers"] = {virtual: "include/ao/Virtual.h"}
        self.included = [
            "./include/ao/sample/Sample.h",
            f"{self.root.as_posix()}/semantic.h",
            virtual,
            "bazel-out/k8-dbg/bin/lib/sample/Generated.h",
            "external/dependency/Dependency.h",
            "/usr/include/stdio.h",
            "./include/ao/sample/Sample.h",
            # The listing escapes literal POSIX filename backslashes too.
            "lib\\\\sample/Escaped.h",
            self.root.as_posix() + "/lib\\\\sample\\\\Rooted.h",
        ]
        self.verified()
        self.assertEqual(self.run_phase("analyze"), 0)
        self.assertEqual(
            self.report()["includes"],
            [
                "include/ao/Virtual.h",
                "include/ao/sample/Sample.h",
                r"lib\sample/Escaped.h",
                r"lib\sample\Rooted.h",
                "semantic.h",
            ],
        )

    def test_windows_include_listing_normalizes_escaped_native_separators(self):
        self.use_windows_sdk()
        self.included = [
            "lib\\\\sample/Escaped.h",
            self.root.as_posix().replace("/", "\\\\") + "\\\\lib\\\\sample\\\\Rooted.h",
        ]
        self.verified()
        self.assertEqual(self.run_phase("analyze"), 0)
        self.assertEqual(self.report()["includes"], ["lib/sample/Escaped.h", "lib/sample/Rooted.h"])

    def test_fragment_is_reported_inside_its_consumer_translation_unit(self):
        fragment = "include/ao/sample/Table.def"
        alias = "bazel-out/k8-dbg/bin/include/_virtual_includes/sample/ao/sample/Table.def"
        (self.root / fragment).write_text("// fragment\n", encoding="utf-8")
        self.context["main"] = fragment
        self.context["virtual_headers"] = {alias: fragment}
        self.included = [alias]
        self.verified()
        self.assertEqual(self.run_phase("analyze"), 0)
        argv = self.database["arguments"]
        self.assertEqual(self.calls[-1][-1], str(self.root / self.context["source"]))
        self.assertIn(str(self.root / self.context["source"]), argv)
        self.assertNotIn("-x", argv)
        self.assertFalse(any("AOBUS_LINT_HEADER_MAIN" in argument for argument in argv))
        line_filter = next(arg for arg in self.calls[-1] if arg.startswith("-line-filter="))
        # A Windows include directory and include token can mix separators.
        self.assertEqual(
            [entry["name"] for entry in json.loads(line_filter.removeprefix("-line-filter="))], ["Table.def"]
        )
        for spelling in (fragment, "include\\ao/sample/Table.def", alias, self.root.as_posix() + "/" + fragment):
            self.assertRegex(spelling, self.header_filter())
        self.assertEqual(self.header_filter(), ".*")
        self.assertEqual(self.report()["identity"]["main"], fragment)
        self.assertEqual(self.report()["includes"], [fragment])

    def test_fragment_warnings_use_normalized_locations_not_lexical_include_spellings(self):
        fragment = "include/ao/sample/Table.def"
        (self.root / fragment).write_text("// fragment\n", encoding="utf-8")
        self.context["main"] = fragment
        lexical = "include/ao/sample/detail/../Table.def"
        self.included = [lexical]
        self.analysis_output = (
            "include/ao/other/Table.def:1:1: warning: other fragment [aobus-example]\n"
            "    1 | other declaration\n"
            "include/ao/other/Context.h:2:1: note: other note\n"
            f"{self.root.as_posix()}/{lexical}:3:1: warning: selected fragment [aobus-example]\n"
            "    3 | selected declaration\n"
            "lib/sample/Sample.cpp:4:1: note: selected note\n"
            "lib/sample/Sample.cpp:5:1: warning: consumer [aobus-example]\n"
        )
        self.verified()
        self.assertEqual(self.run_phase("analyze"), 0)
        self.assertEqual(self.report()["outcome"], "warning")
        self.assertEqual(self.report()["includes"], [fragment])
        text = Path("result.log").read_text(encoding="utf-8")
        self.assertIn(f"{fragment}:3:1: warning: selected fragment", text)
        self.assertIn("selected note", text)
        self.assertNotIn("other", text)
        self.assertNotIn("warning: consumer", text)

    def test_fragment_unlocated_warnings_cannot_be_reinterpreted_as_quoted_locations(self):
        fragment = "include/ao/sample/Table.def"
        (self.root / fragment).write_text("// fragment\n", encoding="utf-8")
        self.context["main"] = fragment
        self.included = [fragment]
        self.analysis_output = (
            "warning: quoted other/Table.def:1:1: warning: text [aobus-example]\n"
            "clang-tidy: warning: quoted other/Table.def(1,1): warning: text [aobus-example]\n"
        )
        self.verified()
        self.assertEqual(self.run_phase("analyze"), 0)
        self.assertEqual(self.report()["outcome"], "warning")
        self.assertEqual(Path("result.log").read_text(encoding="utf-8"), self.analysis_output)

    def test_fragment_ignores_other_file_warnings_but_keeps_consumer_errors(self):
        fragment = "include/ao/sample/Table.def"
        (self.root / fragment).write_text("// fragment\n", encoding="utf-8")
        self.context["main"] = fragment
        self.included = [fragment]
        self.verified()
        other = "include/ao/other/Table.def:1:1: warning: unrelated [aobus-example]\n"
        self.analysis_output = other
        self.assertEqual(self.run_phase("analyze"), 0)
        self.assertEqual(self.report()["outcome"], "clean")
        self.assertEqual(Path("result.log").read_text(encoding="utf-8"), "")
        self.analysis_status = 1
        self.analysis_output = other + "lib/sample/Sample.cpp:2:1: error: invalid source [clang-diagnostic-error]\n"
        self.assertEqual(self.run_phase("analyze"), 0)
        self.assertEqual(self.report()["outcome"], "compiler_error")
        self.assertIn("invalid source", Path("result.log").read_text(encoding="utf-8"))
        self.assertNotIn("unrelated", Path("result.log").read_text(encoding="utf-8"))

    def test_fragment_scope_cannot_hide_an_incomplete_attempt_after_an_unrelated_warning(self):
        fragment = "include/ao/sample/Table.def"
        (self.root / fragment).write_text("// fragment\n", encoding="utf-8")
        self.context["main"] = fragment
        self.included = [fragment]
        self.analysis_output = (
            "include/ao/other/Table.def:1:1: warning: unrelated [aobus-example]\nLLVM ERROR: incomplete attempt\n"
        )
        self.verified()
        self.assertEqual(self.run_phase("analyze"), 2)
        self.assertFalse(Path("result.report.json").exists())
        self.assertIn("LLVM ERROR", self.stderr.getvalue())

    def test_fragment_consumer_that_never_included_it_completes_no_report(self):
        fragment = "include/ao/sample/Table.def"
        (self.root / fragment).write_text("// fragment\n", encoding="utf-8")
        self.context["main"] = fragment
        self.included = ["include/ao/sample/Sample.h"]
        self.verified()
        self.assertEqual(self.run_phase("analyze"), 2)
        self.assertFalse(Path("result.report.json").exists())
        self.assertIn("did not include fragment", self.stderr.getvalue())

    def test_source_replay_never_enables_the_header_main_pch_mode(self):
        self.verified()
        self.assertEqual(self.run_phase("analyze"), 0)
        database = self.database
        self.assertFalse(any("AOBUS_LINT_HEADER_MAIN" in argument for argument in database["arguments"]))
        self.assertIn("semantic.h", database["arguments"])

    def test_reserved_header_mode_cannot_be_overridden_by_native_compile_arguments(self):
        self.verified()
        original = self.context["argv"][:]
        for main in (self.context["source"], "include/ao/sample/Sample.h"):
            for override in (
                ["-DAOBUS_LINT_HEADER_MAIN=0"],
                ["/DAOBUS_LINT_HEADER_MAIN"],
                ["-D", "AOBUS_LINT_HEADER_MAIN=1"],
                ["/clang:-DAOBUS_LINT_HEADER_MAIN=0"],
                ["-Xclang=-DAOBUS_LINT_HEADER_MAIN=0"],
                ["-DAOBUS_LINT_HEADER_MAIN()=1"],
                ["-Wp,-DAOBUS_LINT_HEADER_MAIN=1"],
                ["-Xclang", "-D", "-Xclang", "AOBUS_LINT_HEADER_MAIN=1"],
                ["/clang:-D", "/clang:AOBUS_LINT_HEADER_MAIN=1"],
                ["-UAOBUS_LINT_HEADER_MAIN"],
                ["/U", "AOBUS_LINT_HEADER_MAIN"],
            ):
                with self.subTest(main=main, override=override):
                    self.context["main"] = main
                    self.context["argv"] = [*original, *override]
                    self.write_inputs()
                    self.assertEqual(self.run_phase("analyze"), 2)
                    self.assertFalse(Path("result.report.json").exists())
        self.assertEqual(self.calls, [])
        self.assertEqual(self.driver_calls, [])

    def test_objc_header_retains_arc_sdk_and_uses_objective_cpp_header_language(self):
        Path("lib/sample/Sample.mm").write_text("// objc\n", encoding="utf-8")
        self.context["source"] = "lib/sample/Sample.mm"
        self.context["main"] = "include/ao/sample/Sample.h"
        self.context["argv"] = [_APPLE_ENTRY, "-fobjc-arc", "-isysroot", "/native/sdk", "lib/sample/Sample.mm"]
        self.context["platform"] = self.sdk["platform"] = "macos"
        self.verified()
        with patch.dict(os.environ, {"APPLE_SUPPORT_LLVM_ROOT": "/native/llvm"}):
            self.assertEqual(self.run_phase("analyze"), 0)
        argv = self.database["arguments"]
        self.assertEqual(argv[2:5], ["-fobjc-arc", "-isysroot", "/native/sdk"])
        self.assertIn("objective-c++-header", argv)

    def test_macos_analysis_names_the_llvm_compiler_that_the_apple_entry_executes(self):
        self.context["platform"] = self.sdk["platform"] = "macos"
        for entry, tool in ((_APPLE_ENTRY, "clang++"), (_APPLE_ENTRY.removesuffix("_pp"), "clang")):
            with self.subTest(entry=entry):
                self.context["argv"] = [entry, "-std=c++26", "-c", "lib/sample/Sample.cpp"]
                self.verified()
                with patch.dict(os.environ, {"APPLE_SUPPORT_LLVM_ROOT": "/native/llvm"}):
                    self.assertEqual(self.run_phase("analyze"), 0)
                self.assertEqual(
                    self.database["arguments"],
                    [
                        f"/native/llvm/bin/{tool}",
                        "--no-default-config",
                        "-std=c++26",
                        "-c",
                        str(self.root / "lib/sample/Sample.cpp"),
                    ],
                )
                self.assertEqual(json.loads(Path("context.json").read_text(encoding="utf-8"))["argv"][0], entry)
                self.assertEqual(self.driver_calls, [])

    def test_macos_analysis_without_the_apple_entry_or_an_absolute_llvm_root_completes_no_report(self):
        self.context["platform"] = self.sdk["platform"] = "macos"
        for entry, environment in (
            ("/native/llvm/bin/clang++", {"APPLE_SUPPORT_LLVM_ROOT": "/native/llvm"}),
            (_APPLE_ENTRY, {}),
            (_APPLE_ENTRY, {"APPLE_SUPPORT_LLVM_ROOT": "native/llvm"}),
        ):
            with self.subTest(entry=entry, environment=environment):
                self.context["argv"] = [entry, "-c", "lib/sample/Sample.cpp"]
                self.verified()
                Path("result.report.json").write_text('{"outcome":"clean"}', encoding="utf-8")
                with patch.dict(os.environ, environment):
                    if not environment:
                        os.environ.pop("APPLE_SUPPORT_LLVM_ROOT", None)
                    self.assertEqual(self.run_phase("analyze"), 2)
                self.assertFalse(Path("result.report.json").exists())
                self.assertEqual(self.calls, [])

    def test_windows_linked_host_retains_fi_pch_header_and_only_drops_audited_codegen_flags(self):
        self.context["platform"] = self.sdk["platform"] = "windows"
        resource = self.root / "sdk/lib/clang/22"
        (resource / "include").mkdir(parents=True)
        self.sdk["resource_dir"] = str(resource)
        self.sdk["host_kind"] = "standalone"
        self.sdk["analysis_arguments"] = [f"-resource-dir={resource}", "-D_USE_STD_VECTOR_ALGORITHMS=0"]
        self.context["argv"] = [
            "cl.exe",
            "/c",
            "/GL",
            "/GL-",
            "/FIpch.h",
            "/fp:precise",
            "/W4",
            "/WX",
            "/MDd",
            "/Od",
            "/Z7",
            "/Iinclude",
            '/D__DATE__="redacted"',
            self.context["source"],
        ]
        self.verified()
        self.assertEqual(self.run_phase("analyze"), 0)
        argv = self.database["arguments"]
        self.assertIn("/FIpch.h", argv)
        self.assertFalse(any("AOBUS_LINT_HEADER_MAIN" in argument for argument in argv))
        self.assertIn("/fp:precise", argv)
        self.assertNotIn("/GL", argv)
        self.assertNotIn("/c", argv)
        self.assertNotIn("/WX", argv)
        for argument in ("/W4", "/MDd", "/Od", "/Z7", "/Iinclude", '/D__DATE__="redacted"'):
            self.assertIn(argument, argv)
        self.assertEqual(json.loads(Path("context.json").read_text(encoding="utf-8"))["argv"], self.context["argv"])
        self.assertIn("--extra-arg-before=-D_USE_STD_VECTOR_ALGORITHMS=0", self.calls[0])
        self.assertFalse(any(arg.startswith("-load") for arg in self.calls[0]))
        self.context["main"] = "include/ao/sample/Sample.h"
        self.write_inputs()
        self.assertEqual(self.run_phase("analyze"), 0)
        header = self.database
        self.assertEqual(header["file"], str(Path(self.context["main"]).absolute()))
        self.assertIn("/FIpch.h", header["arguments"])
        self.assertIn("-DAOBUS_LINT_HEADER_MAIN=1", header["arguments"])

    def test_header_main_drops_only_a_proven_native_self_forced_include(self):
        self.context["platform"] = self.sdk["platform"] = "windows"
        resource = self.root / "sdk/lib/clang/22"
        (resource / "include").mkdir(parents=True)
        self.sdk["resource_dir"] = str(resource)
        self.sdk["host_kind"] = "standalone"
        self.sdk["analysis_arguments"] = [f"-resource-dir={resource}", "-D_USE_STD_VECTOR_ALGORITHMS=0"]
        self.context["owner"] = "@@//app/windows-winui:winui"
        self.context["source"] = "app/windows-winui/App.xaml.cpp"
        for path in (
            "app/windows-winui/App.xaml.cpp",
            "app/windows-winui/pch.h",
            "app/windows-winui/MainWindow.h",
            "app/windows-winui/other.h",
            "other/sub/pch.h",
        ):
            target = Path(path)
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_text("// declared input\n", encoding="utf-8")
        source = self.context["source"]

        def header_arguments(main):
            return [
                "-DAOBUS_LINT_HEADER_MAIN=1",
                "-x",
                "c++-header",
                str(self.root / main),
            ]

        cases = (
            (
                "self forced include resolves through an include directory",
                "app/windows-winui/pch.h",
                ["cl.exe", "/c", "/FIpch.h", "/Iapp/windows-winui", "/W4", source],
                ["cl.exe", "/Iapp/windows-winui", "/W4", *header_arguments("app/windows-winui/pch.h")],
            ),
            (
                "split self forced include resolves through the main file directory",
                "app/windows-winui/pch.h",
                ["cl.exe", "/FI", "pch.h", "/W4", source],
                ["cl.exe", "/W4", *header_arguments("app/windows-winui/pch.h")],
            ),
            (
                "lowercase flag spelling still names the self forced include",
                "app/windows-winui/pch.h",
                ["cl.exe", "/fipch.h", source],
                ["cl.exe", *header_arguments("app/windows-winui/pch.h")],
            ),
            (
                "ordinary source replay keeps the native forced include",
                "app/windows-winui/App.xaml.cpp",
                ["cl.exe", "/FIpch.h", source],
                ["cl.exe", "/FIpch.h", str(self.root / source)],
            ),
            (
                "another header main keeps the forced prefix",
                "app/windows-winui/MainWindow.h",
                ["cl.exe", "/FIpch.h", "/Iapp/windows-winui", source],
                [
                    "cl.exe",
                    "/FIpch.h",
                    "/Iapp/windows-winui",
                    *header_arguments("app/windows-winui/MainWindow.h"),
                ],
            ),
            (
                "same basename in another file is not the selected header",
                "app/windows-winui/pch.h",
                ["cl.exe", "/FIsub/pch.h", "/Iother", source],
                ["cl.exe", "/FIsub/pch.h", "/Iother", *header_arguments("app/windows-winui/pch.h")],
            ),
            (
                "unresolved forced include fails closed",
                "app/windows-winui/pch.h",
                ["cl.exe", "/FImissing.h", source],
                ["cl.exe", "/FImissing.h", *header_arguments("app/windows-winui/pch.h")],
            ),
            (
                "value-less split flag fails closed",
                "app/windows-winui/pch.h",
                ["cl.exe", "/W4", source, "/FI"],
                ["cl.exe", "/W4", *header_arguments("app/windows-winui/pch.h"), "/FI"],
            ),
            (
                "only the proven self include drops among multiple forced includes",
                "app/windows-winui/pch.h",
                ["cl.exe", "/FIother.h", "/FIpch.h", "/Iapp/windows-winui", "/W4", source],
                [
                    "cl.exe",
                    "/FIother.h",
                    "/Iapp/windows-winui",
                    "/W4",
                    *header_arguments("app/windows-winui/pch.h"),
                ],
            ),
            (
                "include directory resolves a path-component forced include",
                "app/windows-winui/pch.h",
                ["cl.exe", "/FIwindows-winui/pch.h", "/Iapp", source],
                ["cl.exe", "/Iapp", *header_arguments("app/windows-winui/pch.h")],
            ),
            (
                "absolute include directory resolves through the physical root",
                "app/windows-winui/pch.h",
                ["cl.exe", "/FIwindows-winui/pch.h", f"/I{self.root / 'app'}", source],
                ["cl.exe", f"/I{self.root / 'app'}", *header_arguments("app/windows-winui/pch.h")],
            ),
            (
                "absolute forced include value fails closed",
                "app/windows-winui/pch.h",
                ["cl.exe", "/FI" + str(self.root / "app/windows-winui/pch.h"), source],
                [
                    "cl.exe",
                    "/FI" + str(self.root / "app/windows-winui/pch.h"),
                    *header_arguments("app/windows-winui/pch.h"),
                ],
            ),
        )
        self.verified()
        for name, main, argv, expected in cases:
            with self.subTest(case=name):
                self.context["main"] = main
                self.context["argv"] = argv
                self.write_inputs()
                self.assertEqual(self.run_phase("analyze"), 0)
                self.assertEqual(self.database["arguments"], expected)
                self.assertEqual(self.calls[-1][-1], str(self.root / main))
                self.assertEqual(self.report()["identity"]["main"], main)

    def test_working_directory_shadow_keeps_the_native_forced_include(self):
        self.context["platform"] = self.sdk["platform"] = "windows"
        resource = self.root / "sdk/lib/clang/22"
        (resource / "include").mkdir(parents=True)
        self.sdk["resource_dir"] = str(resource)
        self.sdk["host_kind"] = "standalone"
        self.sdk["analysis_arguments"] = [f"-resource-dir={resource}", "-D_USE_STD_VECTOR_ALGORITHMS=0"]
        self.context["owner"] = "@@//app/windows-winui:winui"
        self.context["source"] = "app/windows-winui/App.xaml.cpp"
        self.context["main"] = "app/windows-winui/pch.h"
        for path in ("app/windows-winui/App.xaml.cpp", "app/windows-winui/pch.h"):
            target = Path(path)
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_text("// declared input\n", encoding="utf-8")
        # The Windows driver resolves a forced include through the working
        # directory before the main file's directory, so a different
        # same-named header there is the file the native context includes.
        Path("pch.h").write_text("// a different working-directory header\n", encoding="utf-8")
        self.context["argv"] = ["cl.exe", "/FIpch.h", "/Iapp/windows-winui", self.context["source"]]
        self.verified()
        self.assertEqual(self.run_phase("analyze"), 0)
        arguments = self.database["arguments"]
        self.assertIn("/FIpch.h", arguments)
        self.assertIn("/Iapp/windows-winui", arguments)
        self.assertIn("-DAOBUS_LINT_HEADER_MAIN=1", arguments)

    def test_self_forced_include_consumer_is_windows_only(self):
        self.context["owner"] = "@@//app/windows-winui:winui"
        self.context["source"] = "app/windows-winui/App.xaml.cpp"
        self.context["main"] = "app/windows-winui/pch.h"
        for path in ("app/windows-winui/App.xaml.cpp", "app/windows-winui/pch.h"):
            target = Path(path)
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_text("// declared input\n", encoding="utf-8")
        self.context["argv"] = [
            "/native/clang++",
            "-std=c++26",
            "/FIpch.h",
            "/Iapp/windows-winui",
            self.context["source"],
        ]
        self.verified()
        self.assertEqual(self.run_phase("analyze"), 0)
        arguments = self.database["arguments"]
        self.assertIn("/FIpch.h", arguments)
        self.assertIn("/Iapp/windows-winui", arguments)
        self.assertIn("-DAOBUS_LINT_HEADER_MAIN=1", arguments)
        self.assertIn("c++-header", arguments)

    def test_wrong_version_or_incomplete_registry_never_produces_verification(self):
        for output in ("LLVM version 22.1.9\n", "LLVM version 22.1.8\n"):
            with self.subTest(output=output):

                def fake(command, output=output, **_kwargs):
                    self.assertEqual(command[0], str(self.host))
                    if command[1:] == ["--version"]:
                        return subprocess.CompletedProcess(command, 0, output)
                    self.assertEqual(command[1:], [f"-load={self.plugin}", "-checks=-*,aobus-*", "-list-checks"])
                    return subprocess.CompletedProcess(command, 0, "Enabled checks:\n")

                with (
                    patch.object(action.subprocess, "run", side_effect=fake),
                    contextlib.redirect_stderr(io.StringIO()),
                ):
                    self.assertEqual(
                        action.main(
                            [
                                "verify",
                                "--policy",
                                "policy.json",
                                "--sdk",
                                "sdk.json",
                                "--tidy",
                                str(self.host),
                                "--plugin",
                                str(self.plugin),
                                "--output",
                                "verified.json",
                                "--log",
                                "verify.log",
                            ]
                        ),
                        2,
                    )
                self.assertFalse(Path("verified.json").exists())

    @unittest.skipIf(os.name == "nt", "POSIX executable fixture; Windows argv is covered above")
    def test_script_entry_runs_a_declared_host_process_without_portal_preparation(self):
        self.context["argv"][0] = str(self.host)
        self.write_inputs()
        self.host.write_text(
            f"#!{sys.executable}\n"
            "import json, os, pathlib, shlex, sys\n"
            "args = sys.argv[1:]\n"
            "if args == ['--version']: print('LLVM version 22.1.8')\n"
            "elif args[-1:] == ['-###']:\n"
            f"    assert args == {[*self.context['argv'][1:], '-###']!r}\n"
            "    assert os.environ['NIX_DEBUG'] == '1'\n"
            "    compiler = '/nix/store/w3ifgwjmsvy8dwlfir4i4jz17kzxr1xb-clang-22.1.8/bin/clang++'\n"
            "    sections = [('extra flags before', []), ('original flags', args), ('extra flags after', [])]\n"
            "    for name, flags in sections:\n"
            "        print(name + ' to ' + compiler + ':')\n"
            "        if flags:\n"
            "            print(''.join('  ' + shlex.quote(flag) + '\\n' for flag in flags), end='')\n"
            "        else: print(\"  ''\")\n"
            "    print('clang version 22.1.8')\n"
            "elif args == ['-load=' + str(pathlib.Path('plugin.so').absolute()),\n"
            "              '-checks=-*,aobus-*', '-list-checks']:\n"
            "    print('Enabled checks:\\n    aobus-example')\n"
            "else:\n"
            "    db = pathlib.Path(args[args.index('-p') + 1]) / 'compile_commands.json'\n"
            "    entry, = json.loads(db.read_text())\n"
            "    assert entry['file'] == args[-1] and pathlib.Path(args[-1]).is_file()\n"
            "    print(args[-1] + ':1:1: warning: actual child [aobus-example]')\n",
            encoding="utf-8",
        )
        self.host.chmod(0o700)
        base = [sys.executable, str(PROJECT_ROOT / "script/ao/core/bazel_lint_action.py")]
        common = [
            "--policy",
            "policy.json",
            "--sdk",
            "sdk.json",
            "--tidy",
            str(self.host),
            "--plugin",
            str(self.plugin),
        ]
        verified = subprocess.run(
            [*base, "verify", *common, "--output", "verified.json", "--log", "verify.log"],
            capture_output=True,
            text=True,
        )
        self.assertEqual(verified.returncode, 0, verified.stderr)
        analyzed = subprocess.run(
            [
                *base,
                "analyze",
                *common,
                "--context",
                "context.json",
                "--verification",
                "verified.json",
                "--report",
                "result.report.json",
                "--log",
                "result.log",
            ],
            capture_output=True,
            text=True,
        )
        self.assertEqual(analyzed.returncode, 0, analyzed.stderr)
        self.assertEqual(self.report()["outcome"], "warning")


if __name__ == "__main__":
    unittest.main()
