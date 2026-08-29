"""Frozen concept-report scope: roots, exclusions, overload treatment, and hop chains.

The report measures public declarations from one configured debug build. This
module is the denominator contract: changing a field here is a measurement
change, not a silent implementation detail.
"""

from __future__ import annotations

from dataclasses import dataclass
from pathlib import Path

from .paths import PROJECT_ROOT

SCHEMA_VERSION = 3
REPORT_NAME = "concept-report.json"

CORE_ROOTS: tuple[str, ...] = ("include/ao",)
APPLICATION_ROOTS: tuple[str, ...] = ("app/include/ao",)
HEADER_SUFFIXES: tuple[str, ...] = (".h", ".hh", ".hpp", ".hxx")

# Compiler-generated entities never enter the public-declaration total. Public
# callables are counted symmetrically: moving a method to a free function must
# not change the denominator merely because its owner spelling changed.
EXCLUSIONS: tuple[str, ...] = (
    "implicit declarations, including injected class names",
    "anonymous namespaces and function-local types",
    "enumerators; the enum itself is one declaration",
    "non-public class and struct members, fields, and implicit special members",
    "template pattern children of ClassTemplateDecl and FunctionTemplateDecl; the template is one declaration",
    "forward declarations of a name that also has a complete definition in the scanned set",
)

# Each overload is one callable declaration, keyed by qualified name plus
# canonical type. Changing this would move the Phase 0 denominator.
OVERLOAD_TREATMENT = "each public callable overload, free function or explicit member, is one declaration"
DEPENDENCY_EDGE_TREATMENT = (
    "one edge per source overload and target public type; a member callable has an owner edge so member/free "
    "spelling is symmetric"
)

ROLE_SUFFIXES: tuple[str, ...] = (
    "Coordinator",
    "Presentation",
    "Projection",
    "Recommender",
    "Controller",
    "Formatter",
    "Lifecycle",
    "Dependencies",
    "Resolver",
    "Provider",
    "Registry",
    "Session",
    "Service",
    "Adapter",
    "Builder",
    "Catalog",
    "Context",
    "Parser",
    "Policy",
    "Reader",
    "Writer",
    "Cache",
    "Lease",
    "Model",
    "Store",
)


@dataclass(frozen=True)
class ConstructionChain:
    """One frontend leaf's construction path, counted as crossed API boundaries."""

    leaf: str
    path: str
    steps: tuple[str, ...]

    @property
    def hops(self) -> int:
        return len(self.steps)


# Keep these named leaves stable so structural changes compare the same paths.
# hops is the number of API boundaries after the leaf's translation unit.
CONSTRUCTION_CHAINS: tuple[ConstructionChain, ...] = (
    ConstructionChain(
        leaf="gtk.playback.seekSlider",
        path="app/linux-gtk/layout/component/playback/SeekSliderComponent.cpp",
        steps=(
            "registerSeekSliderComponent(ComponentRegistry&, PlaybackService&)",
            "SeekSliderComponent(PlaybackService&)",
            "SeekControlWidget(PlaybackService)",
        ),
    ),
    ConstructionChain(
        leaf="rt.ResourceByteMemoryCache",
        path="app/include/ao/rt/resource/ResourceByteMemoryCache.h",
        steps=("ResourceByteMemoryCache(async::Runtime&, ReadBytes)",),
    ),
    ConstructionChain(
        leaf="uimodel.ActivityStatusFeedProjection",
        path="app/uimodel/status/activity/ActivityStatusFeedProjection.h",
        steps=("ActivityStatusFeedProjection(MessageCatalog, NotificationFeedState)",),
    ),
    ConstructionChain(
        leaf="winui.TrackListController",
        path="app/windows-winui/track/TrackListController.h",
        steps=("TrackListController(AppRuntime&, TrackColumnLayouts&, MessageCatalog)",),
    ),
    ConstructionChain(
        leaf="winui.SmtcBridge",
        path="app/windows-winui/platform/SmtcBridge.h",
        steps=("SmtcBridge(HWND, DispatcherQueue, AppRuntime&, PlaybackActions&, ResourceByteMemoryCache&)",),
    ),
)


def public_headers(root: Path = PROJECT_ROOT) -> dict[str, tuple[Path, ...]]:
    """Return sorted self-contained public headers for each measured area."""
    return {
        "core": _headers_under(root, CORE_ROOTS),
        "application": _headers_under(root, APPLICATION_ROOTS),
    }


def validate_construction_chains(root: Path = PROJECT_ROOT) -> list[str]:
    """Return missing catalog paths; an empty list means the seed set is intact."""
    missing: list[str] = []
    for chain in CONSTRUCTION_CHAINS:
        if not (root / chain.path).is_file():
            missing.append(chain.path)
    return missing


def scope_manifest() -> dict[str, object]:
    """Machine-readable denominator contract copied into every report."""
    return {
        "schemaVersion": SCHEMA_VERSION,
        "coreRoots": list(CORE_ROOTS),
        "applicationRoots": list(APPLICATION_ROOTS),
        "headerSuffixes": list(HEADER_SUFFIXES),
        "exclusions": list(EXCLUSIONS),
        "overloadTreatment": OVERLOAD_TREATMENT,
        "dependencyEdgeTreatment": DEPENDENCY_EDGE_TREATMENT,
        "dedup": "kind plus qualified name; callable declarations also include canonical type",
        "includeWeight": (
            "distinct project headers that appear in the Clang AST location graph "
            "of the header parsed as a translation unit, including the header itself, "
            "with total bytes of those files"
        ),
        "rebuildFanOut": (
            "translation units recorded by ninja -t deps that list the header as a "
            "dependency; median and 95th percentile across the area"
        ),
        "constructionHops": (
            "frozen frontend leaf chains; each step is one crossed API boundary; "
            "the per-change review updates the named chain rather than an aggregate"
        ),
        "headerIncludes": "reported separately from dependency edges and never substituted for them",
    }


def _headers_under(root: Path, relative_roots: tuple[str, ...]) -> tuple[Path, ...]:
    files: list[Path] = []
    for relative in relative_roots:
        base = root / relative
        if not base.is_dir():
            continue
        for path in sorted(base.rglob("*")):
            if path.is_file() and path.suffix in HEADER_SUFFIXES:
                files.append(path)
    return tuple(files)
