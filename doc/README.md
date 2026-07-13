# Aobus documentation system

This directory is the durable knowledge base for Aobus.
Its structure separates user tasks, system architecture, current behavior, exact surfaces, contributor workflow, historical decisions, and proposed changes so that one document never has to serve incompatible purposes.

## Documentation map

| Directory | Owns |
|---|---|
| [`user/`](user/README.md) | Task-oriented documentation for people using Aobus. |
| [`architecture/`](architecture/README.md) | The architecture landscape: system structure, ownership, dependency direction, capability coverage, and cross-cutting models. |
| [`spec/`](spec/README.md) | Normative current behavior: invariants, state machines, transitions, failures, cancellation, and persistence semantics. |
| [`reference/`](reference/README.md) | Exhaustive command, schema, grammar, protocol, configuration, and format surfaces. |
| [`development/`](development/README.md) | Contributor setup, standards, testing, tooling, and repository workflow. |
| [`decision/`](decision/README.md) | Accepted or superseded architectural decisions and their historical rationale. |
| [`rfc/`](rfc/README.md) | Tracked proposals under review before they become current behavior. |
| `plan/` | Local execution plans; intentionally ignored by Git. |
| [`template/`](template/README.md) | Required starting structures for each document type. |

## Authority by document type

Each durable fact has one owner.

- Architecture owns who is responsible, which direction dependencies flow, and how subsystems compose.
- Specifications own what behavior must hold and what tests must prove.
- Reference owns the exact names, fields, grammar, values, defaults, and wire or storage shapes.
- Decisions own why a consequential choice was made and which alternatives were rejected.
- RFCs own a proposal only while it is being evaluated; they are never authority for current behavior.
- User and development guides own task sequences, not the underlying product or repository contract.

When one subject needs multiple document types, split it and link the owners.
For example, session restore behavior belongs in a specification, the serialized payload belongs in reference, runtime ownership belongs in architecture, and the rejected alternatives belong in a decision record.

### Architecture portfolio

This documentation system owns the architecture-role definitions and the threshold for creating a focused architecture.
The [architecture landscape](architecture/README.md) applies those rules and owns the current document assignments, navigation, relationships, and coverage gaps.
Architecture documents use these roles without introducing matching subdirectories:

| Role | Architectural question |
|---|---|
| System map | What are the process-wide layers, composition roots, and dependency direction? |
| Foundation or cross-cutting concern | Which constraint or mechanism applies across several otherwise independent systems? |
| Domain system | Which reusable product or core capability owns an independent state and dependency graph? |
| Application system | Which application-level authority composes domain capabilities into interactive state or policy? |
| End-to-end vertical slice | Where does one focused concern require its own evidence, translation, or lifetime model across several layers? |

These roles are navigation classifications, not additional document types or front-matter fields.
Keep `architecture/` flat unless filesystem scale creates a concrete navigation problem; do not mirror the code tree or the role names as directories.

Create a focused architecture document only when the subject has an independent ownership, dependency, execution, or lifetime graph that the existing architecture owners cannot explain without mixing unrelated concerns.
Crossing two layers is evidence but is not sufficient by itself.
A complex feature with no independent structural model belongs under its existing architecture owner, with observable behavior in specification and exact surfaces in reference.

The landscape records a coverage gap before a new architecture owner is justified.
A coverage gap describes missing documentation authority; it is not a proposed product design and does not replace an RFC when system behavior or boundaries must change.
An existing but undesirable dependency or lifetime graph is still current architecture and must be documented honestly.
Link the RFC that proposes its replacement, but do not write the proposed boundary as current before implementation.

## Required metadata

Every new document under `user/`, `architecture/`, `spec/`, `reference/`, `development/`, `decision/`, or `rfc/` starts with this flat YAML front matter:

```yaml
---
id: playback.cursor
type: spec
status: current
domain: playback
summary: Defines track succession over a live source projection.
---
```

The metadata is deliberately a restricted flat-scalar subset so repository tooling can validate it without another YAML dependency.

| Field | Rule |
|---|---|
| `id` | Stable, repository-unique lowercase identifier using dots or hyphens. Do not change it merely because the file moves. |
| `type` | One of `index`, `user-guide`, `architecture`, `spec`, `reference`, `development`, `decision`, or `rfc`. |
| `status` | A type-appropriate lifecycle state from the table below. |
| `domain` | Lowercase subsystem or concern, such as `library`, `playback`, `query`, `linux-gtk`, or `documentation`. |
| `summary` | One concise sentence describing the fact or task the document owns. |
| `depends-on` | Required only for RFCs. Comma-separated stable RFC document ids for direct hard dependencies, or `none`. |

Allowed status values:

| Type | Statuses |
|---|---|
| `index`, `user-guide`, `architecture`, `spec`, `reference`, `development` | `draft`, `current`, `deprecated` |
| `decision` | `proposed`, `accepted`, `superseded`, `rejected` |
| `rfc` | `draft`, `in-review`, `accepted`, `rejected`, `implemented` |

Status is explicit; absence never implies current behavior in the new structure.
For an RFC, `draft` means a durable proposal candidate in the repository inventory; it does not mean that review, scheduling, or implementation is active.
Change an RFC to `in-review` only when adjudication is active, then record its accepted, rejected, or implemented outcome rather than inferring progress from its sequence number.
Supersession relationships belong in the document body until the decision/RFC workflow needs machine-readable graph validation.
RFC dependency metadata follows the additional contract below.

## Choosing the owner

Use this test before creating or migrating a document:

