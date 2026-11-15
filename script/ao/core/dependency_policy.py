"""Dependency-contract validation and installed vcpkg/NuGet identity parsing."""

from __future__ import annotations

import hashlib
import json
import re
import xml.etree.ElementTree as ET
import zipfile
from dataclasses import dataclass
from datetime import UTC, date, datetime
from pathlib import Path
from typing import Any

from .paths import PROJECT_ROOT

CONTRACT_FILE = PROJECT_ROOT / "dependency-contract.json"
SUPPORTED_PLATFORMS = frozenset({"linux", "macos", "windows"})
# Native resolver ownership. Linux retains the pinned Nix package set; macOS
# and Windows share the governed vcpkg manifest and registry snapshots.
VCPKG_PLATFORMS = frozenset({"macos", "windows"})
_VERSION_RE = re.compile(r"^[0-9]+(?:\.[0-9]+)*$")


class DependencyPolicyError(RuntimeError):
    """Raised when dependency policy or resolution evidence is invalid."""


@dataclass(frozen=True)
class EffectivePolicy:
    kind: str
    minimum: str
    exclusive_maximum: str | None = None
    exception_id: str | None = None

    @property
    def requested(self) -> str:
        if self.kind == "exact":
            return self.minimum
        if self.exclusive_maximum is not None:
            return f"{self.minimum}...<{self.exclusive_maximum}"
        return self.minimum

    def accepts(self, version: str) -> bool:
        actual = _version_tuple(version)
        minimum = _version_tuple(self.minimum)
        if self.kind == "exact":
            return actual == minimum
        if actual < minimum:
            return False
        return self.exclusive_maximum is None or actual < _version_tuple(self.exclusive_maximum)


@dataclass(frozen=True)
class VcpkgPackage:
    version: str
    triplet: str
    features: tuple[str, ...]

    @property
    def upstream_version(self) -> str:
        return self.version.split("#", maxsplit=1)[0]


def _mapping(value: object, context: str) -> dict[str, Any]:
    if not isinstance(value, dict):
        raise DependencyPolicyError(f"{context} must be a JSON object")
    return value


def _list(value: object, context: str) -> list[Any]:
    if not isinstance(value, list):
        raise DependencyPolicyError(f"{context} must be a JSON array")
    return value


def _string(value: object, context: str) -> str:
    if not isinstance(value, str) or not value:
        raise DependencyPolicyError(f"{context} must be a non-empty string")
    return value


def _string_list(value: object, context: str) -> list[str]:
    values = _list(value, context)
    result = [_string(item, f"{context}[]") for item in values]
    if not result:
        raise DependencyPolicyError(f"{context} must not be empty")
    if len(result) != len(set(result)):
        raise DependencyPolicyError(f"{context} contains duplicate values")
    return result


def _dependency_platforms(definition: dict[str, Any], context: str) -> list[str]:
    platforms = _string_list(definition.get("platforms"), f"{context}.platforms")
    unknown = set(platforms) - SUPPORTED_PLATFORMS
    if unknown:
        raise DependencyPolicyError(f"{context}.platforms contains unsupported values {sorted(unknown)}")
    return platforms


def _version_tuple(value: str) -> tuple[int, ...]:
    if not _VERSION_RE.fullmatch(value):
        raise DependencyPolicyError(f"unsupported governed version {value!r}; expected dotted numeric components")
    return tuple(int(component) for component in value.split("."))


def _normalized_nuget_version(value: str) -> tuple[int, ...]:
    components = _version_tuple(value)
    while len(components) > 1 and components[-1] == 0:
        components = components[:-1]
    return components


def _utc_today() -> date:
    return datetime.now(UTC).date()


def _utc_date(value: object, context: str) -> date:
    text = _string(value, context)
    try:
        parsed = date.fromisoformat(text)
    except ValueError as exc:
        raise DependencyPolicyError(f"{context} must be a UTC date in YYYY-MM-DD form") from exc
    if parsed.isoformat() != text:
        raise DependencyPolicyError(f"{context} must be a UTC date in YYYY-MM-DD form")
    return parsed


def read_json(path: Path) -> dict[str, Any]:
    try:
        return _mapping(json.loads(path.read_text(encoding="utf-8")), str(path))
    except (OSError, json.JSONDecodeError) as exc:
        raise DependencyPolicyError(f"cannot read valid JSON from {path}: {exc}") from exc


