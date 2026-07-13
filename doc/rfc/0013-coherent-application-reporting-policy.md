---
id: rfc.0013.coherent-application-reporting-policy
type: rfc
status: draft
domain: reporting
summary: Proposes explicit operation-level reporting dispositions and one semantic reporting owner across runtime and frontends.
depends-on: none
---
# RFC 0013: Coherent application reporting policy

## Problem

The current architecture correctly states that recovery and reporting are separate decisions and that no global manager should convert every failed `Result` into a notification.
The codebase does not yet give individual application operations an equally explicit reporting contract.

Reporting decisions are distributed across layers:

- runtime playback services post and aggregate their own notifications;
- GTK library import/export and tag-edit workflows post directly to the shared runtime feed;
- TUI posts application feedback directly;
- several runtime workspace and playback commands log recoverable failures without a typed caller-visible outcome;
- several GTK list, track, layout, and portal paths log failures locally, while similar operations notify or retain inline status;
- asynchronous invariant faults use separate executor or stderr paths.

Some of these choices are correct.
A best-effort preference save may be diagnostic-only, an editor parse rejection should remain inline, and a playback device loss needs cross-view visibility.
The smell is that the choice is implicit in call-site code rather than owned by an operation specification and tested across frontends.

Direct `NotificationService::post` accepts severity, text, stickiness, timeout, rich content, and actions but carries no declaration of why the feed is the right channel.
Direct log calls likewise do not prove that a user-visible failure was intentionally diagnostic-only.
During refactors, a lower layer can add a report while a frontend keeps its own, producing duplicates, or a frontend can omit a report because another platform happened to implement it.

Message strings are also doing too much work.
Several callers choose severity, aggregation, or displayed context at the same site that formats a diagnostic log.
That makes cross-frontend equivalence and localization difficult and encourages policy to follow text rather than typed operation outcomes.

## Dependencies

- Hard: None.
- Conditional: None.
- Integration: [RFC 0003](0003-library-mutation-pipeline.md), [RFC 0004](0004-scalable-library-tasks.md), [RFC 0005](0005-coherent-playback-boundary.md), [RFC 0011](0011-executor-affine-reporting-feed.md).

RFCs 0003 and 0004 introduce library mutation/task owners whose outcomes must have one reporting disposition.
RFC 0005 reallocates playback application ownership.
RFC 0011 changes the accepted feed command and lifetime surface.
This policy can be implemented against current services, but joint implementations align at those owner boundaries.

## Goals

- Give every user-initiated or background application operation one semantic reporting owner.
- Require the owning specification to declare the primary reporting disposition for each observable failure or completion class.
- Distinguish inline correction, typed observation, shared feed, diagnostic-only, and silent expected outcomes.
- Prevent duplicate user reports from lower layers and frontend adapters for one semantic operation.
- Preserve structured error code, source evidence, operation identity, and aggregation context until final presentation.
- Make cross-frontend reporting behavior equivalent where the underlying operation is shared.
- Keep platform-specific presentation and command-scoped CLI output at application leaves.
- Provide a reviewable migration path for existing direct notification and log call sites.

## Non-goals

- Introduce a global recovery manager or universal error bus.
- Require every recoverable failure to enter the notification feed.
- Make log output user-visible application state.
- Move frontend-local validation, file chooser cancellation, widget errors, or platform integration diagnostics into runtime automatically.
- Standardize exact user-visible strings, widget layout, terminal rendering, or CLI phrasing in this RFC.
- Replace domain-specific result, event, state, and recovery contracts with one generic report type.

## Proposed design

### Reporting dispositions

Current behavior specifications gain an operation reporting matrix using these semantic dispositions:

- `Inline`: the initiating surface retains typed rejection or validation state for immediate correction;
- `Observed`: the domain publishes typed state/event needed by consumers, with no automatic user report;
- `Feed`: the outcome requires cross-view or post-workflow visibility in the runtime reporting feed;
- `DiagnosticOnly`: the product continues or falls back and only operator/developer evidence is required;
- `SilentExpected`: cancellation, no-op, absence, stale-observation rejection, or another expected path intentionally produces no report;
- `FatalLeaf`: an unexpected invariant escapes to an application leaf, which emits structured diagnostics and may present one generic fatal/internal-error message.

