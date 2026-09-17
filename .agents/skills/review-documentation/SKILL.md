---
name: review-documentation
description: Review Aobus documentation for freshness, reader-role fit, duplication, navigation, and lost knowledge, including whole-tree audits and structural migrations.
---

# Review Aobus documentation

Review is read-only unless fixes are requested. Establish the requested scope.
Use `doc/development/documentation.md` for reader roles, evidence, and file
boundaries; `doc/system/overview.md` and the affected topic explain code boundaries.

For each scoped page, identify the reader question and check both freshness and
role fit. Read a whole document when reviewing its content, not only headings,
search matches, or changed lines. Verify behavior, commands, defaults, examples,
platform support, and ownership claims against relevant source, tests, assets,
or current policy. A recent edit, valid link, or passing docs check proves none
of those claims. Distinguish source inspection from executed or native UI evidence.

Assess a decision's status and current-contract links separately from its
recorded rationale. Do not infer that rationale from present code. Report
conflicts between a normative contract and implementation rather than silently
weakening the contract to match a possible code defect.

Prioritize incorrect behavior, role mismatch, duplicated detailed rules,
misleading proposals, broken reading paths, and lost migration facts. A finding
identifies evidence and reader impact; preference-only rewrites are not defects.
Judge file boundaries by reader questions and independently useful contracts;
one topic may combine structure, behavior, and reference values.
Read each claim in its enclosing heading and consumer scope before applying it
elsewhere. Verify external review claims against the current checkout and
distinguish documentation conflict from inconsistent product behavior.

For an explicitly exhaustive review, inventory every in-scope page and keep a
temporary per-page record outside the repository: role, evidence, findings or
no finding, and unresolved verification limits. Do not substitute sampling or
an unchanged-body comparison for freshness review. Report unreviewed pages;
do not mark them complete because another page covers the same subsystem.
Ordinary targeted reviews do not need a whole-tree inventory.

For restructuring, inspect the temporary fact ledger and changed inbound links,
then follow representative tasks from the reader entrance. Protect transaction,
cancellation, observer reentrancy, compatibility, and teardown guarantees even
when simple internal API inventories are removed.
Run `./ao docs check`; its success does not prove factual agreement.
Completion and result reuse follow
`doc/development/test/validation-and-review.md`.
