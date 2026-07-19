---
id: reporting.notification-feed
type: spec
status: current
domain: system
summary: Defines bounded runtime notification feed mutation, observation, identity, revision, typed lifetime, and expiry ownership.
---
# Notification feed specification

## Scope

This specification owns the current behavior of the in-memory runtime notification feed exposed by `rt::NotificationService`.
It defines post, keyed create-or-update, update, progress, typed lifetime, bounded retention, expiry, dismissal, snapshot, revision, and observation behavior.

It does not decide which domain outcome deserves a notification, choose recovery or severity for a subsystem, apply presentation-local hiding, or define activity-status eligibility.
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
- A **feed update** is one immutable, revision-correlated observation containing the mutation kind, affected ids, and complete post-mutation snapshot.
- A **producer** is the runtime service or application workflow that decides to post or update an entry.
- A **report key** is producer-owned semantic correlation used by the explicit keyed create-or-update command; it is not a rendered message or raw feed id.
- A **lifetime generation** identifies a scheduled transient period of one live entry and is never reused by that entry.
- **Authoritative expiry** removes a transient entry from the runtime feed for every consumer, unlike presentation-local suppression.
- **Automatic history eviction** removes the oldest eligible `SessionHistory` entries inside the same committed revision as the command that needs capacity.

## Invariants

- One `NotificationService` instance owns one independent in-memory feed and one monotonically increasing id sequence for its lifetime.
- Feed order is post order unless entries are removed; updates do not reorder an entry.
- Every effective feed mutation increments `revision` exactly once before observers are invoked.
- Every effective mutation publishes exactly one canonical update whose revision equals its snapshot revision.
- A missing-id or semantically unchanged command does not increment revision, publish an update, or restart transient lifetime.
- Every command returns a typed reply distinguishing `Applied`, `Missing`, `Unchanged`, and `Rejected`.
- Every request explicitly selects `Transient(duration)`, `SessionHistory`, or `UntilDismissed`; severity does not imply lifetime.
- A transient duration is positive. A transient post starts at lifetime generation `1`, while a newly posted retained entry starts at generation `0`.
- `Transient` entries leave the authoritative feed after their service-owned deadline. `SessionHistory` and `UntilDismissed` do not schedule expiry.
- Every effective message, content, progress, or progress-clear update to a transient entry advances its generation and restarts its full duration.
- Expiry returns through the callback executor and removes an entry only when both id and generation still match, so a cancelled or already queued old timer cannot expire updated state.
- Feed reads, commands, subscription registration, and observer delivery are confined to the runtime callback executor.
- An observer-initiated mutation receives a later revision and cannot change the immutable snapshot delivered for the current revision.
- Observer failure cannot roll back committed state, escape the initiating command, or prevent another connected observer from receiving the same update.
- Entry count, recent session-history count, action count, every text value, and total retained text bytes remain within the construction-time feed limits.
- Capacity pressure evicts only the oldest eligible `SessionHistory` entries. The target of an update, `Transient` entries, and `UntilDismissed` entries are never silently evicted.
- A command that still cannot fit after eligible history eviction returns `Rejected`, consumes no new id, and leaves feed state and revision unchanged.
- An active report key identifies at most one feed entry. A keyed command updates that entry or creates one when the key is absent.
- The service does not inspect `Result`, catch arbitrary failures, deduplicate domain events, or aggregate lower failures automatically.

## State model

The service state is:

- an ordered vector of `NotificationEntry` values;
- a `revision`, initially zero;
- a next-id counter, initially zero;
- immutable construction-time `NotificationFeedLimits`;
- one cancellable expiry registration per transient entry;
- one synchronous canonical observer set;
- a publication queue and a flag that prevent nested observer delivery.

The first accepted post returns id `1` for a newly constructed service.
Ids are never reused during that service lifetime, including after dismissal.
Rejected creation does not advance the id counter.

The bounded logical text payload is the sum of `std::string::size()` for report keys, resolved message strings or structured-report subject/detail arguments, titles, icon names, action ids and labels, and progress labels.
The feed retains a structured report without resolving or duplicating its catalog message.
The service canonicalizes accepted strings and action vectors instead of retaining producer-reserved spare capacity.

## Commands and transitions

| Command | Existing target required | State transition | Update kind and affected ids |
|---|---|---|---|
| `post(...)` | No | Validate bounds, evict eligible history if required, append a new entry with the next id, and for `Transient` set generation `1` and schedule its duration; increment revision. | `Posted`; the new id. |
| `createOrUpdate(key, request)` | No | Update the active matching key without reordering it, or apply the post transition when absent. Complete request equality is unchanged. | `ReportUpdated` for an existing key or `Posted` for a new key; the resulting id. |
| `updateMessage(id, message)` | Yes | Replace only `message`; restart transient lifetime when applicable; increment revision. | `MessageUpdated`; `id`. |
| `updateContent(id, content)` | Yes | Replace the complete content payload; restart transient lifetime when applicable; increment revision. | `ContentUpdated`; `id`. |
| `updateProgress(id, progress)` | Yes | Set only `content.optProgress`; restart transient lifetime when applicable; increment revision. | `ProgressUpdated`; `id`. |
| `clearProgress(id)` | Yes, with progress | Clear only `content.optProgress`; restart transient lifetime when applicable; increment revision. | `ProgressCleared`; `id`. |
| Scheduled expiry | Matching transient id and generation | Remove that entry; increment revision. | `Expired`; `id`. |
| `dismiss(id)` | Yes | Remove that entry; increment revision. | `Dismissed`; `id`. |
| `dismissAll()` | At least one entry | Remove every entry; increment revision once. | `Cleared`; every removed id in feed order. |

