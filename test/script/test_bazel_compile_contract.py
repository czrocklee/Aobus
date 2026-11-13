"""Compiler-policy regressions and semantic checks for captured aquery jsonproto.

Native callers supply their own evidence paths. Capture with --output=jsonproto,
--include_artifacts=true, --include_commandline=true, --include_param_files=true
and --include_pruned_inputs=true so SPA checks use potential inputs consistently.
These unit tests exercise the checker; they do not run or certify native actions.
"""

import copy
import json
import subprocess
import unittest
from dataclasses import replace

from ao.core.bazel_contract import (
    LINUX_EXTRA_WARNINGS,
    SPA_FLAGS,
    CompileAction,
    compile_contract_errors,
    extract_compile_actions,
    extract_native_actions,
    include_paths,
    link_archive_contract_errors,
    parse_unix_warnings,
    unix_warnings,
)
from ao.core.gitfiles import git_command
from ao.core.paths import PROJECT_ROOT


class BazelCompilePolicyTest(unittest.TestCase):
    def test_warning_policy_reads_only_the_shared_list_literal(self):
        # The checker shares the list the Unix toolchains load instead of restating it.
        policy = '"""Policy."""\n\nOTHER = ["-Wfoo"]\nUNIX_WARNINGS = [\n    "-Wall",\n    "-Wformat=2",\n]\n'
        self.assertEqual(parse_unix_warnings(policy), {"-Wall", "-Wformat=2"})
        self.assertTrue(unix_warnings())

    def test_ignored_prefixes_never_hide_tracked_files(self):
        lines = (PROJECT_ROOT / ".bazelignore").read_text(encoding="utf-8").splitlines()
        prefixes = [line for line in lines if line and not line.startswith("#")]
        tracked = subprocess.run(
            git_command("ls-files", "-z", source_discovery=True),
            cwd=PROJECT_ROOT,
            capture_output=True,
            text=True,
            check=True,
        ).stdout.split("\0")

        def hidden(prefix: str) -> list[str]:
            return [path for path in tracked if path.startswith(prefix + "/")]

        self.assertTrue(hidden("app"))
        for prefix in prefixes:
            # Bazel takes each line as a literal workspace-relative prefix.
            self.assertNotRegex(prefix, r"[*?\[\\]|^/|/$", prefix)
            self.assertEqual(hidden(prefix), [], prefix)

    # macOS public-macro, tool-map and bootstrap wiring is exercised in
    # test_bazel_macos_composition. The semantic command-line checks below stay
    # independent of the platform implementation and its upstream feature graph.


