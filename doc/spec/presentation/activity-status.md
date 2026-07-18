---
id: presentation.activity-status
type: spec
status: current
domain: presentation
summary: Defines the shared UIModel projection of runtime notifications and library-task activity into compact and detail state.
---
# Activity-status specification

## Scope

This specification owns the frontend-neutral UIModel projection that combines runtime notifications and active library-task progress into compact activity status and detail state.
It defines eligibility, priority, grouping, ordering, presentation-local compact lifetime, and suppression.

It does not own failure classification, recovery, the authoritative notification feed, library-task execution, or GTK/TUI rendering geometry.
Those facts belong to the [failure and reporting architecture](../../architecture/failure-and-reporting.md), [notification feed specification](../reporting/notification-feed.md), [library task specification](../library/runtime/task-execution.md), and frontend documents.
The exact projection fields, enum values, constants, and helper surfaces belong to the [activity-status reference](../../reference/presentation/activity-status.md).

## Code boundary

The [system architecture](../../architecture/system-overview.md) places this policy in UIModel, and the [presentation architecture](../../architecture/presentation.md) defines its relation to runtime state and frontends.
Public state and view-model types live under `app/include/ao/uimodel/status/activity/`; projection implementation lives under `app/uimodel/status/activity/`.

The projection may depend on runtime notification and library-change values.
It does not dismiss the runtime feed, mutate library work, choose recovery, or depend on GTK, FTXUI, or another platform type.

## Terminology

- **Compact state** is the single inline activity summary shared by GTK and TUI.
- **Detail state** is the narrower list of eligible notification rows plus optional active library-task detail.
- A **persistent compact** is a warning or error summary that does not receive an automatic timeout.
- A **presentation-transient compact** is a locally retained info or successful-completion summary with a UIModel-owned automatic timeout.
- A **runtime-transient notification** is a feed entry whose authoritative removal is scheduled by `NotificationService`; it does not receive a second UIModel deadline.
- **Local suppression** hides a notification only from the activity projection while its runtime feed entry remains authoritative for other consumers.
- **Openable detail** means the detail state has a notification row or active library-task row; each frontend additionally controls how users reach it.

## Invariants

- Runtime notification entries remain unchanged by all activity-status projection commands.
- `ActivityStatusViewModel` consumes only canonical notification-feed updates, accepts each increasing feed revision at most once, and renders at most once for that accepted revision.
- Active library-task progress owns the compact slot until the task completes.
- Without an active task, unsuppressed `Default` error notifications outrank warnings; warnings outrank transient info and completion; an empty projection is idle.
- Notifications at the selected highest persistent severity are grouped; lower severities remain in detail but do not contribute to that compact group.
- `DetailOnly` notifications may enter detail but never compact, and `Hidden` notifications enter neither activity surface.
- Plain `Default` info without until-dismissed lifetime, progress, title, icon, or actions may create compact text but does not create a detail row.
- Detail ordering places progress notifications first and orders each progress/non-progress partition by newest notification id first.
- Compact or detail dismissal is local suppression and never calls `NotificationService::dismiss`.
- Until-dismissed or progress detail rows cannot be locally dismissed from activity status.
- Local suppression is pruned after the corresponding notification leaves the runtime feed, so a future entry cannot inherit stale suppression.
- Updating the source of a visible transient info compact refreshes compact and detail state from the same immutable feed snapshot.
- Authoritative runtime expiry arrives as a feed revision and removes that notification from compact and detail projection together.
- Presentation-local timeout expiry reprojects the current feed and reveals the highest unsuppressed persistent compact, if any, without removing retained feed state.

## State model

The projection retains:

- current compact and detail view state;
- whether a library task is active and its most recent progress;
- a notification posted while task progress owns compact state;
- locally suppressed compact notification ids;
- locally suppressed detail notification ids;
- in `ActivityStatusViewModel`, the last accepted notification-feed revision;
- in `ActivityStatusViewModel`, an optional steady-clock deadline only for the current presentation-transient compact.

Initial projection includes current eligible detail rows and current unsuppressed persistent warning/error compact state.
Historical plain info does not become compact merely because a view model subscribes after it was posted.

## Commands and transitions

### Notification feed updates

A newly posted `Default` warning or error participates immediately in persistent projection unless library-task progress currently owns compact state.
A newly posted `Default` info becomes transient compact only when no unsuppressed persistent warning or error already owns compact.
While a task is active, compact-eligible notification posts are deferred from compact presentation; detail is still refreshed.

The view model ignores an update with an empty snapshot, a snapshot revision that differs from the update revision, or a revision no newer than the last accepted revision.
One accepted update is projected once from its immutable snapshot and invokes the render callback at most once.

Non-post feed mutations always refresh detail.
They preserve a still-valid info or success transient when no persistent compact supersedes it, and remove a notification-derived transient when its source leaves the feed.
Message, content, progress, and progress-clear mutations affecting the source of the visible info transient reproject that compact from the same snapshot instead of retaining stale text or content.
An `Expired` mutation follows the same source-removal path as explicit dismissal but remains distinguishable in the canonical runtime update.

### Persistent compact grouping

The projection selects the highest severity present among unsuppressed compact-eligible warning and error entries.
With one selected entry it uses that entry's title when non-empty, otherwise its message.
With multiple selected entries it reports the severity group count.
The compact state retains every selected source id so later suppression or feed removal can reproject correctly.

### Library task progress and completion

Task progress produces processing compact state, exposes the supplied fraction, and adds task detail.
Compact display normalizes messages beginning with `Scanning:` or `Updating:` to a stable library-level summary while detail retains the full message.

Completion removes task progress and first reprojects persistent notifications.
Only when no persistent compact remains does it show a transient success summary; zero additions and nonzero additions have distinct summary text.

