"""ao council - build and run the Aobus council executable."""

import argparse
from pathlib import Path

from ..core import builddir
from ..core.proc import die, run

HELP = "Build and run the Aobus council executable"

EPILOG = """\
examples:
  ./ao council validate-config --registry config/agent-council.yaml
  ./ao council run --registry config/agent-council.yaml --repo "$PWD" --out /tmp/out /tmp/intent.yaml
  ./ao council -n -p /tmp/build/debug validate-config --registry config/agent-council.yaml
"""


def register(subparsers: "argparse._SubParsersAction[argparse.ArgumentParser]") -> None:
    parser = subparsers.add_parser(
        "council", help=HELP, description=HELP, epilog=EPILOG, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    parser.add_argument("-p", "--path", metavar="<dir>", help="build directory (default: /tmp/build/debug)")
    parser.add_argument("-n", "--no-build", action="store_true", help="skip the incremental executable build")
    parser.add_argument(
        "council_args", nargs=argparse.REMAINDER, metavar="arg", help="arguments passed to aobus-council"
    )
    parser.set_defaults(func=run_command)


def run_command(args: argparse.Namespace) -> int:
    build_path = Path(args.path) if args.path else builddir.build_dir("debug")
    executable = build_path / "tool" / "council" / "aobus-council"
    council_args = list(args.council_args)

    if not council_args:
        raise die("missing aobus-council subcommand. Try './ao council validate-config --registry ...'.")

    if not args.no_build:
        if not build_path.is_dir():
            raise die(f"build directory {build_path} does not exist. Run ./ao build first to configure the project.")
        print("=====================================")
        print(f"Building aobus-council in {build_path}...")
        print("=====================================")
        if run(["cmake", "--build", str(build_path), "--parallel", "--target", "aobus-council"]) != 0:
            raise die("council build failed.")

    if not executable.is_file():
        raise die(f"aobus-council not found at {executable}. Build first, e.g. with ./ao build --target aobus-council.")

    return run([str(executable), *council_args])
