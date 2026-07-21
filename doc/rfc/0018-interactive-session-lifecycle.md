---
id: rfc.0018.interactive-session-lifecycle
type: rfc
status: draft
domain: runtime
summary: Proposes one frontend-neutral interactive session lifecycle for startup, workspace and playback restoration, checkpointing, degraded operation, and shutdown.
depends-on: rfc.0017.versioned-workspace-session
---
# RFC 0018: Frontend-neutral interactive session lifecycle

## Problem

`AppRuntime` owns workspace, view, playback, persistence, source, and executor lifetimes, but it does not own the lifecycle that makes those services into a ready interactive application.
Instead it exposes individual stores and restore/save methods and relies on each composition root to call them in a valid order.

GTK implements the most complete sequence in `MainWindowCoordinator`.
It reloads All Tracks, registers library subscriptions, rebuilds list pages, restores workspace, applies per-list presentation preferences, creates and immediately saves a default All Tracks view when needed, restores playback, navigates to the playback source, and reveals the restored track.
Several failures are logged and startup continues, but there is no typed state describing which parts became ready, which stored inputs remain unresolved, or which future checkpoints must be suppressed.

The GTK order also exposes conflicting presentation authorities.
Workspace restore reconstructs and records the exact presentation stored for each view and seeds navigation history from the active restored view.
GTK then applies per-list presentation preferences directly through `ViewService`, after the history point has been committed.
The visible restored presentation can therefore differ from both the saved workspace value and its initial navigation point without one owner declaring the precedence or committing a coherent workspace revision.

TUI constructs the same `AppRuntime` and injects a workspace `ConfigStore`, but never calls workspace or playback restore and never checkpoints them around its event loop.
`LibraryController` creates an All Tracks view independently.
Users therefore receive different session continuity from two interactive frontends over the same supposedly frontend-neutral runtime graph.

Shutdown is likewise assembled locally.
GTK may request save on explicit save, hide, destructor, release, and library-switch preparation, with a window-local boolean preventing only the prepared-switch stale write.
TUI calls `playback.stop()`, requests async stop, joins the runtime, and exits without a workspace or playback checkpoint.
`AppRuntime` also performs playback-persistence shutdown and playback callback shutdown in its destructor, so composition roots and the runtime share an implicit teardown protocol without one idempotent public lifecycle outcome.

Checkpointing does not represent one application operation.
GTK writes window state, presentation state, global application session, playback session, and workspace session through separate wrappers with mixed void, result-returning, and logging behavior.
There is no checkpoint identity, component receipt, or one admission rule for repeated lifecycle triggers.

Construction permits invalid persistence composition.
`AppRuntimeDependencies::workspaceConfigStorePtr` defaults to null, while `AppRuntime` dereferences the resulting pointer directly and also uses it as the fallback playback-session store.
The API cannot distinguish “persistence intentionally disabled” from “required dependency accidentally omitted.”

These are not GTK polish issues.
Startup ordering, restored-state precedence, checkpoint acceptance, unresolved-input protection, and service quiescence are application-runtime behavior shared by every interactive frontend.
The frontends should supply platform resources and UI-local state, but they should not independently reinvent the runtime session state machine.

## Dependencies

- Hard: [RFC 0017](0017-versioned-workspace-session.md) supplies exact library-bound workspace restore outcomes and the unresolved-input protection required by this lifecycle.
- Conditional: [RFC 0005](0005-coherent-playback-boundary.md) blocks migration of playback startup, restore, checkpoint, and quiescence to one coherent lifecycle port.
- Integration: [RFC 0010](0010-versioned-presentation-state.md), [RFC 0013](0013-coherent-application-reporting-policy.md).

The lifecycle state machine and frontend convergence can begin with adapters over current playback and store APIs.
The final playback role cannot be declared coherent while callers still coordinate transport and succession through separate public services.
The final checkpoint receipt cannot claim applied grouped commits while workspace and GTK semantic wrappers still hide the current store's result behind void/log-only APIs.

## Goals

