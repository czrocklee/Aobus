---
id: rfc.0011.executor-affine-reporting-feed
type: rfc
status: implemented
domain: reporting
summary: Proposed the executor-affine bounded notification feed and coherent activity projection.
depends-on: none
---
# RFC 0011: Executor-affine reporting feed

## Disposition

Implemented in a narrower form.
The current service is executor-affine, bounded, generation-checks transient expiry, publishes immutable snapshots, contains observer faults, queues reentrant publication, and supports producer-owned report keys.

The later simplification removed feed revisions, mutation replies, dismissal commands, generic rich content, notification actions and progress, presentation modes, and the unused topic taxonomy.
Activity status retains only severity/lifetime projection, local hiding, and real library-task progress.
The [notification feed specification](../spec/reporting/notification-feed.md) and [activity-status specification](../spec/presentation/activity-status.md) are the current authorities; the proposal text below is historical.

## Problem

The runtime notification model has useful semantic separation from logging and recovery, and its feed mechanics now form the bounded executor-affine state machine proposed here.
The remaining gaps are content authority and cross-frontend discoverability.

The first five migration stages replaced the previous mutable four-signal publication boundary with an executor-affine immutable update stream and one-revision UIModel projection, added authoritative transient expiry, bounded retained history and content, and adopted typed shared report/progress presentation.
`SessionHistory` now yields oldest-first under capacity pressure, actionable entries are rejected rather than silently removed, and keyed producers no longer retain raw feed ids.

Finally, `DetailOnly` can create detail state without a compact state.
GTK currently opens detail only through a visible compact readout, making that entry unreachable in an idle-hidden layout, while TUI has a direct notification-center command.
The shared model exposes the state but does not define a cross-frontend discoverability contract.

The former inert `NotificationContentState::templateId` and English library-progress prefix tests were removed by RFC 0030.
Shared playback reports now retain one closed template plus typed arguments, and library progress retains a typed operation kind plus subject until UIModel resolves the catalog text.
The remaining generic title, action, progress-label, and icon fields still require an explicit future policy if shared runtime producers begin using them.

## Dependencies

- Hard: None.
- Conditional: None.
- Integration: [RFC 0005](0005-coherent-playback-boundary.md), [RFC 0012](0012-structured-async-fault-diagnostics.md), [RFC 0013](0013-coherent-application-reporting-policy.md), [RFC 0030](0030-structured-presentation-vocabulary.md).

RFC 0005 reallocates playback notification ownership and must publish through the same feed transaction if both proposals are implemented.
RFC 0012 can receive observer-fault diagnostics, and RFC 0013 defines which operation owners publish into this feed; neither is required to implement the feed state machine itself.
RFC 0030 owns UIModel template resolution and shared presentation copy when structured report content is adopted.

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
- Ensure every accepted content field is consumed and remove message-prefix parsing from feed projection policy.
- Preserve structured report identity and typed arguments through the feed when RFCs 0013 and 0030 provide them, without making the feed a text catalog.

## Non-goals

- Create a process-wide exception handler, recovery manager, or automatic `Result`-to-notification adapter.
- Decide whether a particular domain failure deserves inline validation, a notification, a typed event, or diagnostic logging.
- Move subsystem retry, skip, fallback, or stop policy into `NotificationService`.
- Persist notification history across application runs.
- Require GTK and TUI to use identical widgets, geometry, commands, or visual styling.
- Turn diagnostic logs into feed entries automatically.
- Standardize exact shared user-visible copy or select a localization framework; RFC 0030 owns that presentation boundary.

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

The four previous public observation streams are replaced by one canonical update stream carrying an immutable value equivalent to:

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
The implemented RFC 0012 handler can carry the original exception and a short observer context, while feed-specific revision evidence remains owned by this proposal.

### Typed mutation outcomes

All commands now return one `NotificationMutationReply` shape that distinguishes `Applied`, `Missing`, `Unchanged`, and `Rejected` and carries the correlated id.
Post returns the new id only when request validation and bounded candidate construction accept the input without partially mutating the feed.

Commands do not infer success from emitted callbacks.
The immutable update is an observation of an already accepted mutation, while the command result describes whether that mutation was accepted.

### Authoritative retention

`NotificationRequest` replaces the independent sticky and timeout interpretation with a typed lifetime policy.
The implemented subset is:

- `Transient(duration)` remains authoritative until its service-owned expiry deadline;
- `UntilDismissed` remains until explicit dismissal;
- `SessionHistory` remains retained for the current session and is the class eligible for bounded recent-history eviction in stage 4.

`WhileActive` remains deferred until a real producer and explicit completion transition land together; the current model does not carry an inert lifetime case.

The runtime service owns expiry and removes or transitions entries on the callback executor.
UIModel may animate or locally suppress compact state, but it no longer treats a local timer as authoritative feed lifetime.
All consumers observe the same expiry revision.
Timer generations remain monotonic across keyed transitions through retained lifetimes, because cancellation cannot retract a callback that already reached the callback executor.

