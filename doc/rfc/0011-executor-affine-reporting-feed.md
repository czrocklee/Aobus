---
id: rfc.0011.executor-affine-reporting-feed
type: rfc
status: draft
domain: reporting
summary: Proposes an executor-confined, revisioned, bounded reporting feed with authoritative retention and coherent activity projection.
depends-on: none
---
# RFC 0011: Executor-affine reporting feed

## Problem

The current runtime notification model has useful semantic separation from logging and recovery, but its state, observation, retention, and UIModel projection contracts do not form one coherent publication boundary.

[`NotificationService`](../../app/include/ao/rt/NotificationService.h) owns a mutable vector and four synchronous signals without receiving an executor or enforcing thread affinity.
[`Signal`](../../app/include/ao/rt/Signal.h) deliberately supports reentrant connection and emission, but it has no synchronization; correctness therefore depends on every producer and subscriber already sharing one undocumented execution domain.
Playback producers usually return through their callback executor, while GTK and TUI also post directly from frontend workflows.
The type boundary does not prove these calls are serialized.

One effective mutation emits a specific signal and then `onChanged`.
If a specific-signal observer throws, `Signal::emit` runs the remaining observers and then rethrows, so `NotificationService` never reaches `onChanged` even though its state and revision already changed.
Consumers can therefore miss the canonical refresh after a committed mutation, and the mutating caller receives an exception after the operation has already taken effect.

[`ActivityStatusViewModel`](../../app/uimodel/status/activity/ActivityStatusViewModel.cpp) subscribes independently to `onPosted` and `onChanged`.
One post consequently projects and renders twice and resets a transient deadline twice.
Updates arrive only through the coarse changed signal: persistent compact state is reprojected, but an already-visible transient info compact is intentionally preserved as a previous value, so an updated source can leave stale compact text while detail reflects the new feed.
Current tests require only that rendering occurs, not one coherent render per feed revision.

Retention is also split across owners.
`NotificationService` stores timeout data but never removes an entry; activity status expires only its local compact projection.
Ordinary info notifications posted by TUI, GTK workflows, and runtime producers can therefore remain in the authoritative feed for the complete runtime lifetime even after no presentation shows them.
The feed and action vectors have no model-level bounds, while GTK independently caps displayed detail rows and actions.

Finally, `DetailOnly` can create detail state without a compact state.
GTK currently opens detail only through a visible compact readout, making that entry unreachable in an idle-hidden layout, while TUI has a direct notification-center command.
The shared model exposes the state but does not define a cross-frontend discoverability contract.

## Dependencies

- Hard: None.
- Conditional: None.
- Integration: [RFC 0005](0005-coherent-playback-boundary.md), [RFC 0012](0012-structured-async-fault-diagnostics.md), [RFC 0013](0013-coherent-application-reporting-policy.md).

RFC 0005 reallocates playback notification ownership and must publish through the same feed transaction if both proposals are implemented.
RFC 0012 can receive observer-fault diagnostics, and RFC 0013 defines which operation owners publish into this feed; neither is required to implement the feed state machine itself.

## Goals

- Make one callback executor the explicit mutation and observation domain for the runtime reporting feed.
- Publish exactly one immutable, revision-correlated update for every effective feed mutation.
- Prevent observer failure or reentrant mutation from splitting committed state from canonical observation.
- Give the runtime feed authoritative, bounded retention so transient entries do not accumulate invisibly for the complete session.
- Replace ambiguous `sticky` plus optional-timeout combinations with a typed lifetime policy.
- Make activity projection consume one feed-update stream and render at most once per accepted feed revision.
- Ensure post, update, expiry, dismissal, and clear operations update compact and detail state from the same snapshot.
- Define cross-frontend discoverability for detail state that has no compact representation.
- Validate model-level bounds independently of renderer-specific row and action limits.

## Non-goals

- Create a process-wide exception handler, recovery manager, or automatic `Result`-to-notification adapter.
- Decide whether a particular domain failure deserves inline validation, a notification, a typed event, or diagnostic logging.
- Move subsystem retry, skip, fallback, or stop policy into `NotificationService`.
- Persist notification history across application runs.
- Require GTK and TUI to use identical widgets, geometry, commands, or visual styling.
- Turn diagnostic logs into feed entries automatically.

## Proposed design

### Explicit execution domain

