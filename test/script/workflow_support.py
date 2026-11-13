"""Small declared-artifact fixture shared by native workflow boundary tests."""

import os
import sys
from pathlib import Path

from ao.core.bazel_workflows import ExecutableArtifact


def executable_artifact(
    tree: Path, *, target: str = "//fixture:program", config: str = "debug", compiler: str = "fixture-compiler"
) -> ExecutableArtifact:
    execution_root = tree / "state/output/execroot/_main"
    binary = execution_root / "bazel-out/native/bin/fixture/program"
    binary.parent.mkdir(parents=True, exist_ok=True)
    binary.write_bytes(b"fixture executable; never launched as native code")
    binary.chmod(0o755)
    runfiles = Path(str(binary) + ".runfiles")
    runfiles.mkdir()
    data = execution_root / "fixture/data.txt"
    data.parent.mkdir(parents=True, exist_ok=True)
    data.write_text("declared fixture data\n", encoding="utf-8")
    logical = runfiles / "_main/fixture/data.txt"
    logical.parent.mkdir(parents=True)
    os.link(data, logical)
    return ExecutableArtifact(
        target=target,
        configuration="a" * 64,
        config=config,
        tree=tree,
        execution_root=execution_root,
        executable=binary,
        declared_outputs=(binary,),
        runfiles=("fixture/data.txt",),
        runfiles_directory=runfiles,
        runfiles_manifest=None,
        compiler_identity=compiler,
        requested_by=(target,),
    )


def lint_fixture_document(tree: Path) -> dict:
    """Tiny real-file action boundary; never a native tool or product graph."""
    execution_root = tree / "state/output/execroot/_main"
    inputs = [
        "script/ao/core/bazel_lint_fixture.py",
        "external/policy/policy.json",
        "external/sdk/metadata.json",
        "bazel-out/native/bin/tool/lint/verification.json",
        "external/sdk/clang-tidy",
        "bazel-out/exec/bin/tool/lint/plugin.so",
        "test/lint/fixture/example/Case.cpp",
    ]
    outputs = [
        "bazel-out/native/bin/test/lint/lint_test.fixture-result.json",
        "bazel-out/native/bin/test/lint/lint_test.fixture.log",
        "bazel-out/native/bin/test/lint/lint_test.w",
    ]
    for relative in inputs:
        path = execution_root / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(b"declared fixture input, never executed\n")
    argv = [str(Path(sys.executable).absolute()), "-I", "-S", "-B", "-X", "utf8", inputs[0]]
    for name, value in zip(
        ("policy", "sdk", "verification", "tidy", "plugin", "report", "log", "work"),
        [*inputs[1:6], *outputs],
        strict=True,
    ):
        argv.extend(["--" + name, value])
    argv.extend(["--jobs", "2"])
    paths = [*inputs, *outputs]
    return {
        "targets": [{"id": 1, "label": "//test/lint:lint_test"}],
        "configuration": [{"id": 2, "checksum": "a" * 64, "isTool": False}],
        "pathFragments": [{"id": index + 1, "label": path} for index, path in enumerate(paths)],
        "artifacts": [{"id": index + 1, "pathFragmentId": index + 1} for index in range(len(paths))],
        "depSetOfFiles": [{"id": 3, "directArtifactIds": list(range(1, len(inputs) + 1))}],
        "actions": [
            {
                "targetId": 1,
                "configurationId": 2,
                "actionKey": "c" * 64,
                "mnemonic": "AobusLintFixtures",
                "arguments": argv,
                "inputDepSetIds": [3],
                "outputIds": list(range(len(inputs) + 1, len(paths) + 1)),
                "environmentVariables": [
                    {"key": "PRIVATE_ACTION_ENV", "value": "selected"},
                    {"key": "CPATH", "value": ""},
                ],
            }
        ],
    }