def validate_contract(contract: dict[str, Any], *, today: date | None = None) -> dict[str, Any]:
    if contract.get("leadResolver") != "nix":
        raise DependencyPolicyError("dependency contract leadResolver must be 'nix'")

    dependencies = _mapping(contract.get("dependencies"), "dependencies")
    if not dependencies:
        raise DependencyPolicyError("dependencies must not be empty")
    for name, raw_dependency in dependencies.items():
        _string(name, "dependency key")
        dependency = _mapping(raw_dependency, f"dependencies.{name}")
        platforms = _dependency_platforms(dependency, f"dependencies.{name}")
        policy = _mapping(dependency.get("policy"), f"dependencies.{name}.policy")
        kind = policy.get("kind")
        if kind == "exact":
            if set(policy) != {"kind", "version"}:
                raise DependencyPolicyError(
                    f"dependencies.{name}.policy exact policy may contain only kind and version"
                )
            _version_tuple(_string(policy.get("version"), f"dependencies.{name}.policy.version"))
        elif kind == "range":
            if not set(policy).issubset({"kind", "minimum", "exclusiveMaximum"}):
                raise DependencyPolicyError(f"dependencies.{name}.policy range policy has unknown fields")
            minimum = _string(policy.get("minimum"), f"dependencies.{name}.policy.minimum")
            _version_tuple(minimum)
            maximum_value = policy.get("exclusiveMaximum")
            if maximum_value is not None:
                maximum = _string(maximum_value, f"dependencies.{name}.policy.exclusiveMaximum")
                if _version_tuple(minimum) >= _version_tuple(maximum):
                    raise DependencyPolicyError(
                        f"dependencies.{name}.policy.exclusiveMaximum must be greater than minimum"
                    )
        else:
            raise DependencyPolicyError(f"dependencies.{name}.policy.kind must be 'exact' or 'range'")

        has_nix = "nix" in dependency
        if "linux" in platforms:
            nix = _mapping(dependency.get("nix"), f"dependencies.{name}.nix")
            _string(nix.get("attribute"), f"dependencies.{name}.nix.attribute")
        elif has_nix:
            raise DependencyPolicyError(f"dependencies.{name} declares a Nix resolver without Linux support")

        has_vcpkg = "vcpkg" in dependency
        has_nuget = "nuget" in dependency
        if "macos" in platforms and not has_vcpkg:
            raise DependencyPolicyError(f"dependencies.{name} must declare a vcpkg resolver for macOS")
        if "windows" in platforms and has_vcpkg == has_nuget:
            raise DependencyPolicyError(
                f"dependencies.{name} must declare exactly one Windows resolver: vcpkg or nuget"
            )
        if not VCPKG_PLATFORMS.intersection(platforms) and has_vcpkg:
            raise DependencyPolicyError(
                f"dependencies.{name} declares a vcpkg resolver without macOS or Windows support"
            )
        if "windows" not in platforms and has_nuget:
            raise DependencyPolicyError(f"dependencies.{name} declares a NuGet resolver without Windows support")

        if has_vcpkg:
            vcpkg = _mapping(dependency["vcpkg"], f"dependencies.{name}.vcpkg")
            _string(vcpkg.get("strategy"), f"dependencies.{name}.vcpkg.strategy")
            _string_list(vcpkg.get("ports"), f"dependencies.{name}.vcpkg.ports")
        if has_nuget:
            if policy.get("kind") != "exact":
                raise DependencyPolicyError(f"dependencies.{name} NuGet policy must be exact")
            nuget = _mapping(dependency["nuget"], f"dependencies.{name}.nuget")
            _string(nuget.get("package"), f"dependencies.{name}.nuget.package")
            _string(nuget.get("lock"), f"dependencies.{name}.nuget.lock")

        if "capabilities" in dependency:
            _string_list(dependency["capabilities"], f"dependencies.{name}.capabilities")

    exceptions = _list(contract.get("exceptions"), "exceptions")
    seen_ids: set[str] = set()
    current_date = _utc_today() if today is None else today
    for index, raw_exception in enumerate(exceptions):
        context = f"exceptions[{index}]"
        exception = _mapping(raw_exception, context)
        exception_id = _string(exception.get("id"), f"{context}.id")
        if exception_id in seen_ids:
            raise DependencyPolicyError(f"duplicate dependency exception ID {exception_id!r}")
        seen_ids.add(exception_id)
        exception_dependency = _string(exception.get("dependency"), f"{context}.dependency")
        platform = _string(exception.get("platform"), f"{context}.platform")
        if platform not in SUPPORTED_PLATFORMS:
            raise DependencyPolicyError(f"{context}.platform must be one of {sorted(SUPPORTED_PLATFORMS)}")
        definition = _mapping(dependencies[exception_dependency], f"dependencies.{exception_dependency}")
        if platform not in _dependency_platforms(definition, f"dependencies.{exception_dependency}"):
            raise DependencyPolicyError(
                f"{context}.platform {platform!r} is not supported by dependency {exception_dependency!r}"
            )
        _version_tuple(_string(exception.get("allowedVersion"), f"{context}.allowedVersion"))
        for field in ("reason", "owner", "issue", "exitCondition"):
            _string(exception.get(field), f"{context}.{field}")
        created = _utc_date(exception.get("created"), f"{context}.created")
        expires = _utc_date(exception.get("expires"), f"{context}.expires")
        if expires < created:
            raise DependencyPolicyError(f"{context}.expires precedes its creation date")
        if (expires - created).days > 30:
            raise DependencyPolicyError(f"{context} exceeds the maximum 30-day exception window")
        if expires < current_date:
            raise DependencyPolicyError(f"dependency exception {exception_id!r} expired on {expires.isoformat()}")
    return contract