class BazelAqueryContractTest(unittest.TestCase):
    @staticmethod
    def action(platform="linux", mode="opt", extra=(), inputs=(), quote_includes=()):
        windows = platform == "windows"
        flags = ["cl.exe" if windows else "clang++", "-Dgsl_CONFIG_CONTRACT_CHECKING_ON"]
        flags += (
            [
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
            ]
            if windows
            else ["-std=c++26", *sorted(unix_warnings())]
        )
        if platform == "linux":
            flags += sorted(LINUX_EXTRA_WARNINGS)
        if mode == "opt":
            flags += ["/O2" if windows else "-O2", "-DNDEBUG", "-Dgsl_CONFIG_CONTRACT_VIOLATION_TERMINATES"]
        elif mode == "dbg":
            flags += ["/Od", "/Z7"] if windows else ["-O0", "-g"]
            flags += ["-Dgsl_CONFIG_CONTRACT_VIOLATION_ASSERTS"]
        else:
            flags += ["/Od"] if windows else ["-O0", "-g0"]
        return CompileAction(
            "//test:audio_test_support",
            (*flags, *extra, "test/audio/EngineTestSupport.cpp"),
            frozenset(inputs),
            frozenset(quote_includes),
        )

    def test_all_platform_mode_contracts_accept_effective_policy(self):
        for platform in ("linux", "macos", "windows"):
            for mode in ("dbg", "opt", "fastbuild"):
                with self.subTest(platform=platform, mode=mode):
                    self.assertEqual(compile_contract_errors([self.action(platform, mode)], platform, mode), [])

    def test_cpp_standard_is_required_and_later_downgrades_are_rejected(self):
        for platform in ("linux", "macos", "windows"):
            windows = platform == "windows"
            required = "/std:c++latest" if windows else "-std=c++26"
            downgrade = "/std:c++20" if windows else "-std=c++20"
            alias = "-std:c++latest" if windows else "--std=c++26"
            for mode in ("dbg", "opt", "fastbuild"):
                with self.subTest(platform=platform, mode=mode):
                    action = self.action(platform, mode)
                    missing = replace(action, argv=tuple(flag for flag in action.argv if flag != required))
                    errors = compile_contract_errors([missing], platform, mode)
                    self.assertEqual(len(errors), 1)
                    self.assertIn("language standard", errors[0])
                    for override in (downgrade, "-std:c++17" if windows else "--std=gnu++26"):
                        overridden = replace(action, argv=(*action.argv, override))
                        self.assertEqual(compile_contract_errors([overridden], platform, mode), errors)
                        restored = replace(overridden, argv=(*overridden.argv, alias))
                        self.assertEqual(compile_contract_errors([restored], platform, mode), [])
                    aliased = replace(action, argv=tuple(alias if flag == required else flag for flag in action.argv))
                    self.assertEqual(compile_contract_errors([aliased], platform, mode), [])

    def test_windows_language_switches_are_required_and_last_opposite_wins(self):
        for required, opposite in (
            ("/permissive-", "/permissive"),
            ("/Zc:__cplusplus", "/Zc:__cplusplus-"),
            ("/Zc:preprocessor", "/Zc:preprocessor-"),
        ):
            for mode in ("dbg", "opt", "fastbuild"):
                with self.subTest(required=required, mode=mode):
                    action = self.action("windows", mode)
                    missing = replace(action, argv=tuple(flag for flag in action.argv if flag != required))
                    errors = compile_contract_errors([missing], "windows", mode)
                    self.assertEqual(len(errors), 1)
                    self.assertIn(required, errors[0])
                    overridden = self.action("windows", mode, ["-" + opposite[1:]])
                    self.assertEqual(compile_contract_errors([overridden], "windows", mode), errors)
                    restored = self.action("windows", mode, [opposite, "-" + required[1:]])
                    self.assertEqual(compile_contract_errors([restored], "windows", mode), [])

    def test_windows_utf8_requires_switch_and_tracks_explicit_charset_overrides(self):
        action = self.action("windows")
        missing = replace(action, argv=tuple(flag for flag in action.argv if flag != "/utf-8"))
        errors = compile_contract_errors([missing], "windows", "opt")
        self.assertEqual(len(errors), 1)
        self.assertIn("/utf-8", errors[0])
        for kind in ("source", "execution"):
            for prefix in ("/", "-"):
                with self.subTest(kind=kind, prefix=prefix):
                    override = prefix + kind + "-charset:.1252"
                    self.assertEqual(
                        compile_contract_errors([self.action("windows", extra=[override])], "windows", "opt"), errors
                    )
                    for restoration in (
                        "/utf-8",
                        "-utf-8",
                        prefix + kind + "-charset:utf-8",
                        prefix + kind + "-charset:.65001",
                    ):
                        restored = self.action("windows", extra=[override, restoration])
                        self.assertEqual(compile_contract_errors([restored], "windows", "opt"), [])
        # Explicit charsets alone do not establish the required toolchain switch.
        without_switch = replace(missing, argv=(*missing.argv, "/source-charset:utf-8", "/execution-charset:utf-8"))
        self.assertEqual(compile_contract_errors([without_switch], "windows", "opt"), errors)

    def test_windows_critical_defines_reject_undefinition_or_redefinition(self):
        for name in (
            "NOMINMAX",
            "WIN32_LEAN_AND_MEAN",
            "UNICODE",
            "_UNICODE",
            "_CRT_SECURE_NO_WARNINGS",
            "WINVER",
            "_WIN32_WINNT",
        ):
            expected = "0x0A00" if name in {"WINVER", "_WIN32_WINNT"} else "1"
            mode = "opt"
            with self.subTest(name=name):
                action = self.action("windows", mode)
                missing = replace(action, argv=tuple(flag for flag in action.argv if not flag.startswith("/D" + name)))
                errors = compile_contract_errors([missing], "windows", mode)
                self.assertEqual(len(errors), 1)
                self.assertIn(name + "=", errors[0])
                for override in (["/U" + name], ["-U", name], ["/D" + name + "=0"], ["-D", name + "=invalid"]):
                    overridden = self.action("windows", mode, override)
                    self.assertEqual(compile_contract_errors([overridden], "windows", mode), errors)
                    restored = self.action("windows", mode, [*override, "/D" + name + "=" + expected])
                    self.assertEqual(compile_contract_errors([restored], "windows", mode), [])
        for name in ("WINVER", "_WIN32_WINNT"):
            for value in ("0X0a00", "2560"):
                with self.subTest(name=name, value=value):
                    self.assertEqual(
                        compile_contract_errors(
                            [self.action("windows", extra=["/D" + name + "=" + value])], "windows", "opt"
                        ),
                        [],
                    )

    def test_later_overrides_cannot_masquerade_as_optimized_or_debug_builds(self):
        for platform, mode, override in (
            ("linux", "opt", "-O0"),
            ("macos", "opt", "-UNDEBUG"),
            ("windows", "opt", "/Od"),
            ("linux", "dbg", "-g0"),
            ("macos", "dbg", "-DNDEBUG=0"),
            ("windows", "dbg", "/Z7-"),
        ):
            with self.subTest(platform=platform, mode=mode, override=override):
                self.assertTrue(compile_contract_errors([self.action(platform, mode, [override])], platform, mode))

    def test_gsl_remains_enabled_independently_of_ndebug(self):
        for override in (
            "-Ugsl_CONFIG_CONTRACT_CHECKING_ON",
            "/Dgsl_CONFIG_CONTRACT_CHECKING_ON=0",
            "-Dgsl_CONFIG_CONTRACT_CHECKING_OFF",
            "-Dgsl_CONFIG_CONTRACT_VIOLATION_ASSERTS",
        ):
            with self.subTest(override=override):
                errors = compile_contract_errors([self.action(extra=[override])], "linux", "opt")
                self.assertTrue(any("GSL" in error for error in errors))

    def test_warning_presence_does_not_hide_effective_suppression(self):
        for platform, override in (
            ("linux", "-Wno-error"),
            ("macos", "-Wno-extra"),
            ("linux", "-w"),
            ("macos", "-Wno-everything"),
            ("windows", "/W0"),
            ("windows", "/WX-"),
            ("windows", "/w"),
        ):
            with self.subTest(platform=platform, override=override):
                errors = compile_contract_errors([self.action(platform, extra=[override])], platform, "opt")
                self.assertTrue(any("warning" in error for error in errors))

    def test_linux_toolchain_diagnostics_cannot_be_omitted_or_overridden(self):
        for required in LINUX_EXTRA_WARNINGS:
            with self.subTest(required=required):
                action = self.action()
                missing = replace(action, argv=tuple(flag for flag in action.argv if flag != required))
                errors = compile_contract_errors([missing], "linux", "opt")
                self.assertEqual(len(errors), 1)
                self.assertIn(required, errors[0])
                opposite = (
                    "-W" + required.removeprefix("-Wno-")
                    if required.startswith("-Wno-")
                    else required.replace("-W", "-Wno-", 1)
                )
                overridden = self.action(extra=[opposite])
                self.assertEqual(compile_contract_errors([overridden], "linux", "opt"), errors)
                restored = self.action(extra=[opposite, required])
                self.assertEqual(compile_contract_errors([restored], "linux", "opt"), [])

    def test_external_system_search_paths_preserve_quotes_and_windows_spaces(self):
        for platform, includes in (
            ("linux", ["-iquote", ".", "-Iinclude", "-isystem", "external/+deps/artifacts/include"]),
            ("macos", ["-iquoteexternal/+deps", "-isystemexternal/+deps/include"]),
            ("windows", ["/Iinclude", "/external:I", "external/+deps/Program Files/include", "/external:W0"]),
        ):
            with self.subTest(platform=platform):
                self.assertEqual(compile_contract_errors([self.action(platform, extra=includes)], platform, "opt"), [])
        self.assertEqual(
            list(include_paths(["/external:IC:\\Program Files\\include"])), [("system", "C:/Program Files/include")]
        )
        for platform, includes in (
            ("linux", ["-Iexternal/+deps/include"]),
            ("windows", ["/I", "external/+deps/include"]),
        ):
            errors = compile_contract_errors([self.action(platform, extra=includes)], platform, "opt")
            self.assertTrue(any("imported include" in error for error in errors))

    def test_msvc_quote_context_accepts_only_exact_declared_roots(self):
        roots = ("", "external/+deps", "bazel-out/x64_windows-opt/bin/external/+deps")
        flags = ["/I.", "/Iexternal/+deps", "/Ibazel-out/x64_windows-opt/bin/external/+deps"]
        action = self.action("windows", extra=flags, quote_includes=roots)
        self.assertEqual(compile_contract_errors([action], "windows", "opt"), [])
        self.assertTrue(compile_contract_errors([self.action("windows", extra=flags)], "windows", "opt"))
        child = self.action("windows", extra=["/Iexternal/+deps/include"], quote_includes=roots)
        self.assertTrue(
            any("imported include" in error for error in compile_contract_errors([child], "windows", "opt"))
        )

    def test_declared_quote_context_does_not_exempt_unix_or_ambient_nix_includes(self):
        for platform in ("linux", "macos"):
            with self.subTest(platform=platform):
                action = self.action(
                    platform, extra=["-I.", "-Iexternal/+deps"], quote_includes=[".", "external/+deps"]
                )
                self.assertEqual(len(compile_contract_errors([action], platform, "opt")), 2)
        nix = "/nix/store/icu-dev/include"
        action = self.action("windows", extra=["/I" + nix], quote_includes=[nix])
        self.assertTrue(any("ambient Nix" in error for error in compile_contract_errors([action], "windows", "opt")))

    def test_msvc_external_includes_reject_missing_or_invalid_warning_level(self):
        for options in (
            [],
            ["/external:w0"],
            ["/EXTERNAL:W0"],
            ["/external:W0-"],
            ["/external:W0", "/external:W1"],
        ):
            with self.subTest(options=options):
                action = self.action("windows")
                action = replace(
                    action,
                    argv=(
                        *(flag for flag in action.argv if flag != "/external:W0"),
                        "/external:I",
                        "external/+deps/include",
                        *options,
                    ),
                )
                self.assertEqual(
                    compile_contract_errors([action], "windows", "opt"),
                    [
                        f"{action.label} (test/audio/EngineTestSupport.cpp): "
                        "MSVC external include paths require effective /external:W0"
                    ],
                )

    def test_msvc_external_warning_level_accepts_either_order_and_last_valid_level(self):
        for options in (
            ["/external:W0", "/external:I", "external/+deps/include"],
            ["/external:Iexternal/+deps/include", "/external:W0"],
            ["/external:W4", "/external:Iexternal/+deps/include", "/external:W0"],
            ["-external:Iexternal/+deps/include", "-external:W0"],
        ):
            with self.subTest(options=options):
                self.assertEqual(compile_contract_errors([self.action("windows", extra=options)], "windows", "opt"), [])

    def test_only_explicit_compiler_include_roots_allow_absolute_nix_includes(self):
        compiler = "/nix/store/compiler/lib/clang/22/include"
        action = self.action(extra=["-isystem", compiler])
        self.assertEqual(compile_contract_errors([action], "linux", "opt", compiler_include_roots=[compiler]), [])
        leaked = self.action(extra=["-isystem", "/nix/store/icu-dev/include"])
        errors = compile_contract_errors([leaked], "linux", "opt", compiler_include_roots=[compiler])
        self.assertTrue(any("ambient Nix" in error for error in errors))
        self.assertTrue(compile_contract_errors([self.action(extra=["-isystem", ""])], "linux", "opt"))
        with self.assertRaises(ValueError):
            compile_contract_errors([action], "linux", "opt", compiler_include_roots=["/nix/store"])

    def test_spa_requirements_cover_support_actions_and_last_effective_flags(self):
        inputs = ["external/+deps/artifacts/pipewire/spa-0.2/spa/utils/defs.h"]
        missing = self.action(inputs=inputs)
        errors = compile_contract_errors([missing], "linux", "opt", require_spa=True)
        self.assertEqual(sum("declared SPA" in error for error in errors), 2)
        self.assertTrue(all("//test:audio_test_support" in error for error in errors))
        complete = self.action(extra=SPA_FLAGS, inputs=inputs)
        self.assertEqual(compile_contract_errors([complete], "linux", "opt", require_spa=True), [])
        override = self.action(extra=[*SPA_FLAGS, "-fstrict-aliasing"], inputs=inputs)
        self.assertTrue(compile_contract_errors([override], "linux", "opt", require_spa=True))
        self.assertIn("no declared SPA", compile_contract_errors([self.action()], "linux", "opt", require_spa=True)[0])
        self.assertIn("no first-party", compile_contract_errors([], "linux", "opt")[0])

    def test_aquery_resolves_transitive_input_paths_and_expands_msvc_response_argv(self):
        document = {
            "targets": [{"id": 1, "label": "//test:audio_test_support"}],
            "pathFragments": [
                {"id": 1, "label": "external"},
                {"id": 2, "parentId": 1, "label": "spa"},
                {"id": 3, "parentId": 2, "label": "defs.h"},
            ],
            "artifacts": [{"id": 1, "pathFragmentId": 3}],
            "depSetOfFiles": [{"id": 1, "directArtifactIds": [1]}, {"id": 2, "transitiveDepSetIds": [1]}],
            "actions": [
                {"mnemonic": "CppLink"},
                {
                    "mnemonic": "CppCompile",
                    "targetId": 1,
                    "arguments": ["cl.exe", "@flags.params"],
                    "paramFiles": [
                        {
                            "execPath": "flags.params",
                            "arguments": ["/O2", "/external:IC:/Program Files/include", "/external:W0"],
                        }
                    ],
                    "inputDepSetIds": [2],
                },
            ],
        }
        self.assertEqual(
            extract_compile_actions(document),
            [
                CompileAction(
                    "//test:audio_test_support",
                    ("cl.exe", "/O2", "/external:IC:/Program Files/include", "/external:W0"),
                    frozenset({"external/spa/defs.h"}),
                )
            ],
        )
        document["actions"][1]["paramFiles"] = []
        with self.assertRaisesRegex(ValueError, "unexpanded"):
            extract_compile_actions(document)
        document["actions"][1]["arguments"] = ["cl.exe"]
        document["actions"][1]["inputDepSetIds"] = [999]
        with self.assertRaises(KeyError):
            extract_compile_actions(document)


