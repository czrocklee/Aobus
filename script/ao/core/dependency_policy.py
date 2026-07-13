"""Dependency-contract validation and native resolution reporting."""

from __future__ import annotations

import hashlib
import json
import re
from dataclasses import dataclass
from datetime import UTC, date, datetime
from pathlib import Path
from typing import Any

from .paths import PROJECT_ROOT

CONTRACT_FILE = PROJECT_ROOT / "dependency-contract.json"
BUILD_REPORT_NAME = "aobus-dependencies.json"
SUPPORTED_SCHEMA = 1
SUPPORTED_PLATFORMS = frozenset({"linux", "windows"})
SUPPORTED_BUILD_CONDITIONS = frozenset({"AOBUS_BUILD_TUI"})
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


def _version_tuple(value: str) -> tuple[int, ...]:
    if not _VERSION_RE.fullmatch(value):
        raise DependencyPolicyError(f"unsupported governed version {value!r}; expected dotted numeric components")
    return tuple(int(component) for component in value.split("."))


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
    if contract.get("schemaVersion") != SUPPORTED_SCHEMA:
        raise DependencyPolicyError(
            f"unsupported dependency contract schema {contract.get('schemaVersion')!r}; expected {SUPPORTED_SCHEMA}"
        )
    if contract.get("leadResolver") != "nix":
        raise DependencyPolicyError("dependency contract leadResolver must be 'nix'")

    dependencies = _mapping(contract.get("dependencies"), "dependencies")
    if not dependencies:
        raise DependencyPolicyError("dependencies must not be empty")
    for name, raw_dependency in dependencies.items():
        _string(name, "dependency key")
        dependency = _mapping(raw_dependency, f"dependencies.{name}")
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

        nix = _mapping(dependency.get("nix"), f"dependencies.{name}.nix")
        _string(nix.get("attribute"), f"dependencies.{name}.nix.attribute")
        vcpkg = _mapping(dependency.get("vcpkg"), f"dependencies.{name}.vcpkg")
        _string(vcpkg.get("strategy"), f"dependencies.{name}.vcpkg.strategy")
        _string_list(vcpkg.get("ports"), f"dependencies.{name}.vcpkg.ports")
        cmake = _mapping(dependency.get("cmake"), f"dependencies.{name}.cmake")
        _string(cmake.get("package"), f"dependencies.{name}.cmake.package")
        _string_list(cmake.get("targets"), f"dependencies.{name}.cmake.targets")
        if condition := cmake.get("requiredWhen"):
            condition_name = _string(condition, f"dependencies.{name}.cmake.requiredWhen")
            if condition_name not in SUPPORTED_BUILD_CONDITIONS:
                raise DependencyPolicyError(
                    f"dependencies.{name}.cmake.requiredWhen names unsupported option {condition_name!r}"
                )
        if "capabilities" in dependency:
            _string_list(dependency["capabilities"], f"dependencies.{name}.capabilities")

    exceptions = _list(contract.get("exceptions"), "exceptions")
    seen_ids: set[str] = set()
    seen_targets: set[tuple[str, str]] = set()
    current_date = _utc_today() if today is None else today
    for index, raw_exception in enumerate(exceptions):
        context = f"exceptions[{index}]"
        exception = _mapping(raw_exception, context)
        exception_id = _string(exception.get("id"), f"{context}.id")
        if exception_id in seen_ids:
            raise DependencyPolicyError(f"duplicate dependency exception ID {exception_id!r}")
        seen_ids.add(exception_id)
        exception_dependency = _string(exception.get("dependency"), f"{context}.dependency")
        if exception_dependency not in dependencies:
            raise DependencyPolicyError(f"{context} refers to unknown dependency {exception_dependency!r}")
        platform = _string(exception.get("platform"), f"{context}.platform")
        if platform not in SUPPORTED_PLATFORMS:
            raise DependencyPolicyError(f"{context}.platform must be one of {sorted(SUPPORTED_PLATFORMS)}")
        target = (exception_dependency, platform)
        if target in seen_targets:
            raise DependencyPolicyError(f"multiple active exceptions target {exception_dependency!r} on {platform!r}")
        seen_targets.add(target)
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


def upstream_vcpkg_version(version: str) -> str:
    return version.split("#", maxsplit=1)[0]


