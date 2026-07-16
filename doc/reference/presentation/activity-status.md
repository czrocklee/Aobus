---
id: presentation.activity-status-surface
type: reference
status: current
domain: presentation
summary: Enumerates activity-status view-state types, defaults, helper mappings, and UIModel command surface.
---
# Activity-status surface reference

## Scope and version

This reference enumerates the current in-process `ao::uimodel` activity-status state and view-model surface.
The exact structs are C++ presentation API, not persisted state or a frontend rendering schema.

Projection priority, detail eligibility, suppression, and timeout behavior belong to the [activity-status specification](../../spec/presentation/activity-status.md).

## Code boundary

The [system architecture](../../architecture/system-overview.md) places this surface in UIModel, and the [presentation architecture](../../architecture/presentation.md) defines its allowed runtime and frontend dependencies.
The public authority is `app/include/ao/uimodel/status/activity/`; it may contain runtime notification ids and values but no GTK or FTXUI types.

## Surface

### Kind and CSS mapping

`ActivityStatusKind` is a scoped enum with underlying type `std::uint8_t`.

| `ActivityStatusKind` | `activityStatusKindCssClass` |
|---|---|
| `Idle` | `ao-activity-status-idle` |
| `Processing` | `ao-activity-status-processing` |
| `Success` | `ao-activity-status-success` |
| `Info` | `ao-activity-status-info` |
| `Warning` | `ao-activity-status-warning` |
| `Error` | `ao-activity-status-error` |

The shared default auto-dismiss timeout is `kActivityStatusDefaultAutoDismissTimeout`, currently `5000ms`.

### Action values

| Type | Fields |
|---|---|
| `ActivityActionDescriptor` | `std::string id`, `std::string label` |
| `ActivityActionAvailability` | `bool visible`, `bool enabled`, `std::string label`, `std::string disabledReason` |
| `ActivityResolvedActionState` | `std::string id`, `bool enabled`, `std::string label`, `std::string disabledReason` |

`ActivityActionAvailabilityResolver` has signature equivalent to:

```cpp
ActivityActionAvailability(std::string_view id, std::string_view producerLabel);
```

`resolveActivityActionStates` accepts descriptors, a resolver, and `maxVisibleActions`.
It preserves source order, skips invisible or empty resolved labels, includes disabled actions and reasons, and stops after the requested visible limit.
An empty resolver or zero limit returns an empty vector.

### Compact state

| `ActivityCompactState` field | Type | Default |
|---|---|---|
| `kind` | `ActivityStatusKind` | `Idle` |
| `text` | `std::string` | empty |
| `optProgressFraction` | `std::optional<double>` | empty |
| `groupedCount` | `std::size_t` | `0` |
| `persistent` | `bool` | `false` |
| `dismissible` | `bool` | `false` |
| `hasDetails` | `bool` | `false` |
| `optAutoDismissTimeout` | `std::optional<std::chrono::milliseconds>` | empty |
| `sourceNotificationIds` | `std::vector<rt::NotificationId>` | empty |

### Detail state

| Type | Fields |
|---|---|
| `ActivityDetailItem` | `id`, `severity`, `title`, `message`, `iconName`, `sticky`, `dismissible`, `optProgressMode`, `progressFraction`, `progressLabel`, `actions` |
| `ActivityTaskDetail` | `message`, `progressFraction` |
| `ActivityDetailState` | `items`, `optLibraryTask`, `hasActiveProgress` |
| `ActivityStatusViewState` | `compact`, `detail` |

`ActivityDetailItem` defaults to invalid id `0`, info severity, empty text and actions, non-sticky, non-dismissible, no progress mode, and progress fraction `0.0`.
`ActivityTaskDetail` defaults to empty message and fraction `0.0`.

`hasDetailContent(detail)` returns true when `detail.items` is non-empty or `detail.optLibraryTask` has a value.
`hasActiveProgress` is separate: it reports active library-task or notification progress, not mere detail presence.

### View-model construction and commands

`ActivityStatusViewModelOptions` contains:

| Field | Type | Default |
|---|---|---|
| `libraryChanges` | `rt::LibraryChanges const*` | `nullptr` |
| `clock` | `ActivityStatusClock` | empty, replaced by `steady_clock::now` |
| `emitInitialState` | `bool` | `true` |

The constructor requires `rt::NotificationService&`, an `onRender(ActivityStatusViewState const&)` callback, and optional options.

| Member | Return |
|---|---|
| `viewState() const noexcept` | `ActivityStatusViewState const&` |
| `hasPendingAutoDismiss() const noexcept` | `bool` |
| `expireTransientIfDue()` | `bool` |
| `expireTransient()` | `void` |
| `dismissCompact()` | `void` |
| `dismissDetailNotificationFromActivity(NotificationId)` | `void` |
| `handleLibraryTaskProgress(std::string, double)` | `void` |
| `handleLibraryTaskCompleted(LibraryTaskCompleted const&)` | `void` |

`expireTransientIfDue()` returns `true` only when it performs an expiry transition.

### Current generated compact text

| Situation | Text |
|---|---|
| Multiple selected info notifications | `<N> notifications` |
| Multiple selected warnings | `<N> warnings` |
| Multiple selected errors | `<N> errors` |
| Library progress beginning `Scanning:` | `Scanning library` |
| Library progress beginning `Updating:` | `Updating library` |
| Successful completion with zero affected tracks | `Library is up to date` |
| Successful completion with nonzero affected tracks | `Scan complete: <N> tracks added` |

These strings are current UIModel output and are not localization keys.
`CompletedWithIssues`, `Failed`, and `Cancelled` clear task progress without synthesizing a success message; notification projection may then surface an owning warning or error.

## Validation rules

The UIModel does not clamp progress fractions.
Action resolution requires a visible result with a non-empty resolved label; the resolver owns registry lookup and label fallback.

Notification ids in local projection state have service-lifetime scope.
Callers must not persist them or use them after the notification owner has been replaced.

## Compatibility and versioning

This is an in-process C++ surface without independent versioning.
Field layout and enum ordinals are not persistence guarantees.
Frontend adapters update together with UIModel changes.

## Examples

```cpp
auto options = ao::uimodel::ActivityStatusViewModelOptions{
  .libraryChanges = &runtime.library().changes(),
};
```

## Implementation authority

- [`ActivityStatusViewState.h`](../../../app/include/ao/uimodel/status/activity/ActivityStatusViewState.h)
- [`ActivityStatusViewModel.h`](../../../app/include/ao/uimodel/status/activity/ActivityStatusViewModel.h)
- [`ActivityStatusFeedProjection.cpp`](../../../app/uimodel/status/activity/ActivityStatusFeedProjection.cpp)

## Test authority

- Projection tests under [`test/unit/uimodel/status/activity/`](../../../test/unit/uimodel/status/activity) lock exact state and helper outputs.
- [`ActivityStatusWidgetTest.cpp`](../../../test/unit/linux-gtk/status/ActivityStatusWidgetTest.cpp) protects GTK consumption of this surface.

## Related documents

- [Activity-status specification](../../spec/presentation/activity-status.md)
- [Notification model reference](../reporting/notification.md)
- [Presentation architecture](../../architecture/presentation.md)