One operation may combine a typed observation with one feed summary or diagnostic event when the specification says why both are required.
The matrix identifies one primary user-facing disposition and prevents independent layers from each deciding to report.

### One semantic reporting owner

The runtime domain service that owns recovery also owns reporting disposition for shared application behavior.
It may delegate final text/template selection to UIModel, but it publishes a typed operation outcome with enough context to make that mapping deterministic.

Frontend workflows own reporting only for platform-local operations such as portal interaction or toolkit resource failure.
When a GTK or TUI workflow merely adapts a shared runtime operation, it renders or forwards the runtime-owned outcome and does not create an independent semantic report.

Core mechanisms never post notifications.
They return results or typed events to the domain owner.
Lower layers may add diagnostic context but cannot select user severity, retention, navigation, or retry actions.

### Typed operation report intent

Shared operations that select `Feed` produce a frontend-neutral report intent rather than calling `NotificationService` from arbitrary layers.
The value carries typed fields equivalent to:

```text
OperationReport
  operation kind
  operation/correlation identity
  severity class
  lifetime class
  template or message key
  typed display arguments
  optional progress
  optional typed actions
  optional aggregation key
  diagnostic correlation id
```

The type does not carry widget objects, terminal commands, exception pointers, or recovery callbacks.
Actions identify registered application commands and are resolved at the frontend boundary.

A narrow reporting adapter owned by application runtime accepts an already-classified `OperationReport` and submits the corresponding feed command.
It does not accept arbitrary `Error` and cannot infer a disposition from error code or message.

Operations selecting `Inline`, `Observed`, or `DiagnosticOnly` do not construct this value merely for uniformity.

### Structured diagnostics without duplication

Recoverable errors retain `Error::Code`, message, and source evidence through the reporting owner.
When a feed report and diagnostic log both apply, the owner emits one structured diagnostic with the operation and correlation id, then one user summary that omits internal detail.

Lower layers do not each log the same propagated failure unless they add unique boundary evidence that would otherwise be lost.
Logging a recoverable failure does not substitute for returning its typed outcome or publishing its declared feed report.

Unexpected async faults follow RFC 0012 when implemented and remain diagnostic-only by default.
Expected cancellation remains silent unless a specification deliberately defines a user-visible cancellation completion.

### Cross-frontend policy

For each runtime operation available to multiple frontends, one specification defines equivalent semantic visibility.
GTK and TUI may render a feed report differently, but one platform cannot silently log a shared failure while another reports it unless the difference is an explicit capability or interaction constraint.

CLI command-scoped failures continue to use stderr and exit status because the CLI invocation itself is the initiating surface.
A long-running CLI mode may render progress directly without inserting command-local history into `NotificationService`.

### Migration inventory

Implementation begins with a generated or reviewed inventory of direct notification posts and error/critical logs under runtime and frontends.
Each call site is classified by operation owner and disposition before code moves.

The first migration targets shared domain operations currently reported in frontend adapters:

- library scan/import/export completion and failure;
- track/list mutation completion and failure;
- workspace restore/checkpoint failures;
- playback recovery and route/device failures.

Platform integration paths such as portal cancellation, MPRIS availability, optional CSS, and toolkit resource failures remain frontend-owned but gain explicit `DiagnosticOnly`, `Inline`, or `FatalLeaf` classification.

### Guardrails

Public core targets continue to forbid runtime notification dependencies.
Normal frontend code uses typed runtime/UIModel reporting adapters rather than raw feed post when adapting a shared operation.

A lightweight repository check may restrict direct `NotificationService::post` to declared reporting-owner files and tests.
The guardrail verifies ownership location, not semantic correctness; specifications and behavior tests remain the authority.

## Alternatives

### Central ErrorManager

A manager that accepts arbitrary exceptions and results would have to rediscover domain recovery, aggregation, lifetime, and user-actionability.
It would become a dependency inversion violation and a message-parsing policy hub.