### Detail eligibility

A notification is detail-eligible when it is not `Hidden` and one of these conditions holds:

- presentation is `DetailOnly`;
- severity is `Warning` or `Error`;
- its lifetime is `UntilDismissed`;
- it carries progress;
- it has a non-empty title or icon name;
- it has at least one action.

Detail copies notification severity, title, message, icon, progress, actions, and derived dismissibility into frontend-neutral activity values.
It also exposes whether any notification or library-task progress is active.

### Local dismissal

Compact dismissal records every current compact source id as locally suppressed, resets compact to idle, and retains eligible detail.
A later new persistent notification can reappear; suppressing an error does not suppress a later warning with a different id.

Detail dismissal accepts only a currently eligible entry that is neither until-dismissed nor carrying progress.
It hides that row and removes it from persistent compact grouping when applicable, without changing the runtime feed.

### Runtime and presentation lifetime

`NotificationService` owns authoritative `Transient(duration)` expiry.
An info entry with that runtime lifetime produces no `optAutoDismissTimeout`; its `Expired` feed revision removes it for every consumer.
This prevents GTK and TUI event-loop timing from becoming competing authorities over feed retention.

`SessionHistory` and `UntilDismissed` info may still be compact-presented temporarily.
Those retained entries use `kActivityStatusDefaultAutoDismissTimeout` as presentation-only suppression, and the runtime feed entry remains after the local compact disappears.
Until-dismissed info remains detail-eligible and not locally dismissible.
Warning and error compact state has no presentation auto-dismiss timeout regardless of lifetime.

Successful library-task completion is synthetic UIModel state rather than a notification entry and also uses the shared presentation timeout.
`ActivityStatusViewModel` therefore retains its injected steady clock and explicit expiry checks for these presentation-only cases.

## Failure and cancellation

Projection operations are synchronous value transformations and expose no recoverable error channel.
Malformed optional presentation data is narrowed by exact helper policy; it does not trigger subsystem recovery or mutate the producer.

The view model owns subscriptions only for its lifetime.
Runtime and library-change owners outlive the view model according to the application composition order.
An exception escaping notification projection or its render callback is an observer fault contained and diagnosed by the runtime notification publication boundary after the feed revision has committed.

## Frontend observations

GTK opens its activity detail popover only when detail exists and compact is visible, so a `DetailOnly` entry has no GTK compact affordance in an idle-hidden layout.
TUI provides an explicit notification-center command in addition to the compact status slot, so it may expose detail independently of compact visibility.

Frontend action adapters resolve activity action ids against their own action registry or command surface.
Unknown or invisible actions are omitted, disabled actions retain their reason, empty producer labels may be replaced by a registered label, and activation does not imply feed dismissal.

GTK currently bounds one rendering pass to four notification rows and two resolved actions per row.
These are GTK adapter limits, not runtime notification-model limits.

## Implementation map

- [`ActivityStatusViewState.h`](../../../app/include/ao/uimodel/status/activity/ActivityStatusViewState.h) defines shared projection values and helpers.
- [`ActivityStatusViewModel.h`](../../../app/include/ao/uimodel/status/activity/ActivityStatusViewModel.h) defines subscriptions, transient expiry, and presentation-local commands.
- [`ActivityStatusFeedProjection.cpp`](../../../app/uimodel/status/activity/ActivityStatusFeedProjection.cpp) implements priority, eligibility, ordering, and suppression.
- [`ActivityStatusViewModel.cpp`](../../../app/uimodel/status/activity/ActivityStatusViewModel.cpp) composes runtime notification and library-task observations.
- GTK [`ActivityStatusWidget.cpp`](../../../app/linux-gtk/status/ActivityStatusWidget.cpp) and TUI [`StatusBar.cpp`](../../../app/tui/StatusBar.cpp) plus [`NotificationCenterPanel.cpp`](../../../app/tui/NotificationCenterPanel.cpp) are frontend adapters.

## Test map

- [`ActivityStatusFeedProjectionCompactTest.cpp`](../../../test/unit/uimodel/status/activity/ActivityStatusFeedProjectionCompactTest.cpp) proves compact priority, task ownership, deferral, and completion.
- [`ActivityStatusFeedProjectionNotificationTest.cpp`](../../../test/unit/uimodel/status/activity/ActivityStatusFeedProjectionNotificationTest.cpp) proves grouping, runtime-transient timeout exclusion, source removal, and local suppression.
- [`ActivityStatusFeedProjectionDetailTest.cpp`](../../../test/unit/uimodel/status/activity/ActivityStatusFeedProjectionDetailTest.cpp) proves eligibility, ordering, rich fields, progress, actions, and clearability.
- [`ActivityStatusFeedProjectionPresentationTest.cpp`](../../../test/unit/uimodel/status/activity/ActivityStatusFeedProjectionPresentationTest.cpp) proves `Default`, `DetailOnly`, and `Hidden` behavior.
- [`ActivityStatusViewModelTest.cpp`](../../../test/unit/uimodel/status/activity/ActivityStatusViewModelTest.cpp) proves one render per accepted feed revision, coherent transient updates, presentation-only injected-clock expiry, and library task events.
- [`ActivityStatusWidgetTest.cpp`](../../../test/unit/linux-gtk/status/ActivityStatusWidgetTest.cpp) protects the GTK adapter and action-resolution boundary.

## Related documents

- [Presentation architecture](../../architecture/presentation.md)
- [Failure and reporting architecture](../../architecture/failure-and-reporting.md)
- [Notification feed specification](../reporting/notification-feed.md)
- [Activity-status reference](../../reference/presentation/activity-status.md)
- [Selection-summary specification](selection-summary.md)
- [TUI interaction specification](../tui/interaction.md)
