"""Regressions for the `ao check` orchestration.

The parser, selection and audits are real; only the workspace/SDK boundary,
the owned external service lifecycles and the Bazel subprocess are faked. The
subprocess fake answers each phase's exact action and query from a small
independent witness that the real extractors consume.
"""

import contextlib
import io
import json
import os
import re
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from types import SimpleNamespace
from unittest import mock

from ao.__main__ import make_parser
from ao.command import check as check_command
from ao.core import bazel_check, bazel_workspace
from ao.core.bazel_test_ownership import ConfiguredTarget
from ao.core.bazel_test_services import stable_service_root
from ao.core.paths import PROJECT_ROOT

PRODUCT_ROOT = "//:check_products"
TEST_ROOT = "//:check_tests"
NATIVE_TEST_ROOT = "//:check_native_tests"
RUN_ROOT = "//:check_runs"
UNIT_TEST = "//test:unit"
BACKEND_TEST = "//test:backend_test"
BACKEND_SOURCE = "test/backend/BackendTest.cpp"
RUN_BINARY = "//app:runner"
CANARY = "//test:canary"
CANARY_SOURCE = "test/canary/Canary.cpp"
SUITE = "//:fixture_suite"
UNIT_SOURCE = "test/unit/ValueTest.cpp"
ORPHAN_SOURCE = "test/unit/OrphanTest.cpp"
UNSELECTED_TEST = "//test:unselected"
UNSELECTED_SOURCE = "test/unit/UnselectedTest.cpp"
CHECKSUM = "c" * 64
# Representative base launch rc written by the faked workspace preparation.
BASE_RC_CONTENT = "build --define=AOBUS_BASE_RC=fixture\n"
OWNERSHIP_SUFFIX = ".aobus-test-ownership.json"


def _quoted_set(labels):
    """Bazel set() of JSON-quoted labels, matching the completion query contract."""
    return "set(" + " ".join(json.dumps(label) for label in labels) + ")"


def _analysis_roots_expression(labels):
    return f"config({_quoted_set(labels)}, target)"


def _completion_roots_expression(labels):
    return _analysis_roots_expression(labels)


def _leaf_selector(root):
    return f"labels(srcs, config({root}, target))"


def _action_of(invocation):
    """The Bazel action follows the launch rc flags, not a fixed position."""
    return next(argument for argument in invocation[1:] if not argument.startswith("-"))


def _completion_witness(declared_kind_overrides=None, *, backend=False):
    """A minimal consistent declared/configured graph for the three check roots.

    The roots own one product binary, one registered test and one unattended
    native run. Every document is a hand-built witness; the production
    extractors and audits in ``bazel_test_ownership`` do the verification.
    """
    witness = _CompletionWitness(declared_kind_overrides)
    witness.add(PRODUCT_ROOT, "filegroup", deps=("//app:product",), testonly=True)
    witness.add(
        "//app:product",
        "cc_binary",
        srcs=("//app:main.cpp",),
        sources=("app/main.cpp",),
        executable="bazel-out/k8-dbg/bin/app/product",
    )
    witness.add(TEST_ROOT, "filegroup", deps=(UNIT_TEST,), testonly=True)
    witness.add(
        UNIT_TEST,
        "cc_test",
        srcs=("//test:unit/ValueTest.cpp",),
        sources=(UNIT_SOURCE,),
        executable="bazel-out/k8-dbg/bin/test/unit_test",
        testonly=True,
    )
    witness.add(RUN_ROOT, "filegroup", deps=(RUN_BINARY,), srcs=(RUN_BINARY,), testonly=True)
    witness.add(
        RUN_BINARY,
        "cc_binary",
        srcs=("//app:runner.cpp",),
        sources=("app/runner.cpp",),
        executable="bazel-out/k8-dbg/bin/app/runner",
        testonly=True,
    )
    # A manual error-canary library: analyzed through the explicit scope, never built.
    witness.add(
        CANARY,
        "cc_library",
        srcs=("//test:canary/Canary.cpp",),
        sources=(CANARY_SOURCE,),
        testonly=True,
        tags=("manual",),
    )
    # A declared transparent suite: present in the query graph, excluded from analysis.
    witness.add_declared_only(SUITE, "test_suite", deps=(UNIT_TEST,))
    if backend:
        witness.add(
            BACKEND_TEST,
            "cc_test",
            srcs=("//test:backend/BackendTest.cpp",),
            sources=(BACKEND_SOURCE,),
            executable="bazel-out/k8-dbg/bin/test/backend_test",
            testonly=True,
        )
        witness.select_native_extra(BACKEND_TEST)
    return witness