- Give every `AppRuntime` one explicit interactive lifecycle owner and state machine.
- Make startup ordering, workspace bootstrap, playback restoration, checkpointing, quiescence, and shutdown frontend-neutral.
- Preserve UI-local layout and platform resource ownership at the frontend boundary.
- Define one presentation precedence rule for restored views, new views, per-list defaults, and custom presets.
- Return typed startup and checkpoint outcomes instead of logging and continuing implicitly.
- Represent degraded readiness and unresolved persisted input without overwriting rejected state.
- Give GTK and TUI equivalent workspace and playback session continuity when persistence is enabled.
- Make persistence capability explicit and type-safe at runtime construction.
- Serialize repeated lifecycle checkpoint triggers and return exact per-component outcomes.
- Define one idempotent shutdown sequence that frontends invoke rather than partially reproducing it.
- Publish lifecycle phase and operation identity for frontend progress, reporting, and active-library replacement.

## Non-goals

- Move window geometry, shell layout, portal state, GTK widgets, terminal geometry, or toolkit objects into runtime.
- Create one transaction across all managed files and the library database.
- Require CLI commands to construct `AppRuntime` or interactive session state.
- Make every frontend persist identical UI-local preferences.
- Hide domain restore errors behind a generic boolean.
- Define exact workspace, playback, presentation, or global-application payload fields.
- Start library scans automatically as part of every interactive session.
- Turn `AppRuntime` into a universal facade for frontend behavior.
- Preserve the current nullable persistence dependency or ad hoc startup APIs.

## Proposed design

### Lifecycle owner and phases

Compose an `InteractiveSessionLifecycle` inside `AppRuntime` beside workspace and playback.
It coordinates public lifecycle roles from those domains but does not absorb their implementations.

Its observable state uses explicit phases:

```text
Constructed
  -> Starting
  -> Ready | ReadyDegraded
  -> Checkpointing -> Ready | ReadyDegraded
  -> Quiescing
  -> Stopped
```

An operation receives a monotonically increasing `LifecycleOperationId`.
The phase snapshot records the active library UUID, current workspace revision, playback revision when available, persistence capability, unresolved-input flags, and the latest typed operation outcome.

Only one startup, checkpoint, or shutdown transition is active at a time.
Shutdown supersedes queued checkpoints, while a library-switch preparation can request one final checkpoint and then close ordinary admission.

`AppRuntime` construction establishes service ownership but does not imply that sessions were restored or frontend observers may treat the runtime as ready.
Frontends register platform providers and supply startup policy before invoking `start()`.

### Explicit persistence capability

Replace nullable store fields with explicit composition values:

```text
WorkspacePersistence
  Disabled
  Managed(store owner)

PlaybackPersistence
  Disabled
  Managed(store owner or shared writer binding)
```

The exact type may use variants or required owner objects.
It must be impossible to request managed workspace or playback behavior without a valid store/writer lifetime.
An intentionally ephemeral TUI run selects `Disabled` and receives an explicit non-restoring lifecycle outcome rather than accidentally omitting a pointer.

When workspace and playback share one physical document, both bind to one serialized writer and transaction owner.
They do not retain independent mutable `ConfigStore` access.
When they use separate documents, the lifecycle preserves separate receipts rather than pretending the files commit atomically.

### Startup input

The frontend supplies a value-only `InteractiveStartupPolicy` after constructing UIModel preferences and platform providers.
It contains concepts equivalent to:

- whether workspace and playback restore are enabled;
- the semantic default view target for a first-run or intentionally empty workspace;
- a stable presentation-default snapshot or resolver for new views;
- whether a rejected workspace may start in a clean degraded state;
- playback output preference captured from application preferences;
- frontend capabilities needed to interpret later report actions.

Runtime does not depend on UIModel or frontend types.
A presentation resolver is a narrow value/callable port over stable `ListId` and presentation values, captured for the startup operation.
It cannot reach widgets, mutable UIModel state, or storage while runtime commits a workspace transaction.

Global library selection and window geometry remain outside this policy.
The active-library owner constructs the runtime for one already selected library, then invokes this lifecycle.

### Startup transaction sequence

The frontend-neutral startup sequence is:

1. Enter `Starting`, close ordinary interactive command admission, and capture library identity plus startup policy.
2. Establish required managed stores without mutating rejected documents.
3. Initialize library-backed runtime sources and subscriptions needed by workspace/view construction.
4. Read and prepare the RFC 0017 workspace candidate.
5. If the session is first-run or intentionally empty and policy requests a default, prepare that default view in the same workspace candidate path.
6. Apply presentation precedence while candidates are still uncommitted.
7. Commit one workspace revision through RFC 0016.
8. Restore playback intent only after the workspace and library identity are ready.
9. Translate playback reveal through the application-navigation boundary and correlate it with the committed workspace revision.
10. Publish `Ready` or `ReadyDegraded` with a complete startup receipt, then open interactive command admission.

