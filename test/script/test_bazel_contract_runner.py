"""Process and selection boundaries of ``ao check contracts``."""

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

from ao.core import bazel_native as native
from ao.portal import bazel_validation, bazel_verify

_TARGET = "//:native_policy_inputs"
_FACTS = "bazel-out/configured/facts-fixture.json"
_COMPILER = "external/+deps+aobus_toolchain/tools/clang++"
_POLICY = {"NIX_HARDENING_ENABLE_fixture": "stackprotector"}


@unittest.skipUnless(sys.platform == "linux", "fixtures execute native Linux Python children")
class BazelContractRunnerTest(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix="ao-contract-runner-")
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        self.tree = self.root / "tree"
        self.prepared = self.root / "configuration"
        self.prepared.mkdir()
        self.checkout = self.root / "checkout"
        self.checkout.mkdir()
        (self.checkout / "source.cpp").touch()
        self.execroot = self.root / "execution root"
        self.facts = self.execroot / _FACTS
        self.facts.parent.mkdir(parents=True)
        self.control = self.root / "control.json"
        self.calls = self.root / "calls.log"
        self.diagnostics = io.StringIO()
        self.executable = self.child(
            self.root / "bazel",
            f"""
import json, pathlib, sys
root = pathlib.Path(__file__).parent
fixture = json.loads((root / 'control.json').read_text())
startup, action, *args = sys.argv[1:]
assert startup == {("--bazelrc=" + str(self.prepared / "aobus.bazelrc"))!r}, sys.argv
with (root / 'calls.log').open('a') as calls:
    calls.write(action + '\\n')
if action == 'build':
    assert [arg for arg in args if not arg.startswith('--')] == [{_TARGET!r}], args
elif action == 'cquery':
    assert args == [{_TARGET!r}, '--output=files'], args
    print(fixture['reported_facts'])
elif action == 'info':
    assert args == ['execution_root'], args
    print({str(self.execroot)!r})
else:
    raise AssertionError(sys.argv)
if action == fixture['failed_action']:
    print('native child failure at ' + action, file=sys.stderr)
    sys.exit(29)
""",
        )
        self.enterContext(patch.object(native, "native_platform", return_value="linux"))
        self.enterContext(patch.object(native, "require_supported_source"))
        self.enterContext(patch.object(bazel_verify, "PROJECT_ROOT", self.checkout))
        self.enterContext(patch.object(bazel_verify, "_expected_hardening_environment", return_value=_POLICY))
        # Command-flow tests don't restate the lower-level compile/link policy.
        self.validator = self.enterContext(patch.object(bazel_validation, "validate_native_policy", return_value=[]))

    def child(self, path, code):
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(f"#!{sys.executable}\n" + code, encoding="utf-8")
        path.chmod(0o755)
        return path

    def write_facts(self, *, platform="linux", mode="dbg", profile=None, compiles=()):
        records = []
        for label, argv, is_tool in compiles:
            records.append(
                json.dumps(
                    {
                        "label": label,
                        "kind": "cc_library",
                        "is_tool": is_tool,
                        "configuration": "bazel-out/k8-dbg/bin",
                        "quote_includes": [],
                        "winui_projections": [],
                        "declared": [],
                        "compiles": [{"argv": argv, "env": {"CAPTURED_DRIVER": "present"}, "spa_inputs": []}],
                        "native_actions": [],
                    }
                )
            )
        document = {
            "schema": 2,
            "platform": platform,
            "mode": mode,
            "profile": profile,
            "gtk_enabled": True,
            "system_media_enabled": True,
            "builtin_include_roots": [],
            "records": records,
            "executed": [],
            "test_wrappers": [],
            "built": [],
        }
        self.facts.write_text(json.dumps(document), encoding="utf-8")

    def verify(self, config="debug", *, failed_action=None, reported_facts=_FACTS):
        self.control.write_text(
            json.dumps({"failed_action": failed_action, "reported_facts": reported_facts}), encoding="utf-8"
        )
        with contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(self.diagnostics):
            return bazel_verify.verify(self.executable, self.tree, self.prepared, config)

    def test_configured_output_selects_the_checkout_driver_not_an_exec_or_generated_compile(self):
        argv = [_COMPILER, "-c", "source.cpp"]
        self.write_facts(
            compiles=[
                ("//tool:tool", ["external/tool/g++", "-c", "source.cpp"], True),
                ("//app:generated", [_COMPILER, "-c", "bazel-out/configured/generated.cpp"], False),
                ("//app:source", argv, False),
            ]
        )
        observed = self.root / "driver-ran"
        cc1 = f"{shlex.quote('/compiler with spaces/clang')} -cc1 -Dgsl_CONFIG_CONTRACT_CHECKING_ON -stack-protector 2"
        self.child(
            self.execroot / _COMPILER,
            f"""
import os, pathlib, sys
assert sys.argv[1:] == ['-c', 'source.cpp', '-###'], sys.argv
assert pathlib.Path('source.cpp').is_file()
assert os.environ.get('CAPTURED_DRIVER') == 'present'
assert 'AMBIENT_DRIVER' not in os.environ
pathlib.Path({str(observed)!r}).touch()
print({cc1!r}, file=sys.stderr)
""",
        )
        with patch.dict(os.environ, {"AMBIENT_DRIVER": "must not leak"}):
            self.assertEqual(self.verify(), 0, self.diagnostics.getvalue())
        self.assertTrue(observed.is_file())
        self.assertIn("info", self.calls.read_text().splitlines())

    def test_failed_output_query_preserves_the_native_exit_and_diagnostic(self):
        self.assertEqual(self.verify(failed_action="cquery"), 29)
        self.validator.assert_not_called()
        self.assertEqual(self.calls.read_text().splitlines(), ["build", "cquery"])
        log = next(self.tree.glob("contracts/debug/verify-*/files.stderr.log"))
        self.assertIn("native child failure at cquery", log.read_text())
        self.assertIn(str(log), self.diagnostics.getvalue())

    def test_wrong_configured_facts_cannot_yield_a_green_verdict(self):
        self.write_facts(mode="fastbuild")
        self.assertEqual(self.verify(), 1)
        self.assertIn("fastbuild", self.diagnostics.getvalue())
        self.assertIn("debug", self.diagnostics.getvalue())
        self.validator.assert_not_called()

    def test_policy_violations_fail_without_starting_the_driver(self):
        self.write_facts()
        violation = "//app:source uses a downgraded language standard"
        self.validator.return_value = [violation]
        self.assertEqual(self.verify(reported_facts=str(self.facts)), 1)
        self.assertIn(violation, self.diagnostics.getvalue())
        self.assertEqual(self.calls.read_text().splitlines(), ["build", "cquery"])

    def test_windows_release_uses_the_frozen_toolset_not_the_ambient_one(self):
        self.write_facts(platform="windows", mode="opt", profile="release")
        (self.prepared / "request.json").write_text(
            '{"compiler_environment": {"VCToolsVersion": "14.51.11111"}}', encoding="utf-8"
        )
        with (
            patch.object(native, "native_platform", return_value="windows"),
            patch.dict(os.environ, {"VCToolsVersion": "14.50.11111"}),
        ):
            self.assertEqual(self.verify("release", reported_facts=str(self.facts)), 0, self.diagnostics.getvalue())
        self.assertEqual(self.validator.call_args.kwargs["windows_tools_version"], "14.51.11111")
        self.assertEqual(self.calls.read_text().splitlines(), ["build", "cquery"])

    def test_successful_driver_exit_does_not_hide_disabled_gcc_stack_protection(self):
        gcc = self.child(
            self.root / "g++",
            """
import sys
print('cc1plus -O2 -Dgsl_CONFIG_CONTRACT_CHECKING_ON -fstack-protector-strong -fno-stack-protector', file=sys.stderr)
print('as source.s', file=sys.stderr)
""",
        )
        action = bazel_verify.CompileAction("//app:source", (str(gcc), "-c", "source.cpp"))
        evidence = {"stdout": str(self.root / "driver.stdout"), "stderr": str(self.root / "driver.stderr")}
        self.assertEqual(bazel_verify._verify_driver(action, "opt", _POLICY, evidence), 1)
        self.assertTrue(any("stackprotector" in issue for issue in evidence["errors"]))


if __name__ == "__main__":
    unittest.main()
