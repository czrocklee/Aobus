# Runtime and UIModel Boundary

This document defines the current ownership boundary between the frontend-neutral
application runtime, UIModel, and platform frontends. It is the review contract
for application-layer changes; physical target direction alone is insufficient
when behavior is owned by the wrong layer.

## Dependency direction

```text
core/library -> ao_app_runtime -> ao_app_uimodel -> GTK/TUI
                         \-----> CLI when UIModel is unnecessary
```

Runtime never depends on UIModel. UIModel may issue runtime commands and project
view state from more than one runtime service, but it cannot own storage, audio
control, or cross-service runtime policy. Frontends bind runtime/UIModel values
to platform lifecycle and rendering facilities.

## Ownership

| Concern | Owner | Boundary contract |
| --- | --- | --- |
| Storage, transactions, and library mutation | runtime/core | Runtime facades expose value types, readers, writers, tasks, and change events. |
| Playback transport and engine control | runtime | `PlaybackService` owns request resolution, transport, prepared-token correlation, now-playing state, output, seek, and volume. |
| Live playback context and succession | runtime | `PlaybackSequenceService` owns the captured launch context, source lease, projection cursor, next/previous, repeat, shuffle, and bounded recovery walks. |
| Playback failure recovery control | runtime composition | One private sequence recovery control port is installed; public failure events are observational. |
| Playback-session payload and restoration | runtime | Runtime validates and persists cursor plus transport intent, then restores it atomically; frontends trigger lifecycle saves and restores. |
| Workspace and view lifecycle | runtime | One aggregate keeps existing, open, and active views coherent. |
| Structural presentation state | runtime | IDs, fields, grouping, sorting, and persisted structural specs are canonical. |
| Display labels, descriptions, formatting, and menu state | UIModel | UIModel projects structural runtime values into user-facing state. |
| Drafts, gestures, and UI-local preferences | UIModel | Local state may influence runtime commands but is not a second runtime authority. |
| Widgets, CSS, native icons, dialogs, and event loops | frontend | Adapters map semantic values to platform facilities. |

## Runtime rules

- Runtime owns every authoritative state value that changes behavior across
  frontends.
- Runtime services alone may coordinate storage, audio control, executor work,
  and other runtime services.
- Mutable services are executor-affine unless their public contract explicitly
  says otherwise. Subscription creation and reset follow the same affinity.
- A successful command updates its snapshot and revision before emitting its
  command event inline. Worker/backend callbacks first marshal onto the owning
  executor.
- A no-op or rejected command does not increment a revision or publish a change.
- Public events are observational. Subscriber presence, callback return values,
  and subscription count cannot alter runtime policy.

`PlaybackService` is the transport boundary. It starts resolved playback
requests, controls the engine, owns transport and output state, and correlates
prepared transitions by exact `PreparedNextToken`. It does not resolve list
order or own repeat and shuffle policy.

`PlaybackSequenceService` is the application succession authority. GTK and TUI
call `playFromView(viewId, startTrackId)` with identities only. The service asks
`ViewService` for one coherent launch context, owns its own source lease and
live projection cursor, and applies the same next/previous, repeat, shuffle,
prepared-next, natural-advance, and recovery policy for every frontend. A
frontend never passes an ordered track vector or reconstructs succession from
visible rows. See [Playback Cursor](playback-cursor.md) for the cursor rules.

## Failure recovery control and observation

`PlaybackService` translates an engine failure and refreshes its snapshot before
asking the single runtime recovery control port for a disposition. The result is
one of `Unhandled`, `Recovered`, or `Stopped`. Only `PlaybackSequenceService`,
bound through runtime composition, may return that disposition. It accepts a
failure only when the active source/current identity or exact prepared token
belongs to its session; stale and unrelated failures remain `Unhandled`.

After the control decision, `PlaybackService` emits a self-contained public
failure event containing the disposition. Logging, GTK, TUI, MPRIS, and other
observers cannot influence recovery or suppress fallback notification behavior.
Unhandled track failures receive the normal playback error notification;
recovered or terminal sequence failures receive the mutually exclusive
sequence notification. Output failures always retain the service-level output
error path.

## Playback-session responsibility

`PlaybackSessionPersistence`, composed by `AppRuntime`, owns:

- composite cursor-context and transport payload construction;
- schema and internal-consistency validation;
- source/current recovery and missing-source fallback;
- coherent prepare/commit restoration of sequence and transport state;
- dirty revision and successful-save acknowledgement;
- config load, save, and flush error classification.

Frontends own only lifecycle timing: startup restore, significant-event save,
dirty-event debounce and bounded retry through
`PlaybackSessionSaveService`, periodic save where appropriate, and
shutdown save. They do not parse the persisted payload, query storage to
reconstruct membership, or call low-level sequence/transport restoration
separately.

## UIModel rules

- UIModel subscribes to runtime snapshots/events and maps user intent to runtime
  commands.
- UIModel may own editor drafts, gesture state, display projection, and
  UI-local preferences.
- UIModel does not own transactions, audio control, retry/recovery loops, or
  cross-service orchestration.
- UI-local state cannot be the only source required to restore canonical runtime
  behavior.
- Formatting and presentation policy remain independently testable without a
  storage/audio graph.

A healthy ViewModel accepts stable runtime values or a narrow command port,
derives one immutable view state, and publishes only when that view state changes.
It does not construct a storage-backed source or call `PlaybackService::playTrack`
to bypass sequence semantics.

## Frontend rules

- Frontends own toolkit lifecycle, timers/debounce, widgets, terminal surfaces,
  CSS, and native icon mapping.
- Equivalent GTK and TUI user actions use the same runtime service.
- Playback launch sends only `ViewId` and `TrackId`; frontend row-order vectors
  never cross the runtime boundary.
- Frontends cannot instantiate storage-backed sources, evaluators, or
  projections from a raw `MusicLibrary`.
- A healthy adapter translates a UI event to one UIModel/runtime command and maps
  semantic output to the platform. It contains no successor, recovery,
  persistence-payload, or storage policy.

## Semantic and platform metadata

Runtime values may carry semantic severity, lifetime, action IDs, progress,
presentation IDs, and notification topics. GTK symbolic icon names, CSS classes,
platform templates, and display-only built-in labels live outside runtime.
UIModel supplies display projection where the representation is platform
neutral; the GTK/TUI adapter performs platform-specific mapping.

## Review examples

- Healthy runtime service: owns an executor-affine snapshot, validates commands,
  coordinates lower services, increments one revision, then emits one typed
  event.
- Healthy ViewModel: converts runtime semantic state to labels/menu state and
  delegates commands through a required runtime collaborator.
- Healthy frontend adapter: converts a click or key event to a command and maps a
  semantic icon/status enum to GTK or terminal presentation.
- Boundary violation: a GTK coordinator opens an LMDB transaction, reconstructs
  playback order, or changes recovery behavior by connecting a failure signal.
