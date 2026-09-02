---
id: development.application-layer-review
type: development
status: current
domain: development
summary: Defines the contributor review workflow for runtime, UIModel, and frontend ownership boundaries.
---
# Application-layer review

## Scope

This guide defines how contributors review changes that cross runtime, UIModel, GTK, TUI, or CLI.
Architectural authority remains in the [system](../architecture/system-overview.md), [runtime execution](../architecture/runtime-execution.md), and [presentation](../architecture/presentation.md) documents; this guide turns those boundaries into a review workflow.

## Policy

Physical target direction is necessary but insufficient: behavior must also be owned by the correct layer.

For every proposed public role, ask these questions:

1. What does it own?
2. What does it guarantee beyond the types it contains?
3. Which correctness contract is lost if it is deleted?
4. Has the object reached its final address before any callback, observer, provider, restoration, or borrowed reference is published?
5. If state is shared, which independent participant may retain it, and what borrowed owner or executor is its lifetime ceiling?

If none has an answer, absorb the role into its real owner or use a function.
These questions are not a score.

Then place the surviving behavior by authority: cross-frontend source-of-truth
state and service coordination belong to runtime; deterministic
platform-neutral presentation and interaction policy belongs to UIModel; and
widget, terminal, command-parser, native-resource, or event-loop adaptation
belongs to the frontend. Observational callbacks never let subscriber presence
or return values choose runtime policy.

Runtime commands update their authoritative snapshot and revision before publishing the corresponding observation.
A no-op or rejected command does not manufacture a revision.
Worker and backend callbacks marshal to the owning executor before mutating service state.

UIModel may retain drafts, gestures, view state, and UI-local preferences.
It must not open storage transactions, control the audio engine, own retry/recovery loops, or reconstruct cross-service policy.
A healthy view model accepts stable values or a narrow command port and publishes only when its immutable view state changes.

Frontend adapters translate platform input into one UIModel/runtime action and semantic output into a platform representation.
GTK and TUI must use the same runtime authority for equivalent actions.
For example, playback launch crosses the boundary as `ViewId` plus `TrackId`; a frontend does not send a reconstructed row-order vector.

## Workflow

Before editing:

1. Find the owning architecture and focused specification.
2. Identify the authoritative state, command owner, observation owner, and platform adapter.
3. Inspect target dependencies and the matching unit-test layer.

During review, also draw construction and teardown in address order.
Prefer direct mandatory values and references, optional values for genuine phases, and value-returning factories when an outer box provides no identity.
Move-only composition roots need `noexcept` move construction, deleted move assignment unless replacement is intentional, and inert moved-from destruction.
A callback-admission or generation token does not protect a raw owner: verify retire-before-cancel plus the drain/join that keeps the owner alive through every admitted callback.

During review, reject these shapes unless an owning architecture explicitly documents a migration seam:

- GTK/TUI code opening LMDB transactions or constructing storage-backed sources;
- UIModel code including GTK, FTXUI, LMDB stores, player, engine, or backend control headers;
- public failure observers selecting recovery behavior;
- frontend timers implementing runtime persistence or retry policy;
- duplicated GTK and TUI ordering, succession, filtering, or recovery algorithms;
- a platform type crossing into runtime state;
- a whole shared PImpl used only to survive one callback stack;
- provider registration, restoration, or observer binding before final runtime placement;
- a weak admission token presented as proof that raw owner memory remains alive.

Known direct-library migration seams are documented in the [presentation architecture](../architecture/presentation.md); their presence is not permission to add new seams.

## Validation

Run the narrow tests for the changed owner and adapter, then the repository validation required by [validation and review](test/validation-and-review.md).
The completion `./ao check` gate explicitly builds `aobus_guardrails`, which owns the application architecture audit declared in [`ArchitectureAudit.cmake`](../../app/cmake/ArchitectureAudit.cmake).
Ordinary application builds leave those repository-wide scans to that gate.

Review evidence should identify:

- the architecture/specification that owns the behavior;
- the test that protects the authority boundary;
- the final placement and moved-from behavior of any value-returning lifecycle facade;
- the independent participant and lifetime ceiling for every shared State;
- any executor, subscription, admission, cancellation, drain/join, and teardown ordering involved.

## Troubleshooting

If ownership appears split, describe the authoritative state and command first; the adapter normally becomes obvious afterward.
If both frontends implement the same policy, move the deterministic part into runtime or UIModel according to whether it changes application behavior or only presentation.
If a guardrail blocks a legitimate dependency, update the architecture and guardrail together rather than adding a local include exception without an owner.

## Related documents

- [System architecture](../architecture/system-overview.md)
- [Runtime execution architecture](../architecture/runtime-execution.md)
- [Presentation architecture](../architecture/presentation.md)
- [Failure and reporting architecture](../architecture/failure-and-reporting.md)
- [UIModel organization](uimodel-organization.md)