The sequence is a coordinated operation but not a false transaction across audio, filesystem, and UI.
Each stage has an explicit acceptance point and outcome.
Failure policy determines whether later stages are skipped, degraded startup continues, or the complete startup is rejected.

Frontend observers that require ready runtime state subscribe before `start()` but render only after the ready snapshot.
UI-local layout loading and final window presentation occur after runtime readiness unless a specific layout value is needed to construct the startup policy.

### Presentation precedence

Adopt one rule across frontends:

1. A valid exact presentation persisted with a restored view wins for that view.
2. A per-list presentation preference is a default for a new view or a legacy view without an exact presentation.
3. The semantic default presentation is used only when neither source supplies a valid value.
4. A user command after startup may change presentation through one workspace transaction and update whichever preference owner its specification selects.

Runtime resolves that precedence before committing restored/default candidates.
It never restores a view and history point, then lets a frontend mutate the presentation behind workspace history.

RFC 0010 supplies stable persisted presentation decoding.
UI-local column widths and toolkit layout remain outside this precedence because they are not `TrackPresentationSpec` state.

If the product instead wants per-list preference to override every restored view, that must be an explicit alternative selected before acceptance and the workspace candidate/history must contain the overridden value at commit.
Post-commit direct mutation is not permitted under either policy.

### Typed startup outcome

`start()` returns a structured receipt rather than a bare success:

```text
InteractiveStartupReceipt
  operationId
  phase
  libraryId
  workspace outcome + committed revision
  playback restore outcome + revision
  reveal outcome
  unresolved managed inputs
  degraded reasons
```

Expected first run and disabled persistence are successful typed states.
An unsupported workspace version, library mismatch, corrupt playback session, unavailable output, and reveal rejection remain distinguishable.

`ReadyDegraded` means the runtime is usable under documented fallback, not that every error was swallowed.
It carries unresolved-input flags that suppress automatic overwrite and guide reporting/recovery policy.
A blocking library or runtime initialization failure never becomes ready merely because a default view could be created.

### Checkpoint coordinator

All lifecycle save triggers submit one `CheckpointRequest` to the runtime lifecycle.
The request declares a reason such as explicit save, hide, shutdown, or library switch.

The coordinator:

1. Admits one checkpoint operation at a time and folds compatible pending lifecycle requests into the strongest reason.
2. Captures current workspace, playback, and other runtime-owned managed state synchronously on the callback executor.
3. Sends immutable payload candidates to their single writer owners.
4. Collects one receipt per physical document and semantic component.
5. Publishes one checkpoint completion with applied, visibility-only, failed, superseded, or partially applied component outcomes.

Repeated hide, destructor, and release triggers do not create competing background writers.
Frontends may still checkpoint UI-local stores, but they correlate those receipts with the same lifecycle operation id at the active-library/application-shell boundary.

No cross-file atomicity is claimed.
If workspace commits and playback fails, the receipt says so; playback waits for the next natural or lifecycle checkpoint and does not retain a dirty revision or schedule a retry.
Shutdown and switching policy decides which component failures block transition.

The current grouped store supplies candidate-isolated whole-document saves.
Lifecycle receipts correlate those results with the operation and component snapshot identities already owned by each domain; they do not introduce persistence-only revisions.
They inherit the atomic replacement boundary in which a returned error is pre-replacement and success records an applied platform replacement without stronger power-loss evidence.

### Degraded state and recovery

The lifecycle owns whether an unresolved domain session permits interactive startup, but the domain owner retains the reason and recovery commands.

Examples include:

- missing workspace group: ready, first run, ordinary checkpoint allowed;
- unsupported future workspace version: ready degraded with clean in-memory workspace, automatic workspace overwrite suppressed;
- workspace library mismatch: ready degraded or startup rejected according to active-library policy, explicit select/reset/export required;
- malformed optional UI preference: ready with the owner-selected fallback and report disposition;
- core library open failure: startup rejected;
- playback restore rejection: workspace ready, playback idle, rejected playback input preserved under policy.

