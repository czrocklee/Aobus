# Writing and maintaining documentation

Organize knowledge around a reader's task or question, not a required document type.
The [documentation home](../README.md) is the reading entrance; this page is only for authors and reviewers.

## File boundaries

- `user/` explains product tasks; `development/` explains contribution and maintenance tasks.
- `system/` groups current structure and behavioral contracts by topic. A page may explain ownership, behavior, and relevant values together.
- `reference/` provides independently useful lookup material: languages, commands, protocols, formats, compatibility rules, and semantic tables.
- `decision/` preserves consequential choices and their historical reasons, not current behavior.

Keep project docs machine-independent: platform requirements, portal defaults
and public interfaces belong here. Put actual hosts, accounts, paths, source
transfers and login/power recipes in home skills or operator guidance.
Use generic examples; do not link personal home-directory files.
When a frontend's support status changes, align the root README and development guide, user tasks, frontend specifications, and persistence references in the same change. In the user guides, give steps only for supported, implemented surfaces.

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

Check changed claims against the relevant code and tests. A disagreement may be an implementation bug: do not silently rewrite or weaken a normative contract to match code; report the conflict instead.
If the contract should stand, keep it and describe what readers can observe beside it under a bold label such as **Known limitation:**. Give a verified workaround if one is known; otherwise say what readers cannot rely on. Keep probe details and reproduction evidence in the PR or an issue.
Name the relevant implementation and tests where they help readers verify a contract; there is no mandatory exhaustive map for every page.
When restructuring several owners, keep a temporary fact ledger outside the repository recording the source fact, destination, evidence, affected links, and reason for deletion or consolidation.
This is migration evidence, not a permanent registry or a requirement for ordinary edits.
Keep each negative constraint beside the guarantee it limits. A test that proves adapter behavior need not prove native routing or physical input. Drop review-only reports about what a change left untouched; put that evidence in the PR instead.

## Writing a page

Use English and descriptive headings; choose their number and order to fit the subject.
State the scope near the beginning and distinguish explanation from requirements with clear wording such as `must`, `never`, and `may` only for real contracts.
There is no mandatory template or required section sequence.
Useful questions, not a form to fill:

- For a task: what does the reader need, what do they do, and how do they recognize success? Lead with the common action or command and its visible result, then give constraints and diagnosis. Branch only the platform-specific steps. Use the words on the screen or command line; explain internal concepts by what the reader sees or can do, without losing the constraint.
- For a mechanism: who owns state, what crosses a boundary, what can fail, and what happens during cancellation or destruction? Make the responsible object, thread, or boundary the subject. Replace strings of slash-joined nouns with a clause that says who does what.
- For a format or language: what is accepted, what does it mean, and which compatibility or rejection rules apply? Prefer tables, grammar blocks, and lists. Give rows in the same table parallel structure; use prose to explain meaning, compatibility, or rejection.
- For a decision: what evidence and consequences explain the choice at the time? State the cost accepted and the concrete reason each alternative was rejected.

Describe the current system, not the difference from a previous implementation or a previous version of the page. Keep compatibility rules, upgrade instructions, and domain states when readers still need them. Reasons for replacing an implementation belong in the commit or PR, or in a decision when reversal would be costly.

For a probe or smoke test, give the command, prerequisites, and what a pass does and does not establish. Let test code own the fixture and sequence of assertions. In an implementation or test map, pair each file with the contract it owns. Document checker maintenance rules and pitfalls; leave individual AST branches to the source and fixtures.

Split paragraphs when readers need to find the topics independently. Use a table for comparable budgets, state transitions, or frontend differences. Keep exact examples valid, use language labels on fenced blocks, and prefer semantic line breaks.
Link a topic from the tasks it serves. Each non-entry page in the five pillars also needs a direct link from its reader's entry page:

- `user/`, `reference/`, and `decision/`: the pillar's `README.md`.
- `system/`: `system/README.md` for a top-level page; the subsystem's `README.md` for pages within it.
- `development/`: `development/README.md`, or the delegated testing or linting entrance (`development/test.md` or `development/linting.md`).

If a page has a `## Related documents` section, end the page there with a list of links. A quoted heading or an example is not a page section. Keep a link and its brief annotation in the same paragraph of a list item. The paragraph may wrap, and entries may contain nested link lists. Put contract text beside the rule it qualifies, not after the list.
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
The checker validates flat scalar metadata and supplied ids and status values under `doc/`, not a type-specific lifecycle or directory taxonomy.
All discovered Markdown locations exclude a closed leading front-matter block from links, anchors, and navigation; other locations retain their own metadata schemas.

## Validation

Run `./ao check docs` from the repository root for documentation semantics, then
`./ao check format --docs` for read-only formatting verification of the global Markdown inventory.
The semantics check accepts no paths or commit scope and needs no native SDK.
For formatter policy, see [authored-text formatting](linting.md#authored-text-formatting).
Both commands discover Markdown under `doc/` (excluding `doc/plan/`), `.agents/skills/`, `tool/`, `app/`, and `asset/`, plus root `README.md`, `AGENTS.md`, and `CONTRIBUTING.md`; Markdown under other trees such as `lib/`, `test/`, `bazel/`, or `script/` is not checked.
The semantics check validates optional metadata and ids, inline and reference-style links, Markdown anchors, and the ban on links to local plans. Under `doc/` it checks reachability from the documentation home and the direct entry-page links described above. In every discovered document it checks that a Related documents section contains a link list and no other rendered blocks through the end of the page.
The governed Markdown parser supplies CommonMark block and inline structure, with the GFM table extension enabled.
Links, reference definitions, headings, and HTML anchors use that same parse; code examples do not contribute links or anchors.
Link labels may wrap within an inline container, and diagnostics identify the physical opening line.
Local-target diagnostics show readable decoded destinations; URL resolution keeps the parser's normalized URL so encoded delimiters and literal percent signs retain their meaning.
Reference matching retains the raw label, including code-span delimiters, rather than the label's rendered text.
The checker additionally reports undefined explicit references (`[text][label]` or `[label][]`); undefined shortcut text is not an error.
Image targets are checked but do not make documentation reachable.
This is a local-link and navigation checker, not a full GitHub renderer or an external-URL availability check.
The parser and its dependency are pinned through the [tooling dependency contract](dependency-governance.md#repository-tooling).
It also checks the [naming tooling contract](lint/naming-checks.md) against the audit vocabulary and registered lint checks.
The checker does not require templates, fixed headings, architecture portfolios, RFC dependency declarations, or registration in another pillar's index. Direct links do not replace reachability from the documentation home.

Mechanical success does not prove factual agreement. Review the moved facts and try representative reader tasks, especially for transactions, persistence, cancellation, and lifetime guarantees.
Changes to validators or other code additionally follow [completion validation](test/validation-and-review.md).
Documentation simplification does not relax code architecture audits, lint rules, or native correctness gates.
