---
id: rfc.0026.generation-bound-platform-requests
type: rfc
status: draft
domain: presentation
summary: Proposes a generation-bound lifetime protocol for asynchronous platform requests before their completions can affect a frontend, runtime, library, or external artifact.
depends-on: none
---
# RFC 0026: Generation-bound platform requests

## Problem

GTK platform adapters start asynchronous operations whose toolkit callback can outlive the object and application state that initiated them.
The most direct examples are native `Gtk::FileDialog` folder/open/save requests in `ImportExportCoordinator`.
Their callbacks capture `this`, then may call import/export workflows or request active-library replacement when the native operation completes.
The coordinator has a default destructor and no Gio cancellation handle, weak owner state, request id, or runtime/library generation check.

Several distinct lifetimes can change while such a request is pending:

- the dialog, coordinator, window, or application can close;
- the active `AppRuntime` and `MusicLibrary` can be replaced;
- a newer request can supersede the older request;
- the owning workflow can be cancelled while the platform callback still arrives; and
- an asynchronous resource/export result can become stale relative to current playback or MPRIS metadata.

Current code often protects the later worker workflow with a lifetime scope, but that scope starts only after the platform callback has dereferenced its owner and handed off a path.
The native callback boundary itself lacks a reusable protocol.

Deferring active-library replacement until after a chooser callback returns avoids synchronously destroying the callback's current stack, but it does not prove that a delayed callback still addresses the same window/runtime generation.
Likewise, a callback being delivered on the GTK main context proves executor affinity, not semantic freshness.

The problem is broader than file dialogs but narrower than a new "platform services" subsystem.
Portals, native choosers, D-Bus requests, MPRIS-derived artifacts, and future desktop integrations remain platform adapters under presentation and interactive lifecycle ownership.
They need one lifetime/evidence pattern so each adapter does not invent a different weak-pointer convention.

## Dependencies

- Hard: None.
- Conditional: [RFC 0019](0019-transactional-active-library-switch.md), [RFC 0021](0021-bounded-resource-delivery.md).
- Integration: [RFC 0012](0012-structured-async-fault-diagnostics.md), [RFC 0018](0018-interactive-session-lifecycle.md).

The active-library request phase depends conditionally on RFC 0019 for prepared-candidate activation and rollback-safe switching.
Asynchronous MPRIS/resource artifact requests depend conditionally on RFC 0021 for bounded byte delivery and transform cancellation.
RFC 0012 should receive unexpected completion faults without duplicating user reporting.
RFC 0018 should supply the frontend-neutral session generation and admission/teardown transition if both are implemented.

## Goals

- Prevent every platform completion from dereferencing a destroyed owner.
- Bind side-effecting completion to the frontend, runtime, and library generation that authorized it.
- Give each request stable identity, cancellation, and exactly-one terminal outcome.
- Define latest-wins, independent, and non-superseding request policies explicitly.
- Reject stale results before runtime mutation, active-library replacement, notification, file write, or external publication.
- Keep GTK/Gio/D-Bus/native types inside platform adapters.
- Reuse the existing callback executor and lifetime scopes without treating executor affinity as freshness.
- Make teardown, duplicate callback, and out-of-order completion testable without a real portal.

## Non-goals

- Create one universal platform-service facade or move all desktop integrations into runtime.
- Hide platform-specific error codes, chooser UI, D-Bus registration, or native resource ownership behind core APIs.
- Make a native chooser cancellable when the platform implementation cannot actually dismiss it; stale suppression is still required.
- Define active-library transaction semantics owned by RFC 0019.
- Define resource byte/decode budgets owned by RFC 0021.
- Add persistence for pending platform requests.

## Proposed design

### Request scope and immutable evidence

Introduce a small platform-neutral request-lifetime mechanism, used by but not owning GTK adapters:

```text
PlatformRequestEvidence {
  requestId
  ownerGeneration
  frontendGeneration
  optional runtimeGeneration
  optional libraryIdentity/generation
}

PlatformRequestScope {
  shared control state
  stop source / platform cancellation adapter
  admission and terminal state
}
```