class _CompletionWitness:
    """Independent query XML, cquery jsonproto/starlark and aquery witnesses."""

    def __init__(self, declared_kind_overrides=None):
        self._declared_kind_overrides = dict(declared_kind_overrides or ())
        # Seed nodes: (label, kind, deps, sources, executable, testonly, tags, configuration)
        self._nodes = []
        self._declared = []  # (label, declared kind, attributes)
        self._paths = {}
        self._configurations = {}
        self._dependency_paths = {}
        self._transitions = []  # (path, node) in the current cquery universe and aquery graph
        self._outside = []  # (path, node) aquery-only, unreachable from analysis seeds
        self._native_extras = []
        self.incompatible_owners = set()
        self.unreached = set()

    def add(
        self,
        label,
        kind,
        *,
        deps=(),
        srcs=(),
        sources=(),
        executable=None,
        testonly=False,
        tags=(),
        configuration=CHECKSUM,
        dependency_paths=None,
    ):
        path = f"bazel-out/k8-dbg/bin/{len(self._nodes) + 1}{OWNERSHIP_SUFFIX}"
        self._paths[label] = path
        self._configurations[label] = configuration
        node = (label, kind, tuple(deps), tuple(sources), executable, testonly, tuple(tags), configuration)
        self._nodes.append(node)
        if dependency_paths is not None:
            self._dependency_paths[path] = tuple(dependency_paths)
        self._declared.append(
            (
                label,
                self._declared_kind_overrides.get(label, kind),
                {"srcs": tuple(srcs), "deps": tuple(deps), "tags": tuple(tags)},
            )
        )
        return path

    def add_declared_only(self, label, kind, *, deps=(), srcs=(), tags=()):
        """A declared transparent rule Bazel analysis must exclude, never a node."""
        self._declared.append((label, kind, {"srcs": tuple(srcs), "deps": tuple(deps), "tags": tuple(tags)}))

    def _query_xml(self):
        lines = ["<query>"]
        for label, kind, attributes in self._declared:
            lines.append(f'  <rule name="{label}" class="{kind}">')
            for name, values in attributes.items():
                if not values:
                    continue
                lines.append(f'    <list name="{name}">')
                lines.extend(f'      <label value="{value}"/>' for value in values)
                lines.append("    </list>")
            lines.append("  </rule>")
        lines.append("</query>")
        return "\n".join(lines).encode()

    def _cquery_jsonproto(self, identities):
        """cquery jsonproto. A list of labels uses each seed's target configuration."""
        if identities and isinstance(identities[0], str):
            identities = [(label, self._configurations.get(label, CHECKSUM)) for label in identities]
        configurations = []
        index = {}
        for _, checksum in identities:
            if checksum not in index:
                index[checksum] = len(configurations) + 1
                configurations.append({"id": index[checksum], "checksum": checksum})
        return json.dumps(
            {
                "results": [
                    {
                        "target": {"type": "RULE", "rule": {"name": label, "ruleClass": "rule"}},
                        "configurationId": index[checksum],
                    }
                    for label, checksum in identities
                ],
                "configurations": configurations,
            }
        ).encode()

    def selected_tests(self):
        return f"{UNIT_TEST}\n".encode()

    def completion_root_labels(self, platform):
        return [PRODUCT_ROOT, RUN_ROOT, *sorted(self.executed_test_labels(platform))]

    def completion_roots(self, platform):
        """The configured completion roots: products, runs and the selected tests."""
        return self._cquery_jsonproto(
            [(label, self._configurations[label]) for label in self.completion_root_labels(platform)]
        )

    def root_compatibility(self, platform):
        return "".join(f"{label}|False\n" for label in self.completion_root_labels(platform)).encode()

    def executables(self):
        return [node[0] for node in self._nodes if node[1] in ("cc_test", "cc_binary")]

    def reached_executables(self):
        """Executables below the completion roots, beside Bazel's rule wrappers."""
        labels = [label for label in self.executables() if label not in self.unreached]
        labels.append("@@rules_cc+//cc/private/rules_impl/wrappers:cc_test_impl_wrapper")
        return "".join(f"{label}\n" for label in labels).encode()

    def owner_compatibility(self, owners):
        return "".join(f"{label}|{label in self.incompatible_owners}\n" for label in owners).encode()

    def configuration_of(self, label):
        return self._configurations[label]

    def native_runs(self):
        return f"{RUN_BINARY}\n".encode()

    def select_native_extra(self, label):
        """The configured native suite selects this leaf."""
        self._native_extras.append(label)

    def native_extra_tests(self, platform):
        """cquery labels(srcs, //:check_native_tests)."""
        return "".join(f"{label}\n" for label in self._native_extras).encode()

    def executed_test_labels(self, platform):
        """The test summaries Bazel actually emits for the executed selection."""
        return [UNIT_TEST, *self._native_extras]

    def tests_bep(self, labels):
        """A minimal representative test BEP stream: progress, fresh summaries, end."""
        events = [{"id": {"progress": {"opaqueCount": 1}}, "progress": {"stdout": "fake analysis progress"}}]
        events.extend(
            {
                "id": {"testSummary": {"label": label, "configuration": {"id": self.configuration_of(label)}}},
                "testSummary": {"overallStatus": "PASSED", "totalRunCount": 1, "totalNumCached": 0},
            }
            for label in labels
        )
        events.append({"lastMessage": True})
        return events


class _FakeBazel:
    """Discriminated Bazel boundary: one answer per real check phase query."""

    def __init__(self, witness, events, platform):
        self.witness = witness
        self.invocations = []
        self.events = events
        self.platform = platform
        self.exits = {"build": 0, "test": 0, "run": 0}
        # Optional per-test mutation of the default fresh test BEP stream.
        self.bep_override = None
        self.completion_roots_override = None
        self.reached_override = None
        self.owner_queries = []

    def run(self, argv, *, cwd, env, stdout, stderr, check=False):
        self.invocations.append(list(argv))
        action = _action_of(argv)
        self.events.append(("bazel", action))
        arguments = [
            argument
            for argument in argv[argv.index(action) + 1 :]
            if not argument.startswith(("--jobs=", "--remote_cache="))
        ]
        payload, exit_code = self._answer(action, arguments)
        stdout.write(payload)
        if action == "test":
            # The test phase writes its BEP file at the argv-requested path.
            sink = next(
                argument[len("--build_event_json_file=") :]
                for argument in argv
                if argument.startswith("--build_event_json_file=")
            )
            events = self.witness.tests_bep(self.witness.executed_test_labels(self.platform))
            if self.bep_override is not None:
                events = self.bep_override(list(events))
            Path(sink).write_text("".join(json.dumps(event) + "\n" for event in events), encoding="utf-8")
        if exit_code:
            stderr.write(f"simulated {action} failure\n".encode())
        return SimpleNamespace(returncode=exit_code)

    def _answer(self, action, arguments):
        expression = arguments[0] if arguments else ""
        if action == "query" and "--output=xml" in arguments:
            return self.witness._query_xml(), 0
        if action == "query" and "--output=label" in arguments:
            return self.witness.selected_tests(), 0
        completion = _completion_roots_expression(self.witness.completion_root_labels(self.platform))
        if action == "cquery" and expression == completion:
            if "--output=starlark" in arguments:
                return self.witness.root_compatibility(self.platform), 0
            if "--output=jsonproto" not in arguments:
                raise AssertionError(f"completion roots must be config(set) jsonproto: {arguments!r}")
            payload = self.completion_roots_override
            return (self.witness.completion_roots(self.platform) if payload is None else payload), 0
        if action == "cquery" and expression == f'kind("cc_test|cc_binary", deps({completion}))':
            payload = self.reached_override
            return (self.witness.reached_executables() if payload is None else payload), 0
        if action == "cquery" and any(
            argument.startswith("--starlark:expr=str(target.label) + ") for argument in arguments
        ):
            # Owner compatibility: one row per declared owner of a registered source.
            self.owner_queries.append(re.findall(r'"([^"]+)"', expression))
            return self.witness.owner_compatibility(self.owner_queries[-1]), 0
        if action == "cquery" and "--output=starlark" in arguments and expression == _leaf_selector(NATIVE_TEST_ROOT):
            return self.witness.native_extra_tests(self.platform), 0
        if action == "cquery" and "--output=starlark" in arguments and expression == _leaf_selector(RUN_ROOT):
            return self.witness.native_runs(), 0
        if action in self.exits:
            return b"", self.exits[action]
        raise AssertionError(f"unexpected Bazel invocation: {action} {arguments!r}")


