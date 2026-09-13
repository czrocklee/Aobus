---
name: review-documentation
description: Review Aobus documentation for factual drift, ownership, lifecycle, and navigation; includes structural migrations and RFCs.
---

# Review Aobus documentation

Review is read-only unless fixes are requested. Check affected claims against
their implementation or policy owner. `doc/README.md` owns taxonomy and lifecycle;
use its owner-selection, template, and fact-ledger rules for structural reviews.
For code-boundary claims, consult `doc/architecture/system-overview.md` and the
owning subsystem architecture.

Prioritize incorrect behavior, duplicated authority, lifecycle drift, and lost
migration facts. A finding identifies the conflicting evidence and its impact;
preference-only rewrites are not defects.
Read a claim in its enclosing heading and consumer scope before applying it to
another subsystem. Verify external review claims against the current checkout;
distinguish an actual documentation conflict from inconsistent product behavior.

Run `./ao docs check` for mechanical validation. Completion scope and result
reuse follow `doc/development/test/validation-and-review.md`.