The feed has configurable production bounds for total entries, recent non-active history, per-entry action count, text sizes, and total retained text payload.
Production defaults are 256 total entries, 128 session-history entries, 8 actions per entry, 4096 bytes per text value, and 256 KiB total text.
Active progress and explicitly persistent error entries are not silently evicted; when their bound would be exceeded, post is rejected or the producer must aggregate by a stable report key.
Eligible recent-history eviction is reported inside the same atomic revision as the command that required it.
Rejected commands also produce a direct application-log diagnostic so best-effort reporting callers cannot lose the rejection silently.

### Stable report identity and aggregation

An explicit create-or-update command accepts a typed report key owned by the producing workflow.
An active matching key updates the existing entry rather than relying on message equality or a producer-retained raw id; an absent key creates a new entry.

The feed does not choose aggregation policy.
The operation owner decides that several lower failures describe one report, supplies the key, and updates its semantic summary.

### Structured content without inert fields

The feed stores one accepted content representation, never parallel resolved and unresolved authorities.
Frontend-local or otherwise deliberately preformatted reports use a bounded resolved-content value.
Shared operation reports introduced with RFCs 0013 and 0030 use a typed template id plus bounded typed arguments, and UIModel resolves that value for compact and detail presentation.
A request cannot carry both forms.

The structured variant is added only in the same implementation slice as its UIModel consumer.
Until then, the unused `templateId` field is removed rather than preserved speculatively.
After the structured path lands, every template id reaching an interactive composition has either an exhaustive catalog entry or an explicit unknown-template fallback and diagnostic.

Progress and summary semantics remain typed through the snapshot.
Library activity carries an operation kind such as scan or metadata update instead of requiring prefix tests on a label.
Counts, completion disposition, and playback skip/stop facts remain arguments rather than preformatted pluralized messages.
The feed treats these values as opaque accepted state: it does not translate them, select copy, or branch recovery and aggregation policy on rendered text.

Bounds apply to template ids, typed argument payload, and resolved-content strings.
Toolkit icon names and markup are not semantic feed identity; platform-neutral icon/action kinds remain structured until a frontend adapter selects native presentation.

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
Existing producers have migrated from sticky/timeout fields to explicit typed lifetime.
Migration from retained ids to typed keys remains part of aggregation work where required.

Migration occurs in stages:

1. Completed: introduce canonical update values, affinity checks, deterministic delivery, and remove the unconsumed compatibility signals.
2. Completed: migrate UIModel to one revision stream and add regression tests for post/update and one-render-per-revision.
3. Completed: introduce typed lifetime and service-owned expiry, generation-check stale timers, and restrict UIModel deadlines to presentation-only state.
4. Completed: add typed mutation outcomes, bounds, atomic history eviction, keyed update, and playback producer migration.
5. Completed: remove the inert template field and English prefix parsing, then add structured playback reports and library-progress kinds atomically with RFC 0030's UIModel resolver.
6. Remove the remaining ambiguous fields after all consumers move.

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
- Active/until-dismissed bound exhaustion fails explicitly instead of silently evicting actionable state.
- GTK and TUI tests prove detail-only state has a reachable interaction path.
- Existing playback aggregation, local suppression, progress, action-resolution, and dismissal tests migrate without changing their domain ownership.
- Content tests prove every stored field affects projection or is absent; no compatibility field is retained only for a hypothetical future consumer.
- Scan/update activity and playback summaries select typed kinds and arguments without matching English title/message prefixes.
- Structured reports retain template identity and typed arguments through immutable snapshots and resolve exactly once in UIModel.
- ThreadSanitizer exercises concurrent producer callbacks returning through their runtime owners.
- The completed implementation passes `./ao check` and the documentation gate.

## Open questions

- Should immutable feed snapshots be values, shared immutable state, or revision-addressable views?
- Should a future real progress owner add `WhileActive`, or express completion by an explicit update into one of the three implemented lifetimes?
- Does an update to a locally suppressed entry resurface by default, or only when the producer increments an explicit presentation generation?
- Should detail-only support be mandatory for every interactive frontend or negotiated as a composition capability?
- Should observer diagnostics use the existing RFC 0012 exception handler directly or a feed-specific adapter that adds revision context?

## Promotion plan

If accepted, update the [failure and reporting architecture](../architecture/failure-and-reporting.md) with executor, lifetime, and canonical publication ownership.
Update the [runtime execution architecture](../architecture/runtime-execution.md) with reporting-feed affinity and teardown order.

Update the current [notification feed specification](../spec/reporting/notification-feed.md), [notification model reference](../reference/reporting/notification.md), [activity-status specification](../spec/presentation/activity-status.md), and [activity-status surface reference](../reference/presentation/activity-status.md) with implemented current contracts in the same change as each migration stage.
Update GTK and TUI user/development documentation when detail discoverability or interaction changes.
Record a decision if the accepted lifetime and immutable-publication model has alternatives worth preserving beyond this RFC.
