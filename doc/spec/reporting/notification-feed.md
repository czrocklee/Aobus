---
id: reporting.notification-feed
type: spec
status: current
domain: system
summary: Defines the bounded, executor-affine runtime notification feed and its lifetime behavior.
---
# Notification feed specification

## Scope

This specification owns the in-memory feed exposed by `rt::NotificationService`: posting, keyed replacement, bounded retention, transient expiry, and observation.

It does not decide which application event deserves a notification, perform recovery, or define activity-status presentation.
Those responsibilities belong to the [failure and reporting architecture](../../architecture/failure-and-reporting.md), the producing subsystem, and the [activity-status specification](../presentation/activity-status.md).
The exact C++ surface belongs to the [notification model reference](../../reference/reporting/notification.md).

## Code boundary

This behavior belongs to `NotificationService` in the **application runtime**
layer of the [system architecture](../../architecture/system-overview.md), under
the [failure and reporting architecture](../../architecture/failure-and-reporting.md).
Public declarations live in `app/include/ao/rt/`, and implementation lives in
`app/runtime/NotificationService.cpp`; neither depends on UIModel or frontend
types. Construction, destruction, reads, commands, subscription changes, and
observer callbacks are confined to the runtime callback executor.

## Invariants

- One service owns one ordered feed and one monotonically increasing id sequence.
- Accepted posts append; keyed updates preserve the existing id and position.
- Every effective mutation publishes one `NotificationFeedUpdate` with a complete immutable post-mutation snapshot.
- A semantically unchanged keyed request publishes nothing and does not restart its lifetime.
- Commands return `void`. Invalid or over-capacity requests leave the feed unchanged and write an application-log diagnostic.
- Every request explicitly chooses `Transient(duration)`, `History`, or `Pinned`; severity does not imply lifetime.
- `Transient` entries expire authoritatively after a positive duration. `History` entries are retained but may be evicted for capacity. `Pinned` entries are retained and never evicted automatically.
- A transient period has a generation. Expiry removes an entry only when its id and generation still match.
- Observer-initiated mutations are queued until the current update has reached every observer, so nested publication cannot change an earlier snapshot.
- Observer failure cannot roll back committed state or prevent later updates. It is reported through the owning `async::Runtime`.
- The service does not infer domain failures, aggregate unrelated reports, or resolve presentation text.

## State

The service retains:

- an immutable shared `NotificationFeedState` containing ordered entries;
- the next notification id;
- construction-time `NotificationFeedLimits`;
- one expiry slot per live entry;
- a synchronous observer set;
- a FIFO queue used only while publishing reentrant updates.

The first accepted id is `1`; id `0` is invalid.
Rejected posts do not consume an id.
`feed()` returns a value copy, while each update carries a shared immutable snapshot for callback consumers that need to retain it.

## Commands

| Command | Effect |
|---|---|
| `post(severity, message, lifetime)` | Builds a request and applies `post(request)`. |
| `post(request)` | Validates the request, evicts oldest eligible history if required, appends a new entry, schedules transient expiry when applicable, and publishes `Posted`. |
| `createOrUpdate(key, request)` | Updates the entry with that key without reordering it, or performs a keyed post when absent. An effective replacement publishes `ReportUpdated`. |
| Scheduled expiry | Removes the matching transient id and generation, then publishes `Expired`. Stale callbacks do nothing. |

The update identifies the command target with one `id`.
Its immutable snapshot is the authority for any history eviction caused by that commit.

## Bounds and rejection

Each report key, plain message, and structured report subject/detail must fit `maxTextBytes`.
The complete feed must fit `maxEntries`, `maxHistoryEntries`, and `maxTotalTextBytes`.
A report key must be non-empty and a transient duration must be positive.

Before commit, the service removes oldest `History` entries other than the command target until the candidate fits.
If only transient, pinned, or protected entries remain, the candidate is rejected without partial mutation.
Structured playback reports remain structured; their subject and detail count toward the same text bounds.

## Publication and failure safety

Candidate state, update storage, expiry scheduling, and the next-id watermark are prepared before the authoritative feed changes.
After commit, observer delivery is synchronous.
A mutation requested by an observer appends a later immutable update to the publication queue and is drained only after the current emission returns.

Expiry waits are cancellable tasks, but cancellation is only an optimization.
The id-generation check is the correctness guard when a timer callback was already queued.
Queued callbacks retain a weak control block; service teardown retires that block before cancelling timers, so late callbacks become no-ops.

## Frontend boundary

Runtime expiry changes the feed for every consumer.
UIModel may hide compact or detail presentation locally, but that does not mutate the feed.
The feed contains no frontend actions, icons, progress widgets, presentation modes, or dismissal commands.

## Implementation map

- [`NotificationIds.h`](../../../app/include/ao/rt/NotificationIds.h) defines strong ids and report keys.
- [`NotificationState.h`](../../../app/include/ao/rt/NotificationState.h) defines requests, entries, lifetimes, limits, and updates.
- [`NotificationService.h`](../../../app/include/ao/rt/NotificationService.h) defines the service surface.
- [`NotificationService.cpp`](../../../app/runtime/NotificationService.cpp) owns validation, commit, publication, and expiry.

## Test map

- [`NotificationServiceTest.cpp`](../../../test/unit/runtime/NotificationServiceTest.cpp) protects identity, bounds, history eviction, keyed replacement, observer faults, and reentrant FIFO delivery.
- [`NotificationServiceExpiryTest.cpp`](../../../test/unit/runtime/NotificationServiceExpiryTest.cpp) protects executor-returned expiry, generation checks, cancellation, and teardown safety.

## Related documents

- [Failure and reporting architecture](../../architecture/failure-and-reporting.md)
- [Notification model reference](../../reference/reporting/notification.md)
- [Activity-status specification](../presentation/activity-status.md)
