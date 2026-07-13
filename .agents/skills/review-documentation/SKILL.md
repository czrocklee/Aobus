---
name: review-documentation
description: Review Aobus documentation changes for factual correctness, single-owner authority, taxonomy placement, lifecycle state, navigation, and agreement with code and tests. Use when asked to review Markdown, documentation migrations, architecture/spec/reference splits, ADRs, RFCs, user or development guides, or the documentation system itself.
---

# Review Aobus documentation

Review is read-only unless the user also asks for fixes.

## Required reading

Read these authorities completely before reviewing:

- `doc/README.md` for taxonomy, lifecycle, formatting, migration, and code-boundary policy;
- the template corresponding to each affected document type;
- the nearest index;
- `doc/architecture/system-overview.md` and the owning subsystem architecture when reviewing architecture, specification, or reference code boundaries.

Read the implementation, tests, legacy authorities, and inbound links needed to verify affected claims.

## Review order

1. Verify every behavioral, command, schema, path, and version claim against current code or tests.
2. Apply the authority and owner-selection rules in `doc/README.md`; verify the document delegates facts owned elsewhere instead of duplicating them.
3. Verify code-boundary statements and implementation paths agree with the system overview, owning subsystem architecture, build graph, and include constraints.
4. Find duplicated facts that could drift, especially tables, state matrices, command lists, and serialized layouts.
5. For architecture changes, verify the landscape role, relationship, and coverage rows and confirm the focused-architecture threshold against the real ownership or lifetime graph.
6. Verify lifecycle truth against `doc/README.md`: proposals, historical rationale, and current authorities must not impersonate one another.
7. Verify every legacy fact-ledger row has one owner or a justified disposition before allowing source deletion.
8. Verify metadata, stable ids, singular directory names, directory placement, template structure, indexes, inbound links, and implementation/test maps.
9. Run `./ao docs check` for mechanical validation.

## Finding priorities

Lead with correctness findings, especially examples that can cause destructive or silently empty operations.
Then report contract/implementation drift, duplicate authority, wrong lifecycle or placement, broken navigation, and finally formatting inconsistencies.

For each finding, name the affected document and line, the contradicting authority, the user or maintainer impact, and the required correction.
Do not report preference-only rewrites as defects.

## Handoff

State the review scope and technical assumptions.
List findings by priority, followed by unresolved migration risks and validation performed.
If no findings remain, say so explicitly and identify any validation intentionally not run.