class _OwnedPipewire:
    """Stands in for the portal-owned private PipeWire service lifecycle."""

    def __init__(self, events):
        self.starts = 0
        self.stops = 0
        self.events = events
        self.calls = []

    @contextlib.contextmanager
    def service(self, log_dir, session_environment, *, runtime_root=None):
        self.starts += 1
        self.calls.append((Path(log_dir), dict(session_environment), runtime_root))
        self.events.append("pipewire-start")
        try:
            # Owned runtime and require flags merge into the already-owned GTK session.
            yield {
                **session_environment,
                "TMPDIR": "/tmp/aobus-check-fake/pipewire/tmp",
                "XDG_RUNTIME_DIR": "/tmp/aobus-check-fake/pipewire",
                "PIPEWIRE_RUNTIME_DIR": "/tmp/aobus-check-fake/pipewire",
                "PIPEWIRE_REMOTE": "pipewire-0",
                "AOBUS_REQUIRE_PIPEWIRE": "1",
            }
        finally:
            self.stops += 1
            self.events.append("pipewire-stop")


class _OwnedDisplays:
    """Stands in for the portal-owned Xvfb/private-D-Bus service lifecycle."""

    def __init__(self, events):
        self.starts = 0
        self.stops = 0
        self.startup_error = None
        self.events = events
        self.environment = {
            "DBUS_SESSION_BUS_ADDRESS": "unix:path=/run/aobus-check-fake/session",
            "DISPLAY": ":417",
        }
        self.ipc_roots = []

    @contextlib.contextmanager
    def service(self, count=1, *, ipc_root=None):
        self.starts += 1
        self.ipc_roots.append(ipc_root)
        self.events.append("service-start")
        if self.startup_error is not None:
            print("Error: controlled GTK service startup failure", file=sys.stderr)
            raise self.startup_error
        try:
            yield [dict(self.environment) for _ in range(count)]
        finally:
            self.stops += 1
            self.events.append("service-stop")


class _CheckHarness:
    """Real check orchestration behind faked workspace, gate and Bazel boundaries."""

    def __init__(
        self, source_root, tree, *, witness=None, guardrail_exit=0, contract_exit=0, bazel_exits=None, platform="linux"
    ):
        self.source_root = Path(source_root)
        self.tree = Path(tree)
        self.base = self.tree / "base-workspace"
        self.executable = self.tree / "fake-bazel"
        self.server_javabase = Path("/nix/store/aobusfakejdk-check-boundary")
        self.platform = platform
        self.events = []
        self.bazel = _FakeBazel(witness if witness is not None else _completion_witness(), self.events, self.platform)
        if bazel_exits:
            self.bazel.exits.update(bazel_exits)
        self.workspace_calls = []
        self.prepare_calls = []
        self.invocation_calls = []
        self.guardrail_calls = []
        self.contract_calls = []
        self.displays = _OwnedDisplays(self.events)
        self.pipewire = _OwnedPipewire(self.events)
        self._guardrail_exit = guardrail_exit
        self._contract_exit = contract_exit

    # Workspace/SDK boundary: the check owns the tree, prepared rc and executable.

    def invocation(self, executable, generated, *, isolate_rc=False):
        # The rc isolation itself is real production behavior; record the requests.
        self.invocation_calls.append((executable, generated, isolate_rc))
        return bazel_workspace.invocation(executable, generated, isolate_rc=isolate_rc)

    def __enter__(self):
        stack = contextlib.ExitStack()
        stack.enter_context(mock.patch("ao.core.bazel_check.PROJECT_ROOT", self.source_root))
        stack.enter_context(mock.patch("ao.core.bazel_check.bazel_workspace", self))
        stack.enter_context(mock.patch("ao.core.bazel_check.guardrails", self._guardrails_module()))
        stack.enter_context(mock.patch("ao.core.bazel_check.bazel_contract", self._contract_module()))
        stack.enter_context(
            mock.patch(
                "ao.core.bazel_check.subprocess",
                SimpleNamespace(run=self.bazel.run, SubprocessError=subprocess.SubprocessError),
            )
        )
        stack.enter_context(mock.patch("ao.command.test.virtual_gtk_displays", self.displays.service))
        stack.enter_context(mock.patch("ao.core.bazel_test_services.private_pipewire", self.pipewire.service))
        stack.enter_context(mock.patch("ao.core.bazel_native.native_platform", return_value=self.platform))
        # The flat command re-enters the same faked workspace boundary, and its
        # repository tooling suite is an owned stage, not part of this witness.
        stack.enter_context(mock.patch("ao.command.check.bazel_workspace", self))
        stack.enter_context(mock.patch("ao.core.tooltest.run", return_value=0))
        # Only the selected-Nix-JDK SDK boundary is faked; the invocation logic stays real.
        stack.enter_context(
            mock.patch("ao.core.bazel_workspace._linux_server_javabase", return_value=self.server_javabase)
        )
        self._stack = stack
        return self

    def __exit__(self, *exception):
        self._stack.close()
        return False

    def _guardrails_module(self):
        harness = self

        def run(source_root, output_dir, platform):
            harness.guardrail_calls.append((Path(source_root), Path(output_dir), platform))
            return SimpleNamespace(
                errors=("fixture guardrail finding",) if harness._guardrail_exit else (),
                warnings=(),
                report_path=Path(output_dir) / "guardrails.json",
                exit_code=harness._guardrail_exit,
            )

        return SimpleNamespace(run=run)

    def _contract_module(self):
        harness = self

        def verify(executable, build_tree, prepared, config, *, jobs, isolate_rc=False):
            harness.contract_calls.append((prepared, config, jobs, isolate_rc))
            return harness._contract_exit

        return SimpleNamespace(verify=verify)

    def run(self, *cli):
        arguments = make_parser().parse_args(("check", *cli))
        captured_out, captured_err = io.StringIO(), io.StringIO()
        with contextlib.redirect_stdout(captured_out), contextlib.redirect_stderr(captured_err):
            status = check_command.run_command(arguments)
        return status, captured_out.getvalue(), captured_err.getvalue()

    def receipts(self, config="debug"):
        return [
            json.loads(path.read_text(encoding="utf-8"))
            for path in sorted((self.tree / "checks" / config).glob("check-*/receipt.json"))
        ]

    def workspace_path(self, path, config):
        self.workspace_calls.append((path, config))
        return self.tree

    def prepare_workspace(self, build_tree, config, prefix=None, *, graph_options=()):
        self.prepare_calls.append((build_tree, config, prefix, graph_options))
        # The base launch rc is real fixture IO: the check copies it into its context.
        self.base.mkdir(parents=True, exist_ok=True)
        (self.base / "aobus.bazelrc").write_text(BASE_RC_CONTENT, encoding="utf-8")
        (self.base / "request.json").write_text(
            json.dumps({"schema": 1, "host": self.platform, "source": str(self.source_root), "prefix": prefix}),
            encoding="utf-8",
        )
        return self.base

    def bazel_executable(self):
        return self.executable


