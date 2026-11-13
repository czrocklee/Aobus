"""Native Python-child regressions for analysis receipts; no real Bazel runs."""

import contextlib
import io
import json
import os
import shlex
import sys
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

from ao.core import bazel_contract, bazel_workspace
from ao.core import bazel_native as native


def fake_compiler(root):
    executable = root / "fake-clang"
    executable.write_text(
        f"#!{sys.executable}\n"
        "import json, os, pathlib, sys\n"
        "root = pathlib.Path(__file__).resolve().parent\n"
        "with (root / 'driver.calls.jsonl').open('a') as sink:\n"
        "    sink.write(json.dumps({'argv': sys.argv, 'environment': dict(os.environ), 'cwd': os.getcwd()}) + '\\n')\n"
        "if sys.argv[-1] != '-###':\n"
        "    sys.exit(99)\n"
        "sys.stdout.buffer.write((root / 'driver.stdout').read_bytes())\n"
        "sys.stderr.buffer.write((root / 'driver.stderr').read_bytes())\n"
        "sys.exit(int((root / 'driver.exit').read_text()))\n",
        encoding="utf-8",
    )
    executable.chmod(0o755)
    (root / "driver.stdout").write_bytes(b"synthetic driver stdout\n")
    (root / "driver.exit").write_text("0", encoding="utf-8")
    return executable


def write_cc1(root, flags):
    # Include spaces in the executable to exercise Clang's quoted command format.
    command = ["/fixture compiler/clang", "-cc1", "-D", "gsl_CONFIG_CONTRACT_CHECKING_ON", *flags]
    (root / "driver.stderr").write_text("fixture Clang driver\n " + shlex.join(command) + "\n", encoding="utf-8")


