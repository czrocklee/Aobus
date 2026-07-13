---
id: rfc.0016.coherent-workspace-transactions
type: rfc
status: draft
domain: workspace
summary: Proposes a validated revisioned workspace aggregate with atomic commands, coherent observation, and explicit application navigation ownership.
depends-on: none
---
# RFC 0016: Coherent workspace transactions

## Problem

`WorkspaceService` is documented as the canonical aggregate of open and focused views, but its public mutation surface does not enforce that aggregate.
`addView()` accepts any `ViewId`, including the invalid id, an unknown id, or a destroyed view.
`setFocusedView()` does not require its target to be open or even owned by the same live `ViewService`.
`closeView()` advances the workspace revision and emits focus even when the id was not open, then logs and discards a possible view-destruction failure.

These are not merely permissive test helpers.
TUI production code calls `setFocusedView()` directly, and current tests construct workspace state through `ViewService::createView()`, `addView()`, and `setFocusedView()` as independent steps.
The type system therefore permits an active view outside `openViews`, an open destroyed view, and a revision that changed without an aggregate state transition.

Composite commands have a second coherence gap.
`jumpToAlbum()` focuses a view before applying its presentation, swallows target-resolution failure, logs presentation failure, invokes a void playback reveal command, and only then commits history.
If a later step fails, observers can see a partially applied focus or presentation without a result identifying the accepted subset.
Session restore prepares all `ViewService` candidates before workspace mutation, but then exposes the candidate through repeated `addView()`, preset replacement, focus, and history operations rather than one aggregate commit.

Observation is fragmented across `onFocusedViewChanged`, `onNavigationHistoryChanged`, and `onCustomPresetsChanged`.
Only the focus signal identifies a view, only the history signal identifies directional availability, and none carries the complete `WorkspaceViewState` revision.
Consumers that need open views, focus, history availability, and preset catalog state must reread mutable services and infer which values belong together.

Workspace signals emit synchronously.
The common signal implementation continues notifying later observers after one throws and then rethrows the first exception to the command caller.
At that point workspace state is already mutated, so an observer failure can turn an applied command into an apparent exceptional failure.
An observer may also invoke another workspace command recursively while the outer command is still publishing.

Finally, `WorkspaceService` depends directly on `PlaybackService` only to implement album reveal.
That relationship makes the workspace aggregate a cross-domain application coordinator and conflicts with the proposed playback boundary, which moves reveal navigation out of playback transport.
The missing abstraction is not a larger workspace facade; it is an explicit application-navigation owner above validated workspace transactions.

## Dependencies

- Hard: None.
- Conditional: None.
- Integration: [RFC 0005](0005-coherent-playback-boundary.md), [RFC 0006](0006-coherent-derived-track-views.md), [RFC 0013](0013-coherent-application-reporting-policy.md).

RFC 0005 moves reveal intent out of playback transport and must align with the application-navigation owner proposed here.
RFC 0006 introduces revisioned view filter commands and atomic projection replacement; workspace commands that prepare or mutate views must preserve those view-level outcomes.
RFC 0013 defines the reporting disposition for rejected, partially recoverable, and diagnostic-only workspace commands.
This RFC can be implemented against the current playback, view, and reporting surfaces, but joint implementations share command receipts and ownership rather than adding adapters in both directions.

## Goals

- Make every published workspace snapshot satisfy open-view, focus, liveness, ownership, and uniqueness invariants.
- Replace raw public aggregate mutators with validated semantic commands.
- Give every command an explicit `Applied`, `NoChange`, `Rejected`, or superseded outcome.
- Prepare every fallible resource before one no-fail workspace commit.
- Advance one workspace revision exactly once for one accepted semantic transition.
- Publish one self-contained revisioned workspace observation after the complete transition.
- Prevent observer reentrancy or observer failure from changing command acceptance.
- Preserve specialized observations only as projections of the canonical workspace revision.
- Keep navigation history consistent with the workspace revision that produced each committed point.
- Move cross-domain reveal and similar application intents above `WorkspaceService`.
- Provide a transaction boundary that session restoration can use without exposing incremental restored state.

## Non-goals

