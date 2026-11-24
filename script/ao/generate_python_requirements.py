"""Render the standalone pip locks from the reviewed Python wheel artifacts."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
import tomllib
from dataclasses import dataclass
from pathlib import Path
from urllib.parse import unquote, urlsplit

_ROOT = Path(__file__).parent
_DOCS = frozenset({"markdown-it-py", "mdurl"})
_CHECKERS = frozenset({"mypy", "ruff"})
_HOSTS = {"macos": ("arm64", "x86_64"), "windows": ("x86_64",)}
_TARGETS = {f"{host}-{arch}": (host, arch) for host, arches in _HOSTS.items() for arch in arches}
_TARGETS["docs"] = ("docs", "")
# A filename-safe token, not a PEP 440 validator. uv and pip admit versions.
_VERSION_TOKEN = r"[0-9][a-zA-Z0-9]*(?:\.[a-zA-Z0-9]+)*"
_WHEEL = re.compile(rf"([a-zA-Z0-9_]+)-({_VERSION_TOKEN})-([a-z0-9.]+)-([a-z0-9]+)-([a-z0-9_]+)\.whl")
_SHA256 = re.compile(r"[0-9a-f]{64}")
_PYPI = re.compile(r"https://files\.pythonhosted\.org/packages/[0-9a-f]{2}/[0-9a-f]{2}/[0-9a-f]{60}/([^/]+)")
_HEADER = "# Generated from python-wheels.json; run python3 script/ao/generate_python_requirements.py.\n"


@dataclass(frozen=True)
class _Wheel:
    name: str
    version: str
    filename: str
    url: str
    sha256: str
    python: str
    abi: str
    platform: str


def _object(value: object, keys: set[str], where: str) -> dict[str, object]:
    if not isinstance(value, dict) or value.keys() != keys:
        raise ValueError(f"{where}: expected exactly {', '.join(sorted(keys))}")
    return value


def _json_object(pairs: list[tuple[str, object]]) -> dict[str, object]:
    result: dict[str, object] = {}
    for key, value in pairs:
        if key in result:
            raise ValueError(f"duplicate JSON key: {key}")
        result[key] = value
    return result


def _read_json(path: Path) -> object:
    try:
        return json.loads(path.read_text(encoding="utf-8"), object_pairs_hook=_json_object)
    except ValueError as error:
        raise ValueError(f"{path.name}: {error}") from error


def _parse_wheel(record: dict[str, object], where: str) -> _Wheel:
    filename, url, sha256 = (record[key] for key in ("filename", "url", "sha256"))
    if not isinstance(filename, str) or not (wheel := _WHEEL.fullmatch(filename)):
        raise ValueError(f"{where}: invalid wheel filename: {filename!r}")
    if not isinstance(url, str) or not (link := _PYPI.fullmatch(url)) or link[1] != filename:
        raise ValueError(f"{where}: expected the official PyPI URL for {filename}")
    if not isinstance(sha256, str) or not _SHA256.fullmatch(sha256):
        raise ValueError(f"{where}: invalid SHA-256 for {filename}")
    name, version, python, abi, platform = wheel.groups()
    if not name[0].isalnum() or not name[-1].isalnum():
        raise ValueError(f"{where}: invalid distribution name in {filename}")
    return _Wheel(re.sub(r"_+", "-", name).lower(), version, filename, url, sha256, python, abi, platform)


def _matches_platform_arch_and_abi(wheel: _Wheel, host: str, arch: str, minor: int) -> bool:
    if wheel.platform == "any":
        return wheel.python in {"py3", "py2.py3"} and wheel.abi == "none"
    if host == "docs":
        return False
    if host == "windows":
        platform_matches = wheel.platform == "win_amd64"
    else:
        # pip checks the macOS deployment tag against the actual host at install time.
        macos = re.fullmatch(r"macosx_[0-9]+_[0-9]+_(arm64|x86_64|universal2)", wheel.platform)
        platform_matches = macos is not None and macos[1] in {arch, "universal2"}
    if not platform_matches:
        return False
    cp = f"cp3{minor}"
    if wheel.python == cp and wheel.abi == cp:
        return True
    if wheel.python in {"py3", "py2.py3"} and wheel.abi == "none":
        return True  # e.g. a platform executable with no CPython ABI dependency
    abi3 = re.fullmatch(r"cp3([0-9]+)", wheel.python)
    return wheel.abi == "abi3" and abi3 is not None and 2 <= int(abi3[1]) <= minor


def _select(value: object, policy: dict[str, object], minor: int) -> dict[str, dict[str, _Wheel]]:
    if not isinstance(value, list):
        raise ValueError("python-wheels.json: expected a list of wheel artifacts")
    by_target: dict[str, dict[str, _Wheel]] = {target: {} for target in _TARGETS}
    filenames: set[str] = set()
    digests: set[str] = set()
    for index, item in enumerate(value, 1):
        where = f"artifact {index}"
        record = _object(item, {"filename", "url", "sha256", "targets"}, where)
        wheel = _parse_wheel(record, where)
        if wheel.filename.lower() in filenames or wheel.sha256 in digests:
            raise ValueError(f"{where}: duplicate wheel identity: {wheel.filename}")
        filenames.add(wheel.filename.lower())
        digests.add(wheel.sha256)
        if wheel.name in _DOCS | _CHECKERS and policy.get(wheel.name) != wheel.version:
            raise ValueError(
                f"{where}: {wheel.name} wheel version {wheel.version!r} differs from toolchain.json pin "
                f"{policy.get(wheel.name)!r}; use the exact wheel filename version spelling in toolchain.json"
            )
        targets = record["targets"]
        if not isinstance(targets, list) or not targets:
            raise ValueError(f"{where}: expected a nonempty target list")
        seen: set[str] = set()
        for target in targets:
            if not isinstance(target, str) or target not in _TARGETS or target in seen:
                raise ValueError(f"{where}: invalid or duplicate target: {target!r}")
            if (target == "docs") != (wheel.name in _DOCS):
                raise ValueError(f"{where}: incorrect docs/native wheel group for {wheel.name}")
            if not _matches_platform_arch_and_abi(wheel, *_TARGETS[target], minor):
                raise ValueError(
                    f"{target}: wheel platform, architecture or ABI does not match CPython 3.{minor}: {wheel.filename}"
                )
            selected = by_target[target]
            if wheel.name in selected:
                raise ValueError(f"{target}: duplicate normalized pin: {wheel.name}")
            selected[wheel.name] = wheel
            seen.add(target)
    for target, selected in by_target.items():
        missing = (_DOCS if target == "docs" else _CHECKERS) - selected.keys()
        if missing:
            raise ValueError(f"{target}: missing wheels: {', '.join(sorted(missing))}")
    return by_target


def _pins(wheels: dict[str, list[_Wheel]]) -> str:
    result = []
    for name, choices in sorted(wheels.items()):
        hashes = " \\\n".join(f"    --hash=sha256:{digest}" for digest in sorted({wheel.sha256 for wheel in choices}))
        result.append(f"{name}=={choices[0].version} \\\n{hashes}\n")
    return "".join(result)


def _catalog_matrix(manifest: object, policy: object) -> tuple[str, dict[str, dict[str, _Wheel]]]:
    """Admit the artifact matrix shared by projection and offline review."""
    catalog = _object(manifest, {"schema", "artifacts"}, "python-wheels.json")
    if type(catalog["schema"]) is not int or catalog["schema"] != 1:
        raise ValueError("python-wheels.json: expected schema 1")
    if not isinstance(policy, dict):
        raise ValueError("toolchain.json: expected a policy object")
    python = policy.get("python")
    if not isinstance(python, str) or not (release := re.fullmatch(r"3\.([0-9]+)\.[0-9]+", python)):
        raise ValueError("toolchain.json: expected a CPython 3.x.y pin")
    by_target = _select(catalog["artifacts"], policy, int(release[1]))
    for host, arches in _HOSTS.items():
        baseline = by_target[f"{host}-{arches[0]}"]
        for arch in arches[1:]:
            wheels = by_target[f"{host}-{arch}"]
            if wheels.keys() != baseline.keys():
                raise ValueError(f"{host}: package names differ between architectures")
            for name, wheel in wheels.items():
                if baseline[name].version != wheel.version:
                    raise ValueError(
                        f"{host}-{arch}: {name} wheel version {wheel.version!r} differs from "
                        f"{host}-{arches[0]} wheel version {baseline[name].version!r}; choose wheels with identical "
                        "version spelling for both architectures"
                    )
    return python, by_target


def render_requirements(manifest: object, policy: object) -> dict[str, bytes]:
    """Validate the pinned artifact matrix and render the three pip locks."""
    _, by_target = _catalog_matrix(manifest, policy)
    docs = {name: [wheel] for name, wheel in by_target["docs"].items()}
    generated = {"docs-requirements.txt": (_HEADER + _pins(docs)).encode()}
    for host, arches in _HOSTS.items():
        merged: dict[str, list[_Wheel]] = {}
        for arch in arches:
            for name, wheel in by_target[f"{host}-{arch}"].items():
                merged.setdefault(name, []).append(wheel)
        generated[f"{host}-requirements.txt"] = (_HEADER + "-r docs-requirements.txt\n" + _pins(merged)).encode()
    return generated


def sync_requirements(root: Path, *, check: bool) -> None:
    """Verify the checked-in locks without writes, or regenerate them."""
    generated = render_requirements(_read_json(root / "python-wheels.json"), _read_json(root / "toolchain.json"))
    stale = []
    for name, content in generated.items():
        path = root / name
        if check:
            if not path.is_file() or path.read_bytes() != content:
                stale.append(name)
        else:
            path.write_bytes(content)
    if stale:
        raise ValueError(f"stale generated locks: {', '.join(stale)}; run script/ao/generate_python_requirements.py")


def uv_root_pins(policy: object, *, native: bool) -> str:
    """Export governed roots for maintainer resolution, without consulting the wheel catalog."""
    if not isinstance(policy, dict):
        raise ValueError("toolchain.json: expected a policy object")
    roots = _DOCS | _CHECKERS if native else _DOCS
    pins = []
    for name in sorted(roots):
        version = policy.get(name)
        if not isinstance(version, str) or not re.fullmatch(_VERSION_TOKEN, version):
            raise ValueError(f"toolchain.json: invalid version pin for {name}")
        pins.append(f"{name}=={version}\n")
    return "".join(pins)


def _sha256(path: Path) -> str:
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def match_official_wheel(path: Path, metadata: object) -> dict[str, str]:
    """Match actual wheel bytes to one file in a saved PyPI Simple JSON response."""
    if not isinstance(metadata, dict) or not isinstance(files := metadata.get("files"), list):
        raise ValueError("expected a PyPI Simple JSON files list")
    digest = _sha256(path)
    urls = [
        file.get("url")
        for file in files
        if isinstance(file, dict)
        and file.get("filename") == path.name
        and isinstance(hashes := file.get("hashes"), dict)
        and hashes.get("sha256") == digest
    ]
    if len(urls) != 1:
        raise ValueError(f"{path.name} (SHA-256 {digest}): expected one PyPI match, found {len(urls)}")
    wheel = _parse_wheel({"filename": path.name, "sha256": digest, "url": urls[0]}, "PyPI metadata")
    return {"filename": wheel.filename, "url": wheel.url, "sha256": wheel.sha256}


def verify_uv_selection(report: str, expected: set[str], target: str) -> None:
    """Require uv's offline lock to contain one approved wheel per resolved package."""
    selected: set[str] = set()
    for package in tomllib.loads(report).get("packages", []):
        urls = [urlsplit(wheel["url"]) for wheel in package.get("wheels", [])]
        filenames = [unquote(url.path.rsplit("/", 1)[-1]) for url in urls]
        if len(filenames) != 1:
            raise ValueError(f"{target}: expected one wheel for {package['name']}, found {filenames}")
        if urls[0].scheme != "file" or urls[0].netloc not in {"", "localhost"} or filenames[0] in selected:
            raise ValueError(f"{target}: expected a distinct local wheel: {urls[0].geturl()}")
        selected.add(filenames[0])
    if selected != expected:
        raise ValueError(f"{target}: missing {sorted(expected - selected)}; unexpected {sorted(selected - expected)}")