The GTK coordinator retains the scope while alive.
A toolkit callback captures only a strong reference to the control state, immutable evidence, and the toolkit request object needed to finish the callback.
It does not capture a raw coordinator, window, runtime, or workflow pointer.

After the toolkit finish call produces a value, the control state posts or invokes a guarded completion on the owning callback executor.
Only then may it acquire a currently valid owner callback through a weak/scoped registration.

The generic mechanism contains no GTK, Gio, D-Bus, file-dialog, or portal types.
Platform adapters own how native cancellation connects to its stop request.

### Request state machine

Every request follows one terminal state machine:

```text
Pending
  -> Completing
  -> Succeeded | Cancelled | Stale | Rejected | Failed
```

Only one transition out of `Pending` wins.
Duplicate toolkit callbacks, cancellation racing completion, or a later error callback become no-ops after the first terminal claim.

`Cancelled` means the owner/user requested cancellation before the side-effect boundary.
`Stale` means lifetime still permitted delivery but generation/supersession evidence no longer authorizes the effect.
`Rejected` means the returned value violates product policy.
`Failed` carries an operational platform failure.

Expected native user cancellation is silent.
Stale completion is normally diagnostic/trace evidence, not a user error.
Operational failure reporting follows the workflow's single semantic owner.

### Generation authority

Composition roots expose monotonic generation tokens for the lifetimes that platform work can address:

- one frontend/window generation;
- one interactive-runtime generation; and
- one active-library identity/generation.

Rebuilding a widget tree does not necessarily replace the frontend generation; replacing the window/runtime pair does.
The interactive session lifecycle owner decides the exact boundary.

Before any effect, the request compares its captured evidence with current evidence from the host registration.
A matching pointer address is not sufficient because an allocator can reuse storage.
Library identity alone is not sufficient when the same root is reopened into a new runtime generation.

Requests that are not runtime- or library-bound omit those fields rather than filling sentinel values and pretending to be checked.

### Supersession policy

Each request family declares one policy:

- **independent**: every current completion may apply, such as two explicitly initiated exports to different files;
- **latest wins**: starting a newer request makes older pending results stale, such as a replaceable preference probe; or
- **single flight**: a second request is rejected/coalesced while one is pending, such as one active Open Library chooser per window.

The request scope enforces the selected policy through request ids.
Adapters cannot infer latest-wins merely from callback arrival order.

### File chooser handoff

`ImportExportCoordinator` adopts the protocol first.
For Open Library:

1. create a single-flight request with the current window/runtime/library evidence;
2. start `Gtk::FileDialog::select_folder` with a callback that captures only request state and the dialog;
3. finish the native result and normalize/validate the path as isolated value work;
4. claim completion and re-enter the current coordinator registration;
5. revalidate request generation and active-library policy; and
6. hand the value to the lifecycle owner.

Import/export selection follows the same boundary before starting `LibraryImportExportWorkflow`.
The workflow receives an already validated request handoff plus its own lifetime-bound task handle.

Destroying the coordinator closes admission, requests native cancellation when supported, invalidates its registration, and terminalizes pending requests without waiting for the platform callback.
A later native callback may finish its toolkit object but cannot reach the destroyed coordinator.

### Active-library and external side effects

An Open Library completion can request but cannot directly own runtime replacement.
The lifecycle host revalidates the evidence and starts the RFC 0019 preparation/activation protocol when applicable.
An older chooser completion cannot replace a newer active runtime.

Asynchronous external effects use a second validation immediately before their irreversible boundary:

- before writing an export chosen for a specific runtime/library;
- before publishing an MPRIS art URL for a now-playing generation; and
- before posting a success/failure notification tied to a workflow generation.

Validation at request start does not authorize a later side effect indefinitely.
Where the side effect itself cannot be rolled back, its owning workflow uses a prepared temporary artifact or transaction and checks generation before final replacement/publication.

### Error and diagnostic ownership

Platform finish errors are translated once into a typed request failure.
The request mechanism never logs and reports the same error independently.