- Merge `WorkspaceService`, `ViewService`, presentation, playback, and persistence into one class.
- Persist runtime `ViewId` or navigation history.
- Change filter, presentation, projection, playback succession, or list-membership semantics.
- Make every view mutation a workspace mutation; selection and projection updates remain view-owned.
- Introduce worker execution for currently bounded workspace commands.
- Define workspace session fields or migration; that belongs to a separate persistence proposal.
- Guarantee a transaction across the library database, audio engine, and workspace aggregate.
- Preserve source compatibility for `addView()`, `setFocusedView()`, `closeView()`, or the current signal set.

## Proposed design

### Canonical aggregate

`WorkspaceService` owns one immutable logical snapshot and one monotonically increasing `WorkspaceRevision`.
The exact public value is finalized during implementation, but it contains enough state to interpret every aggregate transition together:

```cpp
struct WorkspaceSnapshot final
{
  WorkspaceRevision revision;
  std::vector<ViewId> openViews;
  ViewId activeViewId;
  NavigationAvailability navigation;
  PresetCatalogRevision presetRevision;
};
```

Every published snapshot satisfies:

- every open id is nonzero, unique, owned by the bound `ViewService`, and live;
- the active id is invalid exactly when no view is focused;
- a valid active id occurs exactly once in `openViews`;
- every history point committed for the current cursor describes the active view at the revision that accepted it;
- the preset revision names one complete custom-preset collection;
- a view destroyed by workspace closure is absent from the published open set.

`ViewService` remains the owner of view-local state and resources.
Workspace validates view ownership through a narrow role rather than copying view records into the aggregate.

### Command outcomes and receipts

Every public mutation is a semantic command returning a typed result.
A conceptual receipt is:

```cpp
enum class WorkspaceCommandDisposition
{
  Applied,
  NoChange,
};

struct WorkspaceCommitReceipt final
{
  WorkspaceCommandDisposition disposition;
  WorkspaceRevision before;
  WorkspaceRevision after;
  ViewId activeViewId;
};
```

Invalid identity, missing list, view-preparation failure, stale request, and shutdown rejection remain typed errors.
An absent close target, already-focused live view, duplicate open request, or deduplicated history commit returns `NoChange` without advancing revision or emitting an observation.

Commands never report failure after the workspace commit point.
Allocation required by the snapshot, history, receipt, and observation is prepared before commit.
Cleanup that can fail happens before commit or is owned by a no-throw resource handle whose later diagnostic cannot reverse acceptance.

### Private aggregate mutation

Remove public `addView()` and raw `setFocusedView()`.
Replace their production uses with commands such as:

- `open(target, options)` to resolve, create or reuse, add, focus, and optionally commit history;
- `focus(viewId, options)` to validate and focus an already open live view;
- `close(viewId, options)` to validate membership, select fallback focus, and release workspace ownership;
- `applyPresentation(...)` to update the focused view and optionally commit the resulting navigation point;
- an internal candidate-install command used by session restoration.

Tests that need an unusual state build it through a dedicated fixture or candidate API rather than weakening the production contract.
Repository boundary checks reject production calls to removed raw mutators after migration.

### Prepare, commit, and release

Commands that create or replace a view use a candidate lease from `ViewService`.
The candidate owns prepared source leases, projection state, presentation, and its prospective id, but it is not visible through the live view catalog or workspace observations.

The command sequence is:

1. Validate the request against the current workspace and view revisions.
2. Prepare every new view and normalized presentation in detached candidates.
3. Prepare the complete next workspace snapshot, history state, and observation value.
4. Revalidate the revisions and command generation.
5. Adopt all candidates and swap the aggregate through a no-fail commit.
6. Publish one revisioned observation on a later executor turn.

Closing follows the same ownership rule in reverse.
Workspace validates the id and prepares the next snapshot before transferring the live view to a release handle.
Commit removes the view and publishes the fallback focus exactly once; resource teardown then occurs through the handle without a second semantic outcome.

If view resource destruction can produce a recoverable precondition failure, that validation occurs before the workspace commit.
Post-commit destructor diagnostics cannot make the accepted close appear rejected.

### One observation stream

Expose `snapshot()` plus one canonical `onChanged(WorkspaceChanged)` stream.
`WorkspaceChanged` carries the complete new snapshot and a typed cause such as navigation, focus, close, restore, preset edit, or list deletion.