def _verify_wheelhouse(wheelhouse: Path) -> None:
    policy = _read_json(_ROOT / "toolchain.json")
    manifest = _read_json(_ROOT / "python-wheels.json")
    python, matrix = _catalog_matrix(manifest, policy)
    environment = {key: value for key, value in os.environ.items() if not key.upper().startswith("UV_")}
    for wheel in {wheel for group in matrix.values() for wheel in group.values()}:
        path = wheelhouse / wheel.filename
        if _sha256(path) != wheel.sha256:
            raise ValueError(f"{path}: SHA-256 differs from python-wheels.json")
    for target, (host, arch) in _TARGETS.items():
        platform = "x86_64-pc-windows-msvc"
        if host == "macos":
            platform = ("aarch64" if arch == "arm64" else arch) + "-apple-darwin"
        expected = {wheel.filename for wheel in (matrix["docs"] | matrix[target]).values()}
        # Project only the explicitly reviewed candidates; uv must not choose an
        # alternative wheel merely because another target stages it too.
        with tempfile.TemporaryDirectory(prefix="aobus-wheel-review-") as directory:
            for filename in expected:
                shutil.copyfile(wheelhouse / filename, Path(directory) / filename)
            command = "uv --no-config --offline --no-cache --no-python-downloads pip compile -".split()
            command += "--no-index --only-binary :all: --format pylock.toml".split()
            command += ["--find-links", directory, "--python", sys.executable]
            command += ["--python-version", python, "--python-platform", platform]
            result = subprocess.run(
                command,
                input=uv_root_pins(policy, native=target != "docs"),
                text=True,
                stdout=subprocess.PIPE,
                env=environment,
                check=False,
            )
            if result.returncode:
                raise ValueError(f"{target}: uv offline resolution failed (exit {result.returncode})")
            verify_uv_selection(result.stdout, expected, target)
        print(f"{target}: {len(expected)} verified wheels")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    mode = parser.add_mutually_exclusive_group()
    mode.add_argument("--check", action="store_true", help="reject stale generated locks without changing them")
    mode.add_argument("--uv-input", choices=("docs", "native"), help="print the approved roots for uv pip compile")
    mode.add_argument(
        "--match-wheel", type=Path, nargs=2, metavar=("WHEEL", "METADATA"), help="match local bytes to PyPI JSON"
    )
    mode.add_argument(
        "--verify-wheelhouse", type=Path, metavar="DIRECTORY", help="reconcile reviewed wheels with uv offline"
    )
    args = parser.parse_args()
    try:
        if args.uv_input is not None:
            print(uv_root_pins(_read_json(_ROOT / "toolchain.json"), native=args.uv_input == "native"), end="")
        elif args.match_wheel is not None:
            wheel, metadata = args.match_wheel
            print(json.dumps(match_official_wheel(wheel, _read_json(metadata)), indent=2))
        elif args.verify_wheelhouse is not None:
            _verify_wheelhouse(args.verify_wheelhouse)
        else:
            sync_requirements(_ROOT, check=args.check)
    except (OSError, ValueError) as error:
        print(error, file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
