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

Use these questions in order:

1. Is the state authoritative across frontends? Runtime owns it.
2. Is it deterministic, platform-neutral display or interaction policy? UIModel owns it.
3. Is it widget, terminal, command-parser, native-resource, or event-loop behavior? The frontend owns it.
4. Does one service coordinate storage, audio, executors, or another runtime service? Runtime composition owns that coordination.
5. Is a callback observational? Subscriber presence and return values cannot change runtime policy.

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

During review, reject these shapes unless an owning architecture explicitly documents a migration seam:

- GTK/TUI code opening LMDB transactions or constructing storage-backed sources;
- UIModel code including GTK, FTXUI, LMDB stores, player, engine, or backend control headers;
- public failure observers selecting recovery behavior;
- frontend timers implementing runtime persistence or retry policy;
- duplicated GTK and TUI ordering, succession, filtering, or recovery algorithms;
- a platform type crossing into runtime state.

Known direct-library migration seams are documented in the [presentation architecture](../architecture/presentation.md); their presence is not permission to add new seams.

## Validation

Run the narrow tests for the changed owner and adapter, then the repository validation required by [validation and review](test/validation-and-review.md).
Building application targets also runs the include and UIModel organization guardrails attached in `app/CMakeLists.txt`.

Review evidence should identify:

- the architecture/specification that owns the behavior;
- the test that protects the authority boundary;
- any executor, subscription, cancellation, and teardown ordering involved.

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
