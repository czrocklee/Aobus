"""Test-only views of the documentation parser's production fact snapshot."""

from ao.portal import doc_check


def parse_links(lines, *, include_images=True):
    facts = doc_check._parse_markdown(lines)
    return (
        [(line, target) for line, target, image in facts.links if include_images or not image],
        list(facts.undefined_references),
    )