def parse_vcpkg_status(text: str) -> dict[str, VcpkgPackage]:
    identities: dict[str, tuple[str, str]] = {}
    features_by_name: dict[str, set[str]] = {}
    for paragraph in re.split(r"\r?\n\r?\n", text.strip()):
        if not paragraph.strip():
            continue
        fields: dict[str, str] = {}
        for line in paragraph.splitlines():
            if ": " in line:
                key, value = line.split(": ", maxsplit=1)
                fields[key] = value
        name = fields.get("Package")
        version = fields.get("Version")
        triplet = fields.get("Architecture")
        if not name or not version or not triplet:
            continue
        # vcpkg keeps paragraphs for removed packages (e.g. "deinstall ok
        # not-installed"); only currently installed state is evidence.
        status = fields.get("Status", "")
        if status.split()[-1:] != ["installed"]:
            continue
        identity = (version, triplet)
        if name in identities and identities[name] != identity:
            raise DependencyPolicyError(f"vcpkg status contains conflicting identities for {name}")
        identities[name] = identity
        feature = fields.get("Feature", "core")
        features_by_name.setdefault(name, set()).add(feature)
    return {
        name: VcpkgPackage(
            version=identity[0],
            triplet=identity[1],
            features=tuple(sorted(features_by_name[name])),
        )
        for name, identity in identities.items()
    }


def _sha256(path: Path) -> str:
    try:
        return hashlib.sha256(path.read_bytes()).hexdigest()
    except OSError as exc:
        raise DependencyPolicyError(f"cannot hash dependency input {path}: {exc}") from exc


def _verify_true_map(actual: object, expected_names: list[str], context: str) -> None:
    values = _mapping(actual, context)
    for name in expected_names:
        if values.get(name) is not True:
            raise DependencyPolicyError(f"{context}.{name} was not verified")


def verify_build_report(
    contract: dict[str, Any],
    report: dict[str, Any],
    *,
    contract_path: Path = CONTRACT_FILE,
    project_root: Path = PROJECT_ROOT,
    today: date | None = None,
) -> None:
    validate_contract(contract, today=today)
    if report.get("schemaVersion") != SUPPORTED_SCHEMA:
        raise DependencyPolicyError(f"unsupported dependency report schema {report.get('schemaVersion')!r}")
    report_contract = _mapping(report.get("contract"), "report.contract")
    if report_contract.get("schemaVersion") != contract["schemaVersion"]:
        raise DependencyPolicyError("dependency report contract schema does not match the current contract")
    expected_hash = contract_sha256(contract_path)
    if report_contract.get("sha256") != expected_hash:
        raise DependencyPolicyError(
            "dependency report is stale: its contract hash does not match dependency-contract.json; rebuild first"
        )
    host = _mapping(report.get("host"), "report.host")
    platform = _string(host.get("platform"), "report.host.platform")
    if platform not in SUPPORTED_PLATFORMS:
        raise DependencyPolicyError(f"report has unsupported host platform {platform!r}")
    for filename, field in (
        ("vcpkg-configuration.json", "vcpkgConfigurationSha256"),
        ("vcpkg.json", "vcpkgManifestSha256"),
    ):
        expected = _sha256(project_root / filename)
        if host.get(field) != expected:
            raise DependencyPolicyError(f"dependency report is stale: {filename} changed; rebuild first")

    dependencies = _mapping(contract["dependencies"], "dependencies")
    reported = _mapping(report.get("dependencies"), "report.dependencies")
    for name, raw_definition in dependencies.items():
        definition = _mapping(raw_definition, f"dependencies.{name}")
        if name not in reported:
            raise DependencyPolicyError(f"dependency report is missing governed dependency {name!r}")
        entry = _mapping(reported[name], f"report.dependencies.{name}")
        condition = _mapping(definition["cmake"], f"dependencies.{name}.cmake").get("requiredWhen")
        status = entry.get("status")
        if status == "not-applicable":
            if condition is None:
                raise DependencyPolicyError(f"unconditional governed dependency {name!r} is not applicable")
            condition_report = _mapping(entry.get("condition"), f"report.dependencies.{name}.condition")
            if condition_report != {"name": condition, "value": False}:
                raise DependencyPolicyError(f"dependency {name!r} lacks proof that {condition} was disabled")
            continue
        if status != "verified":
            raise DependencyPolicyError(f"governed dependency {name!r} has invalid report status {status!r}")
        if condition is not None:
            condition_report = _mapping(entry.get("condition"), f"report.dependencies.{name}.condition")
            if condition_report != {"name": condition, "value": True}:
                raise DependencyPolicyError(f"dependency {name!r} lacks proof that {condition} was enabled")
        policy = effective_policy(contract, name, platform, today=today)
        if entry.get("requestedVersion") != policy.requested:
            raise DependencyPolicyError(
                f"{name}: report requested {entry.get('requestedVersion')!r}, policy requires {policy.requested!r}"
            )
        resolved = _string(entry.get("resolvedVersion"), f"report.dependencies.{name}.resolvedVersion")
        if not policy.accepts(resolved):
            raise DependencyPolicyError(
                f"{name}: expected {policy.requested}, resolved {resolved} on {platform}; "
                "see doc/development/dependency-upgrade.md"
            )
        if entry.get("exceptionId") != policy.exception_id:
            raise DependencyPolicyError(f"{name}: report exception does not match the effective platform policy")
        cmake = _mapping(definition["cmake"], f"dependencies.{name}.cmake")
        _verify_true_map(entry.get("targets"), _string_list(cmake["targets"], "cmake.targets"), f"{name}.targets")
        capabilities = definition.get("capabilities", [])
        if capabilities:
            _verify_true_map(
                entry.get("capabilities"),
                _string_list(capabilities, f"dependencies.{name}.capabilities"),
                f"{name}.capabilities",
            )


