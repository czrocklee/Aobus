---
name: write-docs
description: Write, update, move, review, or audit Aobus documentation, including docs in code changes and mixed PR reviews. Use create-skill for AGENTS.md and agent skills. Skip for code-only tasks or merely running existing checks.
---

# Write and review Aobus documentation

Use the [documentation guide](../../../doc/development/documentation.md) for
both writing and review. When adding or moving material, use its
[file boundaries](../../../doc/development/documentation.md#file-boundaries) to
choose the owning page and its [validation instructions](../../../doc/development/documentation.md#validation)
to check links and navigation. For code-layer boundaries, see the
[system overview](../../../doc/system/overview.md).

## Reviewing documentation

Review is read-only unless fixes are requested.

- Read each in-scope document in full within its heading and consumer scope,
  verifying claims, commands, defaults, examples, and external review notes
  against live source, tests, assets, or policy; distinguish source inspection
  from executed or native UI evidence.
- Prioritize wrong behavior, reader-question or directory mismatch, duplicated
  rules, unmarked proposals, broken navigation, and lost safety contracts;
  style-only preferences are not defects.
- Audit changed prose against the [current-state and reader-language rules](../../../doc/development/documentation.md#writing-a-page).
  Read both sides of the diff: for each removed guarantee, prerequisite, or
  exception, including a goal phrased as intended or documented behavior, show
  where the revised docs preserve it or give evidence for its removal. A
  limitation note does not replace the guarantee it qualifies. Word lists only
  find candidates; _existing_ or _unchanged_ can state a precondition or cache
  behavior, so judge each sentence by meaning.
- For a multi-commit series, inspect each commit's own tree, not only the final
  one, and confirm it delivers what its message claims.
- In user tasks, verify interface labels against the
  [root catalog](../../../app/i18n/catalog/root.txt) and the frontend code that
  places them, such as menu builders.
- For an explicitly exhaustive audit, maintain a temporary ledger outside the
  checkout covering every in-scope page (never substitute sampling or
  unchanged-body skips, and list unreviewed pages).
- Remember `./ao check docs` skips Markdown under `lib/`, `test/`, `bazel/`,
  and `script/` and never link-validates inline-code paths
  ([validation](../../../doc/development/documentation.md#validation)).