Every command returns `NotificationMutationReply` with its outcome and correlated id.
Post and keyed creation return the accepted new id; rejected creation returns the invalid id.
Id-targeted commands return the requested id for `Applied`, `Missing`, `Unchanged`, or `Rejected`.
Keyed update returns the matching id even when the replacement is unchanged or rejected.
`dismissAll()` uses the invalid id and returns `Unchanged` on an empty feed.
Explicit dismissal and clear-all cancel the affected expiry registrations after commit.
Cancellation is an efficiency mechanism; id-plus-generation validation remains the correctness guard when timer completion already won and queued its callback.
Each newly scheduled transient period advances the entry's lifetime generation, while a retained transition preserves it.
Therefore a callback queued by an earlier transient period cannot match after retained-to-transient reuse of the same keyed entry.

Before commit, a candidate is checked against per-entry text/action limits and the complete feed limits.
When entry count, session-history count, or total text would exceed its bound, the service removes oldest `SessionHistory` candidates except the command target until the candidate fits.
The canonical update reports the command target in `affectedIds` and automatic removals oldest-first in `evictedIds`; both describe one atomic revision.
If no eligible history remains, the complete candidate is rejected.

`feed()` returns a value snapshot.
Consumers use `revision` to detect change without diffing entry contents, but revision values have service-lifetime scope only.

For an effective command, the service first constructs, bounds, and when necessary history-evicts a complete candidate snapshot and canonical update without changing authoritative state.
After every potentially failing allocation succeeds, it installs the snapshot and id watermark together, then synchronously drains publication.
An observer-initiated command appends its update to the same queue and is delivered after every observer of the current revision returns.

## Failure and cancellation

The feed commands expose typed mutation replies rather than a recoverable `Result` channel and perform no domain failure translation.
A non-positive transient duration, empty report key, overlong text value, or excessive action count is rejected without mutation.
Every rejection also writes an error diagnostic directly to the application log because the feed cannot recursively report failure to accept a report; the typed reply remains the authority for caller behavior.
Progress fractions and other producer-domain meaning are not normalized; producers remain responsible for conforming to their subsystem reporting contract.

Candidate construction or transient scheduling failure leaves the feed, revision, next-id watermark, and publication queue unchanged.
After commit, observer failures are reported through the owning `async::Runtime` with the committed revision and are contained at the publication boundary.
Failure of that diagnostic handler is also contained.

Expiry sleeps are cancellable worker tasks, but they do not touch feed state on a worker.
Completion defers a narrow weak callback to the callback executor; service destruction retires that weak control before cancelling registrations, so an already queued callback becomes a no-op.
The owning `async::Runtime` and callback executor outlive the service.
Its owner destroys subscriptions and observers before the runtime composition releases the service.

## Frontend observations

UIModel and frontends observe canonical updates and snapshots but do not infer that every entry is an error.
Info, warning, error, progress, rich content, and hidden entries may coexist, and consumers may project a narrower local view without deleting the runtime entry.

Presentation-local suppression or timeout is neither authoritative expiry nor `dismiss`.
Only an explicit runtime dismissal command removes an entry for all consumers.

## Implementation map

- [`NotificationIds.h`](../../../app/include/ao/rt/NotificationIds.h) defines notification identity.
- [`NotificationState.h`](../../../app/include/ao/rt/NotificationState.h) defines request, entry, content, progress, and feed values.
- [`NotificationService.h`](../../../app/include/ao/rt/NotificationService.h) defines commands and subscriptions.
- [`NotificationService.cpp`](../../../app/runtime/NotificationService.cpp) owns candidate commit, immutable snapshots, publication ordering, affinity enforcement, and observer-fault containment.
- [`CoreRuntime.cpp`](../../../app/runtime/CoreRuntime.cpp) composes the service lifetime.

## Test map

- [`NotificationServiceTest.cpp`](../../../test/unit/runtime/NotificationServiceTest.cpp) proves typed outcomes, identity, rich-content storage, entry and feed bounds, atomic history eviction, keyed aggregation, exact mutation updates, dismissal, revision correlation, observer-fault containment, and reentrant publication ordering.
- [`NotificationServiceExpiryTest.cpp`](../../../test/unit/runtime/NotificationServiceExpiryTest.cpp) proves executor-returned expiry, monotonic generation rejection across keyed lifetime transitions, unchanged-update suppression, cancellation, dismissal collision, retained lifetimes, and queued-callback teardown safety.
- [`AppRuntimeTest.cpp`](../../../test/unit/runtime/AppRuntimeTest.cpp) protects runtime composition.

## Related documents

- [Failure and reporting architecture](../../architecture/failure-and-reporting.md)
- [Notification model reference](../../reference/reporting/notification.md)
- [Activity-status specification](../presentation/activity-status.md)