class CheckParserTest(unittest.TestCase):
    def test_check_scopes_flavor_tree_jobs_prefix_and_cache_without_target_forwarding(self):
        environment = dict(os.environ)
        environment.pop("AOBUS_BAZEL_REMOTE_CACHE", None)
        with mock.patch.dict(os.environ, environment):
            arguments = make_parser().parse_args(["check"])
            self.assertEqual(
                (arguments.flavor, arguments.path, arguments.vcpkg_prefix, arguments.remote_cache),
                ("debug", None, None, None),
            )
            self.assertIsInstance(arguments.jobs, int)
            self.assertFalse(arguments.fresh)
            self.assertTrue(make_parser().parse_args(["check", "--fresh"]).fresh)
            scoped = make_parser().parse_args(
                [
                    "check",
                    "release",
                    "-p",
                    "/tmp/external-tree",
                    "--jobs",
                    "5",
                    "--vcpkg-prefix",
                    "/tmp/installed-prefix",
                    "--remote-cache",
                    "https://cache.invalid",
                ]
            )
            self.assertEqual(
                (scoped.flavor, scoped.jobs, scoped.path, scoped.vcpkg_prefix, scoped.remote_cache),
                ("release", 5, "/tmp/external-tree", "/tmp/installed-prefix", "https://cache.invalid"),
            )
        with mock.patch.dict(os.environ, {"AOBUS_BAZEL_REMOTE_CACHE": "https://from-env.invalid"}):
            self.assertEqual(make_parser().parse_args(["check"]).remote_cache, "https://from-env.invalid")
        # The complete scope leaves no target selection or option forwarding channel.
        for rejected in (["//:explicit-target"], ["--", "--config=release"], ["--config", "profile"], ["--headless"]):
            with self.assertRaises(SystemExit), contextlib.redirect_stderr(io.StringIO()):
                make_parser().parse_args(["check", *rejected])