### Let every frontend decide

This preserves platform flexibility but duplicates shared policy and makes missing or double reports likely when new frontends or workflows are added.

### Put all recoverable failures in the feed

Inline validation, best-effort preferences, stale callbacks, no-ops, and CLI command rejection have different lifetimes and interaction needs.
A universal feed would create noise and retain outcomes that are meaningful only to one initiating surface.

### Use logs as the reporting contract

Logs are not observable application state, do not prove user visibility, and cannot expose typed actions or cross-view lifecycle.

### Add only documentation

Operation matrices are necessary but insufficient while arbitrary lower and frontend layers can still post duplicate reports.
Typed intent and owner-bound adapters make the policy enforceable in code and tests.

## Compatibility and migration

The proposal changes internal runtime/UIModel/frontend APIs and reporting behavior but introduces no required persisted format.
User-visible severity, lifetime, aggregation, and duplicate behavior may change as call sites move to one owner.

Migration is incremental by operation family:

1. Inventory and classify current reporting call sites without changing behavior.
2. Add operation reporting matrices to current specifications.
3. Introduce typed report intent and the owner-bound adapter.
4. Migrate one domain family at a time with cross-frontend tests.
5. Add direct-post guardrails after legitimate owners are enumerated.
6. Remove duplicated frontend reports and redundant lower-layer logs.

If RFC 0011 is active, the adapter targets its canonical feed commands and typed lifetime.
If not, it adapts to the current `NotificationService` surface without changing the ownership policy.

## Validation

- The call-site inventory accounts for every direct runtime/frontend notification post and every recoverable error/critical log in selected domains.
- Each migrated operation specification declares disposition for rejection, partial success, completion, cancellation, stale observation, and invariant fault where applicable.
- Behavior tests prove exactly one primary user report for one semantic operation across nested core/runtime/frontend layers.
- Inline validation never leaks into global history unless the specification explicitly selects both channels.
- Diagnostic-only fallback produces structured log evidence and no feed entry.
- Feed dispositions preserve operation identity, severity, lifetime, typed arguments, action ids, and aggregation key through the adapter.
- Playback skip walks and scalable library tasks aggregate lower failures at the owner-selected granularity.
- GTK and TUI tests prove equivalent semantic visibility for shared operations.
- CLI tests retain command stderr and exit behavior without requiring the runtime feed.
- Cancellation and stale observation paths remain silent unless explicitly specified.
- A repository guard rejects new direct feed posts outside declared owners after migration.
- The completed implementation passes `./ao check` and the documentation gate.

## Open questions

- Which layer owns final message/template selection for each shared report: runtime, UIModel, or a localization-aware presentation adapter?
- Is one general `OperationReport` value preferable to small domain-specific report intents sharing common concepts?
- Which current log-only failures are genuinely best-effort and which represent missing user visibility?
- Should diagnostic correlation ids be generated per command, per async task, or only when both user and diagnostic channels are selected?
- Which direct notification owners remain legitimate after domain migration?
- How should non-interactive or temporarily hidden frontends advertise report-action capabilities?

## Promotion plan

If accepted, update the [failure and reporting architecture](../architecture/failure-and-reporting.md) with the reporting-disposition vocabulary, owner rules, and typed intent boundary.
Update the [outcome channel specification](../spec/failure/outcome-channel.md) only where command outcomes require an explicit reporting handoff; it remains the authority for failure classification rather than user visibility.

Add a reporting-disposition specification and update each affected library, playback, presentation, and frontend specification with its operation matrix.
Update the [workspace navigation](../spec/workspace/navigation.md), [workspace session](../spec/workspace/session.md), and [GTK active-library lifecycle](../spec/linux-gtk/active-library-lifecycle.md) specifications for the operation dispositions they own.
Update the notification feed specification/reference only for implemented report-intent fields and adapter behavior, coordinating with RFC 0011 when applicable.
Add development guidance and repository checks for reporting-owner review, direct feed posts, structured diagnostics, and cross-frontend behavior tests.
Record a decision if the accepted typed-intent shape and owner model has durable alternatives worth preserving.
