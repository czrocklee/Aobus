---
name: review-documentation
description: Audit Aobus Markdown docs for factual freshness, reader-question fit, and contract-vs-code drift. Do not load for authoring/editing docs without an audit request, or for agent instructions.
---

# Review Aobus documentation

Review is read-only unless fixes are requested. Route directory boundaries,
evidence, decisions, migration rules, and
[`./ao docs check` discovery limits](../../../doc/development/documentation.md#validation)
through [documentation maintenance](../../../doc/development/documentation.md),
and code-layer boundaries through [system overview](../../../doc/system/overview.md).

## Audit discipline

- Read each in-scope document in full within its heading and consumer scope,
  verifying claims, commands, defaults, examples, and external review notes
  against live source, tests, assets, or policy; distinguish source inspection
  from executed or native UI evidence.
- Prioritize wrong behavior, reader-question or directory mismatch, duplicated
  rules, unmarked proposals, broken navigation, and lost safety contracts;
  style-only preferences are not defects.
- For an explicitly exhaustive audit, maintain a temporary ledger outside the
  checkout covering every in-scope page (never substitute sampling or
  unchanged-body skips, and list unreviewed pages).
- Remember `./ao docs check` skips Markdown under `lib/`, `test/`, `bazel/`,
  and `script/` and never link-validates inline-code paths
  ([validation](../../../doc/development/documentation.md#validation)).
