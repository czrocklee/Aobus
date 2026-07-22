---
id: presentation.activity-status-surface
type: reference
status: current
domain: presentation
summary: Enumerates activity-status view-state values, defaults, options, and commands.
---
# Activity-status surface reference

## Scope and version

This reference enumerates the current in-process `ao::uimodel` activity-status API.
It is neither persisted state nor a frontend rendering schema.
Behavior belongs to the [activity-status specification](../../spec/presentation/activity-status.md).

## Code boundary

This surface belongs to the **UIModel** layer in the
[system architecture](../../architecture/system-overview.md), under the
[presentation architecture](../../architecture/presentation.md). Public state
and commands live in `app/include/ao/uimodel/status/activity/`, with projection
implementation in `app/uimodel/status/activity/`. It may consume runtime
notification ids and task events, but contains no GTK or FTXUI types.

## Kind and timeout

`ActivityStatusKind` is a `std::uint8_t` enum with values `Idle`, `Processing`, `Success`, `Info`, `Warning`, and `Error`.

`kActivityStatusDefaultAutoDismissTimeout` is currently `5000ms`.
It applies to temporary UIModel compact presentation, not runtime `Transient` lifetime.

## Compact state

| `ActivityCompactState` field | Type | Default |
|---|---|---|
| `kind` | `ActivityStatusKind` | `Idle` |
| `text` | `std::string` | empty |
| `optProgressFraction` | `std::optional<double>` | empty |
| `dismissible` | `bool` | `false` |
| `hasDetails` | `bool` | `false` |
| `optAutoDismissTimeout` | `std::optional<std::chrono::milliseconds>` | empty |

## Detail state

| Type | Fields and defaults |
|---|---|
| `ActivityDetailItem` | invalid `id`, `Info` severity, empty `message`, `dismissible = false` |
| `ActivityTaskDetail` | empty `message`, `progressFraction = 0.0` |
| `ActivityDetailState` | empty `items`, empty `optLibraryTask` |
| `ActivityStatusViewState` | default `compact`, default `detail` |

`hasDetailContent(detail)` returns true when notification items are non-empty or a library-task detail exists.

## Construction options

| `ActivityStatusViewModelOptions` field | Type | Default |
|---|---|---|
| `libraryChanges` | `rt::LibraryChanges const*` | `nullptr` |
| `clock` | `ActivityStatusClock` | empty; replaced with `steady_clock::now` |
| `emitInitialState` | `bool` | `true` |

The constructor requires `rt::NotificationService&`, an `onRender(ActivityStatusViewState const&)` callback, and optional options.
When `libraryChanges` is present, it subscribes to task progress and completion.

## View-model members

| Member | Return |
|---|---|
| `viewState() const noexcept` | `ActivityStatusViewState const&` |
| `autoDismissCompactIfDue()` | `bool` |
| `autoDismissCompact()` | `void` |
| `dismissCompact()` | `void` |
| `hideDetailNotification(NotificationId)` | `void` |

`autoDismissCompactIfDue()` returns true only when it clears a due temporary compact presentation.
`autoDismissCompact()` is used by a frontend-owned timer that already waited for `optAutoDismissTimeout`.
Neither command mutates the runtime notification feed.

## Current generated text

| Situation | Text |
|---|---|
| Multiple warnings | `<N> warnings` |
| Multiple errors | `<N> errors` |
| Library task `Scanning` | `Scanning library` |
| Library task `Updating` | `Updating library` |
| Library task `Fingerprinting` | `Fingerprinting` plus optional subject |
| Library task `IndexingAudioIdentity` | `Indexing audio identity` plus optional subject |
| Successful completion with zero affected tracks | `Library is up to date` |
| Successful completion with affected tracks | `Scan complete: <formatted track count> added` |

Notification and library-task text is resolved through the [presentation text catalog](text-catalog.md).
Progress subjects do not select behavior by prefix.

## Validation and lifetime

UIModel does not clamp task progress fractions.
Notification ids have service-lifetime scope and must not be persisted.
The notification service and optional library-change source must outlive the view model.

This is an in-process C++ surface without independent versioning.
Frontend adapters update together with UIModel changes.

## Implementation authority

- [`ActivityStatusViewState.h`](../../../app/include/ao/uimodel/status/activity/ActivityStatusViewState.h)
- [`ActivityStatusViewModel.h`](../../../app/include/ao/uimodel/status/activity/ActivityStatusViewModel.h)
- [`ActivityStatusFeedProjection.cpp`](../../../app/uimodel/status/activity/ActivityStatusFeedProjection.cpp)

## Test authority

- [`ActivityStatusViewModelTest.cpp`](../../../test/unit/uimodel/status/activity/ActivityStatusViewModelTest.cpp)

## Related documents

- [Activity-status specification](../../spec/presentation/activity-status.md)
- [Notification model reference](../reporting/notification.md)
- [Presentation architecture](../../architecture/presentation.md)
- [Presentation text catalog](text-catalog.md)