class BazelExecutionConfigurationTest(unittest.TestCase):
    def test_aquery_role_comes_from_configuration_ids_not_paths_or_mnemonics(self):
        for is_tool in (False, True):
            with self.subTest(is_tool=is_tool):
                document = BazelActionEnvironmentTest.document()
                document["configuration"] = [{"id": 7, "isTool": is_tool, "mnemonic": "opaque"}]
                document["actions"][0]["configurationId"] = 7
                self.assertIs(extract_compile_actions(document)[0].is_tool, is_tool)

    def test_quote_contexts_are_extracted_per_configured_owner_and_missing_or_ambiguous_evidence_fails(self):
        document = {
            "targets": [{"id": 1, "label": "//lib:consumer"}],
            "configuration": [{"id": 7}, {"id": 8, "isTool": True}],
            "pathFragments": [
                {"id": 1, "label": "target.aobus-compile-context.json"},
                {"id": 2, "label": "tool.aobus-compile-context.json"},
            ],
            "artifacts": [{"id": 1, "pathFragmentId": 1}, {"id": 2, "pathFragmentId": 2}],
            "actions": [
                {
                    "mnemonic": "CppCompile",
                    "targetId": 1,
                    "configurationId": 7,
                    "arguments": ["cl.exe", "/Iexternal/undeclared", "source.cpp"],
                },
                {
                    "mnemonic": "CppCompile",
                    "targetId": 1,
                    "configurationId": 8,
                    "arguments": ["cl.exe", "tool.cpp"],
                },
                {
                    "mnemonic": "FileWrite",
                    "targetId": 1,
                    "configurationId": 7,
                    "outputIds": [1],
                    "fileContents": json.dumps({"quote_includes": ["", "external/private"], "compile_argv": []}),
                },
                {
                    "mnemonic": "FileWrite",
                    "targetId": 1,
                    "configurationId": 8,
                    "outputIds": [2],
                    "fileContents": json.dumps({"quote_includes": ["", "external/exec-private"], "compile_argv": []}),
                },
            ],
        }
        target, tool = extract_compile_actions(document, require_compile_context=True)
        self.assertEqual(target.quote_includes, frozenset({"", "external/private"}))
        self.assertEqual(tool.quote_includes, frozenset({"", "external/exec-private"}))
        self.assertFalse(target.is_tool)
        self.assertTrue(tool.is_tool)
        for failure in ("missing", "duplicate", "wrong-owner", "missing-content", "invalid-content"):
            with self.subTest(failure=failure):
                invalid = copy.deepcopy(document)
                context = invalid["actions"][2]
                if failure == "missing":
                    invalid["actions"].remove(context)
                elif failure == "duplicate":
                    invalid["actions"].append(copy.deepcopy(context))
                elif failure == "wrong-owner":
                    invalid["targets"].append({"id": 2, "label": "//lib:other"})
                    context["targetId"] = 2
                elif failure == "missing-content":
                    del context["fileContents"]
                else:
                    context["fileContents"] = json.dumps(
                        {"quote_includes": ["external/private", 1], "compile_argv": []}
                    )
                with self.assertRaises((KeyError, ValueError)):
                    extract_compile_actions(invalid, require_compile_context=True)

    def test_msvc_param_file_reference_uses_the_same_configured_actions_expanded_command_line(self):
        expanded = ["cl.exe", "/std:c++latest", "/Iexternal/private", "source.cpp"]
        document = {
            "targets": [{"id": 1, "label": "//lib:consumer"}],
            "configuration": [{"id": 7}, {"id": 8, "isTool": True}],
            "pathFragments": [
                {"id": 1, "label": "consumer.aobus-compile-context.json"},
                {"id": 2, "label": "bazel-out/dbg/bin/lib/_objs/consumer/source.obj"},
                {"id": 3, "label": "bazel-out/exec/bin/lib/_objs/consumer/source.obj"},
                {"id": 4, "label": "tool.aobus-compile-context.json"},
            ],
            "artifacts": [
                {"id": 1, "pathFragmentId": 1},
                {"id": 2, "pathFragmentId": 2},
                {"id": 3, "pathFragmentId": 3},
                {"id": 4, "pathFragmentId": 4},
            ],
            "actions": [
                {
                    "mnemonic": "CppCompile",
                    "targetId": 1,
                    "configurationId": 7,
                    "outputIds": [2],
                    "arguments": ["cl.exe", "@bazel-out/dbg/bin/lib/_objs/consumer/source.obj.params"],
                },
                {
                    "mnemonic": "FileWrite",
                    "targetId": 1,
                    "configurationId": 7,
                    "outputIds": [1],
                    "fileContents": json.dumps(
                        {
                            "quote_includes": [""],
                            "compile_argv": [
                                {"outputs": ["bazel-out/dbg/bin/lib/_objs/consumer/source.obj"], "argv": expanded}
                            ],
                        }
                    ),
                },
                {
                    "mnemonic": "FileWrite",
                    "targetId": 1,
                    "configurationId": 8,
                    "outputIds": [4],
                    "fileContents": json.dumps(
                        {
                            "quote_includes": [""],
                            "compile_argv": [
                                {"outputs": ["bazel-out/dbg/bin/lib/_objs/consumer/source.obj"], "argv": ["cl.exe"]}
                            ],
                        }
                    ),
                },
            ],
        }
        (action,) = extract_compile_actions(document, require_compile_context=True)
        self.assertEqual(action.argv, tuple(expanded))
        # Without the aspect's evidence the reference stays unexpanded.
        with self.assertRaisesRegex(ValueError, "unexpanded"):
            extract_compile_actions(document)

        def mutated(change):
            invalid = copy.deepcopy(document)
            context = json.loads(invalid["actions"][1]["fileContents"])
            change(invalid, context)
            invalid["actions"][1]["fileContents"] = json.dumps(context)
            return invalid

        failures = {
            # The tool configuration's capture for the same path never applies to the target action.
            "other-output": (lambda d, c: d["actions"][0].update(outputIds=[3]), "unexpanded"),
            "other-compiler": (lambda d, c: c["compile_argv"][0].update(argv=["clang-cl.exe"]), "another compiler"),
            "duplicate": (lambda d, c: c["compile_argv"].append(c["compile_argv"][0]), "duplicate expanded"),
            "missing-outputs": (lambda d, c: c["compile_argv"][0].update(outputs=[]), "nonempty string lists"),
            "malformed-argv": (lambda d, c: c["compile_argv"][0].update(argv=["cl.exe", 1]), "nonempty string lists"),
            "unknown-key": (lambda d, c: c.update(extra=[]), "quote_includes and compile_argv"),
        }
        for failure, (change, message) in failures.items():
            with self.subTest(failure=failure), self.assertRaisesRegex(ValueError, message):
                extract_compile_actions(mutated(change), require_compile_context=True)

    def test_execution_tools_require_opt_contracts_even_with_debug_targets(self):
        for platform in ("linux", "macos", "windows"):
            with self.subTest(platform=platform):
                target = BazelAqueryContractTest.action(platform, "dbg")
                tool = replace(BazelAqueryContractTest.action(platform, "opt"), is_tool=True)
                self.assertEqual(compile_contract_errors([target, tool], platform, "dbg"), [])
                wrong_tool = replace(target, is_tool=True)
                self.assertTrue(compile_contract_errors([wrong_tool], platform, "dbg"))
                # An optimized ordinary target cannot borrow the tool's exemption.
                self.assertTrue(compile_contract_errors([replace(tool, is_tool=False)], platform, "dbg"))

    def test_windows_imported_headers_require_external_classification_in_target_and_exec_roles(self):
        for is_tool in (False, True):
            for mode in ("dbg", "opt"):
                with self.subTest(is_tool=is_tool, target_mode=mode):
                    compile_mode = "opt" if is_tool else mode
                    action = replace(
                        BazelAqueryContractTest.action(
                            "windows", compile_mode, extra=["/external:Iexternal/provider/include"]
                        ),
                        is_tool=is_tool,
                    )
                    self.assertEqual(compile_contract_errors([action], "windows", mode), [])
                    ordinary = replace(
                        action,
                        argv=tuple(flag.replace("/external:I", "/I") for flag in action.argv),
                    )
                    self.assertTrue(
                        any(
                            "imported include must be system/external" in error
                            for error in compile_contract_errors([ordinary], "windows", mode)
                        )
                    )

    def test_execution_policy_composes_separately_and_never_uses_target_debug_hardening(self):
        clears = dict.fromkeys(
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
        target_policy = {"NIX_HARDENING_ENABLE_fixture": "stackprotector"}
        tool_policy = {"NIX_HARDENING_ENABLE_fixture": "stackprotector fortify"}
        target = replace(
            BazelAqueryContractTest.action(mode="dbg"), environment=tuple((clears | target_policy).items())
        )
        tool = replace(
            BazelAqueryContractTest.action(mode="opt"), is_tool=True, environment=tuple((clears | tool_policy).items())
        )
        self.assertEqual(
            compile_contract_errors(
                [target, tool],
                "linux",
                "dbg",
                expected_environment=target_policy,
                expected_tool_environment=tool_policy,
            ),
            [],
        )
        self.assertTrue(compile_contract_errors([target, tool], "linux", "dbg", expected_environment=target_policy))
        absent_channels = replace(tool, environment=tuple(tool_policy.items()))
        self.assertEqual(
            compile_contract_errors(
                [target, absent_channels],
                "linux",
                "dbg",
                expected_environment=target_policy,
                expected_tool_environment=tool_policy,
            ),
            [],
        )
        for key in (*clears, "NIX_CFLAGS_COMPILE_FOR_BUILD", "NIX_LDFLAGS_FOR_BUILD"):
            with self.subTest(channel=key):
                leaked = replace(absent_channels, environment=tuple((tool_policy | {key: "ambient"}).items()))
                self.assertTrue(
                    compile_contract_errors(
                        [target, leaked],
                        "linux",
                        "dbg",
                        expected_environment=target_policy,
                        expected_tool_environment=tool_policy,
                    )
                )
        missing_target_marker = replace(target, environment=tuple(target_policy.items()))
        self.assertTrue(
            compile_contract_errors(
                [missing_target_marker, absent_channels],
                "linux",
                "dbg",
                expected_environment=target_policy,
                expected_tool_environment=tool_policy,
            )
        )
        wrong = replace(tool, environment=target.environment)
        self.assertTrue(
            compile_contract_errors(
                [target, wrong],
                "linux",
                "dbg",
                expected_environment=target_policy,
                expected_tool_environment=tool_policy,
            )
        )


class BazelActionEnvironmentTest(unittest.TestCase):
    @staticmethod
    def document():
        # This is the real artifact-free shape: no artifact/input tables or IDs.
        return {
            "targets": [{"id": 1, "label": "//lib/utility:utility"}],
            "actions": [
                {
                    "mnemonic": "CppCompile",
                    "targetId": 1,
                    "arguments": ["clang++", "source.cpp"],
                    "environmentVariables": [{"key": "CPATH"}, {"key": "POLICY", "value": "captured"}],
                }
            ],
        }

    def test_artifact_free_jsonproto_accepts_omitted_empty_environment_values(self):
        self.assertEqual(
            extract_compile_actions(self.document()),
            [
                CompileAction(
                    "//lib/utility:utility",
                    ("clang++", "source.cpp"),
                    environment=(("CPATH", ""), ("POLICY", "captured")),
                )
            ],
        )

    def test_expected_policy_and_cleared_channels_apply_to_every_first_party_action(self):
        policy = {"NIX_HARDENING_ENABLE_test_salt": "captured-policy"}
        cleared = dict.fromkeys(
            (
                "CPATH",
                "C_INCLUDE_PATH",
                "CPLUS_INCLUDE_PATH",
                "LIBRARY_PATH",
                "LD_LIBRARY_PATH",
                "NIX_CFLAGS_COMPILE",
                "NIX_CFLAGS_COMPILE_FOR_TARGET",
                "NIX_LDFLAGS",
                "NIX_LDFLAGS_FOR_TARGET",
            ),
            "",
        )
        good = replace(BazelAqueryContractTest.action(), environment=tuple((cleared | policy).items()))
        bad = replace(good, label="//test:other", environment=tuple(cleared.items()))
        external = CompileAction("@@external+//:helper", ("clang++", "-w"))
        self.assertEqual(compile_contract_errors([good, external], "linux", "opt", expected_environment=policy), [])
        errors = compile_contract_errors([good, external, bad], "linux", "opt", expected_environment=policy)
        self.assertEqual(len(errors), 1)
        self.assertIn("//test:other", errors[0])
        self.assertIn("NIX_HARDENING_ENABLE_test_salt", errors[0])
        self.assertIn("no first-party", compile_contract_errors([external], "linux", "opt")[0])
        for key in (*cleared, "NIX_CFLAGS_LINK_FOR_BUILD", "NIX_LDFLAGS_FOR_BUILD"):
            with self.subTest(key=key):
                environment = cleared | policy | {key: "ambient"}
                action = replace(good, environment=tuple(environment.items()))
                self.assertTrue(
                    any(
                        key in error
                        for error in compile_contract_errors([action], "linux", "opt", expected_environment=policy)
                    )
                )
        missing = replace(good, environment=tuple(policy.items()))
        self.assertEqual(
            len(compile_contract_errors([missing], "linux", "opt", expected_environment=policy)), len(cleared)
        )


class BazelProfileCompileContractTest(unittest.TestCase):
    """Release IPO and sampling compile policy from the fixed documented matrix."""

    MAINWINDOW = "app/windows-winui/MainWindow.xaml.cpp"

    @staticmethod
    def unix_action(mode="opt", extra=()):
        return BazelAqueryContractTest.action("linux", mode, extra)

    @staticmethod
    def windows_action(mode="opt", extra=()):
        return BazelAqueryContractTest.action("windows", mode, extra)

    @classmethod
    def with_source(cls, action, source):
        return replace(action, argv=tuple(source if arg.endswith(".cpp") else arg for arg in action.argv))

    def test_release_compiles_require_effective_thin_lto(self):
        self.assertEqual(
            compile_contract_errors(
                [self.unix_action(extra=["-flto=thin", "-fno-assume-unique-vtables"])],
                "linux",
                "opt",
                profile="release",
            ),
            [],
        )
        self.assertEqual(
            compile_contract_errors(
                [BazelAqueryContractTest.action("macos", "opt", ["-flto=thin"])], "macos", "opt", profile="release"
            ),
            [],
        )
        for mutation in ([], ["-flto=full"], ["-fno-lto"], ["-flto=thin", "-fno-lto"]):
            with self.subTest(mutation=mutation):
                errors = compile_contract_errors(
                    [self.unix_action(extra=["-fno-assume-unique-vtables", *mutation])],
                    "linux",
                    "opt",
                    profile="release",
                )
                self.assertEqual(len(errors), 1)
                self.assertIn("effective -flto=thin", errors[0])

    def test_linux_release_compiles_require_effective_no_assume_unique_vtables(self):
        # The pinned LLVM 22 can localize vtable copies across shared objects,
        # so Linux Release C++ compilation must not admit exact-vptr identity;
        # the last effective switch wins in either ordering.
        for mutation in (
            [],
            ["-fassume-unique-vtables"],
            ["-fno-assume-unique-vtables", "-fassume-unique-vtables"],
        ):
            with self.subTest(mutation=mutation):
                errors = compile_contract_errors(
                    [self.unix_action(extra=["-flto=thin", *mutation])], "linux", "opt", profile="release"
                )
                self.assertEqual(len(errors), 1)
                self.assertIn("effective -fno-assume-unique-vtables", errors[0])
        # A positive switch followed by the negative one restores the policy.
        self.assertEqual(
            compile_contract_errors(
                [self.unix_action(extra=["-flto=thin", "-fassume-unique-vtables", "-fno-assume-unique-vtables"])],
                "linux",
                "opt",
                profile="release",
            ),
            [],
        )
        # The requirement is scoped to Linux Release target compilations: other
        # profiles, platforms and execution configurations neither require nor
        # reject the switch.
        sampling = ["-g", "-fno-omit-frame-pointer", "-fno-lto"]
        for extra in (sampling, [*sampling, "-fno-assume-unique-vtables"]):
            with self.subTest(sampling_extra=extra):
                self.assertEqual(
                    compile_contract_errors([self.unix_action(extra=extra)], "linux", "opt", profile="sampling"),
                    [],
                )
        self.assertEqual(compile_contract_errors([self.unix_action("dbg")], "linux", "dbg"), [])
        tool = replace(BazelAqueryContractTest.action("linux", "opt", ["-fno-assume-unique-vtables"]), is_tool=True)
        self.assertEqual(compile_contract_errors([tool], "linux", "dbg", profile="release"), [])

    def test_sampling_compiles_require_effective_debug_information_frame_pointers_and_no_lto(self):
        sampling = ["-g", "-fno-omit-frame-pointer", "-fno-lto"]
        self.assertEqual(
            compile_contract_errors([self.unix_action(extra=sampling)], "linux", "opt", profile="sampling"),
            [],
        )
        for mutation, message in (
            (["-fno-omit-frame-pointer", "-fno-lto"], "debug information"),
            (["-g", "-fno-lto"], "frame pointers"),
            (["-g", "-fno-omit-frame-pointer"], "effective -fno-lto"),
            (["-g", "-fno-omit-frame-pointer", "-fno-lto", "-g0"], "debug information"),
            (["-g", "-fno-omit-frame-pointer", "-fno-lto", "-fomit-frame-pointer"], "frame pointers"),
            (["-g", "-fno-omit-frame-pointer", "-fno-lto", "-flto=thin"], "effective -fno-lto"),
            (["-g", "-fno-omit-frame-pointer", "-flto=thin"], "effective -fno-lto"),
        ):
            with self.subTest(mutation=mutation):
                errors = compile_contract_errors([self.unix_action(extra=mutation)], "linux", "opt", profile="sampling")
                self.assertEqual(len(errors), 1)
                self.assertIn(message, errors[0])
        with self.assertRaises(ValueError):
            compile_contract_errors([self.windows_action()], "windows", "opt", profile="sampling")

    def test_execution_configurations_never_inherit_target_profile_policy(self):
        for platform, profile, leak, message in (
            ("linux", "release", "-flto=thin", "IPO flags"),
            ("linux", "sampling", "-g", "sampling debug information"),
            ("linux", "sampling", "-fno-lto", "IPO flags"),
            ("macos", "release", "-flto=thin", "IPO flags"),
            ("windows", "release", "/GL", "/GL switches"),
        ):
            with self.subTest(platform=platform, leak=leak):
                tool = replace(BazelAqueryContractTest.action(platform, "opt", [leak]), is_tool=True)
                errors = compile_contract_errors([tool], platform, "dbg", profile=profile)
                self.assertEqual(len(errors), 1)
                self.assertIn(message, errors[0])

    def test_ipo_outside_the_release_profile_is_rejected(self):
        for platform, mode in (("linux", "dbg"), ("linux", "opt"), ("macos", "opt")):
            with self.subTest(platform=platform, mode=mode):
                errors = compile_contract_errors(
                    [BazelAqueryContractTest.action(platform, mode, ["-flto=thin"])], platform, mode
                )
                self.assertEqual(len(errors), 1)
                self.assertIn("outside the Release profile", errors[0])

    def test_windows_release_requires_effective_gl_with_the_exact_admitted_exception(self):
        self.assertEqual(
            compile_contract_errors([self.windows_action(extra=["/GL"])], "windows", "opt", profile="release"),
            [],
        )
        missing = compile_contract_errors([self.windows_action()], "windows", "opt", profile="release")
        self.assertEqual(len(missing), 1)
        self.assertIn("effective /GL", missing[0])
        disabled = compile_contract_errors(
            [self.windows_action(extra=["/GL", "/GL-"])], "windows", "opt", profile="release"
        )
        self.assertEqual(disabled, missing)
        restored = compile_contract_errors(
            [self.windows_action(extra=["/GL", "/GL-", "/GL"])], "windows", "opt", profile="release"
        )
        self.assertEqual(restored, [])
        mainwindow = self.with_source(self.windows_action(extra=["/GL-"]), self.MAINWINDOW)
        self.assertEqual(
            compile_contract_errors(
                [mainwindow], "windows", "opt", profile="release", windows_per_file_sources={self.MAINWINDOW}
            ),
            [],
        )
        # Without the admitted toolset exception the same authored source
        # requires /GL like every other Release compilation.
        unadmitted = compile_contract_errors([mainwindow], "windows", "opt", profile="release")
        self.assertEqual(len(unadmitted), 1)
        self.assertIn("effective /GL", unadmitted[0])
        # The exception must keep its effective /GL-; broader exemptions fail.
        reenabled = self.with_source(self.windows_action(extra=["/GL-", "/GL"]), self.MAINWINDOW)
        errors = compile_contract_errors(
            [reenabled], "windows", "opt", profile="release", windows_per_file_sources={self.MAINWINDOW}
        )
        self.assertEqual(len(errors), 1)
        self.assertIn("effective /GL-", errors[0])
        for invalid in ("app/windows-winui/Other.xaml.cpp", self.MAINWINDOW + ".h"):
            with self.subTest(invalid=invalid):
                with self.assertRaises(ValueError):
                    compile_contract_errors(
                        [mainwindow], "windows", "opt", profile="release", windows_per_file_sources={invalid}
                    )
        with self.assertRaises(ValueError):
            compile_contract_errors(
                [self.unix_action(extra=["-flto=thin"])],
                "linux",
                "opt",
                profile="release",
                windows_per_file_sources={self.MAINWINDOW},
            )

    def test_windows_compiled_pch_and_out_of_profile_gl_are_rejected(self):
        with self.assertRaises(ValueError):
            compile_contract_errors([self.windows_action("opt")], "windows", "opt", profile="sampling")
        with self.assertRaises(ValueError):
            compile_contract_errors([self.unix_action()], "linux", "opt", profile="coverage")
        leaked = compile_contract_errors([self.windows_action("dbg", ["/GL"])], "windows", "dbg")
        self.assertEqual(len(leaked), 1)
        self.assertIn("outside the Release profile", leaked[0])
        for flag in ("/Yc", "/Yu", "/Yupch.h"):
            for mode, profile, extra in (("opt", "release", ["/GL"]), ("dbg", None, [])):
                with self.subTest(flag=flag, mode=mode, profile=profile):
                    errors = compile_contract_errors(
                        [self.windows_action(mode, [*extra, flag])], "windows", mode, profile=profile
                    )
                    self.assertEqual(len(errors), 1)
                    self.assertIn("precompiled headers", errors[0])


class BazelNativeActionContractTest(unittest.TestCase):
    """Final-link, archive, Objective-C++ and LTO-pipeline action contracts."""

    MAINWINDOW = BazelProfileCompileContractTest.MAINWINDOW

    @staticmethod
    def document(entries):
        actions = []
        fragments = []
        artifacts = []
        for mnemonic, argv, is_tool, outputs in entries:
            for output in outputs:
                fragments.append({"id": len(fragments) + 1, "label": output})
                artifacts.append({"id": len(artifacts) + 1, "pathFragmentId": len(fragments)})
            actions.append(
                {
                    "mnemonic": mnemonic,
                    "targetId": 1,
                    "configurationId": 2 if is_tool else 1,
                    "arguments": list(argv),
                    "outputIds": list(range(len(artifacts) - len(outputs) + 1, len(artifacts) + 1)),
                }
            )
        return {
            "targets": [{"id": 1, "label": "//app:product"}],
            "configuration": [{"id": 1}, {"id": 2, "isTool": True}],
            "pathFragments": fragments,
            "artifacts": artifacts,
            "actions": actions,
        }

    @classmethod
    def extract(cls, entries):
        return extract_native_actions(cls.document(entries))

    def test_extract_resolves_roles_outputs_and_expands_param_files(self):
        entries = [
            ("CppLink", ["clang++", "-o", "app", "source.o"], False, ["app"]),
            ("CppArchive", ["llvm-ar", "rcs", "out.a", "source.o"], False, ["out.a"]),
            ("ObjcCompile", ["clang++", "-c", "source.mm"], False, ["source.o"]),
            ("CppLinkstampCompile", ["clang++", "-c", "_linkstamp_.cc"], False, ["_linkstamp_.o"]),
            ("ObjcExecutable", ["clang++", "-o", "tool"], True, ["tool"]),
            ("LtoBackend", ["clang++", "-c", "backend.bc"], False, ["backend.o"]),
            ("CppCompile", ["clang++", "-c", "source.cpp"], False, ["ignored.o"]),
        ]
        actions = self.extract(entries)
        self.assertEqual(
            [(action.mnemonic, action.is_tool) for action in actions],
            [
                ("CppLink", False),
                ("CppArchive", False),
                ("ObjcCompile", False),
                ("CppLinkstampCompile", False),
                ("ObjcExecutable", True),
                ("LtoBackend", False),
            ],
        )
        self.assertTrue(all(action.label == "//app:product" for action in actions))
        self.assertEqual(actions[0].outputs, ("app",))
        params = self.document(entries[:1])
        params["actions"][0]["arguments"] = ["clang++", "@flags"]
        params["actions"][0]["paramFiles"] = [{"execPath": "flags", "arguments": ["-o", "app"]}]
        self.assertEqual(extract_native_actions(params)[0].argv, ("clang++", "-o", "app"))

    def test_unix_link_and_archive_policy_per_profile(self):
        link = ("clang++", "-o", "app", "source.o")
        archive = ("llvm-ar", "rcs", "out.a", "source.o")
        for profile, link_flags in ((None, []), ("release", ["-flto=thin"]), ("sampling", ["-fno-lto"])):
            with self.subTest(profile=profile):
                errors, counts = link_archive_contract_errors(
                    self.extract(
                        [
                            ("CppLink", [*link, *link_flags], False, ["app"]),
                            ("CppArchive", archive, False, ["out.a"]),
                        ]
                    ),
                    "linux",
                    profile,
                    expected_archive="llvm-ar",
                )
                self.assertEqual(errors, [])
                self.assertEqual(counts["target_link"], 1)
                self.assertEqual(counts["target_archive"], 1)
        for profile, mutation, message in (
            ("release", [], "effective -flto=thin"),
            ("release", ["-flto=thin", "-fno-lto"], "effective -flto=thin"),
            ("release", ["-fno-lto"], "effective -flto=thin"),
            ("sampling", [], "effective -fno-lto"),
            ("sampling", ["-flto=thin"], "effective -fno-lto"),
            (None, ["-flto=thin"], "outside the Release profile"),
        ):
            with self.subTest(profile=profile, mutation=mutation):
                errors, _ = link_archive_contract_errors(
                    self.extract([("CppLink", [*link, *mutation], False, ["app"])]),
                    "linux",
                    profile,
                    expected_archive="llvm-ar",
                )
                self.assertTrue(any("final-link evidence" not in error and message in error for error in errors))

    def test_archives_use_the_selected_archiver_without_lto_switches(self):
        for profile, link_flags in ((None, []), ("release", ["-flto=thin"]), ("sampling", ["-fno-lto"])):
            with self.subTest(profile=profile):
                errors, _ = link_archive_contract_errors(
                    self.extract(
                        [
                            ("CppArchive", ["llvm-ar", "rcs", "out.a", "source.o"], False, ["out.a"]),
                            ("CppLink", ["clang++", *link_flags, "-o", "app"], False, ["app"]),
                        ]
                    ),
                    "linux",
                    profile,
                    expected_archive="llvm-ar",
                )
                self.assertEqual(errors, [])
                substituted, _ = link_archive_contract_errors(
                    self.extract(
                        [
                            ("CppArchive", ["ar", "rcs", "out.a", "source.o"], False, ["out.a"]),
                            ("CppLink", ["clang++", *link_flags, "-o", "app"], False, ["app"]),
                        ]
                    ),
                    "linux",
                    profile,
                    expected_archive="llvm-ar",
                )
                self.assertTrue(any("selected archiver" in error for error in substituted))
                for lto in ("-flto=thin", "-fno-lto"):
                    with self.subTest(lto=lto):
                        carrying, _ = link_archive_contract_errors(
                            self.extract(
                                [
                                    ("CppArchive", ["llvm-ar", "rcs", "out.a", lto], False, ["out.a"]),
                                    ("CppLink", ["clang++", *link_flags, "-o", "app"], False, ["app"]),
                                ]
                            ),
                            "linux",
                            profile,
                            expected_archive="llvm-ar",
                        )
                        self.assertTrue(any("LTO switches" in error for error in carrying))

    def test_execution_and_lto_pipeline_actions_fail_closed(self):
        entries = [
            ("CppLink", ["clang++", "-flto=thin", "-o", "tool"], True, ["tool"]),
            ("LtoIndexing", ["clang++", "index"], False, ["index.out"]),
        ]
        for profile, link_flags in ((None, []), ("release", ["-flto=thin"]), ("sampling", ["-fno-lto"])):
            with self.subTest(profile=profile):
                errors, counts = link_archive_contract_errors(
                    self.extract(
                        [
                            *entries,
                            ("CppLink", ["clang++", *link_flags, "-o", "app"], False, ["app"]),
                            ("CppArchive", ["llvm-ar", "rcs", "out.a"], False, ["out.a"]),
                        ]
                    ),
                    "linux",
                    profile,
                    expected_archive="llvm-ar",
                )
                self.assertTrue(any("index/backend" in error for error in errors))
                self.assertTrue(any("IPO flags" in error for error in errors))
                self.assertEqual(counts["lto_pipeline"], 1)
                self.assertEqual(counts["exec_link"], 1)

    def test_missing_required_action_classes_fail_closed(self):
        for entries, message in (
            ([("CppArchive", ["llvm-ar", "rcs", "out.a"], False, ["out.a"])], "final-link evidence"),
            ([("CppLink", ["clang++", "-o", "app"], False, ["app"])], "archive evidence"),
        ):
            with self.subTest(entries=entries):
                errors, _ = link_archive_contract_errors(
                    self.extract(entries), "linux", None, expected_archive="llvm-ar"
                )
                self.assertTrue(any(message in error for error in errors))

    def test_macos_objective_c_plus_plus_policy(self):
        link = ("clang++", "-o", "app", "source.o")
        archive = ("llvm-libtool-darwin", "-static", "-o", "out.a", "source.o")
        cases = {
            None: (["-O0", "-g"], []),
            "release": (["-O2", "-DNDEBUG"], []),
            "sampling": (["-O2", "-DNDEBUG", "-g", "-fno-omit-frame-pointer", "-fno-lto"], []),
        }
        for profile, (compile_flags, _) in cases.items():
            with self.subTest(profile=profile):
                errors, counts = link_archive_contract_errors(
                    self.extract(
                        [
                            ("ObjcCompile", ["clang++", *compile_flags, "-c", "source.mm"], False, ["source.o"]),
                            (
                                "CppLink",
                                [
                                    *link,
                                    *(
                                        ["-flto=thin"]
                                        if profile == "release"
                                        else ["-fno-lto"]
                                        if profile == "sampling"
                                        else []
                                    ),
                                ],
                                False,
                                ["app"],
                            ),
                            ("CppArchive", archive, False, ["out.a"]),
                        ]
                    ),
                    "macos",
                    profile,
                    expected_archive="llvm-libtool-darwin",
                )
                self.assertEqual(errors, [])
                self.assertEqual(counts["target_objcpp_compile"], 1)
        for profile, flags, message in (
            ("release", ["-O2", "-DNDEBUG", "-flto=thin"], "ordinary non-LTO native object"),
            ("release", ["-O0", "-g"], "effective optimization and NDEBUG"),
            ("sampling", ["-O2", "-DNDEBUG", "-g", "-fno-omit-frame-pointer"], "effective -fno-lto"),
            ("sampling", ["-O2", "-DNDEBUG", "-g", "-fno-lto"], "frame pointers"),
            ("sampling", ["-O2", "-DNDEBUG", "-fno-omit-frame-pointer", "-fno-lto"], "debug information"),
            (None, ["-O2", "-DNDEBUG"], "debug information without optimization"),
            (None, ["-O0", "-g", "-flto=thin"], "outside the Release profile"),
        ):
            with self.subTest(profile=profile, flags=flags):
                errors, _ = link_archive_contract_errors(
                    self.extract(
                        [
                            ("ObjcCompile", ["clang++", *flags, "-c", "source.mm"], False, ["source.o"]),
                            ("CppLink", ["clang++", "-o", "app"], False, ["app"]),
                            ("CppArchive", archive, False, ["out.a"]),
                        ]
                    ),
                    "macos",
                    profile,
                    expected_archive="llvm-libtool-darwin",
                )
                self.assertTrue(any("final-link" not in error and message in error for error in errors))
        # Objective-C++ evidence is required on macOS and rejected elsewhere.
        without = link_archive_contract_errors(
            self.extract(
                [
                    ("CppLink", ["clang++", "-o", "app"], False, ["app"]),
                    ("CppArchive", archive, False, ["out.a"]),
                ]
            ),
            "macos",
            None,
            expected_archive="llvm-libtool-darwin",
        )
        self.assertTrue(any("Objective-C++" in error for error in without[0]))
        elsewhere = link_archive_contract_errors(
            self.extract(
                [
                    ("ObjcCompile", ["clang++", "-c", "source.mm"], False, ["source.o"]),
                    ("CppLink", ["clang++", "-o", "app"], False, ["app"]),
                    ("CppArchive", ["llvm-ar", "rcs", "out.a"], False, ["out.a"]),
                ]
            ),
            "linux",
            None,
            expected_archive="llvm-ar",
        )
        self.assertTrue(any("only expected on macOS" in error for error in elsewhere[0]))

    def test_windows_link_and_archive_policy(self):
        lib = ("lib.exe", "/OUT:out.lib", "source.obj")
        for profile, link_flags, message in (
            (None, [], None),
            ("release", ["/LTCG"], None),
            ("release", [], "effective /LTCG"),
            ("release", ["/LTCG", "/LTCG:OFF"], "effective /LTCG"),
            ("release", ["/LTCG", "/LTCG-"], "effective /LTCG"),
            (None, ["/LTCG"], "outside the Windows Release profile"),
        ):
            with self.subTest(profile=profile, flags=link_flags):
                errors, _ = link_archive_contract_errors(
                    self.extract(
                        [
                            ("CppLink", ["link.exe", *link_flags, "/OUT:app.exe"], False, ["app.exe"]),
                            ("CppArchive", lib, False, ["out.lib"]),
                        ]
                    ),
                    "windows",
                    profile,
                    expected_archive="lib.exe",
                )
                if message is None:
                    self.assertEqual(errors, [])
                else:
                    self.assertTrue(any("final-link evidence" not in error and message in error for error in errors))
        for tool, message in (
            (("link.exe", "/LTCG", "/OUT:tool.exe"), "/LTCG"),
            (("ld.exe", "/OUT:app.exe"), "link.exe"),
        ):
            with self.subTest(tool=tool):
                errors, _ = link_archive_contract_errors(
                    self.extract(
                        [
                            ("CppLink", tool, True, ["tool.exe"]),
                            ("CppLink", ["link.exe", "/OUT:app.exe"], False, ["app.exe"]),
                            ("CppArchive", lib, False, ["out.lib"]),
                        ]
                    ),
                    "windows",
                    "release",
                    expected_archive="lib.exe",
                )
                self.assertTrue(any(message in error for error in errors))
        carrying, _ = link_archive_contract_errors(
            self.extract(
                [
                    ("CppLink", ["link.exe", "/LTCG", "/OUT:app.exe"], False, ["app.exe"]),
                    ("CppArchive", ["lib.exe", "/LTCG", "/OUT:out.lib"], False, ["out.lib"]),
                ]
            ),
            "windows",
            "release",
            expected_archive="lib.exe",
        )
        self.assertTrue(any("archive arguments must not carry /LTCG" in error for error in carrying))
        with self.assertRaises(ValueError):
            link_archive_contract_errors(
                self.extract(
                    [
                        ("CppLink", ["link.exe", "/OUT:app.exe"], False, ["app.exe"]),
                        ("CppArchive", lib, False, ["out.lib"]),
                    ]
                ),
                "windows",
                "sampling",
                expected_archive="lib.exe",
            )

    def test_linkstamp_policy_follows_the_compiling_profile(self):
        for profile, flags, message in (
            ("release", ["-flto=thin"], None),
            ("release", [], "effective -flto=thin"),
            ("sampling", ["-fno-lto"], None),
            ("sampling", ["-flto=thin"], "effective -fno-lto"),
            (None, [], None),
            (None, ["-flto=thin"], "outside the Release profile"),
        ):
            with self.subTest(profile=profile, flags=flags):
                errors, _ = link_archive_contract_errors(
                    self.extract(
                        [
                            ("CppLinkstampCompile", ["clang++", *flags, "-c", "_linkstamp_.cc"], False, ["stamp.o"]),
                            (
                                "CppLink",
                                [
                                    "clang++",
                                    *({"release": ["-flto=thin"], "sampling": ["-fno-lto"]}.get(profile, [])),
                                    "-o",
                                    "app",
                                ],
                                False,
                                ["app"],
                            ),
                            ("CppArchive", ["llvm-ar", "rcs", "out.a"], False, ["out.a"]),
                        ]
                    ),
                    "linux",
                    profile,
                    expected_archive="llvm-ar",
                )
                if message is None:
                    self.assertEqual(errors, [])
                else:
                    self.assertTrue(any("[CppLinkstampCompile," in error and message in error for error in errors))


if __name__ == "__main__":
    unittest.main()