def load_contract(path: Path = CONTRACT_FILE, *, today: date | None = None) -> dict[str, Any]:
    return validate_contract(read_json(path), today=today)


def contract_sha256(path: Path = CONTRACT_FILE) -> str:
    try:
        return hashlib.sha256(path.read_bytes()).hexdigest()
    except OSError as exc:
        raise DependencyPolicyError(f"cannot hash dependency contract {path}: {exc}") from exc


def effective_policy(
    contract: dict[str, Any], dependency: str, platform: str, *, today: date | None = None
) -> EffectivePolicy:
    validate_contract(contract, today=today)
    if platform not in SUPPORTED_PLATFORMS:
        raise DependencyPolicyError(f"unsupported dependency platform {platform!r}")
    dependencies = _mapping(contract["dependencies"], "dependencies")
    if dependency not in dependencies:
        raise DependencyPolicyError(f"unknown governed dependency {dependency!r}")
    definition = _mapping(dependencies[dependency], f"dependencies.{dependency}")
    policy = _mapping(definition["policy"], f"dependencies.{dependency}.policy")
    kind = _string(policy["kind"], f"dependencies.{dependency}.policy.kind")
    if kind == "exact":
        result = EffectivePolicy(kind="exact", minimum=_string(policy["version"], "policy.version"))
    else:
        maximum = policy.get("exclusiveMaximum")
        result = EffectivePolicy(
            kind="range",
            minimum=_string(policy["minimum"], "policy.minimum"),
            exclusive_maximum=_string(maximum, "policy.exclusiveMaximum") if maximum is not None else None,
        )
    for raw_exception in _list(contract["exceptions"], "exceptions"):
        exception = _mapping(raw_exception, "exception")
        if exception["dependency"] == dependency and exception["platform"] == platform:
            return EffectivePolicy(
                kind="exact",
                minimum=_string(exception["allowedVersion"], "exception.allowedVersion"),
                exception_id=_string(exception["id"], "exception.id"),
            )
    return result