Specialized focus, history-availability, or preset observations may remain temporarily for migration.
After migration they are either removed or mechanically derived from `WorkspaceChanged` and carry its revision.
No specialized signal is an independent state authority.

Observations are queued on the runtime callback executor after command acceptance.
An observer-initiated command enters a later command turn and cannot mutate the snapshot currently being delivered.
Observer exceptions are captured by the application fault boundary and never escape as a command failure.

Consumers compare revisions rather than rereading mutable collaborators to infer ordering.
A newly subscribed consumer first obtains `snapshot()` and then discards any queued observation whose revision is not newer.

### Navigation history transaction

History remains a bounded semantic stack, but its cursor mutation participates in the same workspace command candidate.
A normal navigation prepares the final navigation point from the candidate view state, commits focus and history together, and advances one workspace revision.

Back and forward traversal no longer mutate the live cursor before view restoration.
They select a candidate point without changing current history, prepare or reuse its view, and commit the new cursor, open set, focus, and presentation together.
Failure leaves both history and workspace snapshots unchanged without copying and restoring a live container after the fact.

Presentation changes that intentionally create history likewise prepare the view mutation and history point as one command.
Quick-filter authoring remains governed by RFC 0006 and does not become history merely because a filter request was observed.

### Application navigation owner

Remove the `PlaybackService` dependency from `WorkspaceService`.
Introduce a narrow application-navigation coordinator composed beside workspace and playback inside `AppRuntime`.

Album reveal becomes an explicit application command:

1. Resolve the requested track and target presentation.
2. Commit one workspace navigation transaction to the intended view.
3. Submit reveal intent through the public playback/application state boundary.
4. Return a typed composite outcome identifying whether reveal was accepted for the committed workspace revision.

The operation specification decides whether reveal rejection leaves the successful navigation in place or schedules a compensating navigation.
It never hides that choice behind void commands or logs.
The selected policy is testable and integrates with RFC 0005's removal of reveal signals from transport.

Other cross-domain intents may use this coordinator only when they genuinely compose workspace with another application service.
Ordinary view opening, focus, close, history, and preset commands remain workspace-owned.

### Executor and lifetime boundary

Workspace commands and commits are confined to the runtime callback executor.
The service records or asserts that affinity rather than relying on every caller to know it.

Calls from another thread are rejected or posted through an explicit command port; they do not race the aggregate.
Shutdown closes command admission, invalidates pending candidates, drains accepted observations, disconnects observers, and releases views before `ViewService` and its source cache are destroyed.

This RFC does not require cancellation for bounded synchronous preparation.
If RFC 0006 or later work makes view creation asynchronous, candidate commands carry an intent generation and reject stale completions before the no-fail commit.

## Alternatives

### Validate the three raw mutators in place

Validation would prevent some impossible states, but composite commands and restore would still publish several transitions and fragmented signals.
The raw surface also keeps ownership sequencing at every caller.

### Keep fragmented signals and attach a revision

Revision tags make stale events detectable but do not provide the complete accepted aggregate or prevent observers from seeing intermediate restore and composite-command states.
One canonical observation is simpler to reason about.

### Catch observer exceptions around current signals

Catching prevents an applied command from throwing, but synchronous reentrancy and partial multi-signal observation remain.
Queued post-commit observation gives command acceptance and observation delivery separate proof boundaries.

### Move view ownership entirely into workspace

That would merge view-local projections, filters, selection, and source leases into the aggregate owner.
The selected design retains `ViewService` and adds an explicit candidate/adoption role between the two owners.

### Keep album reveal in workspace

This preserves one convenient call but makes workspace depend on playback and forces it to define cross-domain partial-success policy.
An application-navigation coordinator owns that composition without turning either domain service into a facade for the other.

### Use exceptions for invalid workspace commands

Unknown or stale identities are recoverable application outcomes.
Typed results support reporting policy, tests, and no-change receipts without conflating them with observer faults or invariants.

## Compatibility and migration

This proposal changes internal runtime and consumer APIs but does not change persisted data by itself.
Navigation targets, history semantics, view presentation, and current fallback choices remain behavior oracles unless a later specification deliberately changes them.

Implementation proceeds in phases:

1. Add invariant-focused regression tests for invalid, unknown, destroyed, unopened, duplicate, and already-closed view ids, plus observer reentrancy and observer exceptions.
2. Introduce `WorkspaceRevision`, canonical snapshot, typed command receipts, and one post-commit observation while adapting current commands internally.
3. Add `ViewService` candidate/adoption and release handles, then make navigate, replay, close, and restore use prepared no-fail workspace commits.
4. Migrate TUI, UIModel, GTK, runtime projections, and tests away from raw `addView()` and `setFocusedView()`.
5. Make history cursor, presentation, and preset mutations participate in the same command candidate and remove fragmented state authority.
6. Introduce the application-navigation coordinator, migrate album reveal, and remove `PlaybackService` from workspace construction.
7. Add executor-affinity and production-boundary guardrails, then remove compatibility mutators and signals.

During migration, compatibility signals are emitted only from the canonical committed snapshot so they cannot retain the old independent ordering.

## Validation

- Invalid, unknown, destroyed, or foreign view ids cannot enter `openViews` or become active.
- A valid active view occurs exactly once in the open set after every command and list-deletion callback.
- Closing an absent view returns `NoChange`, does not advance revision, emits nothing, and does not call view destruction.
- Focusing the already-active live view is a no-op unless an explicitly requested history action changes state.
- View preparation, presentation, or replay failure preserves workspace snapshot, view catalog, history cursor, and revision.
- One accepted navigation, focus, close, preset edit, restore, or list-deletion command advances one workspace revision and emits one canonical observation.
- Session restoration with several views is never observable as a sequence of partially open aggregates.
- Every specialized migration event carries and matches the canonical workspace revision.
- Observer-triggered commands execute on a later turn and never alter the snapshot being delivered.
- An observer exception cannot change a command receipt or prevent later valid observers from receiving the committed snapshot.
- Album reveal exposes navigation acceptance and reveal acceptance as typed evidence, with the selected partial-success policy covered explicitly.
- `WorkspaceService` no longer includes, stores, or invokes `PlaybackService`.
- Runtime shutdown rejects new commands and produces no observation after workspace/view teardown.
- Repository searches and boundary tests reject production use of removed raw mutators.
- Existing navigation, history, presentation, session, headless, GTK, and TUI behavior tests remain oracles where this RFC does not intentionally change the contract.
- Completed implementation passes `./ao check`, relevant ThreadSanitizer coverage, and the documentation gate.

## Open questions

- Should `WorkspaceChanged` contain custom preset values directly, an immutable shared catalog, or only a revision plus a snapshot accessor?
- Should close publish before potentially expensive projection teardown, or should candidate release move all expensive work before commit while retaining responsiveness?
- Which current commands need synchronous receipts, and which should use a uniform posted completion port to enforce affinity?
- Should focus history be committed only by semantic navigation commands, or can an explicit focus command request history through options?
- Does album reveal retain successful navigation when playback reveal is rejected, and which reporting disposition makes that partial success clear?
- Should list deletion prune now-unresolvable history points eagerly or retain the current replay-time failure behavior?

## Promotion plan

If accepted and implemented, update the [workspace architecture](../architecture/workspace.md) with candidate ownership, canonical workspace snapshots, the application-navigation coordinator, and executor/lifetime boundaries.
Update the [workspace navigation specification](../spec/workspace/navigation.md) with validated commands, receipts, aggregate invariants, history transactions, observations, reentrancy, and album-reveal disposition.
Update the [workspace session specification](../spec/workspace/session.md) when restore installs one candidate aggregate through this boundary.

Update the [system architecture](../architecture/system-overview.md) only if the application-navigation role changes the public application-runtime map.
Update the [playback architecture](../architecture/playback.md) and applicable playback/navigation specifications when RFC 0005 and this RFC jointly move reveal ownership.
Update presentation specifications only where presentation-plus-navigation becomes one implemented workspace transaction.
Update failure/reporting specifications with the operation disposition selected under RFC 0013.

Add exact public command, snapshot, receipt, revision, and observation fields to a workspace runtime-surface reference if the resulting API needs exhaustive documentation.
Record an ADR if the accepted candidate/adoption boundary, queued observation rule, or application-navigation ownership represents a durable choice among credible alternatives.
