"""Consumer routing and platform deferrals, shared by whole-graph and scoped lint."""

from collections.abc import Sequence
from pathlib import PurePosixPath


def component_parts(parent: tuple[str, ...]) -> tuple[str, ...] | None:
    """Map a public/private source directory to its actual implementation component."""
    if parent[:2] == ("include", "ao"):
        return ("lib", *parent[2:])
    if parent[:4] == ("app", "include", "ao", "rt"):
        return ("app", "runtime", *parent[4:])
    if parent[:3] == ("app", "include", "ao"):
        return ("app", *parent[3:])
    if len(parent) >= 5 and parent[0] == "app" and parent[2:4] == ("include", "ao"):
        return ("app", parent[1], *parent[5:])
    if parent and parent[0] in {"app", "lib", "test", "tool"}:
        return parent
    return None


def common_prefix(left: Sequence[str], right: Sequence[str]) -> int:
    """Return the length of the shared leading directory parts."""
    count = 0
    for first, second in zip(left, right, strict=False):
        if first != second:
            break
        count += 1
    return count


def consumer_rank(item: str, owner: str, source: str) -> tuple[bool, int, str, str]:
    """Rank one consumer: same component first, then the longest shared prefix."""
    item_component = component_parts(PurePosixPath(item).parent.parts)
    foreign = item_component is None or component_parts(PurePosixPath(source).parent.parts) != item_component
    return (
        foreign,
        -common_prefix(PurePosixPath(item).parts, PurePosixPath(source).parts),
        owner,
        source,
    )


NATIVE_PLATFORMS = ("linux", "macos", "windows")


def platform_incompatible(path: str, platform: str) -> bool:
    """Identify native ownership by OS, not by absence of a compile command.

    The platform is explicit so the portal's ambient profile and a declared
    graph action defer exactly the same paths. Callers own their
    unknown-platform contract: this policy refuses a platform it cannot
    classify.
    """
    if platform not in NATIVE_PLATFORMS:
        raise ValueError(f"unsupported native platform: {platform}")
    parts = tuple(part.casefold() for part in PurePosixPath(path).parts)
    joined = "/".join(parts)
    stem = PurePosixPath(path).stem.casefold()
    owned_by_windows = (
        parts[:2] in {("app", "windows"), ("app", "windows-winui")}
        or parts[:2] == ("test", "windows")
        or parts[:2] == ("test", "winui")
        or "wasapi" in joined
        or "win32" in joined
        or "winui" in joined
        or stem.endswith("windows")
        or joined == "tool/lint/aobusclangtidymain.cpp"
    )
    owned_by_linux = (
        parts[:2] == ("app", "linux-gtk")
        or parts[:5] == ("app", "include", "ao", "systemmedia", "linux")
        or parts[:3] == ("app", "systemmedia", "linux")
        or parts[:2] in {("test", "linux"), ("test", "linux-gtk")}
        or parts[:3] == ("test", "systemmedia", "linux")
        or "alsa" in joined
        or "pipewire" in joined
        or stem.endswith("linux")
    )
    owned_by_macos = (
        parts[:2] == ("app", "macos-appkit")
        or parts[:2] == ("test", "macos")
        or parts[:2] == ("test", "macos-appkit")
        or "coreaudio" in joined
        or "corefoundation" in joined
        or stem.endswith("macos")
    )
    if platform == "linux":
        return owned_by_windows or owned_by_macos
    if platform == "windows":
        return owned_by_linux or owned_by_macos or stem.endswith("posix")
    return owned_by_windows or owned_by_linux
