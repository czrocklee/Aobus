"""Global document semantics and the separately usable Markdown inventory."""

from __future__ import annotations

import sys

from ao.core.paths import PROJECT_ROOT
from ao.portal.source_scope import SourceScope


def markdown_scope() -> SourceScope:
    """Discover the global documentation format scope without native SDK work."""
    from ao.portal import doc_check

    return SourceScope(tuple(path.relative_to(PROJECT_ROOT).as_posix() for path in doc_check.discover_markdown()))


def check() -> int:
    """Check global document semantics only; never run a formatter."""
    from ao.portal import doc_check

    issues = doc_check.check_tree()
    for issue in issues:
        print(issue.format(), file=sys.stderr)
    if issues:
        print(f"Documentation check failed: {len(issues)} issue(s).", file=sys.stderr)
        return 1
    print(f"Documentation check passed ({len(doc_check.discover_markdown())} Markdown files).")
    return 0