The workflow owner decides whether to ignore user cancellation/staleness, log operational detail, post a notification, or return a command failure.
Unexpected exceptions escaping a root callback go to the structured async diagnostic sink under RFC 0012.

Diagnostics include request family/id and bounded generation evidence, not native object addresses or user file contents.

## Alternatives

### Capture `this` and cancel dialogs in the destructor

Native cancellation can race or still deliver a completion, and a missed request type restores the use-after-free risk.
Callbacks need ownership-safe state even when cancellation works.

### Use only `sigc::track_obj` or a weak pointer

Weak ownership prevents dereferencing a destroyed object but does not reject a completion from an older runtime/library generation or define exactly-once and supersession behavior.

### Put all platform operations in `AppRuntime`

That would pull GTK/Gio/D-Bus and platform paths into the runtime layer and obscure the adapter boundary.
Runtime supplies semantic generations and commands; adapters retain native mechanics.

### Rely on the GTK main thread

Executor affinity serializes callbacks but does not prove the owner still exists or the requested generation is current.

### Give every adapter a custom generation flag

Local flags solve individual races but create inconsistent terminal outcomes, teardown, and test seams.
A small shared protocol preserves adapter ownership while standardizing evidence.

## Compatibility and migration

No persisted format changes are required.
Normal successful chooser, import/export, Open Library, and platform behavior remains the same.

Behavior changes only for races and teardown: late, duplicate, superseded, or old-generation completions are ignored or reported as typed stale/cancelled outcomes instead of applying to current state.

Migration begins with `ImportExportCoordinator`, whose callbacks directly capture `this`.
Other asynchronous GTK/D-Bus/resource adapters adopt the protocol only when their evidence/lifetime trace shows the same risk; this RFC does not mandate wrapper churn for synchronous platform code.

Tests use fake request launchers/finishers rather than driving a desktop portal.
The public platform callback APIs may gain injection seams but remain GTK-local.

## Validation

- Destroying a coordinator/window with folder, open, or save requests pending cannot dereference it when callbacks later fire.
- Native cancellation racing a successful completion produces exactly one terminal outcome and at most one side effect.
- Duplicate callbacks are idempotently rejected.
- Single-flight and latest-wins fixtures reject or stale the correct request independently of completion order.
- An old window/runtime/library generation cannot open, import into, export from, notify for, or replace the new generation.
- Same-root reopen with a new runtime generation is not confused with the old request.
- Import/export worker handoff starts only after the platform request passes its generation check.
- Irreversible file/publication tests perform a final freshness check at their commit boundary.
- Expected user cancellation and stale completion do not create duplicate error notifications.
- Operational errors and unexpected exceptions reach their single reporting/diagnostic owner.
- ASAN/TSAN tests cover destruction, cancellation, and completion races with deterministic fake callbacks.
- A full `./ao check` passes after migration.

## Open questions

- Should the reusable control state live in `ao::async`, application runtime, or a GTK-local support library with only platform-neutral types?
- Which component owns the monotonic frontend/runtime generation before RFC 0018 is implemented?
- Which current platform operations are independent, latest-wins, or single-flight?
- Can every supported GTK native request accept a cancellable object, and what teardown behavior is guaranteed per platform?
- Which file operations need a prepared-artifact boundary in addition to request freshness checking?

## Promotion plan

If accepted and implemented:

- update the [presentation architecture](../architecture/presentation.md) with platform-request evidence and adapter ownership;
- update the [interactive session lifecycle architecture](../architecture/interactive-session-lifecycle.md) with generation creation, admission close, and stale-completion rules;
- update the [runtime execution architecture](../architecture/runtime-execution.md) with request completion affinity and teardown ordering;
- update the GTK dialog, active-library, import/export, and MPRIS specifications for exactly-once, cancellation, and stale behavior;
- update the resource-delivery architecture/specification if asynchronous MPRIS artifacts adopt the protocol; and
- add deterministic platform-request test guidance and fake launcher/finisher helpers.
