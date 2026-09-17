---
id: presentation.activity-status
---
# Activity status

## Scope

This contract defines the shared UIModel projection consumed by interactive activity surfaces.
It combines runtime notifications with current library-task progress, chooses compact priority, builds detail rows, and owns presentation-local hiding.

It does not mutate the runtime feed, classify domain failures, or define toolkit layout.
Runtime retention belongs to the [notification feed contract](../failure/notification-feed.md).
The [view-state header](../../../app/include/ao/uimodel/status/activity/ActivityStatusViewState.h) and [view-model header](../../../app/include/ao/uimodel/status/activity/ActivityStatusViewModel.h) own declarations and construction options; this page owns the projection contract.

## Code boundary

This behavior belongs to the **UIModel** layer in the
[system architecture](../overview.md), under the
[presentation architecture](README.md). Public state
and the view model live in `app/include/ao/uimodel/status/activity/`, and
projection implementation lives in `app/uimodel/status/activity/`. The
implementation may retain source ids for local hiding, but those ids are not
frontend view state and no GTK or FTXUI type crosses this boundary.

## Invariants

- Projection is synchronous and frontend-neutral.
- Every accepted feed update is projected from its immutable snapshot and triggers at most one render callback.
- Active library-task progress owns compact status until its status-free finished pulse.
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
Construction receives the composition root's text catalog; the projection retains an immutable catalog value.
Replacing it with `setTextCatalog()` rewords the current state without restarting the same notification's dismissal deadline.

## Notification updates

A posted warning or error immediately participates in compact grouping unless a library task is active.
The group uses the highest present severity, shows the newest entry text when there is one entry, and uses a localized severity/count summary when there are several.

A posted info replaces the current temporary info compact when no warning or error is eligible.
An effective keyed update refreshes a visible info compact from the same snapshot.
Expiry and automatic history eviction reproject compact and detail together.

Structured notification reports are resolved once through feature presentation functions over `MessageCatalog` while projecting.
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
UIModel does not clamp the supplied progress fraction.
Task and notification wording comes from the [text catalog](text-catalog.md), not from parsing progress subjects.

`LibraryJobs::onProgressFinished()` clears task progress and restores the current notification projection.
The pulse carries no task outcome, count, or message and never synthesizes success state.
The awaited workflow caller owns success, warning, Error, and cancellation presentation.
If a warning or error arrived while progress was active, finishing progress makes that retained notification eligible for compact status.

## Local compact dismissal

`dismissCompact()` records the current notification source ids, clears compact status, and retains detail.
Later notifications with different ids may surface normally.

History and pinned info compact states use the presentation-only `kActivityStatusDefaultAutoDismissTimeout`, currently `5000ms`.
The view model records a steady-clock deadline for such compact state.
The injected clock defaults to `steady_clock::now`.
`autoDismissCompact()` clears that temporary presentation and reprojects warning/error state; a frontend using it directly must already have waited for the timeout.
`tryAutoDismissCompactIfDue()` performs the same transition only after the deadline and returns true only when it clears a due presentation.
`compactAutoDismissRemaining()` exposes the remaining lifetime without changing it, clamps a due deadline to zero, and returns `nullopt` when no local deadline exists.
Hosts that coalesce renders must schedule from this deadline, since identical final
presentations can have different deadlines after intermediate transitions. An early
timer callback retains a future wakeup while a local deadline remains outstanding.

Warning and error compact states have no local timeout.
Runtime-transient info also has no local timeout because its authoritative service expiry removes it for all consumers.

## Lifetime and failure

The view model owns its subscriptions and the optional local deadline.
The notification service and optional `LibraryJobs` owner outlive it through application composition.
Notification ids have service-lifetime scope and must not be persisted.
This in-process API has no independent version; frontend adapters evolve together with UIModel rather than treating its C++ layout as a persisted or rendering schema.
Projection exposes no recoverable error channel.
Feed handlers are ordinary callables behind the notification signal's owning fatal boundary.
They handle expected fallible work locally; an exception that escapes instead is diagnosed and aborted by that owner.

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
- [`AppKitActivityExpirationScenario.mm`](../../../test/integration/macos/AppKitActivityExpirationScenario.mm) controls the model clock and native timer delivery to protect coalesced A-B-A transitions, early wakeup rearming, and cancellation without a playback timer.

## Related documents

- [Presentation architecture](README.md)
- [Failure and reporting architecture](../failure/README.md)
- [Notification feed specification](../failure/notification-feed.md)