def load_build_report(build_dir: Path) -> dict[str, Any]:
    path = build_dir / BUILD_REPORT_NAME
    if not path.is_file():
        raise DependencyPolicyError(f"dependency report not found at {path}; run ./ao build (or ao.bat build) first")
    return read_json(path)


def _verify_nix_resolution(
    contract: dict[str, Any], report: dict[str, Any], project_root: Path, *, today: date | None = None
) -> dict[str, Any]:
    host = _mapping(report["host"], "report.host")
    path_value = _string(host.get("nixReportPath"), "report.host.nixReportPath")
    nix_report = read_json(Path(path_value))
    if nix_report.get("schemaVersion") != SUPPORTED_SCHEMA:
        raise DependencyPolicyError("unsupported Nix dependency report schema")
    nixpkgs = read_json(project_root / "nixpkgs.json")
    if nix_report.get("nixpkgsRevision") != nixpkgs.get("rev"):
        raise DependencyPolicyError("Nix dependency report does not come from the current nixpkgs revision")
    resolved = _mapping(nix_report.get("dependencies"), "Nix report dependencies")
    for name in _mapping(contract["dependencies"], "dependencies"):
        package = _mapping(resolved.get(name), f"Nix report dependency {name}")
        policy = effective_policy(contract, name, "linux", today=today)
        version = _string(package.get("version"), f"Nix report dependency {name}.version")
        if not policy.accepts(version):
            raise DependencyPolicyError(f"Nix resolved {name} {version}; contract requires {policy.requested}")
        _string(package.get("storePath"), f"Nix report dependency {name}.storePath")
    return nix_report


