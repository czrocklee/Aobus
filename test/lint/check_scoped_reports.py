"""Build one CLI-aspect output group and read its real Bazel delivery. Run outside Bazel."""

import os
import subprocess
import sys
from pathlib import Path

from ao.graph.bazel_lint_reports import main_output_group, read_reports


def check(bep: Path, bazel_build: list[str]) -> None:
    owner = "//test/lint:werror_replay"
    header = "test/lint/scoped_main-group/WerrorReplay.h"
    source = header.removesuffix(".h") + ".cpp"
    main_group = main_output_group(header, windows=os.name == "nt")
    if not main_group:
        raise ValueError(f"Expected a per-main group for {header}")
    groups = ("aobus_lint_index", main_group)
    subprocess.run(
        [
            *bazel_build,
            "--aspects=//bazel/lint:lint.bzl%lint_aspect",
            "--aspects_parameters=aobus_lint_consumers={}",
            "--output_groups=" + ",".join(groups),
            "--experimental_build_event_upload_strategy=local",
            "--remote_download_outputs=all",
            "--norun_validations",
            f"--build_event_json_file={bep}",
            owner,
        ],
        check=True,
    )
    (inventory,) = read_reports(bep, [owner], output_groups=groups)
    # The index describes the native TU as well; only its paired header's reports belong in this delivery.
    pairs = {(r.identity.source, r.identity.main) for r in inventory.reports}
    if not inventory.has_native_tu or pairs != {(source, header)}:
        raise ValueError(f"Expected only {header} in the context of {source}; received {pairs}")
    if any(r.outcome == "compiler_error" for r in inventory.reports):
        raise ValueError(f"Could not analyze {header}")


if __name__ == "__main__":
    check(Path(sys.argv[1]).resolve(), sys.argv[2:])
