"""Check scoped CLI-aspect reports and input evidence. Run outside Bazel."""

import os
import subprocess
import sys
from pathlib import Path

from ao.graph.bazel_lint_reports import OwnerInventory, main_output_group, read_reports


def _build_inventory(
    bep: Path, bazel_build: list[str], owner: str, groups: tuple[str, ...], *options: str
) -> OwnerInventory:
    subprocess.run(
        [
            *bazel_build,
            "--aspects=//bazel/lint:lint.bzl%lint_aspect",
            "--aspects_parameters=aobus_lint_consumers={}",
            "--aspects_parameters=aobus_lint_inputs=on",
            "--output_groups=" + ",".join(groups),
            "--experimental_build_event_upload_strategy=local",
            "--remote_download_outputs=all",
            "--norun_validations",
            f"--build_event_json_file={bep}",
            *options,
            owner,
        ],
        check=True,
    )
    (inventory,) = read_reports(bep, [owner], output_groups=groups, input_index=True)
    return inventory


def check(bep: Path, bazel_build: list[str]) -> None:
    owner = "//test/lint:werror_replay"
    header = "test/lint/scoped_main-group/WerrorReplay.h"
    source = header.removesuffix(".h") + ".cpp"
    virtual_original = "test/lint/scoped_main-group/detail/ReplayValue.h"
    main_group = main_output_group(header, windows=os.name == "nt")
    if not main_group:
        raise ValueError(f"Expected a per-main group for {header}")
    index_group = "aobus_lint_index"
    inventory = _build_inventory(bep, bazel_build, owner, (index_group, main_group))
    # The transitive dependency publishes only the alias; the index must name the checkout original.
    missing = {source, header, virtual_original} - (inventory.candidate_inputs or frozenset())
    if missing:
        raise ValueError(f"Missing authored native inputs from the opted-in index: {', '.join(sorted(missing))}")
    # The index describes the native TU as well; only its paired header's reports belong in this delivery.
    pairs = {(r.identity.source, r.identity.main) for r in inventory.reports}
    if not inventory.has_native_tu or pairs != {(source, header)}:
        raise ValueError(f"Expected only {header} in the context of {source}; received {pairs}")
    if any(r.outcome == "compiler_error" for r in inventory.reports):
        raise ValueError(f"Could not analyze {header}")

    # An empty response changes no compiler options, but the index producer
    # cannot prove its contents. Request only the index; replay rejects @files.
    opaque = _build_inventory(
        bep.with_name(f"{bep.stem}-response{bep.suffix}"),
        bazel_build,
        owner,
        (index_group,),
        f"--cxxopt=@{os.devnull}",
    )
    if not opaque.has_native_tu or opaque.candidate_inputs is not None:
        raise ValueError("Expected a native TU with no configured input evidence for an opaque compiler response")


if __name__ == "__main__":
    check(Path(sys.argv[1]).resolve(), sys.argv[2:])
