"""Small declared-artifact fixture shared by performance boundary tests."""

import os
from pathlib import Path

from ao.portal.bazel_workflows import ExecutableArtifact


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
    )
