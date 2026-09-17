---
name: write-documentation
description: Create, refresh, or rewrite Aobus documentation for its reader role, including navigation, topic boundaries, design notes, and decision records.
---

# Write Aobus documentation

Use `doc/development/documentation.md` for file boundaries, evidence, proposals,
and validation. `doc/README.md` is the reader entrance; `doc/system/README.md`
routes current product contracts by topic.
For a small correction, read the affected claim, its factual source, and the
relevant policy rather than every neighboring document.

Choose the reader's question and the page's role before drafting, using the
roles in `doc/development/documentation.md`. Make the body serve that question.
Rewrite when needed, but preserve accurate, useful content rather than chasing
uniformity. Choose file boundaries by independently useful reader questions
and contracts; structure and behavior may share a page.

Verify changed factual claims and examples against current source, tests,
assets, tools, or policy. Keep observed implementation, intended contract,
and decision rationale distinct; report conflicts rather than silently
turning a possible implementation defect into a weaker guarantee. Preserve
independently useful compatibility and safety contracts, and link headers
or source assets instead of copying declarations or message inventories.

Before removing material in a structural migration, keep temporary fact and
inbound-link evidence outside the repository. Preserve unique facts or explain
why they are incorrect, obsolete, or proposal-only. Update links in documents,
repository instructions, skills, and tooling consumers.
Use `doc/system/overview.md` and the affected topic for code boundaries.
Distinguish current behavior, proposals, and decision rationale. Check decision
status and current-contract links separately from the recorded reasons.
Create a decision only when its rationale has independent long-term value.

Run `./ao docs check`; it does not establish freshness or reader-role fit.
Re-read the completed page for its task, factual evidence, and removed facts.
Try representative reading paths when changing navigation or task routing.
Additional completion validation follows
`doc/development/test/validation-and-review.md`; report unresolved migration
facts or validation boundaries.
