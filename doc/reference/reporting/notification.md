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
`NotificationReportKey` is a strong owning `std::string` used only by explicit keyed create-or-update commands.

| Enum | Values |
|---|---|
| `NotificationSeverity` | `Info`, `Warning`, `Error` |
| `NotificationTopic` | `General`, `PlaybackSequence`, `PlaybackError` |
| `NotificationProgressMode` | `Indeterminate`, `Fraction` |
| `NotificationActivityPresentation` | `Default`, `DetailOnly`, `Hidden` |
| `NotificationLifetimeKind` | `Transient`, `SessionHistory`, `UntilDismissed` |
| `NotificationFeedMutationKind` | `Posted`, `ReportUpdated`, `MessageUpdated`, `ContentUpdated`, `ProgressUpdated`, `ProgressCleared`, `Expired`, `Dismissed`, `Cleared` |
| `NotificationMutationOutcome` | `Applied`, `Missing`, `Unchanged`, `Rejected` |

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

The runtime action vector is bounded by `NotificationFeedLimits::maxActionsPerEntry`.
Presentation adapters may expose a smaller subset.

### Lifetime

`NotificationLifetime` is an immutable policy value constructed through these factories:

| Factory | Meaning |
|---|---|
| `transient(duration)` | Authoritative feed entry expires after the positive duration. The default argument is `kDefaultNotificationTransientDuration`, currently `5000ms`. |
| `sessionHistory()` | Entry remains in the in-memory session feed until an explicit dismissal or clear. |
| `untilDismissed()` | Entry remains authoritative until explicit dismissal and is presented as not locally dismissible in activity detail. |

`kind()` returns `NotificationLifetimeKind`.
`optTransientDuration()` returns the duration only for `Transient` and an empty optional for retained lifetimes.
Severity and lifetime are independent.

### Request and entry

`NotificationRequest` has these fields and defaults:

| Field | Type | Default |
|---|---|---|
| `severity` | `NotificationSeverity` | `Info` |
| `message` | `std::string` | empty |
| `lifetime` | `NotificationLifetime` | none; every request must select one explicitly |
| `activityPresentation` | `NotificationActivityPresentation` | `Default` |
| `content` | `NotificationContentState` | default content |

`NotificationEntry` contains the same fields plus `NotificationId id`, whose default is `0`, optional `NotificationReportKey optReportKey`, and `std::uint64_t lifetimeGeneration`, whose default is `0`.
Its value default for `lifetime` is `sessionHistory()` so an empty feed/value snapshot remains constructible; producer requests do not inherit that default.
Service-produced transient entries start at generation `1`.
Every later scheduled transient period increments the generation, while retained lifetime transitions preserve it, so a live entry never reuses an earlier timer generation.
`NotificationFeedState` contains `std::vector<NotificationEntry> entries` and `std::uint64_t revision`, both initially empty or zero.

`NotificationFeedUpdate` contains:

| Field | Type | Default |
|---|---|---|
| `revision` | `std::uint64_t` | `0` |
| `mutationKind` | `NotificationFeedMutationKind` | `Posted` |
| `affectedIds` | `std::vector<NotificationId>` | empty |
| `evictedIds` | `std::vector<NotificationId>` | empty |
| `feedPtr` | `std::shared_ptr<NotificationFeedState const>` | empty |

Every update produced by `NotificationService` has a non-empty `feedPtr`, and `revision == feedPtr->revision`.
`evictedIds` lists automatic session-history eviction from oldest to newest inside that same revision; explicit dismissal and expiry remain represented by their mutation kind and `affectedIds`.

`NotificationMutationReply` contains `NotificationMutationOutcome outcome`, defaulting to `Rejected`, and `NotificationId id`, defaulting to the invalid id.

### Feed limits

`NotificationFeedLimits` has these production defaults:

| Field | Default | Meaning |
|---|---:|---|
| `maxEntries` | `256` | Maximum entries of every lifetime combined. |
| `maxSessionHistoryEntries` | `128` | Maximum recent `SessionHistory` entries after automatic eviction. |
| `maxActionsPerEntry` | `8` | Maximum actions accepted in one content value. |
| `maxTextBytes` | `4096` | Maximum `std::string::size()` for each report key or text field. |
| `maxTotalTextBytes` | `256 * 1024` | Maximum summed text bytes retained by the complete feed. |

Total text includes report keys, messages, template ids, titles, icon names, action ids and labels, and progress labels.
Limits are immutable for one service instance and may be reduced through constructor injection for deterministic tests.