def parse_vcpkg_status(text: str) -> dict[str, VcpkgPackage]:
    identities: dict[str, tuple[str, str]] = {}
    pending_features: list[tuple[str, str, str, str | None, int | None]] = []

    def port_revision(value: str, name: str) -> int:
        if not re.fullmatch(r"[0-9]+", value):
            raise DependencyPolicyError(f"invalid vcpkg port revision for {name}: {value!r}")
        return int(value)

    for paragraph in re.split(r"\r?\n\r?\n", text.strip()):
        if not paragraph.strip():
            continue
        fields: dict[str, str] = {}
        revision_fields = []
        for line in paragraph.splitlines():
            if ": " in line:
                key, value = line.split(": ", maxsplit=1)
                fields[key] = value
                if key == "Port-Version":
                    revision_fields.append(value)
            elif line == "Port-Version:":
                revision_fields.append("")
        name = fields.get("Package")
        version = fields.get("Version")
        triplet = fields.get("Architecture")
        if not name or not triplet:
            continue
        # vcpkg keeps paragraphs for removed packages (e.g. "deinstall ok
        # not-installed"); only currently installed state is evidence.
        status = fields.get("Status", "")
        if status.split()[-1:] != ["installed"]:
            continue
        feature = fields.get("Feature", "core")
        if feature == "core" and not version:
            continue
        revision = port_revision(revision_fields[0], name) if revision_fields else None
        if version and "#" in version:
            version, _, embedded = version.partition("#")
            revision = port_revision(embedded, name)
        if feature != "core":
            # Feature records never supply a base identity. A bare legacy Version
            # checks upstream only; an explicit revision checks that component too.
            pending_features.append((name, triplet, feature, version or None, revision))
            continue
        if not version:
            continue
        identity = (f"{version}#{revision}" if revision else version, triplet)
        if name in identities and identities[name] != identity:
            raise DependencyPolicyError(f"vcpkg status contains conflicting identities for {name}")
        identities[name] = identity
    features_by_name = {name: {"core"} for name in identities}
    for name, triplet, feature, version, revision in pending_features:
        if name not in identities:
            continue
        base_version, base_triplet = identities[name]
        upstream, _, base_revision = base_version.partition("#")
        if (
            triplet != base_triplet
            or (version is not None and version != upstream)
            or (revision is not None and revision != (int(base_revision) if base_revision else 0))
        ):
            raise DependencyPolicyError(f"vcpkg status contains conflicting identities for {name}")
        features_by_name[name].add(feature)
    return {
        name: VcpkgPackage(
            version=identity[0],
            triplet=identity[1],
            features=tuple(sorted(features_by_name[name])),
        )
        for name, identity in identities.items()
    }


def _packages_config(path: Path) -> dict[str, str]:
    try:
        root = ET.parse(path).getroot()
    except (OSError, ET.ParseError) as exc:
        raise DependencyPolicyError(f"cannot read NuGet lock {path}: {exc}") from exc
    packages: dict[str, str] = {}
    for node in root.findall("package"):
        package_id = node.get("id")
        version = node.get("version")
        if not package_id or not version:
            raise DependencyPolicyError(f"NuGet lock {path} contains a package without id and version")
        if package_id.casefold() in (key.casefold() for key in packages):
            raise DependencyPolicyError(f"NuGet lock {path} contains duplicate package {package_id!r}")
        packages[package_id] = version
    return packages


def verify_nuget_package(package_root: Path, package_id: str, locked_version: str) -> Path:
    """Verify one restored archive's identity against its existing NuGet lock entry."""
    directory = package_root / f"{package_id}.{locked_version}"
    archives = tuple(directory.glob("*.nupkg"))
    if len(archives) != 1:
        raise DependencyPolicyError(f"restored NuGet package {package_id} lacks one package archive in {directory}")
    try:
        with zipfile.ZipFile(archives[0]) as archive:
            nuspec_names = [entry for entry in archive.namelist() if entry.lower().endswith(".nuspec")]
            if len(nuspec_names) != 1:
                raise DependencyPolicyError(f"NuGet package {archives[0]} lacks one nuspec")
            nuspec = ET.fromstring(archive.read(nuspec_names[0]))
    except (OSError, zipfile.BadZipFile, ET.ParseError) as exc:
        raise DependencyPolicyError(f"cannot inspect restored NuGet package {archives[0]}: {exc}") from exc
    metadata = next((node for node in nuspec.iter() if node.tag.endswith("metadata")), None)
    metadata_children = () if metadata is None else metadata
    identity = {
        child.tag.rsplit("}", maxsplit=1)[-1]: child.text
        for child in metadata_children
        if child.tag.endswith(("id", "version"))
    }
    identity_version = identity.get("version")
    try:
        version_matches = isinstance(identity_version, str) and _normalized_nuget_version(
            identity_version
        ) == _normalized_nuget_version(locked_version)
    except DependencyPolicyError:
        version_matches = False
    if str(identity.get("id") or "").casefold() != package_id.casefold() or not version_matches:
        raise DependencyPolicyError(f"restored NuGet identity does not match {package_id} {locked_version}")
    return archives[0]