@unittest.skipUnless(sys.platform == "linux", "runner executes Linux-only child fixtures")
class BazelContractRunnerTest(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix="aobus-contract-test-")
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        self.tree = self.root / "tree"
        self.prepared = self.root / "configuration"
        self.prepared.mkdir()
        (self.prepared / "aobus.bazelrc").write_text("# synthetic launch configuration\n", encoding="utf-8")
        self.executable = self.root / "fake-bazel"
        self.executable.write_text(
            f"#!{sys.executable}\n"
            "import json, os, pathlib, sys\n"
            "root = pathlib.Path(__file__).parent\n"
            "if sys.argv[2] == 'query' and '--output=label' in sys.argv:\n"
            "    name = 'selection'\n"
            "elif sys.argv[2] == 'query' and '--output=location' in sys.argv:\n"
            "    name = 'source-location'\n"
            "elif sys.argv[2] == 'aquery':\n"
            "    name = ('native' if any('CppArchive' in argument for argument in sys.argv) else\n"
            "            'spa' if '--include_pruned_inputs=true' in sys.argv else 'flags')\n"
            "else:\n"
            "    sys.exit('unexpected Bazel command')\n"
            "with (root / 'calls.jsonl').open('a') as sink:\n"
            "    sink.write(json.dumps(sys.argv) + '\\n')\n"
            "if (root / (name + '.expression')).read_text() not in sys.argv:\n"
            "    sys.exit('unexpected query expression: ' + name)\n"
            "if name == 'selection':\n"
            "    (root / 'selection.environment.json').write_text(json.dumps(dict(os.environ)))\n"
            "sys.stderr.write(name + ' synthetic analysis stderr\\n')\n"
            "output = root / (name + '.stdout' if name in {'selection', 'source-location'} else name + '.json')\n"
            "sys.stdout.buffer.write(output.read_bytes())\n"
            "sys.exit(int((root / (name + '.exit')).read_text()))\n",
            encoding="utf-8",
        )
        self.executable.chmod(0o755)
        owned_compiler = fake_compiler(self.root)
        self.compiler = self.root / "tool source:with spaces" / "clang++"
        self.compiler.parent.mkdir()
        self.compiler.symlink_to(owned_compiler)
        self.relative_compiler = "external/+dependencies+aobus_toolchain/tools/clang++"
        self.compiler_label = "@@+dependencies+aobus_toolchain//:tools/clang++"
        self.location_output = f"{self.compiler}:1:1: source file @aobus_toolchain//:tools/clang++\n"
        (self.root / "source-location.stdout").write_text(self.location_output, encoding="utf-8")
        (self.root / "source-location.exit").write_text("0", encoding="utf-8")
        for name in ("source.cpp", "second.cpp"):
            (self.root / name).write_text("// Synthetic input, never compiled.\n", encoding="utf-8")
        self.policy = {"NIX_HARDENING_ENABLE_fixture_salt": "stackprotector"}
        self.environment = (
            self.policy
            # The pinned Python wrapper also sets this in synthetic children.
            | {"LC_ALL": "C.UTF-8", "FIXTURE_DRIVER": "captured", "PYTHONNOUSERSITE": "true"}
            | dict.fromkeys(
                (
                    "CPLUS_INCLUDE_PATH",
                    "C_INCLUDE_PATH",
                    "CPATH",
                    "LIBRARY_PATH",
                    "LD_LIBRARY_PATH",
                    "NIX_CFLAGS_COMPILE",
                    "NIX_CFLAGS_COMPILE_FOR_TARGET",
                    "NIX_LDFLAGS",
                    "NIX_LDFLAGS_FOR_TARGET",
                ),
                "",
            )
        )
        for name in ("flags", "spa", "native"):
            (self.root / f"{name}.exit").write_text("0", encoding="utf-8")
        self.stdout, self.stderr = io.StringIO(), io.StringIO()
        self.latest_receipts = {}
        self.targets = ("//fixture:suite",)
        self.leaves = ("//fixture:first", "//fixture:second")
        (self.root / "selection.stdout").write_text("\n".join(reversed(self.leaves)) + "\n", encoding="utf-8")
        (self.root / "selection.exit").write_text("0", encoding="utf-8")
        for name, expression in (
            ("selection", 'tests(set("//fixture:suite"))'),
            ("flags", 'mnemonic("CppCompile",deps(set("//fixture:first" "//fixture:second")))'),
            ("spa", 'mnemonic("CppCompile",deps(set("//test:audio_test" "//test:audio_platform_test")))'),
            (
                "native",
                'mnemonic("^(CppLink|ObjcLink|ObjcExecutable|CppArchive|ObjcArchive|ObjcCompile'
                '|CppLinkstampCompile|Lto.+|.*LTO.*)$",deps(set("//fixture:first" "//fixture:second")))',
            ),
            ("source-location", f"'{self.compiler_label}'"),
        ):
            (self.root / f"{name}.expression").write_text(expression, encoding="utf-8")
        self.stack = contextlib.ExitStack()
        self.addCleanup(self.stack.close)
        self.stack.enter_context(
            patch.object(
                bazel_contract,
                "_expected_archive",
                side_effect=lambda platform: {
                    "linux": "llvm-ar",
                    "macos": "llvm-libtool-darwin",
                    "windows": "lib.exe",
                }[platform],
            )
        )
        self.stack.enter_context(patch.object(bazel_contract, "PROJECT_ROOT", self.root))
        self.stack.enter_context(patch.object(native, "native_platform", return_value="linux"))
        self.policy_probe = self.stack.enter_context(
            patch.object(bazel_contract, "_expected_hardening_environment", return_value=self.policy)
        )
        self.stack.enter_context(patch.object(bazel_workspace, "DEFAULT_TEST_TARGETS", self.targets, create=True))

    def write_documents(self, config="debug", platform="linux"):
        self.policy["NIX_HARDENING_ENABLE_fixture_salt"] = (
            "stackprotector" if config == "debug" else "stackprotector fortify fortify3"
        )
        self.environment.update(self.policy)
        driver_mode = ["-O0"] if config == "debug" else ["-O2", "-D", "_FORTIFY_SOURCE=2"]
        write_cc1(self.root, [*driver_mode, "-stack-protector", "2"])
        mode_flags = (
            ["-O0", "-g", "-Dgsl_CONFIG_CONTRACT_VIOLATION_ASSERTS"]
            if config == "debug"
            else ["-O2", "-DNDEBUG", "-Dgsl_CONFIG_CONTRACT_VIOLATION_TERMINATES"]
        )
        # The fixed documented matrix: Release compiles and final links carry
        # effective ThinLTO; sampling carries debug information, frame pointers
        # and effective no-LTO instead. Independent expectations, not producer
        # values.
        mode_flags += {
            "debug": [],
            "release": ["-flto=thin"],
            "profile": ["-g", "-fno-omit-frame-pointer", "-fno-lto"],
        }[config]
        if platform == "linux" and config == "release":
            mode_flags.append("-fno-assume-unique-vtables")
        link_flags = {
            "debug": [],
            "release": ["-flto=thin"],
            "profile": ["-fno-lto"],
        }[config]
        archive = {"linux": "llvm-ar", "macos": "llvm-libtool-darwin", "windows": "lib.exe"}[platform]
        native_actions = [
            {
                "mnemonic": "CppLink",
                "targetId": 1,
                "configurationId": 1,
                "arguments": [self.relative_compiler, *link_flags, "-o", "app", "source.o"],
                "outputIds": [1],
            },
            {
                "mnemonic": "CppArchive",
                "targetId": 1,
                "configurationId": 1,
                "arguments": [archive, "rcs", "out.a", "source.o"],
                "outputIds": [2],
            },
        ]
        if platform == "macos":
            (self.root / "native.expression").write_text(
                'mnemonic("^(CppLink|ObjcLink|ObjcExecutable|CppArchive|ObjcArchive|ObjcCompile'
                '|CppLinkstampCompile|Lto.+|.*LTO.*)$",'
                'deps(set("//fixture:first" "//fixture:second" "//:check_products")))',
                encoding="utf-8",
            )
            native_actions.append(
                {
                    "mnemonic": "ObjcCompile",
                    "targetId": 1,
                    "configurationId": 1,
                    "arguments": [
                        self.relative_compiler,
                        *(
                            {
                                "debug": ["-O0", "-g"],
                                "release": ["-O2", "-DNDEBUG"],
                                "profile": ["-O2", "-DNDEBUG", "-g", "-fno-omit-frame-pointer", "-fno-lto"],
                            }[config]
                        ),
                        "-c",
                        "source.mm",
                        "-o",
                        "source.o",
                    ],
                    "outputIds": [3],
                }
            )
        (self.root / "native.json").write_text(
            json.dumps(
                {
                    "targets": [{"id": 1, "label": "//fixture:first"}],
                    "configuration": [{"id": 1}],
                    "pathFragments": [
                        {"id": 1, "label": "app"},
                        {"id": 2, "label": "out.a"},
                        {"id": 3, "label": "source.o"},
                    ],
                    "artifacts": [
                        {"id": 1, "pathFragmentId": 1},
                        {"id": 2, "pathFragmentId": 2},
                        {"id": 3, "pathFragmentId": 3},
                    ],
                    "actions": native_actions,
                }
            ),
            encoding="utf-8",
        )
        for name in ("flags", "spa"):
            action = {
                "mnemonic": "CppCompile",
                "targetId": 1,
                "arguments": [
                    self.relative_compiler,
                    "-std=c++26",
                    *sorted(bazel_contract.unix_warnings() | bazel_contract.LINUX_EXTRA_WARNINGS),
                    *mode_flags,
                    "-Dgsl_CONFIG_CONTRACT_CHECKING_ON",
                    *bazel_contract.SPA_FLAGS,
                    "source.cpp",
                ],
                # Empty proto scalar values are genuinely omitted.
                "environmentVariables": [
                    {"key": key, **({"value": value} if value else {})} for key, value in self.environment.items()
                ],
            }
            document = {"targets": [{"id": 1, "label": "//fixture:first"}], "actions": [action]}
            if name == "spa":
                document.update(
                    {
                        "pathFragments": [{"id": 1, "label": "external/spa/utils/defs.h"}],
                        "artifacts": [{"id": 1, "pathFragmentId": 1}],
                        "depSetOfFiles": [{"id": 1, "directArtifactIds": [1]}],
                    }
                )
                action["inputDepSetIds"] = [1]
            (self.root / f"{name}.json").write_text(json.dumps(document), encoding="utf-8")

    def invoke(self, config="debug", jobs=2, *, isolate_rc=False):
        attempts = self.tree / "contracts" / config
        before = set(attempts.glob("verify-*/receipt.json"))
        with contextlib.redirect_stdout(self.stdout), contextlib.redirect_stderr(self.stderr):
            status = bazel_contract.verify(
                self.executable, self.tree, self.prepared, config, jobs, isolate_rc=isolate_rc
            )
        created = set(attempts.glob("verify-*/receipt.json")) - before
        if created:
            self.assertEqual(len(created), 1)
            self.latest_receipts[config] = created.pop()
        return status

    def receipt(self, config="debug"):
        return json.loads(self.latest_receipts[config].read_text(encoding="utf-8"))

    def test_both_configs_expand_suite_and_capture_waited_exits_counts_policy_and_queries(self):
        for config in ("debug", "release"):
            with self.subTest(config=config):
                self.write_documents(config)
                self.assertEqual(self.invoke(config, jobs=3), 0)
                receipt = self.receipt(config)
                self.assertEqual(receipt["status"], "passed")
                self.assertEqual(receipt["exit"], 0)
                self.assertEqual(receipt["expected_environment"], self.policy)
                self.assertEqual(receipt["platform"], "linux")
                self.assertEqual(receipt["config"], config)
                self.assertEqual(receipt["mode"], "dbg" if config == "debug" else "opt")
                self.assertEqual(receipt["executable"], str(self.executable))
                selection = receipt["selection"]
                self.assertEqual(
                    selection["argv"],
                    [
                        str(self.executable),
                        f"--bazelrc={self.prepared / 'aobus.bazelrc'}",
                        "query",
                        'tests(set("//fixture:suite"))',
                        "--output=label",
                    ],
                )
                self.assertEqual((selection["status"], selection["exit"]), ("passed", 0))
                self.assertEqual(selection["cwd"], str(self.root))
                self.assertEqual(selection["roots"], list(self.targets))
                self.assertEqual(selection["targets"], list(self.leaves))
                self.assertEqual(Path(selection["stdout"]).read_bytes(), (self.root / "selection.stdout").read_bytes())
                self.assertIn("selection synthetic analysis stderr", Path(selection["stderr"]).read_text())
                location = receipt["source_location"]
                self.assertEqual(
                    location["argv"],
                    [
                        str(self.executable),
                        f"--bazelrc={self.prepared / 'aobus.bazelrc'}",
                        "query",
                        f"'{self.compiler_label}'",
                        "--output=location",
                    ],
                )
                self.assertEqual((location["status"], location["exit"]), ("passed", 0))
                self.assertEqual(location["cwd"], str(self.root))
                self.assertEqual(location["captured_compiler"], self.relative_compiler)
                self.assertEqual(location["location"], str(self.compiler))
                self.assertEqual(location["compiler"], str(self.compiler))
                self.assertEqual(Path(location["stdout"]).read_text(), self.location_output)
                self.assertIn("source-location synthetic analysis stderr", Path(location["stderr"]).read_text())
                driver = receipt["driver"]
                self.assertEqual(driver["cwd"], str(self.root))
                self.assertEqual((driver["status"], driver["exit"]), ("passed", 0))
                self.assertEqual(driver["label"], "//fixture:first")
                self.assertEqual(driver["environment"], self.environment)
                self.assertEqual(
                    driver["effective"],
                    {
                        "optimization": "-O0" if config == "debug" else "-O2",
                        "stack_protector": 2,
                        "fortify_source": None if config == "debug" else "2",
                    },
                )
                self.assertEqual(Path(driver["stdout"]).read_text(), "synthetic driver stdout\n")
                flags, spa, native = receipt["queries"]
                self.assertEqual(driver["source_query"], flags["stdout"])
                action = bazel_contract.extract_compile_actions(json.loads(Path(flags["stdout"]).read_text()))[0]
                self.assertEqual(driver["captured_argv"], list(action.argv))
                self.assertEqual(driver["argv"], [str(self.compiler), *action.argv[1:], "-###"])
                self.assertEqual([flags["exit"], spa["exit"], native["exit"]], [0, 0, 0])
                self.assertEqual(flags["counts"], {"cpp_compile": 1, "first_party": 1, "spa": 0})
                self.assertEqual(spa["counts"], {"cpp_compile": 1, "first_party": 1, "spa": 1})
                self.assertEqual((native["status"], native["errors"]), ("passed", []))
                self.assertEqual(
                    native["counts"],
                    {
                        "target_link": 1,
                        "exec_link": 0,
                        "target_archive": 1,
                        "exec_archive": 0,
                        "target_objcpp_compile": 0,
                        "exec_objcpp_compile": 0,
                        "target_linkstamp_compile": 0,
                        "exec_linkstamp_compile": 0,
                        "lto_pipeline": 0,
                    },
                )
                self.assertIn(
                    'mnemonic("^(CppLink|ObjcLink|ObjcExecutable|CppArchive|ObjcArchive|ObjcCompile'
                    '|CppLinkstampCompile|Lto.+|.*LTO.*)$",deps(set("//fixture:first" "//fixture:second")))',
                    native["argv"],
                )
                self.assertIn("--include_artifacts=true", native["argv"])
                self.assertIn('mnemonic("CppCompile",deps(set("//fixture:first" "//fixture:second")))', flags["argv"])
                self.assertIn(
                    'mnemonic("CppCompile",deps(set("//test:audio_test" "//test:audio_platform_test")))', spa["argv"]
                )
                self.assertIn("--include_artifacts=false", flags["argv"])
                self.assertNotIn("--include_pruned_inputs=true", flags["argv"])
                self.assertIn("--include_artifacts=true", spa["argv"])
                self.assertIn("--include_pruned_inputs=true", spa["argv"])
                for query in (flags, spa):
                    self.assertEqual(
                        query["argv"][:3],
                        [str(self.executable), f"--bazelrc={self.prepared / 'aobus.bazelrc'}", "aquery"],
                    )
                    for option in (
                        "--jobs=3",
                        "--skip_incompatible_explicit_targets",
                        "--output=jsonproto",
                        "--include_commandline=true",
                        "--include_param_files=true",
                    ):
                        self.assertIn(option, query["argv"])
                    self.assertTrue(Path(query["stdout"]).is_file())
                    self.assertIn("synthetic analysis stderr", Path(query["stderr"]).read_text())
        calls = [json.loads(line) for line in (self.root / "calls.jsonl").read_text().splitlines()]
        self.assertEqual([call[2] for call in calls], ["query", "aquery", "query", "aquery", "aquery"] * 2)
        drivers = [json.loads(line) for line in (self.root / "driver.calls.jsonl").read_text().splitlines()]
        self.assertEqual(len(drivers), 2)
        self.assertTrue(all(call["argv"][0] == str(self.compiler) and call["argv"][-1] == "-###" for call in drivers))
        self.assertTrue(all(call["cwd"] == str(self.root) for call in drivers))
        self.assertNotIn('"actions"', self.stdout.getvalue())
        self.assertNotIn("Bazel compile-contract flags", self.stdout.getvalue())
        self.assertIn("Bazel compile-contract flags", self.stderr.getvalue())
        self.assertEqual(
            [call.args for call in self.policy_probe.call_args_list],
            [("debug",), ("release",), ("release",), ("release",)],
        )

    def test_macos_checks_captured_policy_without_linux_only_stages(self):
        self.write_documents(platform="macos")
        path = self.root / "flags.json"
        document = json.loads(path.read_text())
        action = document["actions"][0]
        action["arguments"] = [
            flag
            for flag in action["arguments"]
            if flag not in bazel_contract.LINUX_EXTRA_WARNINGS | set(bazel_contract.SPA_FLAGS)
        ]
        action["environmentVariables"] = [
            entry for entry in action["environmentVariables"] if entry["key"] not in self.policy
        ]
        path.write_text(json.dumps(document), encoding="utf-8")
        (self.root / "spa.json").unlink()
        self.compiler.unlink()
        self.policy_probe.side_effect = AssertionError("macOS must not query Nix hardening")
        with patch("ao.core.bazel_native.native_platform", return_value="macos"):
            self.assertEqual(self.invoke(), 0)
            passed = self.receipt()
            self.assertEqual((passed["status"], passed["exit"]), ("passed", 0))
            self.assertEqual(passed["platform"], "macos")
            self.assertEqual(passed["expected_environment"], {})
            flags, native = passed["queries"]
            self.assertEqual((flags["name"], flags["status"], flags["exit"]), ("flags", "passed", 0))
            self.assertEqual(flags["counts"], {"cpp_compile": 1, "first_party": 1, "spa": 0})
            self.assertEqual(flags["errors"], [])
            self.assertEqual((native["name"], native["status"], native["exit"]), ("native", "passed", 0))
            self.assertEqual(native["errors"], [])
            self.assertEqual(native["roots"], ["//fixture:first", "//fixture:second", "//:check_products"])
            self.assertEqual(
                native["counts"],
                {
                    "target_link": 1,
                    "exec_link": 0,
                    "target_archive": 1,
                    "exec_archive": 0,
                    "target_objcpp_compile": 1,
                    "exec_objcpp_compile": 0,
                    "target_linkstamp_compile": 0,
                    "exec_linkstamp_compile": 0,
                    "lto_pipeline": 0,
                },
            )
            self.assertEqual(passed["expected_archive"], "llvm-libtool-darwin")

            next(entry for entry in action["environmentVariables"] if entry["key"] == "CPATH")["value"] = "ambient"
            path.write_text(json.dumps(document), encoding="utf-8")
            self.stdout = io.StringIO()
            self.assertEqual(self.invoke(), 1)
            failed = self.receipt()
            self.assertEqual((failed["status"], failed["exit"]), ("contract failed", 1))
            flags, native_query = failed["queries"]
            self.assertEqual((native_query["status"], native_query["exit"]), ("unrun", None))
            self.assertFalse(Path(native_query["stdout"]).exists())
            self.assertEqual((flags["status"], flags["exit"]), ("contract failed", 0))
            self.assertEqual(len(flags["errors"]), 1)
            self.assertIn("CPATH", flags["errors"][0])
            self.assertEqual(Path(flags["stdout"]).read_bytes(), path.read_bytes())
            self.assertIn("flags synthetic analysis stderr", Path(flags["stderr"]).read_text())
            self.assertEqual(self.stdout.getvalue(), "")
        for receipt in (passed, failed):
            for name in ("driver", "source_location"):
                stage = receipt[name]
                self.assertEqual((stage["status"], stage["exit"]), ("not applicable", None))
                self.assertFalse(Path(stage["stdout"]).exists())
                self.assertFalse(Path(stage["stderr"]).exists())
        self.policy_probe.assert_not_called()
        calls = [json.loads(line) for line in (self.root / "calls.jsonl").read_text().splitlines()]
        self.assertEqual([call[2] for call in calls], ["query", "aquery", "aquery", "query", "aquery"])
        self.assertFalse((self.root / "driver.calls.jsonl").exists())

    def test_windows_requires_inline_configured_compile_context_without_linux_only_stages(self):
        expression = (
            'mnemonic("CppCompile",deps(set("//fixture:first" "//fixture:second")))'
            ' union outputs(".*[.]aobus-compile-context[.]json$",deps(set("//fixture:first" "//fixture:second")))'
        )
        (self.root / "flags.expression").write_text(expression, encoding="utf-8")
        path = self.root / "flags.json"
        argv = [
            "cl.exe",
            "/std:c++latest",
            "/external:W0",
            "/utf-8",
            "/permissive-",
            "/Zc:__cplusplus",
            "/Zc:preprocessor",
            "/DNOMINMAX",
            "/DWIN32_LEAN_AND_MEAN",
            "/DUNICODE",
            "/D_UNICODE",
            "/D_CRT_SECURE_NO_WARNINGS",
            "/DWINVER=0x0A00",
            "/D_WIN32_WINNT=0x0A00",
            "/W4",
            "/WX",
            "/Od",
            "/Z7",
            "/Dgsl_CONFIG_CONTRACT_CHECKING_ON",
            "/Dgsl_CONFIG_CONTRACT_VIOLATION_ASSERTS",
            "/I.",
            "/Iexternal/private",
            "source.cpp",
        ]
        document = {
            "targets": [{"id": 1, "label": "//fixture:first"}],
            "configuration": [{"id": 1}],
            "pathFragments": [
                {"id": 1, "label": "bazel-out/fixture/first.aobus-compile-context.json"},
                {"id": 2, "label": "bazel-out/fixture/_objs/first/source.obj"},
            ],
            "artifacts": [{"id": 1, "pathFragmentId": 1}, {"id": 2, "pathFragmentId": 2}],
            "actions": [
                {
                    "mnemonic": "CppCompile",
                    "targetId": 1,
                    "configurationId": 1,
                    "outputIds": [2],
                    # Windows aquery names the MSVC compile param file, not its contents.
                    "arguments": ["cl.exe", "@bazel-out/fixture/_objs/first/source.obj.params"],
                    "environmentVariables": [{"key": key} for key, value in self.environment.items() if not value],
                },
                {
                    "mnemonic": "FileWrite",
                    "targetId": 1,
                    "configurationId": 1,
                    "outputIds": [1],
                    "fileContents": json.dumps(
                        {
                            "quote_includes": [".", "external/private"],
                            "compile_argv": [{"outputs": ["bazel-out/fixture/_objs/first/source.obj"], "argv": argv}],
                        }
                    ),
                },
            ],
        }
        path.write_text(json.dumps(document), encoding="utf-8")
        (self.root / "native.json").write_text(
            json.dumps(
                {
                    "targets": [{"id": 1, "label": "//fixture:first"}],
                    "configuration": [{"id": 1}],
                    "pathFragments": [{"id": 1, "label": "app.exe"}, {"id": 2, "label": "out.lib"}],
                    "artifacts": [{"id": 1, "pathFragmentId": 1}, {"id": 2, "pathFragmentId": 2}],
                    "actions": [
                        {
                            "mnemonic": "CppLink",
                            "targetId": 1,
                            "configurationId": 1,
                            "arguments": ["link.exe", "/OUT:app.exe", "source.obj"],
                            "outputIds": [1],
                        },
                        {
                            "mnemonic": "CppArchive",
                            "targetId": 1,
                            "configurationId": 1,
                            "arguments": ["lib.exe", "/OUT:out.lib", "source.obj"],
                            "outputIds": [2],
                        },
                    ],
                }
            ),
            encoding="utf-8",
        )
        self.compiler.unlink()
        self.policy_probe.side_effect = AssertionError("Windows must not query Nix hardening")
        with patch("ao.core.bazel_native.native_platform", return_value="windows"):
            self.assertEqual(self.invoke(), 0)
            passed = self.receipt()
            self.assertEqual((passed["status"], passed["exit"]), ("passed", 0))
            self.assertEqual(passed["platform"], "windows")
            self.assertEqual(passed["expected_environment"], {})
            flags, native = passed["queries"]
            self.assertEqual((flags["name"], flags["status"], flags["exit"]), ("flags", "passed", 0))
            self.assertEqual(flags["counts"], {"cpp_compile": 1, "first_party": 1, "spa": 0})
            self.assertEqual(flags["errors"], [])
            self.assertEqual((native["name"], native["status"], native["exit"]), ("native", "passed", 0))
            self.assertEqual(native["errors"], [])
            self.assertEqual(native["counts"]["target_link"], 1)
            self.assertEqual(native["counts"]["target_archive"], 1)
            self.assertEqual(passed["expected_archive"], "lib.exe")
            self.assertIn(expression, flags["argv"])
            for option in (
                "--include_artifacts=true",
                "--include_aspects=true",
                "--include_file_write_contents=true",
                "--aspects=//bazel:compile_context.bzl%compile_context_aspect",
            ):
                self.assertIn(option, flags["argv"])
            self.assertNotIn("--include_pruned_inputs=true", flags["argv"])

            document["actions"].pop()
            path.write_text(json.dumps(document), encoding="utf-8")
            self.stdout = io.StringIO()
            self.assertEqual(self.invoke(), 1)
            failed = self.receipt()
            self.assertEqual((failed["status"], failed["exit"]), ("error", 1))
            # Without the aspect's compile context the param file reference cannot be expanded.
            self.assertIn("unexpanded or recursive response file", failed["error"])
            flags, native_query = failed["queries"]
            self.assertEqual((native_query["status"], native_query["exit"]), ("unrun", None))
            self.assertFalse(Path(native_query["stdout"]).exists())
            self.assertEqual((flags["status"], flags["exit"]), ("error", 0))
            self.assertNotIn("counts", flags)
            self.assertEqual(Path(flags["stdout"]).read_bytes(), path.read_bytes())
            self.assertIn("flags synthetic analysis stderr", Path(flags["stderr"]).read_text())
            self.assertEqual(self.stdout.getvalue(), "")
        for receipt in (passed, failed):
            for name in ("driver", "source_location"):
                stage = receipt[name]
                self.assertEqual((stage["status"], stage["exit"]), ("not applicable", None))
                self.assertFalse(Path(stage["stdout"]).exists())
                self.assertFalse(Path(stage["stderr"]).exists())
        self.policy_probe.assert_not_called()
        calls = [json.loads(line) for line in (self.root / "calls.jsonl").read_text().splitlines()]
        self.assertEqual([call[2] for call in calls], ["query", "aquery", "aquery", "query", "aquery"])
        self.assertFalse((self.root / "driver.calls.jsonl").exists())

    def test_selection_failure_keeps_exit_and_streams_without_starting_analysis(self):
        self.write_documents()
        (self.root / "selection.exit").write_text("29", encoding="utf-8")
        self.assertEqual(self.invoke(), 29)
        receipt = self.receipt()
        self.assertEqual((receipt["status"], receipt["exit"]), ("selection failed", 29))
        selection = receipt["selection"]
        self.assertEqual((selection["status"], selection["exit"]), ("exited", 29))
        self.assertEqual(Path(selection["stdout"]).read_bytes(), (self.root / "selection.stdout").read_bytes())
        self.assertIn("selection synthetic analysis stderr", Path(selection["stderr"]).read_text())
        self.assertTrue(all(query["status"] == "unrun" and query["argv"] is None for query in receipt["queries"]))
        self.assertEqual(receipt["source_location"]["status"], "unrun")
        self.assertEqual(receipt["driver"]["status"], "unrun")
        self.assertFalse((self.root / "driver.calls.jsonl").exists())
        self.assertEqual(len((self.root / "calls.jsonl").read_text().splitlines()), 1)
        self.assertEqual(self.stdout.getvalue(), "")

    def test_only_isolated_contract_children_drop_the_personal_rc_environment(self):
        self.write_documents()
        for isolated in (False, True):
            with (
                self.subTest(isolate_rc=isolated),
                patch.dict(os.environ, {"BAZELRC": "/missing/personal.rc", "CONTRACT_RC_SENTINEL": "retained"}),
                patch.object(
                    bazel_workspace,
                    "invocation",
                    return_value=[str(self.executable), f"--bazelrc={self.prepared / 'aobus.bazelrc'}"],
                ) as invocation,
            ):
                self.assertEqual(self.invoke(isolate_rc=isolated), 0)
                environment = json.loads((self.root / "selection.environment.json").read_text(encoding="utf-8"))
                self.assertEqual(environment["CONTRACT_RC_SENTINEL"], "retained")
                self.assertEqual("BAZELRC" in environment, not isolated)
                self.assertTrue(all(call.kwargs["isolate_rc"] == isolated for call in invocation.call_args_list))

    def test_selected_label_with_query_operator_is_quoted_and_selection_uses_closed_environment(self):
        self.write_documents()
        label = "//fixture:case+variant"
        (self.root / "selection.stdout").write_text(label + "\n", encoding="utf-8")
        expression = 'mnemonic("CppCompile",deps(set("//fixture:case+variant")))'
        (self.root / "flags.expression").write_text(expression, encoding="utf-8")
        (self.root / "native.expression").write_text(
            'mnemonic("^(CppLink|ObjcLink|ObjcExecutable|CppArchive|ObjcArchive|ObjcCompile'
            '|CppLinkstampCompile|Lto.+|.*LTO.*)$",deps(set("//fixture:case+variant")))',
            encoding="utf-8",
        )
        environment = {"LC_ALL": "C.UTF-8", "PYTHONNOUSERSITE": "true", "SELECTION_FIXTURE": "explicit"}
        with (
            patch.dict(os.environ, {"AMBIENT_SELECTION_SENTINEL": "must-not-leak"}),
            patch.object(native, "child_environment", return_value=environment),
        ):
            self.assertEqual(self.invoke(), 0)
        receipt = self.receipt()
        self.assertEqual(receipt["selection"]["targets"], [label])
        self.assertIn(expression, receipt["queries"][0]["argv"])
        self.assertEqual(json.loads((self.root / "selection.environment.json").read_text()), environment)

    def test_first_analysis_failure_keeps_exit_and_streams_without_success_snapshot(self):
        self.write_documents()
        (self.root / "flags.exit").write_text("23", encoding="utf-8")
        self.assertEqual(self.invoke(), 23)
        receipt = self.receipt()
        self.assertEqual((receipt["status"], receipt["exit"]), ("analysis failed", 23))
        flags, spa, _ = receipt["queries"]
        self.assertEqual(flags["exit"], 23)
        self.assertNotIn("counts", flags)
        self.assertEqual((spa["status"], spa["exit"]), ("unrun", None))
        self.assertFalse(Path(spa["stdout"]).exists())
        self.assertEqual(len((self.root / "calls.jsonl").read_text().splitlines()), 2)
        self.assertIn("exited 23", self.stderr.getvalue())
        self.assertEqual(receipt["driver"]["status"], "unrun")
        self.assertFalse((self.root / "driver.calls.jsonl").exists())
        self.assertEqual(self.stdout.getvalue(), "")

    def test_second_analysis_failure_propagates_after_first_contract_success(self):
        self.write_documents()
        (self.root / "spa.exit").write_text("17", encoding="utf-8")
        self.assertEqual(self.invoke(), 17)
        receipt = self.receipt()
        self.assertEqual(receipt["exit"], 17)
        self.assertEqual([query["status"] for query in receipt["queries"]], ["passed", "exited", "unrun"])
        self.assertEqual([query["exit"] for query in receipt["queries"]], [0, 17, None])
        self.assertEqual(self.stdout.getvalue(), "")

    def test_profile_config_verifies_sampling_policy_with_release_hardening(self):
        self.write_documents("profile")
        self.assertEqual(self.invoke("profile"), 0)
        receipt = self.receipt("profile")
        self.assertEqual((receipt["status"], receipt["exit"]), ("passed", 0))
        self.assertEqual(receipt["config"], "profile")
        self.assertEqual(receipt["mode"], "opt")
        self.assertEqual(receipt["profile"], "sampling")
        self.assertEqual(receipt["expected_environment"], self.policy)
        flags, spa, native = receipt["queries"]
        self.assertEqual(flags["status"], "passed")
        self.assertEqual(spa["status"], "passed")
        self.assertEqual((native["status"], native["errors"]), ("passed", []))
        self.assertEqual(native["counts"]["target_link"], 1)
        self.assertEqual(native["counts"]["target_archive"], 1)
        # Sampling links pin effective no-LTO; leaking Release IPO fails closed.
        path = self.root / "native.json"
        document = json.loads(path.read_text())
        document["actions"][0]["arguments"].append("-flto=thin")
        path.write_text(json.dumps(document), encoding="utf-8")
        self.stdout = io.StringIO()
        self.assertEqual(self.invoke("profile"), 1)
        failed = self.receipt("profile")
        self.assertEqual((failed["status"], failed["exit"]), ("contract failed", 1))
        native = failed["queries"][2]
        self.assertTrue(any("effective -fno-lto" in error for error in native["errors"]))
        self.assertEqual(
            [call.args for call in self.policy_probe.call_args_list][-2:],
            [("profile",), ("release",)],
        )

    def test_parse_errors_and_zero_matches_fail_before_later_query(self):
        for text in (
            "not json",
            "[]",
            '{"actions": [], "actions": []}',
            json.dumps({"actions": []}),
            json.dumps(
                {"targets": [], "actions": [{"mnemonic": "CppCompile", "targetId": 1, "arguments": ["clang++"]}]}
            ),
        ):
            with self.subTest(text=text):
                self.write_documents()
                (self.root / "flags.json").write_text(text, encoding="utf-8")
                self.assertEqual(self.invoke(), 1)
                receipt = self.receipt()
                self.assertEqual(receipt["status"], "error")
                self.assertIn("error", receipt)
                self.assertEqual(receipt["queries"][0]["exit"], 0)
                self.assertEqual(receipt["queries"][0]["status"], "error")
                self.assertEqual(receipt["queries"][1]["status"], "unrun")
        self.assertIn("verify:", self.stderr.getvalue())
        self.assertEqual(self.stdout.getvalue(), "")

    def test_source_layout_refusal_keeps_receipt_without_spawning(self):
        message = "Windows Bazel commands require a local source checkout"
        with (
            patch.object(native, "native_platform", return_value="windows"),
            patch.object(
                native,
                "require_supported_source",
                side_effect=native.BazelWorkspaceError(message),
            ) as validate,
        ):
            self.assertEqual(self.invoke(), 1)
        validate.assert_called_once_with()
        receipt = self.receipt()
        self.assertEqual((receipt["status"], receipt["exit"]), ("error", 1))
        self.assertEqual(receipt["error"], message)
        selection = receipt["selection"]
        self.assertEqual((selection["status"], selection["exit"], selection["argv"]), ("unrun", None, None))
        self.assertTrue(all(query["status"] == "unrun" and query["argv"] is None for query in receipt["queries"]))
        self.assertFalse((self.root / "calls.jsonl").exists())
        self.assertFalse((self.root / "driver.calls.jsonl").exists())
        self.policy_probe.assert_not_called()
        self.assertEqual(self.stdout.getvalue(), "")

    def test_contract_failure_reports_all_first_party_actions_but_not_external_helpers(self):
        self.write_documents()
        path = self.root / "flags.json"
        document = json.loads(path.read_text())
        document["targets"] += [{"id": 2, "label": "//fixture:second"}, {"id": 3, "label": "@@external+//:helper"}]
        for identifier in (2, 3):
            document["actions"].append(
                {"mnemonic": "CppCompile", "targetId": identifier, "arguments": ["clang++", "other.cpp"]}
            )
        path.write_text(json.dumps(document), encoding="utf-8")
        self.assertEqual(self.invoke(), 1)
        receipt = self.receipt()
        self.assertEqual(receipt["status"], "contract failed")
        flags, spa, _ = receipt["queries"]
        self.assertEqual(flags["counts"], {"cpp_compile": 3, "first_party": 2, "spa": 0})
        self.assertTrue(all("//fixture:second" in error for error in flags["errors"]))
        self.assertIn("NIX_HARDENING_ENABLE_fixture_salt", self.stderr.getvalue())
        self.assertNotIn("@@external+//:helper", self.stderr.getvalue())
        self.assertEqual(spa["status"], "unrun")

    def test_missing_or_overridden_standard_fails_before_driver_and_spa(self):
        for config in ("debug", "release"):
            for override in (None, "-std=c++20", "--std=gnu++26"):
                with self.subTest(config=config, override=override):
                    self.write_documents(config)
                    path = self.root / "flags.json"
                    document = json.loads(path.read_text())
                    arguments = document["actions"][0]["arguments"]
                    if override is None:
                        arguments.remove("-std=c++26")
                    else:
                        arguments.append(override)
                    path.write_text(json.dumps(document), encoding="utf-8")
                    self.assertEqual(self.invoke(config), 1)
                    receipt = self.receipt(config)
                    self.assertEqual((receipt["status"], receipt["exit"]), ("contract failed", 1))
                    flags, spa, _ = receipt["queries"]
                    self.assertEqual((flags["status"], flags["exit"]), ("contract failed", 0))
                    self.assertEqual(flags["counts"], {"cpp_compile": 1, "first_party": 1, "spa": 0})
                    self.assertEqual(len(flags["errors"]), 1)
                    self.assertIn("language standard", flags["errors"][0])
                    self.assertEqual(spa["status"], "unrun")
                    self.assertEqual(receipt["source_location"]["status"], "unrun")
                    self.assertEqual(receipt["driver"]["status"], "unrun")
        self.assertFalse((self.root / "driver.calls.jsonl").exists())
        self.assertEqual(self.stdout.getvalue(), "")

    def test_driver_failure_or_bad_expansion_stops_before_spa(self):
        for failure in ("exit", "mode", "protector", "malformed"):
            with self.subTest(failure=failure):
                self.write_documents()
                (self.root / "driver.exit").write_text("19" if failure == "exit" else "0", encoding="utf-8")
                if failure == "mode":
                    write_cc1(self.root, ["-O2", "-stack-protector", "2"])
                elif failure == "protector":
                    write_cc1(self.root, ["-O0", "-stack-protector", "1"])
                elif failure == "malformed":
                    (self.root / "driver.stderr").write_text("no cc1 command", encoding="utf-8")
                self.assertEqual(self.invoke(), 19 if failure == "exit" else 1)
                receipt = self.receipt()
                self.assertEqual(receipt["queries"][0]["status"], "passed")
                self.assertEqual(receipt["queries"][1]["status"], "unrun")
                self.assertEqual(receipt["driver"]["exit"], 19 if failure == "exit" else 0)
                self.assertEqual(
                    receipt["driver"]["status"],
                    "error" if failure == "malformed" else "driver failed" if failure == "exit" else "contract failed",
                )
        self.assertEqual(self.stdout.getvalue(), "")

    def test_debug_driver_skips_checked_exec_tools_and_generated_target_sources(self):
        self.write_documents()
        path = self.root / "flags.json"
        document = json.loads(path.read_text())
        target = document["actions"][0]
        target["configurationId"] = 1
        tool = json.loads(json.dumps(target))
        tool.update(targetId=2, configurationId=2)
        tool["arguments"] = [
            value
            for value in tool["arguments"]
            if value not in {"-O0", "-g", "-Dgsl_CONFIG_CONTRACT_VIOLATION_ASSERTS"}
        ] + ["-O2", "-DNDEBUG", "-Dgsl_CONFIG_CONTRACT_VIOLATION_TERMINATES"]
        tool_policy = {"NIX_HARDENING_ENABLE_fixture_salt": "stackprotector fortify fortify3"}
        tool["environmentVariables"] = [
            {"key": key, "value": value} for key, value in (self.environment | tool_policy).items()
        ]
        generated = json.loads(json.dumps(target))
        generated["targetId"] = 3
        generated["arguments"] = [
            "bazel-out/generated.cpp" if value == "source.cpp" else value for value in generated["arguments"]
        ]
        document["configuration"] = [{"id": 1}, {"id": 2, "isTool": True}]
        document["targets"].extend(
            [
                {"id": 2, "label": "//tool/catalog:ao_catalog_compiler"},
                {"id": 3, "label": "//app/i18n:catalog_data"},
            ]
        )
        document["actions"] = [tool, generated, target]
        path.write_text(json.dumps(document))
        self.policy_probe.side_effect = lambda mode: tool_policy if mode == "release" else self.policy
        self.assertEqual(self.invoke(), 0)
        receipt = self.receipt()
        self.assertEqual(receipt["driver"]["label"], "//fixture:first")
        self.assertEqual(receipt["expected_tool_environment"], tool_policy)
        self.assertEqual(receipt["queries"][0]["execution_cpp_compile"], 1)
        self.assertEqual(receipt["queries"][0]["counts"]["first_party"], 3)

    def test_driver_uses_first_first_party_action_with_only_captured_environment(self):
        self.write_documents()
        path = self.root / "flags.json"
        document = json.loads(path.read_text())
        document["targets"].append({"id": 2, "label": "@@external+//:helper"})
        document["actions"].insert(0, {"mnemonic": "CppCompile", "targetId": 2, "arguments": ["never-execute"]})
        path.write_text(json.dumps(document), encoding="utf-8")
        with patch.dict(os.environ, {"AMBIENT_DRIVER_SENTINEL": "must-not-leak"}):
            self.assertEqual(self.invoke(), 0)
        calls = [json.loads(line) for line in (self.root / "driver.calls.jsonl").read_text().splitlines()]
        self.assertEqual(len(calls), 1)
        self.assertEqual(calls[0]["environment"], self.environment)
        self.assertEqual(self.receipt()["driver"]["label"], "//fixture:first")

    def test_source_location_failure_preserves_exit_and_skips_driver_and_spa(self):
        self.write_documents()
        (self.root / "source-location.exit").write_text("31", encoding="utf-8")
        self.assertEqual(self.invoke(), 31)
        receipt = self.receipt()
        self.assertEqual((receipt["status"], receipt["exit"]), ("source location failed", 31))
        location = receipt["source_location"]
        self.assertEqual((location["status"], location["exit"]), ("exited", 31))
        self.assertEqual(Path(location["stdout"]).read_text(), self.location_output)
        self.assertIn("source-location synthetic analysis stderr", Path(location["stderr"]).read_text())
        self.assertEqual(receipt["queries"][0]["status"], "passed")
        self.assertEqual(receipt["queries"][1]["status"], "unrun")
        self.assertEqual(receipt["driver"]["status"], "unrun")
        self.assertFalse((self.root / "driver.calls.jsonl").exists())
        calls = [json.loads(line) for line in (self.root / "calls.jsonl").read_text().splitlines()]
        self.assertEqual([call[2] for call in calls], ["query", "aquery", "query"])
        self.assertEqual(self.stdout.getvalue(), "")

    def test_fresh_analysis_resolves_only_compiler_argv0_without_execroot_or_source_staging(self):
        self.write_documents()
        original = json.loads((self.root / "flags.json").read_text())["actions"][0]["arguments"]
        self.assertNotIn("PATH", self.environment)
        self.assertFalse((self.root / "external").exists())
        self.assertFalse((self.root / "execroot").exists())
        self.assertFalse(self.tree.exists())
        with patch.dict(os.environ, {"AMBIENT_DRIVER_SENTINEL": "must-not-leak"}):
            self.assertEqual(self.invoke(), 0)
        call = json.loads((self.root / "driver.calls.jsonl").read_text())
        resolved = [str(self.compiler), *original[1:], "-###"]
        self.assertEqual(call["argv"], resolved)
        self.assertEqual(call["cwd"], str(self.root))
        self.assertEqual(call["environment"], self.environment)
        self.assertTrue((Path(call["cwd"]) / original[-1]).is_file())
        self.assertEqual(self.receipt()["driver"]["captured_argv"], original)
        self.assertEqual(self.receipt()["driver"]["argv"], resolved)
        self.assertEqual(self.receipt()["driver"]["cwd"], str(self.root))
        self.assertNotEqual(self.compiler, self.compiler.resolve())
        self.assertFalse((self.root / "external").exists())
        self.assertFalse((self.root / "execroot").exists())

    def test_unsupported_compiler_paths_fail_before_location_query_without_path_fallback(self):
        for compiler in (
            "bazel-out/k8-opt/bin/tool/clang++",
            "clang++",
            "external/repo/../clang++",
            "external/../tools/clang++",
            "external/repo/./clang++",
            "external/repo//clang++",
        ):
            with self.subTest(compiler=compiler):
                self.write_documents()
                path = self.root / "flags.json"
                document = json.loads(path.read_text())
                document["actions"][0]["arguments"][0] = compiler
                path.write_text(json.dumps(document), encoding="utf-8")
                self.assertEqual(self.invoke(), 1)
                receipt = self.receipt()
                self.assertIn("unsupported representative compiler", receipt["error"])
                self.assertEqual(receipt["source_location"]["status"], "unrun")
                self.assertEqual(receipt["driver"]["status"], "unrun")
                self.assertEqual(receipt["queries"][1]["status"], "unrun")
        self.assertFalse((self.root / "driver.calls.jsonl").exists())
        calls = [json.loads(line) for line in (self.root / "calls.jsonl").read_text().splitlines()]
        self.assertTrue(all(call[2] == "aquery" or "--output=label" in call for call in calls))

    def test_existing_absolute_owned_compiler_needs_no_source_location_query(self):
        self.write_documents()
        path = self.root / "flags.json"
        document = json.loads(path.read_text())
        document["actions"][0]["arguments"][0] = str(self.compiler)
        path.write_text(json.dumps(document), encoding="utf-8")
        self.assertEqual(self.invoke(), 0)
        receipt = self.receipt()
        self.assertEqual(receipt["source_location"]["status"], "not required")
        self.assertIsNone(receipt["source_location"]["exit"])
        self.assertIsNone(receipt["source_location"]["argv"])
        self.assertFalse(Path(receipt["source_location"]["stdout"]).exists())
        calls = [json.loads(line) for line in (self.root / "calls.jsonl").read_text().splitlines()]
        self.assertEqual([call[2] for call in calls], ["query", "aquery", "aquery", "aquery"])
        self.assertEqual(receipt["driver"]["argv"][0], str(self.compiler))

    def test_missing_checkout_or_only_generated_sources_cannot_claim_a_driver_expansion(self):
        for source in ("bazel-out/generated.cpp", "missing.cpp"):
            with self.subTest(source=source):
                self.write_documents()
                path = self.root / "flags.json"
                document = json.loads(path.read_text())
                document["actions"][0]["arguments"][-1] = source
                if source == "missing.cpp":
                    # A second valid action must not hide a missing checkout input.
                    document["actions"].append(
                        dict(
                            document["actions"][0], arguments=[*document["actions"][0]["arguments"][:-1], "source.cpp"]
                        )
                    )
                path.write_text(json.dumps(document), encoding="utf-8")
                self.assertEqual(self.invoke(), 1)
                receipt = self.receipt()
                expected_error = (
                    "existing first-party source" if source == "missing.cpp" else "target-configuration checkout source"
                )
                self.assertIn(expected_error, receipt["error"])
                self.assertEqual(receipt["source_location"]["status"], "unrun")
                self.assertEqual(receipt["driver"]["status"], "unrun")
                self.assertEqual(receipt["queries"][1]["status"], "unrun")
        self.assertFalse((self.root / "driver.calls.jsonl").exists())

    def test_spa_query_requires_positive_declared_consumer_count(self):
        self.write_documents()
        path = self.root / "spa.json"
        document = json.loads(path.read_text())
        del document["actions"][0]["inputDepSetIds"]
        path.write_text(json.dumps(document), encoding="utf-8")
        self.assertEqual(self.invoke(), 1)
        spa = self.receipt()["queries"][1]
        self.assertEqual(spa["counts"]["spa"], 0)
        self.assertIn("no declared SPA", spa["errors"][0])

    def test_unsupported_host_and_invalid_configuration_do_not_spawn_or_claim_analysis(self):
        with patch.object(native, "native_platform", return_value="unsupported"):
            self.assertEqual(self.invoke(), 1)
        self.assertEqual(self.invoke("unsupported"), 1)
        self.assertEqual(self.invoke(jobs=0), 1)
        self.assertFalse((self.root / "calls.jsonl").exists())
        self.assertFalse(self.tree.exists())
        self.assertEqual(self.stdout.getvalue(), "")


