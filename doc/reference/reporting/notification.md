---
id: reporting.notification-model
type: reference
status: current
domain: system
summary: Enumerates runtime notification identities, enums, fields, defaults, commands, and canonical feed updates.
---
# Notification model reference

## Scope and version

This reference enumerates the current in-process runtime notification model and `NotificationService` API.
The surface is not persisted or transferred between processes and has no independent format version.

Mutation, revision, and observation semantics belong to the [notification feed specification](../../spec/reporting/notification-feed.md).

## Code boundary

The [system architecture](../../architecture/system-overview.md) places the surface in application runtime, while the [failure and reporting architecture](../../architecture/failure-and-reporting.md) defines its reporting role.
The public authority is `app/include/ao/rt/`; UIModel and frontends consume these values, but the types contain no UI toolkit or terminal objects.

## Surface

### Identity and enums

`NotificationId` is a strong `std::uint64_t` value.
`kInvalidNotificationId` is `0`; service-generated ids begin at `1`.

| Enum | Values |
|---|---|
| `NotificationSeverity` | `Info`, `Warning`, `Error` |
| `NotificationTopic` | `General`, `PlaybackSequence`, `PlaybackError` |
| `NotificationProgressMode` | `Indeterminate`, `Fraction` |
| `NotificationActivityPresentation` | `Default`, `DetailOnly`, `Hidden` |
| `NotificationFeedMutationKind` | `Posted`, `MessageUpdated`, `ContentUpdated`, `ProgressUpdated`, `ProgressCleared`, `Dismissed`, `Cleared` |

Each notification enum is a scoped enum with underlying type `std::uint8_t`.
`kDefaultNotificationTemplate` is `notification.message`.

### Progress and action values

| Type | Field | Type | Default |
|---|---|---|---|
| `NotificationProgressState` | `mode` | `NotificationProgressMode` | `Indeterminate` |
|  | `fraction` | `double` | `0.0` |
|  | `label` | `std::string` | empty |
| `NotificationAction` | `id` | `std::string` | empty |
|  | `label` | `std::string` | empty |

### Content

| `NotificationContentState` field | Type | Default |
|---|---|---|
| `topic` | `NotificationTopic` | `General` |
| `templateId` | `std::string` | `notification.message` |
| `title` | `std::string` | empty |
| `iconName` | `std::string` | empty |
| `actions` | `std::vector<NotificationAction>` | empty |
| `optProgress` | `std::optional<NotificationProgressState>` | empty |

The runtime action vector has no model-level maximum.
Presentation adapters may expose a bounded subset; that limit is not a `NotificationContentState` invariant.

### Request and entry

`NotificationRequest` has these fields and defaults:

| Field | Type | Default |
|---|---|---|
| `severity` | `NotificationSeverity` | `Info` |
| `message` | `std::string` | empty |
| `sticky` | `bool` | `false` |
| `optTimeout` | `std::optional<std::chrono::milliseconds>` | empty |
| `activityPresentation` | `NotificationActivityPresentation` | `Default` |
| `content` | `NotificationContentState` | default content |

`NotificationEntry` contains the same fields plus `NotificationId id`, whose default is `0`.
`NotificationFeedState` contains `std::vector<NotificationEntry> entries` and `std::uint64_t revision`, both initially empty or zero.

`NotificationFeedUpdate` contains:

| Field | Type | Default |
|---|---|---|
| `revision` | `std::uint64_t` | `0` |
| `mutationKind` | `NotificationFeedMutationKind` | `Posted` |
| `affectedIds` | `std::vector<NotificationId>` | empty |
| `feedPtr` | `std::shared_ptr<NotificationFeedState const>` | empty |

Every update produced by `NotificationService` has a non-empty `feedPtr`, and `revision == feedPtr->revision`.

### Service API

Construction requires the callback `async::Executor&` and accepts an optional `async::AsyncExceptionHandler` for observer diagnostics.

| Member | Return |
|---|---|
| `feed() const` | `NotificationFeedState` |
| `post(NotificationSeverity, std::string, bool, optional<milliseconds>)` | `NotificationId` |
| `post(NotificationRequest)` | `NotificationId` |
| `updateMessage(NotificationId, std::string)` | `bool` |
| `updateContent(NotificationId, NotificationContentState)` | `void` |
| `updateProgress(NotificationId, NotificationProgressState)` | `void` |
| `clearProgress(NotificationId)` | `void` |
| `dismiss(NotificationId)` | `void` |
| `dismissAll()` | `void` |

The short `post` overload initializes the other request fields to their defaults.

### Observation API

| Member | Handler signature |
|---|---|
| `onFeedUpdated` | `void(NotificationFeedUpdate const&)` |

The member returns an `rt::Subscription` that owns the connection lifetime.
The update reference is valid for the callback invocation; retaining `feedPtr` keeps that immutable snapshot alive.

An observer failure is sent to the injected handler with context `notification feed observer at revision <N>`.
An empty handler disables that diagnostic sink without allowing the observer exception to escape.

## Validation rules

`NotificationService` does not validate or clamp progress fractions, require non-empty messages, resolve action ids, cap action count, or verify template and icon names.
Producers supply semantically valid content, and presentation adapters decide which optional fields they can render.

`sticky` does not override or erase `optTimeout` in the stored model.
Consumers decide how those fields interact in their presentation contract.

## Compatibility and versioning

This is an in-process C++ surface.
Enum ordinals, field layout, and strong-id representation are not storage or IPC compatibility guarantees.
Any surface change requires synchronized implementation, specification, reference, producer, consumer, and test updates.

## Examples

```cpp
auto id = notifications.post(ao::rt::NotificationRequest{
  .severity = ao::rt::NotificationSeverity::Warning,
  .message = "Some tracks could not be imported",
  .sticky = true,
});
```

## Implementation authority

- [`NotificationIds.h`](../../../app/include/ao/rt/NotificationIds.h)
- [`NotificationState.h`](../../../app/include/ao/rt/NotificationState.h)
- [`NotificationService.h`](../../../app/include/ao/rt/NotificationService.h)

## Test authority

- [`NotificationServiceTest.cpp`](../../../test/unit/runtime/NotificationServiceTest.cpp) locks request storage, mutation, immutable-update, reentrancy, and observer-failure behavior.
- Activity projection tests under [`test/unit/uimodel/status/activity/`](../../../test/unit/uimodel/status/activity) lock how the model is narrowed for shared presentation.

## Related documents

- [Notification feed specification](../../spec/reporting/notification-feed.md)
- [Activity-status specification](../../spec/presentation/activity-status.md)
- [Failure and reporting architecture](../../architecture/failure-and-reporting.md)