### Service API

Construction requires the owning `async::Runtime&` and optionally accepts `NotificationFeedLimits`.
The service uses its callback executor for affinity and expiry delivery, its sleeper for delayed expiry, and its exception handler for observer diagnostics.
The runtime must outlive the service.

| Member | Return |
|---|---|
| `feed() const` | `NotificationFeedState` |
| `post(NotificationSeverity, std::string, NotificationLifetime)` | `NotificationMutationReply` |
| `post(NotificationRequest)` | `NotificationMutationReply` |
| `createOrUpdate(NotificationReportKey, NotificationRequest)` | `NotificationMutationReply` |
| `updateMessage(NotificationId, std::string)` | `NotificationMutationReply` |
| `updateContent(NotificationId, NotificationContentState)` | `NotificationMutationReply` |
| `updateProgress(NotificationId, NotificationProgressState)` | `NotificationMutationReply` |
| `clearProgress(NotificationId)` | `NotificationMutationReply` |
| `dismiss(NotificationId)` | `NotificationMutationReply` |
| `dismissAll()` | `NotificationMutationReply` |

The short `post` overload initializes activity presentation and content to their defaults, but still requires explicit lifetime policy.
`createOrUpdate` requires a non-empty key, replaces the complete request-shaped state of a matching entry without reordering it, and posts a new entry when the key is absent.

### Observation API

| Member | Handler signature |
|---|---|
| `onFeedUpdated` | `void(NotificationFeedUpdate const&)` |

The member returns an `async::Subscription` that owns the connection lifetime.
The update reference is valid for the callback invocation; retaining `feedPtr` keeps that immutable snapshot alive.

An observer failure is sent through the owning runtime with context `notification feed observer at revision <N>`.
The runtime's injected handler or stderr fallback owns the final diagnostic without allowing the observer exception to escape.
Rejected commands also write an application-log error diagnostic identifying the mutation, correlated id, and rejection reason.

## Validation rules

`NotificationService` rejects empty report keys, non-positive transient durations, action vectors beyond the configured count, and any report key or text field beyond the configured byte count.
It also applies total entry, recent-history, and retained-text bounds as specified by the notification feed.
It does not clamp progress fractions, require non-empty messages, resolve action ids, or interpret template and icon names.
Producers supply semantically valid content, and presentation adapters decide which optional fields they can render.

`NotificationLifetime::transient` accepts a duration value, and `NotificationService` requires it to be greater than zero when scheduling.
The lifetime type makes retained and timed states mutually exclusive rather than asking consumers to reconcile independent flags.

## Compatibility and versioning

This is an in-process C++ surface.
Enum ordinals, field layout, and strong-id representation are not storage or IPC compatibility guarantees.
Any surface change requires synchronized implementation, specification, reference, producer, consumer, and test updates.

## Examples

```cpp
auto const reply = notifications.createOrUpdate(
  ao::rt::NotificationReportKey{"library.scan.current"},
  ao::rt::NotificationRequest{
    .severity = ao::rt::NotificationSeverity::Warning,
    .message = "Some tracks could not be imported",
    .lifetime = ao::rt::NotificationLifetime::sessionHistory(),
  });
```

## Implementation authority

- [`NotificationIds.h`](../../../app/include/ao/rt/NotificationIds.h)
- [`NotificationState.h`](../../../app/include/ao/rt/NotificationState.h)
- [`NotificationService.h`](../../../app/include/ao/rt/NotificationService.h)

## Test authority

- [`NotificationServiceTest.cpp`](../../../test/unit/runtime/NotificationServiceTest.cpp) locks request storage, typed mutation replies, configured bounds, atomic history eviction, keyed aggregation, immutable updates, reentrancy, and observer-failure behavior.
- [`NotificationServiceExpiryTest.cpp`](../../../test/unit/runtime/NotificationServiceExpiryTest.cpp) locks typed lifetime scheduling, unchanged suppression, keyed lifetime transitions, expiry revisions, generations, cancellation races, and teardown.
- Activity projection tests under [`test/unit/uimodel/status/activity/`](../../../test/unit/uimodel/status/activity) lock how the model is narrowed for shared presentation.

## Related documents

- [Notification feed specification](../../spec/reporting/notification-feed.md)
- [Activity-status specification](../../spec/presentation/activity-status.md)
- [Failure and reporting architecture](../../architecture/failure-and-reporting.md)