def _verify_vcpkg_resolution(
    contract: dict[str, Any],
    report: dict[str, Any],
    build_dir: Path,
    project_root: Path = PROJECT_ROOT,
    *,
    today: date | None = None,
) -> dict[str, object]:
    host = _mapping(report["host"], "report.host")
    installed_value = host.get("vcpkgInstalledDir")
    installed = (
        Path(_string(installed_value, "report.host.vcpkgInstalledDir"))
        if installed_value
        else build_dir / "vcpkg_installed"
    )
    status_file = installed / "vcpkg" / "status"
    try:
        packages = parse_vcpkg_status(status_file.read_text(encoding="utf-8"))
    except OSError as exc:
        raise DependencyPolicyError(f"cannot read vcpkg package status {status_file}: {exc}") from exc
    expected_triplet = _string(host.get("vcpkgTriplet"), "report.host.vcpkgTriplet")
    result: dict[str, object] = {}
    for name, raw_definition in _mapping(contract["dependencies"], "dependencies").items():
        definition = _mapping(raw_definition, f"dependencies.{name}")
        policy = effective_policy(contract, name, "windows", today=today)
        ports = _string_list(_mapping(definition["vcpkg"], f"dependencies.{name}.vcpkg")["ports"], "vcpkg.ports")
        port_results: dict[str, object] = {}
        for port in ports:
            if port not in packages:
                raise DependencyPolicyError(f"vcpkg status is missing governed port {port!r} for {name}")
            package = packages[port]
            if package.triplet != expected_triplet:
                raise DependencyPolicyError(
                    f"vcpkg port {port} uses triplet {package.triplet}, expected {expected_triplet}"
                )
            if not policy.accepts(package.upstream_version):
                raise DependencyPolicyError(
                    f"vcpkg resolved {port} {package.version}; {name} requires {policy.requested}"
                )
            port_results[port] = {
                "version": package.version,
                "upstreamVersion": package.upstream_version,
                "features": list(package.features),
                "triplet": package.triplet,
            }
        result[name] = port_results

    boost_policy = effective_policy(contract, "boost", "windows", today=today)
    boost_family: dict[str, object] = {}
    for port, package in packages.items():
        if not port.startswith("boost-") or port == "boost-vcpkg-helpers":
            continue
        if package.triplet != expected_triplet:
            raise DependencyPolicyError(
                f"vcpkg Boost family port {port} uses triplet {package.triplet}, expected {expected_triplet}"
            )
        if not boost_policy.accepts(package.upstream_version):
            raise DependencyPolicyError(
                f"vcpkg Boost family is mixed: {port} is {package.version}, contract requires {boost_policy.requested}"
            )
        boost_family[port] = {
            "version": package.version,
            "upstreamVersion": package.upstream_version,
            "features": list(package.features),
            "triplet": package.triplet,
        }
    result["boost"] = boost_family

    recipe_helpers: dict[str, object] = {}
    for helper in ("vcpkg-boost", "boost-vcpkg-helpers"):
        if helper in packages:
            package = packages[helper]
            recipe_helpers[helper] = {
                "version": package.version,
                "features": list(package.features),
                "triplet": package.triplet,
            }
    result["recipeHelpers"] = recipe_helpers

    manifest = read_json(project_root / "vcpkg.json")
    direct_ports: set[str] = set()
    for raw_dependency in _list(manifest.get("dependencies"), "vcpkg manifest dependencies"):
        if isinstance(raw_dependency, str):
            direct_ports.add(raw_dependency)
        else:
            direct_ports.add(_string(_mapping(raw_dependency, "vcpkg dependency").get("name"), "vcpkg dependency.name"))
    for raw_feature in _mapping(manifest.get("features", {}), "vcpkg manifest features").values():
        feature = _mapping(raw_feature, "vcpkg feature")
        for raw_dependency in _list(feature.get("dependencies", []), "vcpkg feature dependencies"):
            if isinstance(raw_dependency, str):
                direct_ports.add(raw_dependency)
            else:
                direct_ports.add(
                    _string(_mapping(raw_dependency, "vcpkg dependency").get("name"), "vcpkg dependency.name")
                )
    governed_ports = {
        port
        for raw_definition in _mapping(contract["dependencies"], "dependencies").values()
        for port in _string_list(
            _mapping(_mapping(raw_definition, "dependency")["vcpkg"], "dependency.vcpkg")["ports"],
            "dependency.vcpkg.ports",
        )
    }
    monitor_only: dict[str, object] = {}
    for port in sorted(direct_ports - governed_ports):
        if port not in packages:
            continue
        package = packages[port]
        monitor_only[port] = {
            "version": package.version,
            "features": list(package.features),
            "triplet": package.triplet,
        }
    result["monitorOnly"] = monitor_only
    return result


def verified_report(
    build_dir: Path,
    *,
    contract_path: Path = CONTRACT_FILE,
    project_root: Path = PROJECT_ROOT,
    today: date | None = None,
) -> dict[str, Any]:
    contract = load_contract(contract_path, today=today)
    report = load_build_report(build_dir)
    verify_build_report(
        contract,
        report,
        contract_path=contract_path,
        project_root=project_root,
        today=today,
    )
    platform = _string(_mapping(report["host"], "report.host")["platform"], "report.host.platform")
    enriched = dict(report)
    if platform == "linux":
        enriched["nativeResolution"] = {
            "kind": "nix",
            "report": _verify_nix_resolution(contract, report, project_root, today=today),
        }
    else:
        host = _mapping(report["host"], "report.host")
        enriched["nativeResolution"] = {
            "kind": "vcpkg",
            "configuration": read_json(project_root / "vcpkg-configuration.json"),
            "packages": _verify_vcpkg_resolution(contract, report, build_dir, project_root, today=today),
            "triplet": host.get("vcpkgTriplet"),
            "vcpkgVersion": host.get("vcpkgVersion"),
        }
    return enriched


def format_summary(report: dict[str, Any]) -> str:
    host = _mapping(report["host"], "report.host")
    lines = [f"Aobus dependencies ({host['platform']})", "DEPENDENCY  STATUS          REQUESTED  RESOLVED"]
    for name, raw_entry in sorted(_mapping(report["dependencies"], "report.dependencies").items()):
        entry = _mapping(raw_entry, f"report.dependencies.{name}")
        resolved = entry.get("resolvedVersion") or "-"
        lines.append(f"{name:<11} {entry.get('status')!s:<15} {entry.get('requestedVersion')!s:<10} {resolved}")
    native = _mapping(report.get("nativeResolution"), "report.nativeResolution")
    lines.append(f"Resolver: {native.get('kind')}")
    return "\n".join(lines)