| The document primarily answers... | Type |
|---|---|
| How do I accomplish a user task? | User guide |
| How is the system divided and who owns what? | Architecture |
| What behavior and invariants must an implementation preserve? | Specification |
| What exact commands, fields, grammar, values, or schema exist? | Reference |
| How do contributors work in this repository? | Development guide |
| Why was a consequential choice made? | Decision |
| Should Aobus adopt this proposed change? | RFC |

Do not preserve a mixed document merely to avoid splitting it.
Move each fact to its natural owner, update inbound links, then remove the old duplicate.

For a multi-owner migration, keep a temporary fact ledger outside tracked documentation with these columns:

```text
source fact | fact class | target document id | code evidence | test evidence | inbound links | disposition
```

Every row must end at one current owner or state why the source fact is incorrect, obsolete, or proposal-only.

## Lifecycle

Early private exploration belongs in a local plan or an issue, not in a current design document.
A durable proposal starts as an RFC.

```text
idea -> RFC -> accepted decision (when rationale is durable)
            -> architecture/spec/reference updates
            -> local implementation plan
            -> implemented current documentation
```

An accepted or implemented RFC links to the current documents it produced but never replaces them.
A decision remains historical evidence after its resulting specification changes; mark it superseded and link the replacing decision.

`doc/plan/` remains ignored by Git.
Tracked documents must never link to it because those links break in every other checkout.

### RFC dependencies

Every RFC declares `depends-on` in front matter and includes `## Dependencies` immediately after `## Problem`.
The metadata field contains only direct hard dependencies that block complete implementation of the proposal.
Use stable RFC document ids, not sequence numbers or file names, and write `depends-on: none` when there are no hard dependencies.

The Dependencies section explains the reason for every hard dependency and links to the target RFC.
It also records direct conditional dependencies that block only a named phase or feature and direct integration dependencies that require contract alignment when both proposals are implemented.
Each category uses exactly one `- Hard:`, `- Conditional:`, or `- Integration:` entry.
Write `None.` exactly when a category is empty; otherwise link every target RFC on that category's entry.
Every RFC link on a category entry is treated as an edge, so links to merely related work belong elsewhere.
Related work, an incoming dependency from another RFC, and an opportunity to reuse an implementation are not dependencies unless this proposal's correctness or completion requires them.

Dependency direction is “this RFC depends on the listed RFC.”
All dependency targets must be existing RFCs, cannot refer to the same RFC, and cannot be duplicated within or across categories.
Hard dependencies must match `depends-on` exactly and form an acyclic graph.
Conditional and integration relationships may be cyclic because two proposals can conditionally share infrastructure or require mutual contract alignment.
The RFC index contains one complete dependency row per RFC and must match every RFC section and hard-dependency field.
RFC sequence numbers express document identity only; they do not imply dependency or implementation order.

## Content and formatting rules

- Write code, comments, metadata, and documentation in English.
- Name documentation directories with singular nouns, matching the repository convention (`decision/`, `template/`, and `spec/`, not plural variants).
- Use one H1 title after the front matter and sentence-case H2/H3 headings.
- Use present tense for current architecture, specifications, and reference.
- Keep historical narrative in decisions and future intent in RFCs.
- Use `must`, `never`, and `may` only for real contracts.
- Prefer semantic line breaks: one complete prose sentence per source line; let the renderer wrap it.
- Give every fenced code block a language identifier.
- Use tables for exact mappings and matrices, not as general page layout.
- Link to the fact owner instead of copying its table or rule.
- Reference stable symbols and repository paths in implementation/test maps; do not paste implementation code into a specification.
- Keep exact command and schema inventories in reference, preferably generated or locked by tests.

## Code-boundary alignment

This document is the single authority for architecture-role definitions and the focused-architecture threshold.
The [architecture landscape](architecture/README.md) is the single authority for current role assignments, architecture relationships, and capability coverage.
The [system architecture](architecture/system-overview.md) is the single authority for the top-level Core libraries, application runtime, UIModel, and frontend layer model.
A focused subsystem architecture may refine that model with subsystem ownership, public and implementation paths, allowed dependencies, composition, and lifetime, but it cannot redefine the top-level layers.

Every specification and reference document with an implementation surface includes a `Code boundary` section after its scope.
That section links the system architecture and the owning subsystem architecture when one exists, names the owning layer and relevant public and implementation paths, and identifies an important allowed or forbidden dependency only when needed to locate the contract.
It links instead of copying the layer definitions.

Architecture documents place the same information in `System context` and `Boundaries and dependency direction` rather than adding a second `Code boundary` section.
Implementation and test maps provide evidence for the stated boundary; they do not replace it.
User guides, development guides, decisions, and RFCs describe code boundaries only when those boundaries are material to their own task, workflow, rationale, or proposal.

## Templates

- [Index and subsystem landing page](template/index.md)
- [User guide](template/user-guide.md)
- [Architecture](template/architecture.md)
- [Specification](template/spec.md)
- [Reference](template/reference.md)
- [Development guide](template/development.md)
- [Decision](template/decision.md)
- [RFC](template/rfc.md)

Delete unused template sections rather than filling them with placeholders.
Do not invent a new section order until the existing template demonstrably cannot express the document.

## Validation

Run the documentation gate from the repository root:

```bash
./ao docs check
```

The gate validates metadata, ids, lifecycle states, required document sections, index ownership, architecture portfolio roles, relationships and coverage, RFC dependency fields, sections, targets, links and cycles, unchanged template placeholders, internal inline and reference-style links, anchors across repository-owned Markdown, forbidden plan links, and reachability from this index.
Documentation changes that also modify code follow the normal implementation validation in the [testing policy](development/test.md).