@unittest.skipUnless(sys.platform == "linux", "driver fixture executes native Linux Python children")
class BazelRepresentativeDriverTest(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix="aobus-driver-test-")
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        self.compiler = fake_compiler(self.root)
        root_patch = patch.object(bazel_contract, "PROJECT_ROOT", self.root)
        root_patch.start()
        self.addCleanup(root_patch.stop)
        self.policy_key = "NIX_HARDENING_ENABLE_fixture_salt"
        self.evidence = {
            "stdout": str(self.root / "captured.stdout"),
            "stderr": str(self.root / "captured.stderr"),
            "status": "unrun",
            "exit": None,
        }

    def check(self, flags, mode="dbg", policy="stackprotector"):
        write_cc1(self.root, flags)
        expected = {self.policy_key: policy}
        action = bazel_contract.CompileAction(
            "//fixture:source",
            (str(self.compiler), "-c", "source.cpp"),
            environment=((self.policy_key, policy), ("LC_ALL", "C.UTF-8"), ("PYTHONNOUSERSITE", "true")),
        )
        return bazel_contract._verify_driver(action, mode, expected, self.evidence)

    def test_debug_accepts_absent_or_zero_optimization_and_effective_macro_undefines(self):
        for flags in (
            ["-stack-protector", "2"],
            ["-O0", "-stack-protector", "3", "-D_FORTIFY_SOURCE=0"],
            ["-O2", "-O0", "-stack-protector", "2", "-D", "_FORTIFY_SOURCE=2", "-U", "_FORTIFY_SOURCE"],
        ):
            with self.subTest(flags=flags):
                self.assertEqual(self.check(flags), 0)
                self.assertEqual(self.evidence["status"], "passed")
                self.assertEqual(self.evidence["exit"], 0)
        self.assertEqual(
            self.evidence["effective"], {"optimization": "-O0", "stack_protector": 2, "fortify_source": None}
        )
        self.assertEqual(self.evidence["cc1_argv"][0], "/fixture compiler/clang")

    def test_optimized_accepts_fortify_two_or_three_from_selected_nix_policy(self):
        for optimization, fortify, policy in (
            ("-O2", "2", "stackprotector fortify"),
            ("-O3", "3", "stackprotector fortify3"),
            ("-O2", "2", "stackprotector fortify3"),
        ):
            with self.subTest(optimization=optimization, fortify=fortify, policy=policy):
                self.assertEqual(
                    self.check([optimization, "-stack-protector", "2", "-D_FORTIFY_SOURCE=" + fortify], "opt", policy),
                    0,
                )
                self.assertEqual(self.evidence["effective"]["fortify_source"], fortify)
                self.assertEqual(self.evidence["hardening_policy"], policy.split())

    def test_mode_fortify_protector_and_gsl_regressions_fail_despite_successful_driver_exit(self):
        for mode, flags, policy, diagnostic in (
            ("dbg", ["-O2", "-stack-protector", "2"], "stackprotector", "remain unoptimized"),
            ("dbg", ["-O0", "-D_FORTIFY_SOURCE=2"], "", "must not enable _FORTIFY_SOURCE"),
            ("dbg", ["-O0", "-DNDEBUG=0"], "", "must not define NDEBUG"),
            ("opt", ["-O0"], "", "requires -O2 or -O3"),
            ("opt", ["-O2", "-D_FORTIFY_SOURCE=0"], "fortify", "_FORTIFY_SOURCE=2 or 3"),
            ("opt", ["-O2", "-D_FORTIFY_SOURCE=2", "-U_FORTIFY_SOURCE"], "fortify3", "_FORTIFY_SOURCE=2 or 3"),
            ("dbg", ["-O0", "-stack-protector", "1"], "stackprotector", "strong/all protection"),
            (
                "dbg",
                ["-O0", "-stack-protector", "2", "-stack-protector", "0"],
                "stackprotector",
                "strong/all protection",
            ),
            ("dbg", ["-O0"], "stackprotector", "strong/all protection"),
            ("dbg", ["-O0", "-Ugsl_CONFIG_CONTRACT_CHECKING_ON"], "", "retain GSL"),
        ):
            with self.subTest(mode=mode, flags=flags, policy=policy):
                self.assertEqual(self.check(flags, mode, policy), 1)
                self.assertEqual(self.evidence["exit"], 0)
                self.assertEqual(self.evidence["status"], "contract failed")
                self.assertTrue(any(diagnostic in error for error in self.evidence["errors"]))

    def test_driver_failure_preserves_exact_exit_and_both_streams_without_parsing(self):
        (self.root / "driver.exit").write_text("27", encoding="utf-8")
        self.assertEqual(self.check(["malformed output is irrelevant after process failure"]), 27)
        self.assertEqual(self.evidence["exit"], 27)
        self.assertEqual(self.evidence["status"], "driver failed")
        self.assertNotIn("effective", self.evidence)
        self.assertEqual(Path(self.evidence["stdout"]).read_bytes(), (self.root / "driver.stdout").read_bytes())
        self.assertEqual(Path(self.evidence["stderr"]).read_bytes(), (self.root / "driver.stderr").read_bytes())
        call = json.loads((self.root / "driver.calls.jsonl").read_text())
        self.assertEqual(call["argv"], [str(self.compiler), "-c", "source.cpp", "-###"])
        self.assertEqual(
            call["environment"], {self.policy_key: "stackprotector", "LC_ALL": "C.UTF-8", "PYTHONNOUSERSITE": "true"}
        )
        self.assertEqual(call["cwd"], str(self.root))

    def test_non_utf8_banner_is_retained_while_quoted_cc1_is_decoded(self):
        write_cc1(self.root, ["-O0", "-stack-protector", "2"])
        path = self.root / "driver.stderr"
        raw = b"wrapper warning: \xff\n" + path.read_bytes()
        path.write_bytes(raw)
        action = bazel_contract.CompileAction(
            "//fixture:source", (str(self.compiler), "source.cpp"), environment=(("LC_ALL", "C.UTF-8"),)
        )
        self.assertEqual(
            bazel_contract._verify_driver(action, "dbg", {self.policy_key: "stackprotector"}, self.evidence),
            0,
        )
        self.assertEqual(Path(self.evidence["stderr"]).read_bytes(), raw)


if __name__ == "__main__":
    unittest.main()
