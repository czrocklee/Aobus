---
id: development.design-review
type: development
status: current
domain: development
summary: Defines proportionate design review for boundaries, abstractions, ownership, and structural refactors.
---
# Design review

## Scope

This guide supports review of changes that alter ownership, a public boundary, an abstraction, or the structure of a subsystem.
It is not a mandatory ceremony, score, gate, or checklist for every patch.
Use it proportionately: a local fix may need only a brief explanation, while a cross-layer lifetime change needs stronger evidence.

Design review optimizes understanding, change locality, and provability.
Raw line, file, and class counts are not substitutes for the repository's complete [concept vector](concept-metrics.md) or behavioral evidence.
A smaller diff can create broader obligations, and a larger implementation can isolate a difficult contract.

## Classify the change

Name the kind of change before choosing evidence or review depth.

- A **fix** restores an existing contract or invariant.
- An **architecture change** moves authority, responsibility, dependency direction, or lifetime ownership.
- An **organization-only change** rearranges code without intentionally changing behavior or architectural authority.
- A **product behavior change** changes an observable contract and belongs with its specification or reference owner.

A proposal can span categories.
For example, moving a responsibility while fixing its cancellation behavior is both an architecture change and a fix.
Do not describe a behavior or authority change as organization-only merely because the public syntax remains stable.

## Design qualities

### Factual authority

Each mutable fact has one factual authority.
Other representations may be legitimate caches, projections, or captured snapshots when their role, source, freshness, and invalidation or lifetime are explicit.
Redundant authority exists when multiple representations can independently claim what is current without a rule that resolves disagreement.
Review the meaning of duplicated-looking state rather than counting fields.

### Lifetimes and completion

Ownership and lifetime should be explainable from creation through shutdown.
Identify who starts work, who can cancel it, what completion means, where failure is observed, and which object or executor must remain alive.
Borrowing relationships must state the lifetime that makes them safe.
Shutdown obligations do not disappear when an operation is hidden behind an interface.
Follow the class-design and threading rules in [C++ coding style](coding-style.md) (4.2 and 4.4), and the applicable [concurrency validation](test/concurrency-and-sanitizer.md).

### Responsibility and boundaries

A useful boundary protects an independent responsibility, state model, contract, lifetime, or set of illegal states.
It should make changes to that concern more local and make its behavior provable at an appropriate layer.
Splitting code is not sufficient if callers still coordinate the same scattered responsibility.
Conversely, related implementation may remain together when no independent contract needs protection.

Consumers should receive the least relevant capability needed for their responsibility.
Composition roots may know the complete object graph because constructing and connecting that graph is their job.
Do not force composition knowledge into leaf consumers, and do not hide graph construction behind indirection that has no separate contract.

Interfaces hide replaceable or irrelevant mechanism.
They must not hide semantics that callers need for correctness, including completion, failure, cancellation, ownership, and borrowing.
A narrow concrete API can be better than an abstract interface when mechanism substitution is not part of the contract.

### Tests and seams

Place tests at the [lowest useful layer](test/layer-selection.md) that proves the behavior.
Select ordinary injection and public observation seams through the [testability-seam order](test/fixture-and-helper.md#testability-seams).
Private-access and other exceptional low-level seams require the closed-inventory admission process specified there, not design judgment alone.
Keep seams narrow and avoid production indirection whose only purpose is to mirror test setup.

## Evaluating abstractions

[Naming convention](naming-convention.md#review) owns the public-role justification questions.
Shared semantics or an independent contract, lifetime, or illegal-state boundary can justify an abstraction.
Similarity alone is not shared semantics.
A single consumer is not automatically a defect, and multiple consumers do not automatically justify generalization.

For the current need, compare a justified abstraction with leaving the code in place, extracting a local function, using a concrete helper, composing existing objects, or splitting an implementation translation unit.
The comparison should include caller obligations, failure paths, testability, and maintenance cost rather than only code reduction.

Overengineering occurs when mechanisms, concepts, or maintenance obligations exceed current needs, contracts, or evidenced risk.
Warning signs include:

- A generic framework with one consumer and no independent contract.
- Speculative extension hooks without a current extension requirement.
- Forwarding layers that add no policy, invariant, lifetime control, or stable boundary.
- A base interface accumulating hooks or flags for unrelated cases.
- Platform exceptions leaking into shared models instead of remaining at a platform boundary.

These are prompts for investigation, not absolute bans.
A one-consumer abstraction may protect a difficult lifetime, and a forwarding adapter may enforce a real dependency boundary.
Judge the protected contract and alternatives, not the shape in isolation.

## Reviewing simplification

A simplification proposal identifies the responsibility the current structure protects and where that responsibility will go.
It states any caller obligations that become implicit, especially sequencing, cancellation, error handling, and lifetime rules.
It also names behavior, compatibility, or extension points that change or disappear.
Deletion is not simplification when it merely distributes the same complexity among callers.

Distinguish hard dependencies from scheduling preferences.
A hard dependency is required for correctness or for the interface to exist.
A preferred implementation order, reuse opportunity, or convenient sequencing is soft and should not be presented as a correctness constraint.

## Proposing a refactor

Explain a refactor in proportion to its scope; no particular document or form is required.
Cover the concrete problem and evidence, the smallest solution that addresses it, and the expected benefit compared with its costs.
State behavioral and structural acceptance alongside measurement evidence: for example, a single authority, a removed caller obligation, a bounded dependency, or a preserved behavior contract.

Concept-debloat refactors must publish a comparable before-and-after vector using the [concept-metrics workflow](concept-metrics.md#workflow).
Explain material regressions and their causes, and judge the complete vector with the ownership, capability, and behavioral result under [Decision 0017](../decision/0017-evaluate-concept-metrics-as-a-vector.md), not as independent metric vetoes.
Call out uncertainty instead of converting guesses into architecture rules.

Valid review outcomes are:

- **Implement** when the evidence and boundary are clear.
- **Narrow** when a smaller change captures the demonstrated benefit.
- **Investigate** when a key behavior, dependency, or lifetime claim lacks evidence.
- **Leave unchanged** when alternatives cost more than the current design or no current problem is established.

## Compact examples

Two same-named fields need not be redundant authority: one may be the current library view while another captures the playback context that must remain stable for an in-flight operation.
Review which value is authoritative for each decision and when the snapshot expires.

Splitting a large source file does not split responsibilities when the same callers still orchestrate unrelated state transitions.
A useful split follows a contract or implementation boundary; an organization-only split may still improve navigation but should claim only that benefit.

Moving an include beneath a `detail/` path does not remove the dependency when public behavior, construction, or lifetime still requires that mechanism.
The dependency is removed only when consumers no longer rely on its contract or representation.

## Related documents

- [System architecture](../architecture/system-overview.md) owns the top-level layer and dependency model.
- [Application-layer review](application-layer-review.md) applies ownership review to runtime, UIModel, and frontends.
- [Validation and review](test/validation-and-review.md) selects completion evidence for the changed execution boundary.