Recovery commands carry the lifecycle operation id and domain outcome.
After explicit successful reset or retry, unresolved flags clear and ordinary checkpointing resumes.

### Frontend use

GTK performs only platform composition around the lifecycle:

- select the library and construct platform/store dependencies;
- register audio backends and capture UIModel presentation defaults;
- call `start()` and render its outcome;
- load or present UI-local shell state at the documented gate;
- route save/hide/switch/quit requests to checkpoint or shutdown;
- release observers after lifecycle quiescence.

TUI uses the same runtime start, checkpoint, and shutdown transitions.
When configured with managed persistence it restores workspace and playback just like GTK.
When configured ephemeral it receives explicit disabled outcomes and still uses the same default-view and teardown path.

CLI continues to use `CoreRuntime` and does not acquire this lifecycle implicitly.

### Idempotent shutdown

`shutdown(policy)` is the one public terminal transition for an interactive runtime.
It is idempotent and performs:

1. Close interactive and persistence admission.
2. Optionally complete the required final checkpoint under the selected reason/policy.
3. Cancel or quiesce playback preparation, restore, autosave, and callback producers.
4. Drain accepted lifecycle/workspace observations required by live frontend owners.
5. Disconnect or invalidate runtime callbacks that can target frontend observers.
6. Stop and join worker execution while library-backed collaborators still exist.
7. Enter `Stopped` with one terminal receipt.

The frontend destroys UI observers at the gate specified by the lifecycle contract and no longer calls playback stop plus async stop/join independently.
The destructor retains a no-throw last-resort shutdown path but diagnostics distinguish it from an explicit completed shutdown.

## Alternatives

### Keep startup in each frontend and document the order

Documentation can reduce drift but cannot make missing TUI restore, conflicting presentation precedence, typed degraded state, or checkpoint correlation enforceable.
The next frontend would still need to reproduce the protocol.

### Put the complete lifecycle in `AppRuntime` methods

A handful of methods on `AppRuntime` would be convenient but would mix state-machine ownership with a growing facade surface.
A composed lifecycle role keeps the graph explicit and can expose narrow startup, checkpoint, observation, and shutdown ports.

### Move GTK presentation preferences into runtime storage

That could remove one ordering edge but would merge UI-local defaults and column behavior into runtime persistence.
The selected design accepts a stable value policy and keeps storage ownership at UIModel/frontend.

### Let per-list preferences always override restored presentation

This may be a legitimate product choice, but it must happen while building the workspace candidate and history point.
The selected default gives an exact restored view precedence because it best preserves the user's last visible state.

### Treat every restore failure as first run

That keeps startup simple but can overwrite corrupt, incompatible, or newer-version user state on the first checkpoint.
Typed degraded state separates usability from authorization to replace input.

### Make checkpoint one cross-file transaction

Workspace, playback, presentation, shell, and global application state have different documents and owners.
A journaled multi-file protocol is disproportionate; exact component receipts are sufficient to report partial application.

### Rely on destructors for final save and shutdown

Destructors cannot report actionable failure and are invoked under several partial-construction and exception paths.
An explicit idempotent lifecycle provides observable acceptance while retaining destructor cleanup as a safety net.

## Compatibility and migration

The proposal changes internal runtime, GTK, TUI, and test composition APIs.
It does not require combining existing managed documents or changing their schemas by itself.

Implementation proceeds in phases:

1. Add lifecycle state and typed startup receipts around the current GTK order without moving behavior, and freeze current fallback/error cases in tests.
2. Replace nullable persistence dependencies with explicit managed/disabled capabilities.
3. Move workspace bootstrap, exact presentation precedence, and default-view creation into the lifecycle using RFC 0017 and RFC 0016.
4. Migrate TUI to the same start path and add managed versus ephemeral session behavior tests.
5. Add checkpoint operation ids, serialized admission, unresolved-input suppression, and per-component receipts without playback dirty state or retry scheduling.
6. Expose current store results through semantic writers, integrate RFC 0013 dispositions, and remove void/log-only lifecycle save paths.
7. Integrate RFC 0005's coherent playback lifecycle role and remove composition-root use of internal playback services.
8. Move GTK and TUI teardown to explicit lifecycle shutdown and add admission/quiescence guardrails.
9. Remove legacy `initializeSession()`, direct workspace/playback restore orchestration, and frontend async stop/join sequences.

