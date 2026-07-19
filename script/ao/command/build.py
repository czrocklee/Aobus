"""ao build — configure and build one flavor, without running tests (see `ao check`)."""

import argparse
import os
import shutil
from dataclasses import dataclass
from pathlib import Path

from ..core import builddir
from ..core.paths import PROJECT_ROOT
from ..core.proc import die, run

HELP = "Configure and build a native flavor"
NAME = "build"
# True when ao.bat must initialize the MSVC/vcpkg build environment first.
REQUIRES_BUILD_ENV = True


EPILOG = """\
examples:
  ./ao build                     # incremental debug build
  ./ao build release --clean     # clean release build
  ./ao build --target aobus-gtk  # build a single target
  ./ao build debug --clang       # clang build in its own build tree
  ./ao build pgo1                # PGO step 1: instrumented build
"""

WINDOWS_EPILOG = """\
examples:
  ao.bat build                       # incremental debug build
  ao.bat build release --clean       # clean release build
  ao.bat build --target aobus-tui    # build only the TUI target
  ao.bat build --target aobus        # build only the CLI target
"""


def add_build_arguments(parser: argparse.ArgumentParser) -> None:
    parser.add_argument(
        "flavor", nargs="?", default="debug", choices=builddir.flavors(), help="build flavor (default: debug)"
    )
    parser.add_argument("--clean", action="store_true", help="remove the build directory first")
    parser.add_argument("--clang", action="store_true", help="build with clang in a dedicated build tree")
    sanitizers = parser.add_mutually_exclusive_group()
    sanitizers.add_argument(
        "--asan",
        action="store_true",
        help="enable AddressSanitizer and UndefinedBehaviorSanitizer where available (debug only)",
    )
    sanitizers.add_argument("--tsan", action="store_true", help="enable thread sanitizer (debug only)")
    parser.add_argument("--verbose", action="store_true", help="show full build command lines")
    parser.add_argument("-p", "--path", metavar="<dir>", help="override the build directory")


def register(subparsers: "argparse._SubParsersAction[argparse.ArgumentParser]") -> None:
    parser = subparsers.add_parser(
        NAME,
        help=HELP,
        description=HELP,
        epilog=WINDOWS_EPILOG if builddir.platform_profile().name == "windows" else EPILOG,
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    add_build_arguments(parser)
    parser.add_argument(
        "--target", action="append", default=[], metavar="<target>", help="build specific target(s) only"
    )
    parser.set_defaults(func=run_command)


@dataclass
class BuildResult:
    build_dir: Path
    log: Path
    compiler: str
    preset: str = ""


def _windows_extended_path(path: str) -> str:
    if path.startswith("\\\\?\\"):
        return path
    if path.startswith("\\\\"):
        return "\\\\?\\UNC\\" + path[2:]
    return "\\\\?\\" + path


def _remove_build_directory(path: Path, profile: builddir.PlatformProfile) -> None:
    if profile.name == "windows":
        shutil.rmtree(_windows_extended_path(str(path.resolve())))
        return
    shutil.rmtree(path)


def validate_build_options(args: argparse.Namespace) -> builddir.PlatformProfile:
    """Reject build modes that the native platform cannot provide."""
    profile = builddir.platform_profile()
    if args.clang and profile.name == "windows":
        raise die(
            "Clang application builds are unavailable on Windows; the managed LLVM SDK is reserved for format and tidy."
        )
    if args.tsan and not profile.tsan_suites:
        raise die("ThreadSanitizer is unavailable on the Windows MSVC toolchain.")
    return profile


def do_build(args: argparse.Namespace, targets: list[str]) -> BuildResult:
    """Shared by `ao build` and `ao check`. Raises SystemExit on failure."""
    profile = validate_build_options(args)

    preset = builddir.preset(args.flavor)
    build_dir = (
        Path(args.path)
        if getattr(args, "path", None)
        else builddir.build_dir(
            args.flavor,
            clang=args.clang,
            asan=args.asan,
            tsan=args.tsan,
        )
    )

    if args.clean and build_dir.exists():
        print(f"Cleaning build directory ({build_dir})...")
        _remove_build_directory(build_dir, profile)

    build_dir.mkdir(parents=True, exist_ok=True)
    log = build_dir / "build.log"

    env = {"CC": "clang", "CXX": "clang++"} if args.clang else None
    if args.clang:
        print("clang enabled for this build.")

    configure = ["cmake", "-S", str(PROJECT_ROOT), "--preset", preset, "-B", str(build_dir)]
    configure.append(f"-DCMAKE_VERBOSE_MAKEFILE={'ON' if args.verbose else 'OFF'}")
    if args.asan:
        sanitizer_name = "ASan" if profile.name == "windows" else "ASan/UBSan"
        print(f"{sanitizer_name} enabled for this build.")
        configure.append("-DAOBUS_ENABLE_ASAN=ON")
    if args.tsan:
        print("TSan enabled for this build.")
        configure.append("-DAOBUS_ENABLE_TSAN=ON")

    print(f"Configuring Aobus with preset '{preset}' in '{build_dir}'...")
    if run(configure, env=env, log=log) != 0:
        raise die("configure failed.")

    build = ["cmake", "--build", str(build_dir)]
    if getattr(args, "asan", False) or getattr(args, "tsan", False):
        jobs = max(1, (os.cpu_count() or 1) // 2)
        build += ["--parallel", str(jobs)]
    else:
        build.append("--parallel")
    for target in targets:
        build += ["--target", target]
    if args.verbose:
        build.append("--verbose")

    print("Building Aobus...")
    if run(build, env=env, log=log, append=True) != 0:
        raise die("build failed.")

    compiler = "clang" if args.clang else profile.compiler
    return BuildResult(build_dir=build_dir, log=log, compiler=compiler, preset=preset)


def print_summary(args: argparse.Namespace, result: BuildResult, tests: str) -> None:
    print()
    print("All done!")
    print(f"  Preset: {result.preset or builddir.preset(args.flavor)}")
    print(f"  Build dir: {result.build_dir}")
    print(f"  Log file: {result.log}")
    print(f"  compiler: {result.compiler}")
    print(f"  asan: {args.asan}")
    print(f"  tsan: {args.tsan}")
    print(f"  tests: {tests}")


def print_pgo_instructions(args: argparse.Namespace, result: BuildResult) -> None:
    if args.flavor == "pgo1":
        next_command = "./ao build pgo2" + (" --clang" if args.clang else "")
        print()
        print("============================================")
        print("PGO Step 1 complete.")
        print()
        print("Next: Run the app to generate profile data:")
        print(f"  cd {result.build_dir} && ./app/linux-gtk/aobus-gtk")
        print("  # Use the app normally, then close it")
        print()
        print("Then run:")
        print(f"  {next_command}")
        print("============================================")
    elif args.flavor == "pgo2":
        print()
        print("============================================")
        print("PGO Step 2 complete.")
        print()
        print(f"Optimized binary: {result.build_dir}/app/linux-gtk/aobus-gtk")
        print(f"Run: cd {result.build_dir} && perf record -g -- ./app/linux-gtk/aobus-gtk")
        print("============================================")


def run_command(args: argparse.Namespace) -> int:
    result = do_build(args, args.target)
    print_pgo_instructions(args, result)
    print_summary(args, result, tests="skipped (use ./ao check or ./ao test)")
    return 0