`CoreRuntime` constructs `NotificationService` with its callback executor and a scheduler suitable for delayed expiry.
Every synchronous feed read, subscription mutation, and feed command requires that executor domain.
The service checks the contract in production builds using the repository's executor-affinity mechanism.

Workers, decoder callbacks, backend callbacks, and other foreign domains do not call the feed directly.
Their owning runtime service first accepts and correlates the observation on the callback executor, applies recovery, and only then submits a reporting command.

The feed does not add an uncorrelated fire-and-forget cross-thread post API merely to hide an affinity violation.
A caller that cannot return to the owner executor must use its typed domain callback boundary.

### One canonical feed update

The four current public observation streams are replaced by one canonical update stream carrying an immutable value equivalent to:

```text
NotificationFeedUpdate
  revision
  mutation kind
  affected notification ids
  complete feed snapshot
```

Mutation kinds distinguish post, update, progress update, progress clear, expiry, explicit dismissal, and clear-all.
Convenience observers may filter this stream, but they do not own additional publication and cannot cause a second UIModel render for the same revision.

The service updates state, increments revision once, captures the immutable update, and enqueues it for delivery as one transaction.
An observer-initiated mutation receives the next revision and is delivered only after the current update finishes, preventing nested emission from changing the snapshot under observers of the earlier revision.

Observer exceptions do not escape a command whose state has already committed and do not block later revisions.
Every still-connected observer runs; failures are reported to an injected diagnostic sink with the notification revision and observer context.
If RFC 0012 is implemented, this uses its structured fault surface; otherwise the feed provides an equivalent narrow injected reporter.

### Typed mutation outcomes

All id-targeted commands return one consistent result shape that distinguishes `Applied`, `Missing`, and `Unchanged`.
Post returns a result containing the new id because request validation can reject invalid input without partially mutating the feed.

Commands do not infer success from emitted callbacks.
The immutable update is an observation of an already accepted mutation, while the command result describes whether that mutation was accepted.

### Authoritative retention

`NotificationRequest` replaces the independent sticky and timeout interpretation with a typed lifetime policy:

- `Transient(duration)` remains authoritative until its service-owned expiry deadline;
- `UntilDismissed` remains until explicit dismissal;
- `WhileActive` is retained while progress or an owning operation is active and requires an explicit completion transition;
- `SessionHistory` enters bounded recent history after completion.

The runtime service owns expiry and removes or transitions entries on the callback executor.
UIModel may animate or locally suppress compact state, but it no longer treats a local timer as authoritative feed lifetime.
All consumers observe the same expiry revision.

The feed has configurable production bounds for recent non-active history, per-entry action count, text sizes, and total retained payload.
Active progress and explicitly persistent error entries are not silently evicted; when their bound would be exceeded, post is rejected or the producer must aggregate by a stable report key.
Eviction of eligible recent history is itself a revisioned mutation.

### Stable report identity and aggregation

A request may carry an optional typed report key owned by the producing workflow.
Posting with an active matching key uses an explicit create-or-update command rather than relying on message equality or a producer-retained raw id.

The feed does not choose aggregation policy.
The operation owner decides that several lower failures describe one report, supplies the key, and updates its semantic summary.

### Coherent activity projection

`ActivityStatusViewModel` consumes only canonical `NotificationFeedUpdate` values and tracks the last accepted revision.
It ignores duplicate or older updates and projects compact, detail, suppression, and deadline state once from the update snapshot.

An update to the source of a visible transient compact refreshes its projected text, rich content, and lifetime according to the accepted update.
An expiry token carries the notification identity and lifetime generation so an old timer cannot expire a replacement or a later update.

Local activity suppression remains distinct from authoritative feed dismissal.
The projection records suppression against notification identity plus content generation and defines whether a semantic update remains hidden or deliberately resurfaces; producers do not depend on accidental id reuse or feed removal to reset local state.

The shared presentation state exposes detail availability independently of compact visibility.
Every interactive frontend provides at least one direct route to eligible detail, or rejects `DetailOnly` as unsupported in that composition.
GTK may add an explicit notification-center action or idle affordance; TUI may retain its command and shortcut.

### Ownership after the change

Domain services and workflow coordinators still decide whether and what to report.
`NotificationService` owns only accepted feed state, identity, lifetime, bounds, revision, and observation.
UIModel owns platform-neutral compact/detail projection and local presentation suppression.
Frontends own rendering and action activation.

