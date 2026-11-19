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

from ao.core.paths import PROJECT_ROOT
from ao.policy.bazel_contract import (
    CLANG_EXTRA_WARNINGS,
    LINUX_EXTRA_WARNINGS,
    SPA_FLAGS,
    CompileAction,
    NativeAction,
    compile_contract_errors,
    extract_compile_actions,
    include_paths,
    link_archive_contract_errors,
    parse_unix_warnings,
    unix_warnings,
)
from ao.portal.git_files import git_command


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
        # The real macOS toolchain generates apple_support compiler-entry
        # wrappers, so the macOS fixture keeps the actual wrapper basename.
        driver = "cl.exe" if windows else "wrapped_clang_pp" if platform == "macos" else "clang++"
        flags = [driver, "-Dgsl_CONFIG_CONTRACT_CHECKING_ON"]
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
            else ["-std=c++26", *sorted(unix_warnings() | CLANG_EXTRA_WARNINGS)]
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
            # Fastbuild keeps assertions active without NDEBUG, like dbg.
            flags += ["-Dgsl_CONFIG_CONTRACT_VIOLATION_ASSERTS"]
        if windows:
            # The pinned toolchain links the debug CRT only for dbg.
            flags += ["/MDd" if mode == "dbg" else "/MD"]
        return CompileAction(
            "//test:audio_test_support",
            (*flags, *extra, "test/audio/EngineTestSupport.cpp"),
            frozenset(inputs),
            frozenset(quote_includes),
        )

    @staticmethod
    def gcc_action(mode="opt", extra=()):
        # The selected GCC driver carries the GNU-common warning base and the
        # version-bounded Boost.PFR fallback define, never Clang-only flags.
        flags = ["g++", "-Dgsl_CONFIG_CONTRACT_CHECKING_ON", "-DBOOST_PFR_USE_CPP26=0"]
        flags += ["-std=c++26", *sorted(unix_warnings())]
        if mode == "opt":
            flags += ["-O2", "-DNDEBUG", "-Dgsl_CONFIG_CONTRACT_VIOLATION_TERMINATES"]
        elif mode == "dbg":
            flags += ["-O0", "-g", "-Dgsl_CONFIG_CONTRACT_VIOLATION_ASSERTS"]
        else:
            flags += ["-O0", "-g0", "-Dgsl_CONFIG_CONTRACT_VIOLATION_ASSERTS"]
        return CompileAction(
            "//test:audio_test_support",
            (*flags, *extra, "test/audio/EngineTestSupport.cpp"),
        )

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

    def test_fastbuild_keeps_gsl_assert_violations(self):
        # Fastbuild never defines NDEBUG, so violations must assert like dbg;
        # the TERMINATES mode stays exclusive to the optimized profile.
        for platform in ("linux", "macos", "windows"):
            define = ("/D" if platform == "windows" else "-D") + "gsl_CONFIG_CONTRACT_VIOLATION_"
            undefine = ("/U" if platform == "windows" else "-U") + "gsl_CONFIG_CONTRACT_VIOLATION_"
            for mutation, restoration in (
                ([define + "TERMINATES"], [undefine + "TERMINATES", define + "ASSERTS"]),
                ([define + "ASSERTS=0"], [define + "ASSERTS"]),
                ([undefine + "ASSERTS"], [define + "ASSERTS"]),
            ):
                with self.subTest(platform=platform, mutation=mutation):
                    action = self.action(platform, "fastbuild", mutation)
                    errors = compile_contract_errors([action], platform, "fastbuild")
                    self.assertEqual(len(errors), 1)
                    self.assertIn("GSL contract violation mode must be ASSERTS", errors[0])
                    restored = self.action(platform, "fastbuild", [*mutation, *restoration])
                    self.assertEqual(compile_contract_errors([restored], platform, "fastbuild"), [])

    def test_windows_runtime_library_follows_the_effective_mode(self):
        # dbg links the debug CRT; fastbuild, opt and execution tools use /MD.
        for mode, expected in (("dbg", "/MDd"), ("fastbuild", "/MD"), ("opt", "/MD")):
            with self.subTest(mode=mode):
                action = self.action("windows", mode)
                missing = replace(action, argv=tuple(flag for flag in action.argv if flag != expected))
                errors = compile_contract_errors([missing], "windows", mode)
                self.assertEqual(len(errors), 1)
                self.assertIn("effective " + expected, errors[0])
                for override in ("/MD" if expected == "/MDd" else "/MDd", "/MT", "/MTd"):
                    with self.subTest(override=override):
                        overridden = self.action("windows", mode, [override])
                        self.assertEqual(compile_contract_errors([overridden], "windows", mode), errors)
                        restored = self.action("windows", mode, [override, expected])
                        self.assertEqual(compile_contract_errors([restored], "windows", mode), [])
                # Either switch spelling satisfies the effective selection.
                dashed = replace(
                    action, argv=tuple(("-" + expected[1:]) if flag == expected else flag for flag in action.argv)
                )
                self.assertEqual(compile_contract_errors([dashed], "windows", mode), [])
        # Execution tools keep the optimized /MD selection under a debug target.
        tool = replace(self.action("windows", "opt"), is_tool=True)
        self.assertEqual(compile_contract_errors([tool], "windows", "dbg"), [])
        wrong_tool = replace(self.action("windows", "dbg"), is_tool=True)
        self.assertTrue(compile_contract_errors([wrong_tool], "windows", "dbg"))

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

    def test_gcc_actions_carry_the_common_base_and_version_bounded_pfr(self):
        gcc = self.gcc_action()
        self.assertEqual(compile_contract_errors([gcc], "linux", "opt", gcc_version="16.2.0"), [])
        self.assertEqual(compile_contract_errors([self.gcc_action("dbg")], "linux", "dbg", gcc_version="16.2.0"), [])
        # GCC 16 cannot compile Boost.PFR's C++26 path, so later overrides must
        # not undo the pinned fallback define in either mode.
        for override in (
            ["-UBOOST_PFR_USE_CPP26"],
            ["-DBOOST_PFR_USE_CPP26=1"],
            ["-DBOOST_PFR_USE_CPP26=0", "-UBOOST_PFR_USE_CPP26"],
        ):
            with self.subTest(override=override):
                errors = compile_contract_errors(
                    [self.gcc_action(extra=override)], "linux", "opt", gcc_version="16.2.0"
                )
                self.assertEqual(len(errors), 1)
                self.assertIn("BOOST_PFR_USE_CPP26=0", errors[0])
        self.assertEqual(
            compile_contract_errors(
                [self.gcc_action(extra=["-UBOOST_PFR_USE_CPP26", "-DBOOST_PFR_USE_CPP26=0"])],
                "linux",
                "opt",
                gcc_version="16.2.0",
            ),
            [],
        )
        without_define = replace(
            gcc, argv=tuple(flag for flag in gcc.argv if not flag.startswith("-DBOOST_PFR_USE_CPP26"))
        )
        # The bound is version-gated and the checker never discovers the SDK:
        # verify supplies the selected version, fixtures pass it explicitly.
        self.assertEqual(compile_contract_errors([without_define], "linux", "opt", gcc_version="17.1.0"), [])
        self.assertTrue(compile_contract_errors([without_define], "linux", "opt", gcc_version="16.2.0"))
        self.assertTrue(compile_contract_errors([gcc], "linux", "opt"))
        with self.assertRaises(ValueError):
            compile_contract_errors([gcc], "linux", "opt", gcc_version="sixteen")

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

    def test_winui_consumers_keep_generated_project_and_sdk_warnings_distinct(self):
        project, sdk = "out/Project Headers", "out/SDK"

        def validate(flags):
            action = replace(self.action("windows", extra=flags), winui_projections=((project, sdk),))
            return compile_contract_errors([action], "windows", "opt")

        self.assertEqual(validate(["/I", project, r"/external:Iout\sdk"]), [])
        for flags, bad_root in (
            (["/I" + project, "/I" + sdk], sdk),
            (["/external:I" + project, "/external:I" + sdk], project),
            (["/I" + project, "/external:I" + sdk, "/I" + sdk], sdk),
            (["/I" + project], sdk),
        ):
            with self.subTest(flags=flags):
                errors = validate(flags)
                self.assertEqual(len(errors), 1, errors)
                self.assertIn(bad_root, errors[0])

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

    def test_execution_policy_composes_separately_and_never_uses_target_debug_hardening(self):
        clears = dict.fromkeys(
            (
                "CPLUS_INCLUDE_PATH",
                "C_INCLUDE_PATH",
                "CPATH",
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

    @staticmethod
    def cleared_channels():
        return dict.fromkeys(
            (
                "CPATH",
                "C_INCLUDE_PATH",
                "CPLUS_INCLUDE_PATH",
                "LD_LIBRARY_PATH",
                "NIX_CFLAGS_COMPILE",
                "NIX_CFLAGS_COMPILE_FOR_TARGET",
                "NIX_LDFLAGS",
                "NIX_LDFLAGS_FOR_TARGET",
            ),
            "",
        )

    def test_expected_policy_and_cleared_channels_apply_to_every_first_party_action(self):
        policy = {"NIX_HARDENING_ENABLE_test_salt": "captured-policy"}
        cleared = self.cleared_channels()
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

    def test_linux_target_and_execution_actions_require_library_path_absent(self):
        for is_tool in (False, True):
            action = replace(
                BazelAqueryContractTest.gcc_action(),
                environment=() if is_tool else tuple(self.cleared_channels().items()),
                is_tool=is_tool,
            )

            def errors(action):
                return compile_contract_errors(
                    [action],
                    "linux",
                    "opt",
                    expected_environment={},
                    expected_tool_environment={},
                    gcc_version="16.2.0",
                )

            self.assertEqual(errors(action), [])
            for value in ("", "/ambient/lib"):
                with self.subTest(is_tool=is_tool, value=value):
                    present = replace(action, environment=(*action.environment, ("LIBRARY_PATH", value)))
                    self.assertEqual(len(problems := errors(present)), 1)
                    self.assertIn("must be absent: LIBRARY_PATH", problems[0])

    def test_other_platforms_keep_the_explicitly_empty_target_library_path(self):
        for platform in ("macos", "windows"):
            with self.subTest(platform=platform):
                action = replace(
                    BazelAqueryContractTest.action(platform), environment=tuple(self.cleared_channels().items())
                )
                problems = compile_contract_errors([action], platform, "opt", expected_environment={})
                self.assertEqual(len(problems), 1)
                self.assertIn("must be explicitly cleared: LIBRARY_PATH", problems[0])
                cleared = replace(action, environment=(*action.environment, ("LIBRARY_PATH", "")))
                self.assertEqual(compile_contract_errors([cleared], platform, "opt", expected_environment={}), [])


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

    def test_gcc_release_compiles_use_auto_lto_with_slim_objects(self):
        gcc = BazelAqueryContractTest.gcc_action()
        self.assertEqual(
            compile_contract_errors(
                [replace(gcc, argv=(*gcc.argv, "-flto=auto", "-fno-fat-lto-objects"))],
                "linux",
                "opt",
                profile="release",
                gcc_version="16.2.0",
            ),
            [],
        )
        for mutation, message in (
            ([], "effective -flto=auto"),
            (["-flto=thin"], "effective -flto=auto"),
            (["-fno-fat-lto-objects"], "effective -flto=auto"),
            (["-flto=auto"], "-fno-fat-lto-objects"),
            (["-flto=auto", "-ffat-lto-objects"], "-fno-fat-lto-objects"),
            (["-flto=auto", "-fno-fat-lto-objects", "-fno-lto"], "effective -flto=auto"),
        ):
            with self.subTest(mutation=mutation):
                errors = compile_contract_errors(
                    [replace(gcc, argv=(*gcc.argv, *mutation))],
                    "linux",
                    "opt",
                    profile="release",
                    gcc_version="16.2.0",
                )
                self.assertTrue(errors)
                self.assertIn(message, errors[0] if len(errors) == 1 else "; ".join(errors))
        # The Clang-only Linux vtable workaround is neither required nor rejected
        # for GCC, and sampling keeps the compiler-independent policy.
        self.assertEqual(
            compile_contract_errors(
                [replace(gcc, argv=(*gcc.argv, "-flto=auto", "-fno-fat-lto-objects", "-fno-assume-unique-vtables"))],
                "linux",
                "opt",
                profile="release",
                gcc_version="16.2.0",
            ),
            [],
        )
        sampling = ["-g", "-fno-omit-frame-pointer", "-fno-lto"]
        self.assertEqual(
            compile_contract_errors(
                [replace(gcc, argv=(*gcc.argv, *sampling))],
                "linux",
                "opt",
                profile="sampling",
                gcc_version="16.2.0",
            ),
            [],
        )

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
    """Guard against a passing audit when final link or auxiliary actions drift."""

    @staticmethod
    def action(mnemonic, executable, *flags, tool=False):
        return NativeAction("//app:product", mnemonic, (executable, *flags), (f"out/{mnemonic}",), tool)

    def check(self, platform, profile, link, *other_actions, archive=None, **policy):
        archiver = {"linux": "llvm-ar", "macos": "llvm-libtool-darwin", "windows": "lib.exe"}[platform]
        policy.setdefault("expected_archive", archiver)
        if archive is None:
            archive = self.action("CppArchive", archiver, "out.a", "source.o")
        return link_archive_contract_errors([link, archive, *other_actions], platform, profile, **policy)[0]

    def test_unix_final_link_uses_effective_lto_not_the_first_match(self):
        release = self.action("CppLink", "clang++", "-flto=thin", "-o", "app")
        self.assertEqual(self.check("linux", "release", release), [])
        disabled = replace(release, argv=(*release.argv, "-fno-lto"))
        self.assertTrue(any("effective -flto=thin" in error for error in self.check("linux", "release", disabled)))
        self.assertEqual(self.check("linux", "sampling", disabled), [])
        self.assertTrue(any("effective -fno-lto" in error for error in self.check("linux", "sampling", release)))

    def test_gcc_release_link_cannot_suppress_late_optimizer_warnings(self):
        link = self.action("CppLink", "g++", "-flto=auto", "-Werror", "-o", "app")
        archive = self.action("CppArchive", "gcc-ar", "out.a", "source.o")
        options = {"archive": archive, "expected_archive": "gcc-ar"}
        self.assertEqual(self.check("linux", "release", link, **options), [])
        suppression = replace(link, argv=(*link.argv, "-Wno-error"))
        self.assertTrue(
            any("effective -Werror" in error for error in self.check("linux", "release", suppression, **options))
        )
        external = replace(suppression, label="@dep//pkg:lib")
        self.assertEqual(self.check("linux", "release", external, **options), [])

    def test_execution_archive_uses_its_own_archiver_and_no_ipo_switches(self):
        link = self.action("CppLink", "clang++", "-o", "app")
        tool = self.action("CppArchive", "gcc-ar", "tool.a", "tool.o", tool=True)
        self.assertEqual(self.check("linux", None, link, tool, expected_tool_archive="gcc-ar"), [])
        wrong_archiver = replace(tool, argv=("llvm-ar", *tool.argv[1:]))
        problems = self.check("linux", None, link, wrong_archiver, expected_tool_archive="gcc-ar")
        self.assertTrue(any("exec" in problem and "selected archiver" in problem for problem in problems))
        has_ipo = replace(tool, argv=(*tool.argv, "-flto=thin"))
        self.assertTrue(
            any(
                "LTO switches" in problem
                for problem in self.check("linux", None, link, has_ipo, expected_tool_archive="gcc-ar")
            )
        )

    def test_release_flags_do_not_leak_into_the_execution_link_or_enable_bazel_lto_pipeline(self):
        target = self.action("CppLink", "clang++", "-flto=thin", "-o", "app")
        tool = self.action("CppLink", "clang++", "-flto=thin", "-o", "tool", tool=True)
        pipeline = self.action("LtoIndexing", "clang++", "index")
        problems = self.check("linux", "release", target, tool, pipeline)
        self.assertTrue(any("index/backend" in problem for problem in problems))
        self.assertTrue(any("exec" in problem and "IPO flags" in problem for problem in problems))

    def test_archive_evidence_does_not_substitute_for_a_final_link(self):
        only_archive = self.action("CppArchive", "llvm-ar", "out.a", "source.o")
        problems, _ = link_archive_contract_errors([only_archive], "linux", None, expected_archive="llvm-ar")
        self.assertTrue(any("final-link evidence" in problem for problem in problems))

    def test_macos_release_objcpp_compiles_to_a_native_object_not_bitcode(self):
        link = self.action("CppLink", "clang++", "-flto=thin", "-o", "app")
        objc = self.action("ObjcCompile", "clang++", "-O2", "-DNDEBUG", "-c", "source.mm")
        self.assertEqual(self.check("macos", "release", link, objc), [])
        bitcode = replace(objc, argv=(*objc.argv, "-flto=thin"))
        self.assertTrue(
            any("non-LTO native object" in problem for problem in self.check("macos", "release", link, bitcode))
        )
        self.assertTrue(any("Objective-C++" in problem for problem in self.check("macos", "release", link)))

    def test_fastbuild_objective_c_plus_plus_does_not_use_debug_policy(self):
        link = self.action("CppLink", "clang++", "-o", "app")
        objc = self.action("ObjcCompile", "clang++", "-O0", "-c", "source.mm")
        self.assertEqual(self.check("macos", None, link, objc, mode="fastbuild"), [])
        debug = replace(objc, argv=(*objc.argv, "-g"))
        self.assertEqual(self.check("macos", None, link, debug, mode="dbg"), [])
        self.assertTrue(
            any("debug information" in problem for problem in self.check("macos", None, link, debug, mode="fastbuild"))
        )

    def test_windows_release_link_rejects_a_later_ltcg_disable(self):
        link = self.action("CppLink", "link.exe", "/LTCG", "/OUT:app.exe")
        self.assertEqual(self.check("windows", "release", link), [])
        disabled = replace(link, argv=(*link.argv, "/LTCG:OFF"))
        self.assertTrue(any("effective /LTCG" in problem for problem in self.check("windows", "release", disabled)))

    def test_linkstamps_follow_the_driver_family_not_the_final_link_policy(self):
        link = self.action("CppLink", "g++", "-flto=auto", "-Werror", "-o", "app")
        archive = self.action("CppArchive", "gcc-ar", "out.a", "source.o")
        stamp = self.action("CppLinkstampCompile", "g++", "-flto=auto", "-c", "_linkstamp_.cc")
        options = {"archive": archive, "expected_archive": "gcc-ar"}
        self.assertEqual(self.check("linux", "release", link, stamp, **options), [])
        thin_stamp = replace(stamp, argv=(*stamp.argv, "-flto=thin"))
        self.assertTrue(
            any(
                "CppLinkstampCompile" in problem and "-flto=auto" in problem
                for problem in self.check("linux", "release", link, thin_stamp, **options)
            )
        )


if __name__ == "__main__":
    unittest.main()
