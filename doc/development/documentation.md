# Writing and maintaining documentation

Organize knowledge around a reader's task or question, not a required document type.
The [documentation home](../README.md) is the reading entrance; this page is only for authors and reviewers.

## File boundaries

- `user/` explains product tasks; `development/` explains contribution and maintenance tasks.
- `system/` groups current structure and behavioral contracts by topic. A page may explain ownership, behavior, and relevant values together.
- `reference/` provides independently useful lookup material: languages, commands, protocols, formats, compatibility rules, and semantic tables.
- `decision/` preserves consequential choices and their historical reasons, not current behavior.

Extend an existing page when it answers the same reader question.
Split only for independent lookup, readers, or evolution of a substantial contract; neither crossing code layers nor mixing explanation and specification requires separate files.
A short contract can remain a short file. Related complex contracts need not become one manual.
Use a topic's introduction to explain its model and route readers to the needed detail, not to register classifications, coverage matrices, or every symbol.
The [system overview](../system/overview.md) defines the shared code-layer model; topic pages refine rather than redefine it.

## Facts and evidence

Give each detailed rule a clear source. Brief explanatory summaries may repeat it and link there; do not maintain two exhaustive copies.
Headers own declarations and local API preconditions. Documentation retains cross-call and cross-object semantics, compatibility, and reasoning that declarations cannot convey.
An internal C++ interface can still deserve a contract: PCM byte alignment, exception containment, transactions, cancellation, observer reentrancy, and teardown are not disposable API inventories.
Use source assets or schemas for mechanical lists; generate a view only when a real reader needs one. Do not maintain a second hand-written copy of catalog messages, constructor parameters, or ordinary member defaults.

Check changed claims against the relevant code and tests. A disagreement may be an implementation bug: do not silently rewrite a normative contract to match code.
Name the relevant implementation and tests where they help readers verify a contract; there is no mandatory exhaustive map for every page.
When restructuring several owners, keep a temporary fact ledger outside the repository recording the source fact, destination, evidence, affected links, and reason for deletion or consolidation.
This is migration evidence, not a permanent registry or a requirement for ordinary edits.
Preserve unique negative constraints as well as positive guarantees.

## Writing a page

Use English and descriptive headings; choose their number and order to fit the subject.
State the scope near the beginning and distinguish explanation from requirements with clear wording such as `must`, `never`, and `may` only for real contracts.
There is no mandatory template or required section sequence.
Useful questions, not a form to fill:

- For a task: what must the reader have, what do they do, and how do they verify success? Branch only the platform-specific steps.
- For a mechanism: who owns state, what crosses a boundary, what can fail, and what happens during cancellation or destruction?
- For a format or language: what is accepted, what does it mean, and which compatibility or rejection rules apply?
- For a decision: what evidence, alternatives, and consequences explain the choice at the time?

Use present tense for current behavior. Keep exact examples valid, use language labels on fenced blocks, and prefer semantic line breaks.
Link to a topic from its relevant task or neighboring page; reaching it through a topic body is sufficient, with no separate same-category index registration.
Update inbound links and referenced anchors when moving or consolidating material, including repository instructions, skills, and tooling references.
Do not leave a second current copy at the old location merely to preserve navigation.

## Proposals and decisions

Ordinary changes need no design document. Use an issue, PR, local plan, or a tracked design note when a significant uncertainty benefits from written comparison.
There is no required RFC workflow, numbering, dependency graph, prescribed proposal structure, or automatic ADR requirement.
Make an unimplemented proposal visibly non-current and link the actual baseline; being tracked or reviewed does not make it implemented.

After implementation, update the affected current contracts and keep only independently useful rationale in a decision or the topic itself; avoid retaining a proposal as a second current description.
For an abandoned proposal, preserve useful reasons and repair inbound links before removing it.
Do not invent historical decisions from code alone. When a decision is superseded, make that explicit and link its replacement or current contract without rewriting what was decided at the time.
Tracked documents must not link into the ignored local `doc/plan/` tree.

## Metadata

Front matter is optional. Preserve an existing `id` when moving a document; when consolidating documents, retire redundant ids after updating their consumers rather than creating an alias registry.
An id, when present, remains unique and uses lowercase words separated by dots or hyphens.
No `type`, `domain`, `summary`, or `status: current` field is required.
For historical or proposed material, make its status clear to readers in the body; optional `status` metadata does not replace that explanation.
The checker accepts flat scalar metadata and checks supplied ids and status values, not a type-specific lifecycle or directory taxonomy.

## Validation

Run `./ao docs check` from the repository root.
It checks optional metadata integrity, ids, links, Markdown anchors, reference-style links, forbidden local-plan links, and reachability from the documentation home.
It also checks the [naming tooling contract](lint/naming-checks.md) against the audit vocabulary and registered lint checks.
It does not require templates, fixed headings, architecture portfolios, RFC dependency declarations, or a particular directory classification.

Mechanical success does not prove factual agreement. Review the moved facts and try representative reader tasks, especially for transactions, persistence, cancellation, and lifetime guarantees.
Changes to validators or other code additionally follow [completion validation](test/validation-and-review.md).
Documentation simplification does not relax code architecture audits, lint rules, or native correctness gates.
