---
id: reporting.notification-model
type: reference
status: current
domain: system
summary: Enumerates the runtime notification values, limits, updates, and service API.
---
# Notification model reference

## Scope and version

This reference enumerates the current in-process `ao::rt` notification surface.
It is not persisted and has no independent format version.
Behavior belongs to the [notification feed specification](../../spec/reporting/notification-feed.md).

## Code boundary

This surface belongs to the **application runtime** layer in the
[system architecture](../../architecture/system-overview.md), under the
[failure and reporting architecture](../../architecture/failure-and-reporting.md).
Public values and the service live in `app/include/ao/rt/`, and
`app/runtime/NotificationService.cpp` implements validation, publication, and
expiry. UIModel and frontends may consume the public values, but runtime types
contain no presentation-layer objects.

## Identity and enums

`NotificationId` is a strong `std::uint64_t`; `kInvalidNotificationId` is `0`.
`NotificationReportKey` is a strong owning string used only for keyed create-or-update.

| Enum | Values |
|---|---|
| `NotificationSeverity` | `Info`, `Warning`, `Error` |
| `NotificationLifetimeKind` | `Transient`, `History`, `Pinned` |
| `NotificationFeedMutationKind` | `Posted`, `ReportUpdated`, `Expired` |
| `NotificationReportTemplate` | `PlaybackTrackOpenFailed`, `PlaybackDecodeFailed`, `PlaybackRouteActivationFailed`, `PlaybackDeviceLost`, `PlaybackSequenceFinished`, `PlaybackTracksSkipped`, `PlaybackStoppedAfterFailures`, `PlaybackStoppedForTrack` |

Each enum uses `std::uint8_t` as its underlying type.

## Message

`NotificationMessage` is `std::variant<std::string, NotificationReport>`.
The string alternative is already resolved text.
The structured alternative retains playback report identity until UIModel resolves it through the [presentation text catalog](../presentation/text-catalog.md).

| `NotificationReport` field | Type | Default |
|---|---|---|
| `templateId` | `NotificationReportTemplate` | `PlaybackSequenceFinished` |
| `trackId` | `TrackId` | invalid id |
| `subject` | `std::string` | empty |
| `detail` | `std::string` | empty |
| `count` | `std::size_t` | `0` |

## Lifetime

`NotificationLifetime` is created through these factories:

| Factory | Meaning |
|---|---|
| `transient(duration)` | Expires authoritatively after a positive duration. The default is `kDefaultNotificationTransientDuration`, currently `5000ms`. |
| `history()` | Retained for the session and eligible for oldest-first capacity eviction. |
| `pinned()` | Retained and excluded from automatic history eviction. |

`kind()` returns the lifetime kind.
`optTransientDuration()` returns a duration only for `Transient`.

## Request, entry, and feed

| Type | Fields |
|---|---|
| `NotificationRequest` | `severity`, `message`, `lifetime` |
| `NotificationEntry` | `id`, `optReportKey`, `severity`, `message`, `lifetime` |
| `NotificationFeedState` | `entries` |

`NotificationRequest::severity` defaults to `Info`; `message` defaults to an empty string alternative.
`lifetime` has no default, so every request chooses it explicitly.

An empty `NotificationEntry` uses invalid id, no key, info severity, empty text, and `history()`.
Timer generations are private service control state and do not appear in snapshots.

## Limits

| `NotificationFeedLimits` field | Default |
|---|---:|
| `maxEntries` | `256` |
| `maxHistoryEntries` | `128` |
| `maxTextBytes` | `4096` |
| `maxTotalTextBytes` | `256 KiB` |

The per-text limit applies independently to report keys, plain messages, and structured report subject/detail strings.
The total counts the retained string payload of all entries.

## Update

`NotificationFeedUpdate` contains:

| Field | Type | Default |
|---|---|---|
| `mutationKind` | `NotificationFeedMutationKind` | `Posted` |
| `id` | `NotificationId` | invalid id |
| `feedPtr` | `std::shared_ptr<NotificationFeedState const>` | null |

Service-produced updates always carry a non-null immutable snapshot.
`id` identifies the post, keyed update, or expiry.
The update reference itself is callback-scoped; consumers copy `feedPtr` when retaining the snapshot.

## Service API

`NotificationService(async::Runtime&, NotificationFeedLimits = {})` binds the service to the runtime callback executor.
The runtime must outlive the service.

| Member | Return |
|---|---|
| `feed() const` | `NotificationFeedState` |
| `post(NotificationSeverity, std::string, NotificationLifetime)` | `void` |
| `post(NotificationRequest)` | `void` |
| `createOrUpdate(NotificationReportKey, NotificationRequest)` | `void` |
| `onFeedUpdated(handler)` | `async::Subscription` |

All calls and subscription teardown belong to the callback executor.
Commands report invalid or rejected candidates through the application log and do not expose a reply DTO.

## Compatibility

This is an in-process C++ API.
Enum ordinals and field layout are not persistence guarantees, and callers update together with runtime.

## Implementation authority

- [`NotificationIds.h`](../../../app/include/ao/rt/NotificationIds.h)
- [`NotificationState.h`](../../../app/include/ao/rt/NotificationState.h)
- [`NotificationService.h`](../../../app/include/ao/rt/NotificationService.h)

## Test authority

- [`NotificationServiceTest.cpp`](../../../test/unit/runtime/NotificationServiceTest.cpp)
- [`NotificationServiceExpiryTest.cpp`](../../../test/unit/runtime/NotificationServiceExpiryTest.cpp)

## Related documents

- [Notification feed specification](../../spec/reporting/notification-feed.md)
- [Activity-status surface reference](../presentation/activity-status.md)
- [Failure and reporting architecture](../../architecture/failure-and-reporting.md)