## Alternatives

### Add a mutex to the current service

A mutex would prevent some data races but would not define callback affinity, reentrancy, observer-failure semantics, double publication, retention, or coherent UIModel revisions.
Holding it during synchronous callbacks would add deadlock risk; releasing it would still require an event transaction model.

### Keep four signals and document their order

Documented ordering does not make two signals atomic.
It preserves duplicate projection work and still needs a policy for exceptions between signals and mutations triggered during callbacks.

### Keep lifetime entirely in UIModel

That makes different consumers observe different authoritative histories and leaves invisible runtime entries unbounded.
Timeout and history retention are feed-state concerns; animation and local suppression remain presentation concerns.

### Auto-convert every failed `Result`

This would erase operation ownership, produce duplicate reports, and conflate command-scoped validation with persistent cross-view outcomes.
Reporting disposition remains outside this RFC and is addressed by RFC 0013.

### Persist the complete feed

Persisted history introduces schema, privacy, stale-action, and cross-library identity problems without solving the current in-process consistency defects.
Operational diagnostics already belong in logs.

## Compatibility and migration

The proposal changes in-process runtime and UIModel APIs; there is no persisted notification compatibility obligation.
Existing producers migrate from sticky/timeout fields to typed lifetime and from retained ids to typed keys where aggregation is required.

Migration occurs in stages:

1. Introduce canonical update values, affinity checks, and deterministic delivery while adapting existing signals internally.
2. Migrate UIModel to one revision stream and add regression tests for post/update/expiry and one-render-per-revision.
3. Introduce typed lifetime and service-owned expiry, then remove UIModel ownership of authoritative timeout.
4. Add bounds, keyed update, and producer migration.
5. Remove compatibility signals and ambiguous fields after all consumers move.

Playback producer migration coordinates with RFC 0005 if that proposal is active.
Reporting-owner migration coordinates with RFC 0013 but does not wait for its complete operation audit.

## Validation

- Executor tests prove every feed mutation, observer callback, expiry, and UIModel update occurs on the callback executor.
- Foreign-thread mutation fails the explicit affinity contract without partially changing state.
- Every effective command advances revision once and emits exactly one canonical update; missing and unchanged commands emit none.
- A throwing observer does not suppress later observers, the canonical change, or later revisions, and produces one structured diagnostic.
- Reentrant observer commands receive later revisions and never alter an earlier immutable snapshot.
- Post causes one activity render for its feed revision rather than separate posted/changed renders.
- Updating a visible transient refreshes compact and detail content coherently.
- Stale expiry tokens cannot clear updated or replacement activity.
- Transient entries leave the authoritative feed after expiry, and bounded history tests prove memory does not grow with an unbounded sequence of info reports.
- Active/sticky bound exhaustion fails explicitly instead of silently evicting actionable state.
- GTK and TUI tests prove detail-only state has a reachable interaction path.
- Existing playback aggregation, local suppression, progress, action-resolution, and dismissal tests migrate without changing their domain ownership.
- ThreadSanitizer exercises concurrent producer callbacks returning through their runtime owners.
- The completed implementation passes `./ao check` and the documentation gate.

## Open questions

- Should immutable feed snapshots be values, shared immutable state, or revision-addressable views?
- Which lifetime names best distinguish active progress, transient presentation, retained history, and explicit acknowledgement?
- What production bounds apply to history entries, text, actions, and total payload?
- Does an update to a locally suppressed entry resurface by default, or only when the producer increments an explicit presentation generation?
- Should detail-only support be mandatory for every interactive frontend or negotiated as a composition capability?
- Which observer fault reporter is used before RFC 0012 is implemented?

## Promotion plan

If accepted, update the [failure and reporting architecture](../architecture/failure-and-reporting.md) with executor, lifetime, and canonical publication ownership.
Update the [runtime execution architecture](../architecture/runtime-execution.md) with reporting-feed affinity and teardown order.

Update the current [notification feed specification](../spec/reporting/notification-feed.md), [notification model reference](../reference/reporting/notification.md), [activity-status specification](../spec/presentation/activity-status.md), and [activity-status surface reference](../reference/presentation/activity-status.md) with implemented current contracts in the same change as each migration stage.
Update GTK and TUI user/development documentation when detail discoverability or interaction changes.
Record a decision if the accepted lifetime and immutable-publication model has alternatives worth preserving beyond this RFC.