class BazelCheckOrchestrationTest(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix="aobus-bazel-check orchestration-")
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name).resolve()
        self.source_root = self.root / "source"
        self.tree = self.root / "tree"
        unit = self.source_root / "test" / "unit"
        unit.mkdir(parents=True)
        # A real registration file: the production discovery and audit scan it.
        (unit / "ValueTest.cpp").write_text(
            "#include <catch2/catch_test_macros.hpp>\n\n"
            'TEST_CASE("Value - decodes a value") {\n    REQUIRE(true);\n}\n',
            encoding="utf-8",
        )

    def test_check_runs_the_complete_owned_roots_and_retains_a_passing_receipt(self):
        prefix = self.root / "installed-prefix"
        with _CheckHarness(self.source_root, self.tree) as harness:
            status, stdout, _ = harness.run(
                "release",
                "-p",
                str(self.tree),
                "--jobs",
                "3",
                "--vcpkg-prefix",
                str(prefix),
                "--remote-cache",
                "https://cache.invalid",
            )
        self.assertEqual(status, 0)
        self.assertIn("Bazel completion passed (release); receipt:", stdout)
        (receipt,) = harness.receipts("release")
        attempt = Path(receipt["phases"][0]["stdout"]).parent
        prepared = Path(receipt["prepared"])
        self.assertEqual(receipt["base_prepared"], str(harness.base))
        self.assertEqual(prepared, attempt / "configuration")
        self.assertEqual(
            [phase["name"] for phase in receipt["phases"]],
            [
                "guardrails",
                "native-context",
                "declared-graph",
                "native-test-selection",
                "test-selection",
                "completion-roots",
                "compatibility",
                "reached-executables",
                "owner-compatibility",
                "test-ownership",
                "compile-contracts",
                "build",
                "tests",
                "test-results",
                "native-run-selection",
                "native-run-0",
            ],
        )
        self.assertEqual(
            (receipt["status"], receipt["exit"], receipt["config"], receipt["platform"], receipt["jobs"]),
            ("passed", 0, "release", "linux", 3),
        )
        self.assertEqual(receipt["native_runs"], [RUN_BINARY])
        # The configured native suite selection is part of the receipt and its scope.
        self.assertEqual(receipt["native_tests"], [])
        self.assertEqual(receipt["scope"]["native_tests"], NATIVE_TEST_ROOT)
        # Passing BEP summaries are bound to the exact configured selection.
        self.assertTrue((attempt / "tests.bep.json").is_file())
        self.assertEqual(
            receipt["test_results"],
            [
                {
                    "label": UNIT_TEST,
                    "configuration": CHECKSUM,
                    "status": "PASSED",
                    "runs": 1,
                    "cached": 0,
                    "executed": 1,
                }
            ],
        )
        results = next(phase for phase in receipt["phases"] if phase["name"] == "test-results")
        self.assertTrue(Path(results["stdout"]).is_file())
        self.assertIn("Configured test summaries: 1 passed; 1 executed runs, 0 cached runs.", stdout)
        self.assertIn("hygiene", receipt["scope"]["excluded"])
        self.assertIn("documentation", receipt["scope"]["excluded"])
        # The frozen per-attempt rc carries the whole owned runtime environment and
        # cache flag, so the test argv forwards no test_env/cache flag and every
        # analysis shares one configuration identity.
        self.assertEqual(
            (prepared / "aobus.bazelrc").read_text(encoding="utf-8"),
            "build --define=AOBUS_BASE_RC=fixture\n"
            "common --lockfile_mode=error\n"
            "build --cache_test_results=auto\n"
            'build "--test_env=AOBUS_REQUIRE_PIPEWIRE=1"\n'
            'build "--test_env=DBUS_SESSION_BUS_ADDRESS=unix:path=/run/aobus-check-fake/session"\n'
            'build "--test_env=DISPLAY=:417"\n'
            'build "--test_env=PIPEWIRE_REMOTE=pipewire-0"\n'
            'build "--test_env=PIPEWIRE_RUNTIME_DIR=/tmp/aobus-check-fake/pipewire"\n'
            'build "--test_env=TMPDIR=/tmp/aobus-check-fake/pipewire/tmp"\n'
            'build "--test_env=XDG_RUNTIME_DIR=/tmp/aobus-check-fake/pipewire"\n',
        )
        # Exact full-root invocations: personal/system rc files are isolated behind
        # exactly the checked-in and managed rc plus the selected Nix server JDK.
        controlled_rc = [
            "--nosystem_rc",
            "--nohome_rc",
            "--noworkspace_rc",
            f"--bazelrc={PROJECT_ROOT / '.bazelrc'}",
            f"--server_javabase={harness.server_javabase}",
            f"--bazelrc={prepared / 'aobus.bazelrc'}",
        ]

        def recorded(name):
            return next(phase for phase in receipt["phases"] if phase["name"] == name)["argv"]

        self.assertEqual(
            recorded("declared-graph"),
            [str(harness.executable), *controlled_rc, "query", 'kind("rule", //...)', "--output=xml"],
        )
        # The native suite and run roots are distinct discriminated selections.
        for name, root in (("native-test-selection", NATIVE_TEST_ROOT), ("native-run-selection", RUN_ROOT)):
            with self.subTest(phase=name):
                self.assertEqual(
                    recorded(name),
                    [
                        str(harness.executable),
                        *controlled_rc,
                        "cquery",
                        "--jobs=3",
                        f"labels(srcs, config({root}, target))",
                        "--output=starlark",
                        "--starlark:expr=str(target.label)",
                    ],
                )
        roots_expression = _completion_roots_expression([PRODUCT_ROOT, RUN_ROOT, UNIT_TEST])
        self.assertIn(roots_expression, recorded("completion-roots"))
        self.assertIn(roots_expression, recorded("compatibility"))
        self.assertIn(f'kind("cc_test|cc_binary", deps({roots_expression}))', recorded("reached-executables"))
        # Only the declared owner of a registered test source needs a compatibility answer.
        self.assertEqual(harness.bazel.owner_queries, [[UNIT_TEST]])
        # The build/test/run arguments name only the three check roots.
        for invocation in harness.bazel.invocations:
            if _action_of(invocation) in {"build", "test", "run"}:
                self.assertNotIn(CANARY, " ".join(invocation))
            self.assertNotIn(SUITE, " ".join(invocation))
        for name, action, arguments in (
            ("build", "build", [PRODUCT_ROOT, TEST_ROOT, f"--build_event_json_file={attempt / 'build.bep.json'}"]),
            ("tests", "test", [TEST_ROOT, f"--build_event_json_file={attempt / 'tests.bep.json'}"]),
            ("native-run-0", "run", [RUN_BINARY]),
        ):
            with self.subTest(phase=name):
                self.assertEqual(
                    recorded(name),
                    [
                        str(harness.executable),
                        *controlled_rc,
                        action,
                        "--jobs=3",
                        "--remote_cache=https://cache.invalid",
                        *arguments,
                    ],
                )
        # Scoped options reached the workspace and compile-contract boundaries unchanged;
        # every configured command and the contract verify share the frozen rc path.
        # The flat command admits the selected tree once before the core producer
        # resolves it again inside the locked orchestration.
        self.assertTrue(harness.workspace_calls)
        self.assertEqual({config for _, config in harness.workspace_calls}, {"release"})
        self.assertEqual(harness.workspace_calls[-1], (str(self.tree), "release"))
        self.assertEqual(harness.prepare_calls, [(self.tree, "release", str(prefix), ())])
        self.assertEqual((prepared / "request.json").read_bytes(), (harness.base / "request.json").read_bytes())
        self.assertEqual(harness.contract_calls, [(prepared, "release", 3, True)])
        self.assertEqual(harness.guardrail_calls, [(self.source_root, attempt, "linux")])
        self.assertEqual(len(harness.invocation_calls), len(harness.bazel.invocations))
        self.assertTrue(all(generated == prepared and isolate for _, generated, isolate in harness.invocation_calls))
        # The owned services open before all configured work and close after it:
        # GTK first, then the private PipeWire context with the already-owned session.
        # Both use the tree's stable roots, so a repeat attempt hashes the same test env.
        self.assertEqual((harness.displays.starts, harness.displays.stops), (1, 1))
        self.assertEqual((harness.pipewire.starts, harness.pipewire.stops), (1, 1))
        self.assertEqual(harness.displays.ipc_roots, [stable_service_root(self.tree, "gtk-bus")])
        self.assertEqual(
            harness.pipewire.calls,
            [
                (
                    attempt,
                    {
                        "DBUS_SESSION_BUS_ADDRESS": "unix:path=/run/aobus-check-fake/session",
                        "DISPLAY": ":417",
                    },
                    stable_service_root(self.tree, "pipewire"),
                )
            ],
        )
        self.assertEqual(harness.events[0], "service-start")
        self.assertEqual(harness.events[1], "pipewire-start")
        self.assertEqual(harness.events[-2], "pipewire-stop")
        self.assertEqual(harness.events[-1], "service-stop")
        self.assertEqual(
            [event for event in harness.events if isinstance(event, tuple)],
            [("bazel", _action_of(invocation)) for invocation in harness.bazel.invocations],
        )
        # Only the managed executable, the controlled rc flags and known actions run:
        # no personal rc channel, hygiene or other hidden tool is invoked.
        for invocation in harness.bazel.invocations:
            self.assertEqual(invocation[0], str(harness.executable))
            self.assertEqual(invocation[1:7], controlled_rc)
            self.assertEqual(
                [flag for flag in invocation if flag.startswith("--bazelrc=")], [controlled_rc[3], controlled_rc[5]]
            )
            self.assertIn(_action_of(invocation), {"query", "cquery", "build", "test", "run"})
        # Every phase retains its logs, and the ownership receipt keeps the real audit.
        for phase in receipt["phases"]:
            self.assertTrue(Path(phase["stdout"]).is_file(), phase["name"])
            self.assertTrue(Path(phase["stderr"]).is_file(), phase["name"])
        ownership = json.loads(Path(receipt["ownership"]).read_text(encoding="utf-8"))
        self.assertEqual(ownership["errors"], [])
        self.assertEqual(ownership["owners"], [UNIT_TEST])
        self.assertEqual([case["label"] for case in ownership["selected_tests"]], [UNIT_TEST])

    def _unselected_test_witness(self):
        """A second registered cc_test that no executed test root selects."""
        (self.source_root / UNSELECTED_SOURCE).write_text(
            "#include <catch2/catch_test_macros.hpp>\n\n"
            'TEST_CASE("Unselected - decodes a value") {\n    REQUIRE(true);\n}\n',
            encoding="utf-8",
        )
        witness = _completion_witness()
        witness.add(
            UNSELECTED_TEST,
            "cc_test",
            srcs=("//test:unit/UnselectedTest.cpp",),
            sources=(UNSELECTED_SOURCE,),
            executable="bazel-out/k8-dbg/bin/test/unselected",
            testonly=True,
        )
        return witness

    def test_test_built_by_a_completion_root_but_never_executed_fails_before_compile(self):
        # Reached (for example through //:check_products) but outside the test selection.
        witness = self._unselected_test_witness()
        with _CheckHarness(self.source_root, self.tree, witness=witness) as harness:
            status, _, _ = harness.run()
        self.assertEqual(status, 1)
        (receipt,) = harness.receipts()
        self.assertEqual(receipt["error"], "test-ownership exited 1")
        ownership = json.loads(Path(receipt["ownership"]).read_text(encoding="utf-8"))
        self.assertEqual(ownership["errors"], [f"{UNSELECTED_TEST}: test is not in the executed test selection"])
        self.assertEqual(harness.contract_calls, [])

    def test_owner_incompatible_with_this_host_is_recorded_not_required(self):
        witness = self._unselected_test_witness()
        witness.unreached.add(UNSELECTED_TEST)
        witness.incompatible_owners.add(UNSELECTED_TEST)
        with _CheckHarness(self.source_root, self.tree, witness=witness) as harness:
            harness.run()
        (receipt,) = harness.receipts()
        ownership = json.loads(Path(receipt["ownership"]).read_text(encoding="utf-8"))
        self.assertEqual((ownership["errors"], ownership["incompatible_owners"]), ([], [UNSELECTED_TEST]))

    def test_guardrail_findings_fail_the_check_before_any_bazel_invocation(self):
        with _CheckHarness(self.source_root, self.tree, guardrail_exit=1) as harness:
            status, _, stderr = harness.run()
        self.assertEqual(status, 1)
        (receipt,) = harness.receipts()
        self.assertEqual([phase["name"] for phase in receipt["phases"]], ["guardrails"])
        self.assertEqual((receipt["status"], receipt["exit"], receipt["error"]), ("failed", 1, "guardrails exited 1"))
        self.assertEqual(harness.bazel.invocations, [])
        self.assertEqual(harness.contract_calls, [])
        # Guardrails fail before the owned services are ever opened.
        self.assertEqual((harness.displays.starts, harness.displays.stops), (0, 0))
        self.assertEqual((harness.pipewire.starts, harness.pipewire.stops), (0, 0))
        self.assertIn(
            "fixture guardrail finding",
            Path(receipt["phases"][0]["stdout"]).read_text(encoding="utf-8"),
        )
        self.assertIn("guardrails failed", stderr)

    def test_registration_without_configured_owner_fails_the_real_audit_before_compile_and_test(self):
        (self.source_root / "test" / "unit" / "OrphanTest.cpp").write_text(
            "#include <catch2/catch_test_macros.hpp>\n\n"
            'TEST_CASE("Orphan - has no configured owner") {\n    REQUIRE(true);\n}\n',
            encoding="utf-8",
        )
        with _CheckHarness(self.source_root, self.tree) as harness:
            status, _, _ = harness.run()
        self.assertEqual(status, 1)
        (receipt,) = harness.receipts()
        self.assertEqual(
            [phase["name"] for phase in receipt["phases"]],
            [
                "guardrails",
                "native-context",
                "declared-graph",
                "native-test-selection",
                "test-selection",
                "completion-roots",
                "compatibility",
                "reached-executables",
                "owner-compatibility",
                "test-ownership",
            ],
        )
        self.assertEqual(
            (receipt["status"], receipt["exit"], receipt["error"]), ("failed", 1, "test-ownership exited 1")
        )
        # The services opened before the analysis and close on the ownership failure.
        context = next(phase for phase in receipt["phases"] if phase["name"] == "native-context")
        self.assertEqual((context["status"], context["exit"]), ("failed", 1))
        self.assertEqual((harness.displays.starts, harness.displays.stops), (1, 1))
        self.assertEqual((harness.pipewire.starts, harness.pipewire.stops), (1, 1))
        self.assertEqual(harness.contract_calls, [])
        self.assertFalse(
            any(_action_of(invocation) in {"build", "test", "run"} for invocation in harness.bazel.invocations)
        )
        # The real validator, not a mock, reports the graph/source mismatch.
        ownership = json.loads(Path(receipt["ownership"]).read_text(encoding="utf-8"))
        self.assertTrue(any(ORPHAN_SOURCE in error for error in ownership["errors"]))

    def test_the_failing_phase_subprocess_exit_status_becomes_the_check_exit_status(self):
        with _CheckHarness(self.source_root, self.tree, bazel_exits={"build": 7}) as harness:
            status, _, _ = harness.run()
        self.assertEqual(status, 7)
        (receipt,) = harness.receipts()
        self.assertEqual((receipt["status"], receipt["exit"], receipt["error"]), ("failed", 7, "build exited 7"))
        build = next(phase for phase in receipt["phases"] if phase["name"] == "build")
        self.assertEqual((build["status"], build["exit"]), ("failed", 7))
        self.assertEqual(receipt["phases"][-1]["name"], "build")
        # The failing phase's streams are retained per attempt and in the tree log.
        self.assertIn("simulated build failure", Path(build["stderr"]).read_text(encoding="utf-8"))
        self.assertIn("simulated build failure", (self.tree / "build.log").read_text(encoding="utf-8"))
        # The inner diagnostic is retained in the wrapper's redirected log.
        context = next(phase for phase in receipt["phases"] if phase["name"] == "native-context")
        self.assertIn("build failed", Path(context["stderr"]).read_text(encoding="utf-8"))
        self.assertEqual((harness.displays.starts, harness.displays.stops), (1, 1))
        self.assertEqual((harness.pipewire.starts, harness.pipewire.stops), (1, 1))
        self.assertFalse(any(_action_of(invocation) == "test" for invocation in harness.bazel.invocations))

    def test_linux_tears_down_the_owned_gtk_services_after_a_test_failure(self):
        with _CheckHarness(self.source_root, self.tree, bazel_exits={"test": 1}) as harness:
            status, _, _ = harness.run()
        self.assertEqual(status, 1)
        self.assertEqual((harness.displays.starts, harness.displays.stops), (1, 1))
        (receipt,) = harness.receipts()
        # The native-context wrapper preserves the inner failing phase name while its
        # own record retains the failure and the services still close on teardown.
        self.assertEqual((receipt["status"], receipt["exit"], receipt["error"]), ("failed", 1, "tests exited 1"))
        context = next(phase for phase in receipt["phases"] if phase["name"] == "native-context")
        self.assertEqual((context["status"], context["exit"]), ("failed", 1))
        # The inner phase's diagnostic is retained in the wrapper's redirected log.
        self.assertIn("tests failed", Path(context["stderr"]).read_text(encoding="utf-8"))
        self.assertEqual((harness.pipewire.starts, harness.pipewire.stops), (1, 1))
        self.assertEqual(harness.events[-2:], ["pipewire-stop", "service-stop"])
        tests = next(phase for phase in receipt["phases"] if phase["name"] == "tests")
        self.assertEqual((tests["status"], tests["exit"]), ("failed", 1))
        # The owned environment reached the frozen rc, not the test argv.
        check_rc = (Path(receipt["prepared"]) / "aobus.bazelrc").read_text(encoding="utf-8")
        self.assertIn('build "--test_env=DISPLAY=:417"', check_rc)
        self.assertIn('build "--test_env=DBUS_SESSION_BUS_ADDRESS=unix:path=/run/aobus-check-fake/session"', check_rc)
        self.assertNotIn("--test_env", " ".join(tests["argv"]))

    def test_default_check_reports_executed_cached_and_mixed_passing_results(self):
        for runs, cached in ((1, 0), (1, 1), (3, 2)):
            with self.subTest(runs=runs, cached=cached):
                tree = self.root / f"tree-cache-{runs}-{cached}"
                with _CheckHarness(self.source_root, tree, platform="windows") as harness:

                    def counts(events, runs=runs, cached=cached):
                        events[1]["testSummary"].update(totalRunCount=runs, totalNumCached=cached)
                        return events

                    harness.bazel.bep_override = counts
                    status, stdout, _ = harness.run()
                self.assertEqual(status, 0)
                (receipt,) = harness.receipts()
                self.assertEqual(receipt["test_cache_policy"], "auto")
                self.assertEqual(
                    receipt["test_summary"],
                    {"passed": 1, "executed_runs": runs - cached, "cached_runs": cached},
                )
                self.assertEqual(
                    receipt["test_results"],
                    [
                        {
                            "label": UNIT_TEST,
                            "configuration": CHECKSUM,
                            "status": "PASSED",
                            "runs": runs,
                            "cached": cached,
                            "executed": runs - cached,
                        }
                    ],
                )
                self.assertIn(
                    f"Configured test summaries: 1 passed; {runs - cached} executed runs, {cached} cached runs.",
                    stdout,
                )
                rc = (Path(receipt["prepared"]) / "aobus.bazelrc").read_text(encoding="utf-8")
                self.assertIn("build --cache_test_results=auto", rc)
                self.assertNotIn("--nocache_test_results", rc)
                ownership = json.loads(Path(receipt["ownership"]).read_text(encoding="utf-8"))
                self.assertEqual([target["label"] for target in ownership["selected_tests"]], [UNIT_TEST])
                self.assertNotIn("executed_tests", ownership)

    def test_fresh_check_disables_cache_and_rejects_cached_results(self):
        for cached in (0, 1):
            with self.subTest(cached=cached):
                tree = self.root / f"tree-fresh-{cached}"
                with _CheckHarness(self.source_root, tree, platform="windows") as harness:

                    def counts(events, cached=cached):
                        events[1]["testSummary"]["totalNumCached"] = cached
                        return events

                    harness.bazel.bep_override = counts
                    status, stdout, _ = harness.run("--fresh")
                self.assertEqual(status, 1 if cached else 0)
                (receipt,) = harness.receipts()
                self.assertEqual(receipt["test_cache_policy"], "fresh")
                rc = (Path(receipt["prepared"]) / "aobus.bazelrc").read_text(encoding="utf-8")
                self.assertIn("build --nocache_test_results", rc)
                self.assertNotIn("--cache_test_results=auto", rc)
                if cached:
                    self.assertIn("completion test did not execute freshly", receipt["error"])
                    self.assertNotIn("native_runs", receipt)
                    self.assertNotIn("Configured test summaries:", stdout)
                else:
                    self.assertEqual(receipt["test_summary"], {"passed": 1, "executed_runs": 1, "cached_runs": 0})
                    self.assertIn("Configured test summaries: 1 passed; 1 executed runs, 0 cached runs.", stdout)

    def test_bep_uses_lf_records_not_unicode_line_separators_inside_json_strings(self):
        target = ConfiguredTarget(UNIT_TEST, CHECKSUM)
        path = self.root / "unicode-progress.bep.json"
        summary = {
            "id": {"testSummary": {"label": UNIT_TEST, "configuration": {"id": CHECKSUM}}},
            "testSummary": {"overallStatus": "PASSED", "totalRunCount": 1, "totalNumCached": 0},
        }
        for separator in ("\u0085", "\u2028", "\u2029"):
            with self.subTest(separator=ascii(separator)):
                events = [{"progress": {"stdout": "before" + separator + "after"}}, summary, {"lastMessage": True}]
                path.write_text(
                    "".join(json.dumps(event, ensure_ascii=False) + "\n" for event in events), encoding="utf-8"
                )
                self.assertEqual(
                    bazel_check._test_results(path, frozenset({target}), fresh=True),
                    [{"label": UNIT_TEST, "configuration": CHECKSUM, "status": "PASSED", "runs": 1, "cached": 0}],
                )
        path.write_text(json.dumps(summary) + '\n\u0085\n{"lastMessage": true}\n', encoding="utf-8")
        with self.assertRaises(ValueError):
            bazel_check._test_results(path, frozenset({target}), fresh=True)

    def test_other_platforms_run_tests_without_gtk_services(self):
        with _CheckHarness(self.source_root, self.tree, platform="macos") as harness:
            status, _, _ = harness.run()
        self.assertEqual(status, 0)
        self.assertEqual((harness.displays.starts, harness.displays.stops), (0, 0))
        self.assertEqual((harness.pipewire.starts, harness.pipewire.stops), (0, 0))
        (receipt,) = harness.receipts()
        self.assertEqual(receipt["platform"], "macos")
        self.assertEqual(receipt["native_tests"], [])
        tests = next(phase for phase in receipt["phases"] if phase["name"] == "tests")
        # Other platforms pin no server JDK and inject no owned runtime environment.
        self.assertNotIn("--server_javabase", " ".join(tests["argv"]))
        self.assertNotIn("--test_env", " ".join(tests["argv"]))
        check_rc = (Path(receipt["prepared"]) / "aobus.bazelrc").read_text(encoding="utf-8")
        self.assertNotIn("--test_env", check_rc)
        self.assertIn("build --cache_test_results=auto", check_rc)

    def test_cquery_selected_native_extra_runs_and_is_owned_by_the_real_audit(self):
        backend = self.source_root / "test" / "backend"
        backend.mkdir(parents=True)
        # A real registration: the audit proves the cquery-selected leaf owns it.
        (backend / "BackendTest.cpp").write_text(
            "#include <catch2/catch_test_macros.hpp>\n\n"
            'TEST_CASE("Backend - decodes packets") {\n    REQUIRE(true);\n}\n',
            encoding="utf-8",
        )
        witness = _completion_witness(backend=True)
        with _CheckHarness(self.source_root, self.tree, witness=witness) as harness:
            status, _, _ = harness.run("--jobs", "2")
        self.assertEqual(status, 0)
        (receipt,) = harness.receipts()
        attempt = Path(receipt["phases"][0]["stdout"]).parent
        # The actual cquery-selected leaf is appended to the build and test roots.
        self.assertEqual(receipt["native_tests"], [BACKEND_TEST])
        prepared = Path(receipt["prepared"])
        controlled_rc = [
            "--nosystem_rc",
            "--nohome_rc",
            "--noworkspace_rc",
            f"--bazelrc={PROJECT_ROOT / '.bazelrc'}",
            f"--server_javabase={harness.server_javabase}",
            f"--bazelrc={prepared / 'aobus.bazelrc'}",
        ]

        def recorded(name):
            return next(phase for phase in receipt["phases"] if phase["name"] == name)["argv"]

        self.assertEqual(
            recorded("build"),
            [
                str(harness.executable),
                *controlled_rc,
                "build",
                "--jobs=2",
                PRODUCT_ROOT,
                TEST_ROOT,
                BACKEND_TEST,
                f"--build_event_json_file={attempt / 'build.bep.json'}",
            ],
        )
        self.assertEqual(
            recorded("tests"),
            [
                str(harness.executable),
                *controlled_rc,
                "test",
                "--jobs=2",
                TEST_ROOT,
                BACKEND_TEST,
                f"--build_event_json_file={attempt / 'tests.bep.json'}",
            ],
        )
        # The selected leaf executed freshly and is bound by the BEP selection.
        self.assertEqual(
            receipt["test_results"],
            [
                {
                    "label": BACKEND_TEST,
                    "configuration": CHECKSUM,
                    "status": "PASSED",
                    "runs": 1,
                    "cached": 0,
                    "executed": 1,
                },
                {
                    "label": UNIT_TEST,
                    "configuration": CHECKSUM,
                    "status": "PASSED",
                    "runs": 1,
                    "cached": 0,
                    "executed": 1,
                },
            ],
        )
        # The real audit owns the registered backend source through the executed leaf.
        ownership = json.loads(Path(receipt["ownership"]).read_text(encoding="utf-8"))
        self.assertEqual(ownership["errors"], [])
        self.assertEqual(ownership["incompatible_owners"], [])
        self.assertEqual([case["label"] for case in ownership["selected_tests"]], [BACKEND_TEST, UNIT_TEST])


