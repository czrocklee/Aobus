---
id: presentation.activity-status
type: spec
status: current
domain: presentation
summary: Defines compact and detail projection of notifications and library-task progress.
---
# Activity-status specification

## Scope

This specification owns the UIModel projection consumed by GTK and TUI activity surfaces.
It combines runtime notifications with current library-task progress, chooses compact priority, builds detail rows, and owns presentation-local hiding.

It does not mutate the runtime feed, classify domain failures, or define toolkit layout.
Runtime retention belongs to the [notification feed specification](../reporting/notification-feed.md), and exact fields belong to the [activity-status surface reference](../../reference/presentation/activity-status.md).

## Code boundary

This behavior belongs to the **UIModel** layer in the
[system architecture](../../architecture/system-overview.md), under the
[presentation architecture](../../architecture/presentation.md). Public state
and the view model live in `app/include/ao/uimodel/status/activity/`, and
projection implementation lives in `app/uimodel/status/activity/`. The
implementation may retain source ids for local hiding, but those ids are not
frontend view state and no GTK or FTXUI type crosses this boundary.

## Invariants

- Projection is synchronous and frontend-neutral.
- Every accepted feed update is projected from its immutable snapshot and triggers at most one render callback.
- Active library-task progress owns compact status until completion.
- Without an active task, error notifications outrank warnings. Entries at the selected severity are grouped.
- A newly posted info notification may use compact status only when no warning or error owns it.
- Detail contains warnings, errors, pinned notifications, and active library-task progress.
- Detail notification rows are newest-first.
- Compact and detail dismissal are local UIModel state; they never remove runtime entries.
- Pinned detail rows cannot be hidden locally.
- Suppressed ids are pruned when their entries leave the feed.
- Runtime `Transient` expiry comes from `NotificationService`; UIModel does not create a competing deadline for it.

## Initial state

Construction snapshots the current feed, projects eligible detail, and selects any unsuppressed warning or error compact.
Retained info is not replayed into compact status merely because a new view model subscribes.
The initial state is rendered unless `emitInitialState` is false.

## Notification updates

A posted warning or error immediately participates in compact grouping unless a library task is active.
The group uses the highest present severity, shows the newest entry text when there is one entry, and uses `<N> warnings` or `<N> errors` when there are several.

A posted info replaces the current temporary info compact when no warning or error is eligible.
An effective keyed update refreshes a visible info compact from the same snapshot.
Expiry and automatic history eviction reproject compact and detail together.

Structured notification reports are resolved once through `PresentationTextCatalog` while projecting.
Runtime carries no widget action, icon, or progress presentation state.

## Detail

A notification enters detail when either condition holds:

- severity is `Warning` or `Error`;
- lifetime is `Pinned`.

A non-pinned warning or error row may be hidden from this activity projection.
A pinned row remains visible while its runtime entry exists.
Hiding a detail row also removes it from compact grouping when applicable.

## Library tasks

`LibraryTaskProgressUpdated` supplies a typed operation kind, subject, and fraction.
Projection produces `Processing` compact state and one task detail row without parsing display text.

Completion clears task progress and restores the current notification projection.
When completion is `Succeeded` and no warning or error owns compact status, UIModel shows a temporary success summary:

- zero affected tracks: `Library is up to date`;
- nonzero affected tracks: `Scan complete: <count> added`, using the shared track-count formatter.

Other completion statuses do not synthesize success state.

## Local compact dismissal

`dismissCompact()` records the current notification source ids, clears compact status, and retains detail.
Later notifications with different ids may surface normally.

History and pinned info compact states use the presentation-only `kActivityStatusDefaultAutoDismissTimeout`, currently `5000ms`.
The view model records a steady-clock deadline for such compact state and for synthetic task success.
`autoDismissCompact()` clears that temporary presentation and reprojects warning/error state.
`autoDismissCompactIfDue()` performs the same transition only after the deadline.

Warning and error compact states have no local timeout.
Runtime-transient info also has no local timeout because its authoritative service expiry removes it for all consumers.

## Lifetime and failure

The view model owns its subscriptions and the optional local deadline.
The notification service and optional `LibraryChanges` owner outlive it through application composition.
Projection exposes no recoverable error channel.
If a projection or render callback throws while handling a feed update, the runtime notification observer boundary contains and diagnoses that exception after feed commit.

## Implementation map

- [`ActivityStatusViewState.h`](../../../app/include/ao/uimodel/status/activity/ActivityStatusViewState.h) defines compact and detail values.
- [`ActivityStatusViewModel.h`](../../../app/include/ao/uimodel/status/activity/ActivityStatusViewModel.h) defines construction and local commands.
- [`ActivityStatusFeedProjection.cpp`](../../../app/uimodel/status/activity/ActivityStatusFeedProjection.cpp) implements priority, detail, and suppression.
- [`ActivityStatusViewModel.cpp`](../../../app/uimodel/status/activity/ActivityStatusViewModel.cpp) owns subscriptions and deadlines.

## Test map

- [`ActivityStatusFeedProjectionCompactTest.cpp`](../../../test/unit/uimodel/status/activity/ActivityStatusFeedProjectionCompactTest.cpp) protects compact priority and task transitions.
- [`ActivityStatusFeedProjectionNotificationTest.cpp`](../../../test/unit/uimodel/status/activity/ActivityStatusFeedProjectionNotificationTest.cpp) protects grouping, expiry, updates, and compact suppression.
- [`ActivityStatusFeedProjectionDetailTest.cpp`](../../../test/unit/uimodel/status/activity/ActivityStatusFeedProjectionDetailTest.cpp) protects detail eligibility, ordering, and local hiding.
- [`ActivityStatusViewModelTest.cpp`](../../../test/unit/uimodel/status/activity/ActivityStatusViewModelTest.cpp) protects rendering, deadlines, and subscriptions.

## Related documents

- [Presentation architecture](../../architecture/presentation.md)
- [Failure and reporting architecture](../../architecture/failure-and-reporting.md)
- [Notification feed specification](../reporting/notification-feed.md)
- [Activity-status surface reference](../../reference/presentation/activity-status.md)
