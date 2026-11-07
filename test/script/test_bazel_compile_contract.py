"""Compiler-policy regressions and semantic checks for captured aquery jsonproto.

Native callers supply their own evidence paths. Capture with --output=jsonproto,
--include_artifacts=true, --include_commandline=true, --include_param_files=true
and --include_pruned_inputs=true so SPA checks use potential inputs consistently.
These unit tests exercise the checker; they do not run or certify native actions.
"""

import ast
import copy
import json
import re
import subprocess
import unittest
from dataclasses import replace

from ao.core.bazel_contract import (
    LINUX_EXTRA_WARNINGS,
    SPA_FLAGS,
    UNIX_WARNINGS,
    CompileAction,
    compile_contract_errors,
    extract_compile_actions,
    include_paths,
)
from ao.core.paths import PROJECT_ROOT


class BazelCompilePolicyTest(unittest.TestCase):
    def test_platform_warnings_match_normal_cmake_policy_without_blanket_suppression(self):
        lines = (PROJECT_ROOT / ".bazelrc").read_text(encoding="utf-8").splitlines()
        cmake = (PROJECT_ROOT / "cmake/CompilerOptions.cmake").read_text(encoding="utf-8")
        common = re.search(r"set\(COMMON_WARNINGS(.*?)\n  \)", cmake, re.S).group(1)
        cmake_warnings = set(re.findall(r"-W[\w=+-]+", common)) | set(re.findall(r"-Wno-[\w-]+", cmake)) | {"-Werror"}
        self.assertEqual(UNIX_WARNINGS, cmake_warnings)
        policy = ast.parse((PROJECT_ROOT / "bazel/compiler_policy.bzl").read_text(encoding="utf-8"))
        values = {
            node.targets[0].id: ast.literal_eval(node.value) for node in policy.body if isinstance(node, ast.Assign)
        }
        self.assertEqual(set(values["UNIX_WARNINGS"]), cmake_warnings)
        self.assertEqual(len(values["UNIX_WARNINGS"]), len(cmake_warnings))
        self.assertNotIn("-w", values["UNIX_WARNINGS"])
        for line in lines:
            if line.startswith("build"):
                self.assertNotRegex(line, r"--(?:copt|cxxopt|conlyopt|linkopt|per_file_copt)=")
            if line.startswith("#") or not line.strip():
                continue
            if "external_include_paths" in line:
                # Only the official Windows auto-config scope enables the feature.
                self.assertTrue(line.startswith("build:windows"), line)

    def test_warning_selection_belongs_to_the_repository_not_source_path_filters(self):
        tree = ast.parse((PROJECT_ROOT / "REPO.bazel").read_text(encoding="utf-8"))
        declarations = [node.value for node in tree.body if isinstance(node, ast.Expr)]
        self.assertEqual(len(declarations), 1)
        declaration = declarations[0]
        self.assertIsInstance(declaration, ast.Call)
        self.assertEqual(ast.unparse(declaration.func), "repo")
        self.assertEqual(
            {field.arg: ast.literal_eval(field.value) for field in declaration.keywords},
            # The owned feature keeps the Unix warning set; the standard feature
            # supplies /WX on Windows inside the same repository scope.
            {"features": ["aobus_warnings", "treat_warnings_as_errors"]},
        )
        lines = (PROJECT_ROOT / ".bazelrc").read_text(encoding="utf-8").splitlines()
        active = [line for line in lines if line.strip() and not line.startswith("#")]
        self.assertFalse(any("--per_file_copt" in line for line in active))
        # Warning promotion is repository scope, never a command-line default.
        self.assertFalse(any("--features=aobus_warnings" in line for line in active))
        self.assertFalse(any("--features=treat_warnings_as_errors" in line for line in active))
        self.assertFalse(any("dynamic_link_msvcrt" in line for line in active))
        # Unix behavior stays unchanged: the standard feature is disabled for
        # both the target and the host configuration on the governed Unix hosts.
        for config in ("linux", "macos"):
            self.assertIn(f"build:{config} --features=-treat_warnings_as_errors", active)
            self.assertIn(f"build:{config} --host_features=-treat_warnings_as_errors", active)
        # The only enabling host-feature selection is the Windows include
        # classification; Unix hosts only disable the standard feature above.
        for line in active:
            if line.startswith("build:windows") and "--host_features=" in line:
                self.assertEqual(line, "build:windows --host_features=external_include_paths")

    def test_ignored_prefixes_never_hide_tracked_files(self):
        lines = (PROJECT_ROOT / ".bazelignore").read_text(encoding="utf-8").splitlines()
        prefixes = [line for line in lines if line and not line.startswith("#")]
        tracked = subprocess.run(
            ["git", "ls-files", "-z"], cwd=PROJECT_ROOT, capture_output=True, text=True, check=True
        ).stdout.split("\0")

        def hidden(prefix: str) -> list[str]:
            return [path for path in tracked if path.startswith(prefix + "/")]

        self.assertTrue(hidden("app"))
        for prefix in prefixes:
            # Bazel takes each line as a literal workspace-relative prefix.
            self.assertNotRegex(prefix, r"[*?\[\\]|^/|/$", prefix)
            self.assertEqual(hidden(prefix), [], prefix)

    def test_repository_probes_unset_ambient_flags_without_removing_sdk_discovery(self):
        lines = (PROJECT_ROOT / ".bazelrc").read_text(encoding="utf-8").splitlines()
        unset = {line.removeprefix("common --repo_env==") for line in lines if line.startswith("common --repo_env==")}
        self.assertTrue(
            {
                "CPLUS_INCLUDE_PATH",
                "C_INCLUDE_PATH",
                "CPATH",
                "LIBRARY_PATH",
                "BAZEL_CONLYOPTS",
                "BAZEL_CXXOPTS",
                "BAZEL_LINKOPTS",
            }
            <= unset
        )
        for base in ("NIX_CFLAGS_COMPILE", "NIX_CFLAGS_LINK", "NIX_LDFLAGS"):
            self.assertTrue({base, base + "_FOR_TARGET", base + "_FOR_BUILD"} <= unset)
        self.assertFalse({"CC", "CXX", "SDKROOT", "DEVELOPER_DIR", "INCLUDE", "LIB"} & unset)
        self.assertEqual(
            {
                line
                for line in lines
                if line.startswith("common --repo_env=") and not line.startswith("common --repo_env==")
            },
            {
                "common --repo_env=BAZEL_DO_NOT_DETECT_CPP_TOOLCHAIN=1",
                "common --repo_env=BAZEL_NO_APPLE_CPP_TOOLCHAIN=1",
                "common --repo_env=APPLE_SUPPORT_LAYERING_CHECK_BETA=0",
            },
        )
        self.assertFalse(any("--experimental_strict_repo_env" in line for line in lines if not line.startswith("#")))

    def test_gsl_contract_modes_stay_in_project_policy(self):
        lines = (PROJECT_ROOT / ".bazelrc").read_text(encoding="utf-8").splitlines()
        for profile, mode in (("debug", "dbg"), ("release", "opt")):
            prefix = f"build:{profile} --compilation_mode="
            self.assertEqual([line for line in lines if line.startswith(prefix)], [prefix + mode])
        self.assertFalse(any(line.startswith("build") and "gsl_CONFIG_" in line for line in lines))

        tree = ast.parse((PROJECT_ROOT / "bazel/BUILD.bazel").read_text(encoding="utf-8"))
        declarations = {}
        for node in tree.body:
            if isinstance(node, ast.Expr) and isinstance(node.value, ast.Call):
                call = node.value
                if isinstance(call.func, ast.Name) and call.func.id in {"config_setting", "cc_library"}:
                    fields = {keyword.arg: keyword.value for keyword in call.keywords}
                    declarations[ast.literal_eval(fields["name"])] = (call.func.id, fields)
        rule, optimized = declarations["optimized_contracts"]
        self.assertEqual(rule, "config_setting")
        self.assertEqual(ast.literal_eval(optimized["values"]), {"compilation_mode": "opt"})
        rule, contract = declarations["compile_contract"]
        self.assertEqual(rule, "cc_library")
        self.assertNotIn("deps", contract)
        defines = contract["defines"]
        self.assertIsInstance(defines, ast.BinOp)
        self.assertIsInstance(defines.op, ast.Add)
        common = ast.literal_eval(defines.left)
        self.assertEqual(common, ["gsl_CONFIG_CONTRACT_CHECKING_ON"])
        selector = defines.right
        self.assertIsInstance(selector, ast.Call)
        self.assertEqual(ast.unparse(selector.func), "select")
        self.assertEqual(len(selector.args), 1)
        self.assertFalse(selector.keywords)
        branches = ast.literal_eval(selector.args[0])
        self.assertEqual(
            branches,
            {
                ":optimized_contracts": ["gsl_CONFIG_CONTRACT_VIOLATION_TERMINATES"],
                "//conditions:default": ["gsl_CONFIG_CONTRACT_VIOLATION_ASSERTS"],
            },
        )

    def test_manual_lint_fixture_targets_opt_out_of_both_warning_names(self):
        tree = ast.parse((PROJECT_ROOT / "test/lint/BUILD.bazel").read_text(encoding="utf-8"))
        libraries = {}
        for node in ast.walk(tree):
            if isinstance(node, ast.Call) and isinstance(node.func, ast.Name) and node.func.id == "cc_library":
                fields = {keyword.arg: keyword.value for keyword in node.keywords}
                libraries[ast.literal_eval(fields["name"])] = fields
        self.assertEqual(
            set(libraries),
            {"cache_warning", "cache_clean", "cache_error", "header_replay", "werror_replay"},
        )
        # The deliberately warning-bearing fixture contexts opt out of both the
        # owned and the standard warning-promotion feature; the werror replay
        # keeps promotion so an analysis-only warning exercises the report gate.
        for name in ("cache_warning", "cache_clean", "cache_error", "header_replay"):
            self.assertEqual(
                ast.literal_eval(libraries[name]["features"]),
                ["-aobus_warnings", "-treat_warnings_as_errors"],
            )
        self.assertNotIn("features", libraries["werror_replay"])

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
            else ["-std=c++26", *sorted(UNIX_WARNINGS)]
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

    def test_windows_critical_defines_reject_undefinition_or_redefinition_in_every_mode(self):
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
            for mode in ("dbg", "opt", "fastbuild"):
                with self.subTest(name=name, mode=mode):
                    action = self.action("windows", mode)
                    missing = replace(
                        action, argv=tuple(flag for flag in action.argv if not flag.startswith("/D" + name))
                    )
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

    def test_linux_extra_diagnostics_are_not_required_on_macos(self):
        self.assertFalse(LINUX_EXTRA_WARNINGS & set(self.action("macos").argv))
        self.assertEqual(compile_contract_errors([self.action("macos")], "macos", "opt"), [])

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

    def test_configuration_missing_duplicate_dangling_and_nonboolean_roles_fail_closed(self):
        for case in ("missing", "duplicate", "dangling", "nonboolean"):
            with self.subTest(case=case):
                document = BazelActionEnvironmentTest.document()
                document["configuration"] = [{"id": 7, "isTool": True}]
                document["actions"][0]["configurationId"] = 7
                if case == "missing":
                    del document["actions"][0]["configurationId"]
                elif case == "duplicate":
                    document["configuration"].append({"id": 7})
                elif case == "dangling":
                    document["actions"][0]["configurationId"] = 8
                else:
                    document["configuration"][0]["isTool"] = "true"
                with self.assertRaises((KeyError, ValueError)):
                    extract_compile_actions(document)

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

    def test_duplicate_environment_ids_and_param_paths_fail_closed(self):
        for table in ("targets", "pathFragments", "artifacts", "depSetOfFiles", "environmentVariables", "paramFiles"):
            with self.subTest(table=table):
                document = self.document()
                if table == "environmentVariables":
                    document["actions"][0][table] = [{"key": "CPATH"}, {"key": "CPATH", "value": "leak"}]
                elif table == "paramFiles":
                    document["actions"][0][table] = [{"execPath": "flags", "arguments": []}] * 2
                else:
                    document[table] = [{"id": 1, "label": "duplicate"}] * 2
                with self.assertRaisesRegex(ValueError, "duplicate key"):
                    extract_compile_actions(document)

    def test_dangling_target_artifact_and_fragment_ids_fail_closed(self):
        for table in ("targets", "artifacts", "pathFragments"):
            with self.subTest(table=table):
                document = self.document()
                document["actions"][0]["inputDepSetIds"] = [1]
                document["depSetOfFiles"] = [{"id": 1, "directArtifactIds": [1]}]
                document["artifacts"] = [{"id": 1, "pathFragmentId": 1}]
                document["pathFragments"] = [{"id": 1, "label": "source.cpp"}]
                del document[table]
                with self.assertRaises(KeyError):
                    extract_compile_actions(document)

    def test_recursive_inputs_and_params_and_zero_cpp_actions_fail_closed(self):
        for kind in ("fragment", "depset", "params", "empty", "no-command"):
            with self.subTest(kind=kind):
                document = self.document()
                action = document["actions"][0]
                if kind == "fragment":
                    action["inputDepSetIds"] = [1]
                    document["depSetOfFiles"] = [{"id": 1, "directArtifactIds": [1]}]
                    document["artifacts"] = [{"id": 1, "pathFragmentId": 1}]
                    document["pathFragments"] = [{"id": 1, "parentId": 1, "label": "cycle"}]
                elif kind == "depset":
                    action["inputDepSetIds"] = [1]
                    document["depSetOfFiles"] = [{"id": 1, "transitiveDepSetIds": [1]}]
                elif kind == "params":
                    action["arguments"] = ["@flags"]
                    action["paramFiles"] = [{"execPath": "flags", "arguments": ["@flags"]}]
                elif kind == "empty":
                    action["mnemonic"] = "CppLink"
                else:
                    action["arguments"] = []
                with self.assertRaises(ValueError):
                    extract_compile_actions(document)

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

    def test_missing_operand_and_duplicate_manual_environment_fail_closed(self):
        for extra in (["-I"], ["-D"]):
            action = BazelAqueryContractTest.action(extra=extra)
            # Put the dangling switch last, not before the source operand.
            action = replace(action, argv=(*action.argv[:-2], action.argv[-1], extra[0]))
            with self.assertRaises(ValueError):
                compile_contract_errors([action], "linux", "opt")
        action = replace(BazelAqueryContractTest.action(), environment=(("CPATH", ""), ("CPATH", "leak")))
        with self.assertRaisesRegex(ValueError, "duplicate key"):
            compile_contract_errors([action], "linux", "opt", expected_environment={})


if __name__ == "__main__":
    unittest.main()