class BazelRcIsolationTest(unittest.TestCase):
    def test_invocation_isolates_personal_rc_files_and_pins_the_linux_server_jdk(self):
        executable, generated = Path("/tmp/fake-bazel"), Path("/tmp/prepared")
        server_javabase = Path("/nix/store/fixture-jdk")
        # Ordinary invocations keep the default rc and no server JDK pin.
        self.assertEqual(
            bazel_workspace.invocation(executable, generated),
            [str(executable), f"--bazelrc={generated / 'aobus.bazelrc'}"],
        )
        with (
            mock.patch("ao.core.bazel_native.native_platform", return_value="linux"),
            mock.patch("ao.core.bazel_workspace._linux_server_javabase", return_value=server_javabase),
        ):
            self.assertEqual(
                bazel_workspace.invocation(executable, generated, isolate_rc=True),
                [
                    str(executable),
                    "--nosystem_rc",
                    "--nohome_rc",
                    "--noworkspace_rc",
                    f"--bazelrc={PROJECT_ROOT / '.bazelrc'}",
                    f"--server_javabase={server_javabase}",
                    f"--bazelrc={generated / 'aobus.bazelrc'}",
                ],
            )
        with mock.patch("ao.core.bazel_native.native_platform", return_value="macos"):
            self.assertEqual(
                bazel_workspace.invocation(executable, generated, isolate_rc=True),
                [
                    str(executable),
                    "--nosystem_rc",
                    "--nohome_rc",
                    "--noworkspace_rc",
                    f"--bazelrc={PROJECT_ROOT / '.bazelrc'}",
                    f"--bazelrc={generated / 'aobus.bazelrc'}",
                ],
            )

    def test_ordinary_execute_keeps_the_default_rc_argv(self):
        temporary = tempfile.TemporaryDirectory(prefix="aobus-bazel-check rc-")
        self.addCleanup(temporary.cleanup)
        tree = Path(temporary.name).resolve()
        prepared, executable = tree / "prepared", tree / "fake-bazel"
        invocations = []

        def fake_run(argv, *, cwd, env, stdout, stderr):
            invocations.append(list(argv))
            return SimpleNamespace(returncode=0)

        with mock.patch("ao.core.bazel_workspace.subprocess", SimpleNamespace(run=fake_run, STDOUT=subprocess.STDOUT)):
            with contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()):
                self.assertEqual(
                    bazel_workspace.execute(executable, tree, prepared, "build", "debug", ["//lib:lib"]), 0
                )
        (invocation,) = invocations
        self.assertEqual(
            invocation,
            [str(executable), f"--bazelrc={prepared / 'aobus.bazelrc'}", "build", "--jobs=2", "//lib:lib"],
        )