Each phase updates current specifications before deleting the behavior it replaces.
Frontends may temporarily adapt to the lifecycle while internal calls remain synchronous, but no new frontend-specific session sequence is added.

## Validation

- Constructing managed persistence without a valid owner is impossible; explicit disabled composition remains valid.
- Startup publishes no ready snapshot before the complete workspace candidate and selected playback outcome are known.
- First run creates at most one policy-selected default view through one workspace revision and does not save merely as a side effect of reading absence.
- Exact restored presentation, per-list default, and semantic default precedence is identical in GTK and TUI and matches the accepted rule.
- Initial navigation history describes the presentation actually committed at readiness.
- Unsupported, incompatible, or malformed managed input can enter documented degraded mode without being overwritten by automatic checkpoint.
- GTK and managed TUI restore the same workspace and playback intent from equivalent inputs.
- Ephemeral TUI follows the same bootstrap and shutdown state machine without accessing a store.
- One startup receipt distinguishes every component outcome and names the committed workspace revision and playback restore outcome.
- Repeated explicit save, hide, destructor-adjacent, switch, and shutdown requests pass through one serialized checkpoint admission path without competing background writes.
- Partial checkpoint application preserves exact component receipts without manufacturing persistence-only dirty revisions.
- A checkpoint failure receives exactly the reporting disposition selected under RFC 0013.
- Shutdown closes admission before quiescence, performs at most one required final checkpoint, and produces no callback into destroyed frontend observers.
- Calling shutdown repeatedly returns the same terminal state without repeating semantic writes or teardown.
- GTK and TUI production code no longer call workspace/playback restore, raw store flush, playback stop, or async join as independent lifecycle steps.
- CLI remains on `CoreRuntime` and acquires no interactive state.
- Existing runtime, workspace, playback, GTK, TUI, managed-state, and teardown tests remain behavior oracles where this RFC does not intentionally change policy.
- Completed implementation passes `./ao check`, lifecycle-focused ThreadSanitizer tests, and the documentation gate.

## Open questions

- Does UI-local shell layout load before runtime `Ready` to reduce visual movement, or after `Ready` to avoid layout observers reading incomplete runtime state?
- Which startup failures block readiness versus permit `ReadyDegraded`, and which owner records that matrix?
- Should managed TUI persistence be the default when `configPath` is supplied, or require an explicit command-line capability?
- Which checkpoint reasons require all critical components to apply before library switch or shutdown may continue?
- How long may explicit shutdown wait for persistence before offering a forced diagnostic-only exit?
- Should application-global output selection be applied before playback-session decode, during playback restore, or as a separate route-settlement phase?
- Does a frontend need a progress stream for startup stages, or is phase plus final receipt sufficient at current scale?

## Promotion plan

If accepted and implemented, update the [interactive session lifecycle architecture](../architecture/interactive-session-lifecycle.md) with the lifecycle owner, explicit persistence capabilities, startup/checkpoint/shutdown state machine, and frontend responsibilities.
Update the [system architecture](../architecture/system-overview.md) with the role only if it changes the public application-runtime composition map.
Update the [runtime execution architecture](../architecture/runtime-execution.md) with lifecycle admission, callback draining, and shutdown quiescence.

Update the [workspace session specification](../spec/workspace/session.md) with lifecycle invocation, default-view policy, unresolved-input suppression, and checkpoint receipts.
Update playback session specifications with lifecycle restore/checkpoint/quiescence after RFC 0005 integration.
Add a focused interactive-session lifecycle specification for the exact phases, receipts, degradation matrix, checkpoint reasons, and frontend gates.

Update the [GTK active-library lifecycle specification](../spec/linux-gtk/active-library-lifecycle.md) so GTK delegates runtime startup/checkpoint/shutdown and retains only platform pair ownership.
Add or update TUI lifecycle documentation with managed and ephemeral behavior.
Update presentation specifications with the accepted restored-view/default precedence and keep exact persisted stable ids in RFC 0010's promoted reference.

Update persistence and reporting documents when new lifecycle acknowledgement and RFC 0013 outcomes cross their current boundaries.
Record an ADR if explicit degraded readiness, presentation precedence, or lifecycle ownership is accepted as a durable architectural choice.
