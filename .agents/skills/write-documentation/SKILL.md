---
name: write-documentation
description: Create or revise Aobus documentation, including ownership changes, migrations, ADRs, and RFCs.
---

# Write Aobus documentation

`doc/README.md` owns taxonomy, lifecycle, templates, and migration policy.
For a small correction, read the affected claim, its fact owner, and applicable
policy. For a new document or structural change, use the owner-selection rules,
matching `doc/template/` template, and nearest index.

When moving or splitting legacy material, use the fact-ledger contract before
removing its source. Update affected inbound links and indexes. For code-boundary
claims, consult `doc/architecture/system-overview.md` and the owning subsystem
architecture. Apply the RFC/decision lifecycle in `doc/README.md`; keep current
behavior in its authoritative document and durable rationale in decisions.

Run `./ao docs check`. Additional completion validation follows
`doc/development/test/validation-and-review.md`. Report unresolved ownership or
migration gaps that affect the requested result.