class CheckDispatchTest(unittest.TestCase):
    def test_check_dispatch_hands_the_admitted_request_straight_to_the_core_producer(self):
        temporary = tempfile.TemporaryDirectory(prefix="aobus-check-routes-")
        self.addCleanup(temporary.cleanup)
        tree = Path(temporary.name).resolve() / "tree"
        executed = []
        workspace = SimpleNamespace(
            workspace_path=lambda path, config: tree,
            prepare_workspace=lambda build_tree, config, prefix=None, graph_options=(): tree / "prepared",
            bazel_executable=lambda: tree / "fake-bazel",
            execute=lambda *arguments, **keywords: executed.append(("execute", arguments)) or 0,
        )
        with (
            mock.patch("ao.command.check.bazel_workspace", workspace),
            mock.patch("ao.core.tooltest.run", return_value=0) as tooling,
            mock.patch(
                "ao.core.bazel_check.run",
                side_effect=lambda arguments: executed.append(("check", arguments.config, arguments.path)) or 0,
            ) as check_run,
        ):
            arguments = make_parser().parse_args(["check"])
            with (
                contextlib.redirect_stdout(io.StringIO()),
                contextlib.redirect_stderr(io.StringIO()),
            ):
                self.assertEqual(check_command.run_command(arguments), 0)
            check_run.assert_called_once()
            tooling.assert_called_once()
        self.assertEqual(executed, [("check", "debug", str(tree))])


if __name__ == "__main__":
    unittest.main()
