---
name: write-documentation
description: Create, reorganize, split, or migrate Aobus documentation under the user, architecture, specification, reference, development, decision, and RFC taxonomy. Use when adding durable Markdown, documenting behavior or formats, writing an ADR/RFC, or changing documentation structure and ownership.
---

# Write Aobus documentation

Human documentation owns project policy and product truth; keep this skill as workflow only.

## Required reading

Read these authorities completely before acting:

- `doc/README.md` for taxonomy, lifecycle, formatting, migration, and code-boundary policy;
- the selected file under `doc/template/`;
- the nearest index;
- `doc/architecture/system-overview.md` and the owning subsystem architecture when creating or changing architecture, specification, or reference code boundaries.

Read the relevant implementation, tests, legacy documents, and inbound links needed to verify the facts being changed.

## Create or migrate

1. Apply the owner-selection table in `doc/README.md`; state technical assumptions and choose one fact owner before editing.
2. For a mixed or legacy source, use the fact-ledger contract in `doc/README.md` before deciding which documents to create.
3. Copy the matching template, replace every placeholder, and follow its section ownership.
4. Align architecture, specification, and reference code-boundary statements with the system overview and owning subsystem architecture; link those authorities instead of copying their layer definitions.
5. Verify current claims against code and tests, delegate facts owned by other document types, and add stable implementation and test maps.
6. When architecture coverage changes, update the landscape role, relationship, and coverage tables in the same change.
7. Update the nearest index and every inbound link.
8. During legacy migration, remove the superseded legacy document only after every fact has a new owner in the same change.
9. Run `./ao docs check` and the implementation validation appropriate to any code changed.

Do not link tracked documents to `doc/plan/`.
Do not create redirect stubs or compatibility copies unless the user explicitly requires an external stable URL.
Do not run format or lint tools unless the user explicitly asks for them.

## Decisions and RFCs

Apply the lifecycle in `doc/README.md` and the selected decision or RFC template.
For an RFC, classify direct relationships through the dependency contract in `doc/README.md` and keep `depends-on` aligned with the fixed `Dependencies` section.
When an RFC is accepted or implemented, update its status and link the current authorities it produced; never leave it as the only description of current behavior.

## Handoff

Report:

- document type and authority chosen;
- facts moved, split, or deliberately delegated;
- legacy documents removed or still pending migration;
- indexes and links updated;
- validation commands and results.
