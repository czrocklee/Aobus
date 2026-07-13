---
id: reporting.notification-feed
type: spec
status: current
domain: system
summary: Defines runtime notification feed mutation, observation, identity, revision, and timeout ownership.
---
# Notification feed specification

## Scope

This specification owns the current behavior of the in-memory runtime notification feed exposed by `rt::NotificationService`.
It defines post, update, progress, dismissal, snapshot, revision, and observation behavior.

It does not decide which domain outcome deserves a notification, choose recovery or severity for a subsystem, apply timeout presentation, or define activity-status eligibility.
Those responsibilities belong to the [failure and reporting architecture](../../architecture/failure-and-reporting.md), the reporting producer's subsystem specification, and the [activity-status specification](../presentation/activity-status.md).
The exact notification fields, enum values, and API surface belong to the [notification model reference](../../reference/reporting/notification.md).

## Code boundary

The [system architecture](../../architecture/system-overview.md) places `NotificationService` in application runtime, and the [failure and reporting architecture](../../architecture/failure-and-reporting.md) defines its relationship to failure classification and presentation.
Its public types are under `app/include/ao/rt/`, its implementation is `app/runtime/NotificationService.cpp`, and it has no dependency on UIModel or frontend types.

The service and its synchronous observers participate in the runtime callback domain.
Worker or backend code returns observations through its owning runtime service before mutating the feed.

## Terminology

- A **notification** is one frontend-neutral, user-relevant reporting entry.
- A **feed snapshot** is the ordered entries and revision copied at one service observation.
- A **specific signal** identifies one posted, updated, or dismissed entry.
- The **changed signal** reports that consumers must obtain a fresh feed snapshot.
- A **producer** is the runtime service or application workflow that decides to post or update an entry.

## Invariants

- One `NotificationService` instance owns one independent in-memory feed and one monotonically increasing id sequence for its lifetime.
- Feed order is post order unless entries are removed; updates do not reorder an entry.
- Every effective feed mutation increments `revision` exactly once before observers are invoked.
- A missing-id update or dismissal is a no-op and does not increment revision or emit a signal.
- The service stores producer-supplied severity, content, stickiness, timeout, and activity-presentation data without deriving recovery or presentation policy.
- A timeout is data only; `NotificationService` never starts a timer or automatically dismisses an entry.
- Specific and changed observers see the already-mutated feed when they call `feed()`.
- The service does not inspect `Result`, catch arbitrary failures, deduplicate domain events, or aggregate lower failures automatically.

## State model

The service state is:

- an ordered vector of `NotificationEntry` values;
- a `revision`, initially zero;
- a next-id counter, initially zero;
- four synchronous observer sets: posted, updated, dismissed, and changed.

The first post returns id `1` for a newly constructed service.
Ids are never reused during that service lifetime, including after dismissal.

## Commands and transitions

| Command | Existing target required | State transition | Signals |
|---|---|---|---|
| `post(...)` | No | Append a new entry with the next id; increment revision. | `onPosted(id)`, then `onChanged()`. |
| `updateMessage(id, message)` | Yes | Replace only `message`; increment revision. | `onUpdated(id)`, then `onChanged()`. |
| `updateContent(id, content)` | Yes | Replace the complete content payload; increment revision. | `onUpdated(id)`, then `onChanged()`. |
| `updateProgress(id, progress)` | Yes | Set only `content.optProgress`; increment revision. | `onUpdated(id)`, then `onChanged()`. |
| `clearProgress(id)` | Yes, with progress | Clear only `content.optProgress`; increment revision. | `onUpdated(id)`, then `onChanged()`. |
| `dismiss(id)` | Yes | Remove that entry; increment revision. | `onDismissed(id)`, then `onChanged()`. |
| `dismissAll()` | At least one entry | Remove every entry; increment revision once. | `onChanged()` only. |

`updateMessage` returns `true` only for an existing target.
The other id-based update commands return no status; absence is observable as no mutation and no signal.
Calling `dismissAll()` on an empty feed is a no-op.

`feed()` returns a value snapshot.
Consumers use `revision` to detect change without diffing entry contents, but revision values have service-lifetime scope only.

## Failure and cancellation

The feed commands expose no recoverable `Result` channel and perform no domain failure translation.
Invalid producer data is not normalized by the service; producers remain responsible for conforming to the exact model and their subsystem reporting contract.

The service owns no asynchronous work and has no cancellation state.
Its owner destroys subscriptions and observers before the runtime composition releases the service.

## Frontend observations

UIModel and frontends observe snapshots and signals but do not infer that every entry is an error.
Info, warning, error, progress, rich content, and hidden entries may coexist, and consumers may project a narrower local view without deleting the runtime entry.

Presentation-local suppression is not `dismiss`.
Only an explicit runtime dismissal command removes an entry for all consumers.

## Implementation map

- [`NotificationIds.h`](../../../app/include/ao/rt/NotificationIds.h) defines notification identity.
- [`NotificationState.h`](../../../app/include/ao/rt/NotificationState.h) defines request, entry, content, progress, and feed values.
- [`NotificationService.h`](../../../app/include/ao/rt/NotificationService.h) defines commands and subscriptions.
- [`NotificationService.cpp`](../../../app/runtime/NotificationService.cpp) owns mutation and signal ordering.
- [`CoreRuntime.cpp`](../../../app/runtime/CoreRuntime.cpp) composes the service lifetime.

## Test map

- [`NotificationServiceTest.cpp`](../../../test/unit/runtime/NotificationServiceTest.cpp) proves identity, rich-content storage, update/no-op behavior, signal behavior, dismiss-all behavior, and revision changes.
- [`AppRuntimeTest.cpp`](../../../test/unit/runtime/AppRuntimeTest.cpp) protects runtime composition.

## Related documents

- [Failure and reporting architecture](../../architecture/failure-and-reporting.md)
- [Notification model reference](../../reference/reporting/notification.md)
- [Activity-status specification](../presentation/activity-status.md)
