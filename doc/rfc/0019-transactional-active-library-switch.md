---
id: rfc.0019.transactional-active-library-switch
type: rfc
status: draft
domain: linux-gtk
summary: Proposes a transactional active-library host with canonical location identity, prepared runtime candidates, rollback-safe activation, and library-bound sessions.
depends-on: rfc.0018.interactive-session-lifecycle
---
# RFC 0019: Transactional active-library switching

## Problem

GTK correctly replaces the complete library-bound runtime graph instead of retargeting a live `MusicLibrary`, but the replacement operation itself is not transactional.

The current different-root sequence is:

1. checkpoint the old window through several best-effort paths;
2. delete the globally stored restorable playback session;
3. mark the old window so later saves cannot rewrite its path;
4. remove and destroy the old window/runtime pair;
5. write the requested path to global application state through a void save wrapper;
6. construct, initialize, register, and present the new pair.

The old graph is gone and its playback session has been discarded before the new runtime, database, workspace, views, playback stack, window, and subscriptions are known to be viable.
An exception or initialization failure after old-pair destruction can leave no active window, a global path that may name the failed candidate, and no automatic route back to the previously working library.

Global selection persistence has no caller-visible commit outcome.
`AppConfigStore::saveAppSession()` calls the current fail-closed `ConfigStore::save()` operation but logs its result behind a void wrapper.
The switch therefore proceeds without knowing whether the selected path was replaced successfully, and a later old-window callback is prevented only by one local `librarySwitchPrepared` boolean.

Location identity is inconsistent.
An Open Library selection becomes an absolute lexically normalized path, while startup accepts the saved path when `exists()` is true without requiring a directory or applying the same normalization.
Lexical normalization does not collapse symlinks, junctions, case aliases, or other platform-equivalent locations.
The same physical library can therefore be treated as different, and two spellings can derive different comparison and persistence behavior.

Path validation and opening are separate.
`is_directory()` is checked before candidate construction, leaving ordinary filesystem change between validation, database-path derivation, and runtime open.
Failures from those later stages do not return one typed switch outcome.

Switch request lifetime is implicit.
The portal callback schedules a GLib idle closure that captures the active-window reference by reference and has no request id, cancellation token, supersession rule, or controller-owned admission gate.
Rapid selections may queue several replacements, and shutdown relies on surrounding GTK lifetimes rather than invalidating pending switch work explicitly.

The active pair is represented by a `Glib::RefPtr<MainWindow>` plus an `AppRuntime` hidden in string-keyed GTK object data.
There is no typed pair owner or factory boundary at which to prepare, test, activate, roll back, or destroy the graph.
The current `main.cpp` replacement sequence has no focused end-to-end test; tests cover window preparation and policy fragments only.

Playback persistence makes rollback harder than necessary.
The restorable session is application-global and uses library-scoped list/track identities without a durable active-library binding at this boundary.
Switching therefore destroys the old session preemptively rather than selecting a session that is either namespaced by or validated against the target library UUID.
Switching back cannot resume the old library even when its previous session was valid.

Startup fallback also conflates “no selected library” with a real temporary library at `<temporary-directory>/aobus-empty`.
An ordinary window save can record that path as the last library, turning a bootstrap implementation detail into durable selection state.
The application has no explicit `NoLibrarySelected` or ephemeral-bootstrap identity.

## Dependencies

- Hard: [RFC 0018](0018-interactive-session-lifecycle.md) supplies prepared startup, checkpoint, quiescence, activation, and shutdown receipts for each runtime candidate.
- Conditional: None.
- Integration: [RFC 0005](0005-coherent-playback-boundary.md), [RFC 0013](0013-coherent-application-reporting-policy.md).

RFC 0005's playback owner must align session binding and activation so a candidate cannot restore another library's succession state.
RFC 0013 owns switch progress, rejection, rollback, degraded activation, and checkpoint reporting dispositions.
The current [grouped configuration store](../spec/persistence/config-store.md) already supplies candidate-isolated, result-bearing whole-document replacement; this RFC must expose that result through the global selection owner and correlate it with the host request rather than requiring a store-generation protocol.
The current [atomic replacement contract](../spec/persistence/atomic-replacement.md) makes a returned error pre-replacement and a successful platform replacement applied, without claiming absolute power-loss durability.

## Goals

- Keep the current active pair usable until a replacement candidate is completely prepared.
- Give active-library selection one typed owner, state machine, request identity, and observation stream.
- Define canonical physical-location comparison separately from durable logical library identity.
- Reuse the active pair for an equivalent physical location and replace it for a genuinely different selected root.
- Bind global selection and restorable sessions to the opened library UUID.
- Make every fallible candidate step occur before one no-fail in-memory activation commit.
- Commit global selected-library state through a result-bearing host command before activation.
- Resume the old pair when candidate preparation or pre-activation persistence fails.
- Prevent stale portal, idle, window, and shutdown callbacks from mutating a newer active selection.
- Preserve each library's restorable playback intent instead of destructively deleting another library's session.
- Represent no selected library explicitly without persisting a temporary bootstrap root as user intent.
- Make startup and later Open Library requests use the same candidate and activation machinery.
- Provide deterministic end-to-end tests for every preparation, commit, rollback, supersession, and teardown transition.

## Non-goals

- Retarget an existing `AppRuntime`, `MusicLibrary`, source cache, workspace, or playback graph to another root.
- Support two simultaneously interactive active libraries in the normal GTK product.
- Define library scanning, import/export, or database migration behavior.
- Guarantee a transaction across the library database, every managed-state file, and GTK display server.
- Treat filesystem path text as durable library identity.
- Make active-library switching a Core library concern.
- Move portal dialogs or GTK window presentation into application runtime.
- Preserve source compatibility for `handleOpenNewLibrary()`, hidden object-data runtime ownership, or the current callback wiring.

## Proposed design

### Typed active-library host

Introduce a GTK application-shell `ActiveLibraryHost` that owns exactly one optional active `LibraryWindowPair` and every pending switch request.

```text
ActiveLibraryHost
  -> LibraryPairFactory
       -> AppRuntime + InteractiveSessionLifecycle
       -> MainWindow + frontend observers
  -> GlobalSelectionStore
  -> switch request/state observation
```

`LibraryWindowPair` is a typed RAII owner containing the window, runtime, lifecycle role, canonical location evidence, and durable library UUID.
Runtime lifetime is no longer hidden behind the string key `"app-runtime"`.
The pair destructor enforces frontend-observer-before-runtime release, while explicit lifecycle shutdown supplies the semantic receipt.

The factory creates a candidate without changing the host's active slot.
It receives platform providers and UI-local dependencies as factory inputs rather than reaching back into global `main.cpp` variables.

### Location and library identity

Use two distinct identities:

- `LibraryLocationIdentity` identifies the selected physical directory using platform-aware canonical/equivalent-file evidence;
- `LibraryId` is the durable UUID read from the opened library metadata.

Path intake performs one fallible resolution operation that:

1. converts the user selection to an absolute path;
2. resolves symlinks/junctions and platform case behavior according to a documented supported-filesystem policy;
3. opens or validates the directory with evidence sufficient to detect a changed/non-directory target;
4. derives the database location from the resolved selection policy;
5. returns a typed identity plus diagnostic display path.

String equality alone is not the same-root rule.
When both locations exist, platform-equivalent identity decides reuse.
The canonical display/persistence path is stable but does not replace physical equivalence evidence.

A different location containing the same `LibraryId` is still a different runtime root and therefore creates a replacement candidate.
The shared UUID identifies logical library continuity for sessions and diagnostics; it does not permit mutating the old runtime's root path.

Global selection stores both the canonical location and the expected library UUID.
Startup opens the location and verifies the UUID before treating it as the saved active library.
A mismatch is an explicit incompatible-selection outcome rather than silent interpretation of list/track ids against another database.

### Host state machine

The host publishes a revisioned state equivalent to:

```text
NoLibrary
Active(pair revision, library id, location)
Preparing(request id, requested location, old pair revision)
CandidateReady(request id, candidate library id)
Committing(request id)
Active(new pair revision, library id, location)
Rollback(request id, reason)
ShuttingDown
Stopped
```

One `LibrarySwitchRequestId` identifies request, progress, reporting, and final receipt.
Every transition occurs on the GTK main-context executor.

A newer request may supersede an older request only before the older request enters `Committing`.
After commit begins, later requests queue behind the resulting active pair.
Shutdown closes admission, cancels uncommitted candidates, and waits for any commit turn before releasing the active pair.

Portal and idle callbacks hold a weak controller-owned request handle, not a raw or referenced active-window slot.
Destroying the host invalidates every pending callback.

### Candidate preparation

For a genuinely different location, the factory prepares a complete candidate while the old pair remains active:

1. Open the target root and database and read its durable library UUID.
2. Construct runtime stores with library-bound workspace/playback associations.
3. Construct `AppRuntime`, register platform providers, and create frontend observers/window in a non-active state.
4. Run RFC 0018 startup through a candidate gate, including workspace restore and all fallible library-backed initialization.
5. Validate that no rejected session was interpreted against a different library identity.
6. Prepare GTK application registration and any memory needed for the active-slot swap.
7. Return `LibraryPairCandidate` with a no-fail adoption operation and cleanup ownership.

Candidate preparation does not present the new window, publish it as active, write global selection, start a bootstrap scan, or destroy/quiesce the old pair.
It also does not acquire exclusive playback output or publish cross-application media state that would compete with the active pair.
Those actions are deferred or prepared behind the lifecycle activation gate.

Candidate failure destroys only candidate resources and leaves the old pair and global selection untouched.

### Old-pair preparation

After the candidate is ready, the host asks the old pair for a replacement-preparation receipt through RFC 0018.
The receipt:

- closes new ordinary commands for that pair;
- completes the policy-required final checkpoint;
- quiesces playback/output activity that cannot overlap activation;
- preserves enough lifecycle state to resume if pre-commit work fails;
- prevents later hide/destructor callbacks from writing active-library selection independently.

Checkpoint policy explicitly names blocking components.
A critical workspace/global selection failure can block the switch, while a best-effort UI-local preference failure may permit the switch with a report.
The old pair is hidden or interaction-disabled only when candidate activation is imminent; it is not destroyed.

If preparation fails, candidate resources are released and the old pair remains or resumes `Active` with one typed rollback outcome.

### Global selection transaction

Global selected-library state is one semantic group commit through the current candidate-isolated `ConfigStore::save()` boundary.
It writes the canonical location, expected library UUID, and any active-selection request identity needed for diagnostics and restore validation.

The serialized host compares its captured selection generation before invoking the synchronous store commit.
A stale request cannot overwrite a newer selection merely because its delayed file operation completes.

Pre-replacement failure keeps the old global selection and resumes the old pair.
A successful replacement applies the new global selection under the current platform contract, after which the prepared in-memory activation must not fail.
The host never publishes a new active pair whose persisted selection receipt names a different request/library generation.

### No-fail activation commit

Before global selection replacement, the candidate and old-pair preparation contain every operation that can reject activation.
The commit turn then performs only prepared ownership changes:

1. Adopt the candidate into the active pair slot and advance pair revision.
2. Mark the old pair replaced so no later callback can claim active ownership.
3. Publish the new host snapshot and activate/present the prepared window.
4. Release the old pair through explicit lifecycle shutdown.
5. Start optional bootstrap scanning as a post-activation library task correlated to the new pair revision.

Application/window registration needed for the swap is staged before this turn or uses an API whose failure is represented before global selection commit.
No allocation or storage operation is permitted between global selection application and the in-memory active-slot swap.

If GTK cannot provide a strictly no-fail present operation, presentation is treated as a post-commit frontend outcome.
The new pair remains the selected active runtime, the host reports a presentation failure, and recovery retries presentation or performs a new explicit switch; it does not resurrect the old selection under an ambiguous applied state.

### Library-bound playback sessions

Stop deleting a single global playback-session group merely because the active library changes.
Restorable playback intent is associated with the `LibraryId` whose track/list identities it contains.

Acceptable storage designs include:

- one per-library managed playback document;
- one application document containing independently keyed library-session entries;
- one self-identifying current session retained only when the prior library session is stored elsewhere.

The promoted playback persistence specification selects the exact format.
All designs must satisfy:

- restore checks `LibraryId` before resolving track/list ids;
- candidate preparation cannot consume or delete the active pair's session;
- successful switch selects the target library's session, if any;
- rollback retains the old library's restorable intent;
- switching back can resume the old library under ordinary policy;
- cleanup/retention is explicit and never inferred from path-string equality.

RFC 0005's coherent playback lifecycle owns snapshot and restore semantics inside one library.
This RFC owns selection of the library-bound persistence association during pair replacement.

### Explicit no-library state

Replace the implicit durable `aobus-empty` selection with an explicit host state.
When no saved selection can be opened, startup enters `NoLibrary` or constructs an explicitly ephemeral bootstrap pair whose identity cannot be written as `lastLibrary`.

The shell may still offer Open Library, preferences, diagnostics, and import actions in this state.
If product needs an empty interactive library view, its runtime/store paths are marked ephemeral and global selection checkpoint omits them.

Opening a real library uses the same candidate and commit sequence as later switches.
There is no separate startup-only path that applies weaker validation or normalization.

### Outcomes and reporting

A switch completion receipt contains:

```text
request id
old pair revision and library id, when present
requested and resolved location evidence
candidate library id
candidate startup receipt
old-pair preparation/checkpoint receipt
global selection commit receipt
new pair revision, when applied
rollback or post-commit issue
bootstrap task id, when started
```

Expected cancellation and same-location reuse are typed no-change outcomes.
Candidate rejection, checkpoint block, selection-store failure, supersession, degraded activation, presentation failure, and scan outcome remain distinguishable.

RFC 0013 selects whether each outcome is inline to the portal workflow, observed as progress, enters the shared feed, or remains diagnostic-only.
Lower storage and runtime layers do not post duplicate switch reports.

## Alternatives

### Construct the new pair only after destroying the old one

This minimizes simultaneous resources but makes rollback impossible for database, runtime, workspace, playback, or window initialization failure.
The selected design accepts brief bounded overlap to preserve a working application.

### Construct the candidate, then write global selection after presenting it

If persistence fails, the application shows one active library while restart selects another.
The selected order establishes a result-bearing selection commit before the no-fail active-slot swap.

### Roll back by reconstructing the old pair

Reconstruction can fail and loses live in-memory workspace/playback state.
Retaining the old pair until commit provides actual rollback rather than a second best-effort startup.

### Compare only canonical path strings

Canonical strings improve consistency but do not cover every platform-equivalent file identity or changed target.
The selected location identity records platform evidence and keeps logical library UUID separate.

### Treat matching library UUID as same-root reuse

A copied or moved database may retain its UUID at a different selected music root.
The runtime is root-bound, so physical-location change still requires replacement even though sessions can recognize logical continuity.

### Keep one global playback session and discard it

Deletion prevents cross-library restore but destroys valid user intent before candidate success and prevents switch-back continuity.
Library-bound association makes safety and retention compatible.

### Persist the temporary empty root with a special path prefix

Path conventions remain easy to leak into ordinary selection and do not express capability or lifetime in the type system.
An explicit no-library/ephemeral state is clearer.

### Serialize requests only with GLib idle order

FIFO callback order provides no request identity, cancellation, stale-result rejection, or shutdown ownership.
The host state machine makes those policies testable.

## Compatibility and migration

The proposal changes GTK application-shell ownership and managed global/playback state.
The repository has no internal source-compatibility requirement, so the target pair/factory/controller boundaries do not preserve the current free functions or hidden object-data key.

Implementation proceeds in phases:

1. Extract typed `LibraryWindowPair`, `LibraryPairFactory`, and `ActiveLibraryHost` behind current behavior, then add end-to-end state-machine tests.
2. Introduce `LibrarySwitchRequestId`, host revision, controller-owned callback cancellation, and one observation/receipt surface.
3. Unify startup and Open Library location resolution with canonical/equivalent identity plus saved library UUID verification.
4. Add prepared candidates while retaining the old pair; initially keep current global-save ordering behind a test adapter.
5. Integrate RFC 0018 candidate startup, replacement preparation, resume, activation, and shutdown gates.
6. Make `AppConfigStore` return the current candidate-save result, correlate it with the host generation, and make the active-slot swap no-fail after selection application.
7. Bind playback sessions by library UUID with RFC 0005 and remove destructive pre-switch discard.
8. Introduce explicit `NoLibrary` or ephemeral-bootstrap state and stop persisting the temporary fallback root.
9. Add RFC 0013 reporting dispositions.
10. Remove legacy `handleOpenNewLibrary()`, raw reference captures, string-keyed runtime ownership, and `librarySwitchPrepared` once the host generation makes stale writes impossible.

Existing saved path-only selection may be read once as untrusted legacy input, opened, assigned the actual library UUID, and written through the new transaction on the next explicit successful selection/checkpoint.
Failure or mismatch never rewrites it implicitly.

## Validation

- Same physical directory selected through relative, absolute, dot-segment, symlink/junction, and supported case aliases reuses one active pair.
- A genuinely different root creates a candidate even when copied metadata contains the same library UUID.
- Startup requires a directory, uses the same location resolver as Open Library, and verifies saved UUID when present.
- Target disappearance, type change, permission failure, invalid database, runtime construction failure, workspace rejection, playback rejection, and window initialization failure destroy only the candidate and leave the old pair active.
- Candidate preparation never presents, scans, writes global selection, consumes the old playback session, or publishes itself as active.
- Blocking old-pair checkpoint failure cancels the candidate and resumes the old pair with unchanged global selection.
- Global selection pre-application failure resumes the old pair and leaves no new active snapshot.
- Successful selection replacement activates the matching new pair and never reports that the old selection remains installed.
- The activation commit performs no fallible allocation, storage, or candidate initialization after global selection application.
- Rapid requests obey documented supersession/queue rules and no stale request can replace a newer active pair.
- Portal, idle, hide, destructor, and shutdown callbacks carry a live host/request generation and cannot write stale active selection.
- Shutdown cancels uncommitted candidates, finishes or classifies an in-progress commit, and releases exactly one active pair.
- Switching libraries retains each library's bound playback session; switching back can restore the prior library without cross-library id interpretation.
- An incompatible playback/workspace `LibraryId` is rejected before resolving list or track ids.
- No-library startup never persists the ephemeral bootstrap path as a selected user library.
- Every switch outcome contains correlated request, old pair, candidate, selection commit, activation, and optional scan evidence.
- Focused end-to-end tests cover the extracted host rather than relying only on `MainWindow` and portal policy fragments.
- GTK integration tests prove observer-before-runtime destruction for candidate failure, rollback, successful replacement, and shutdown.
- Completed implementation passes `./ao check`, relevant native Windows path/identity tests, ThreadSanitizer lifecycle tests, and the documentation gate.

## Open questions

- Which platform APIs provide sufficient `LibraryLocationIdentity` evidence on supported Linux and Windows filesystems, especially for network shares?
- Can candidate audio providers be fully prepared without acquiring exclusive output, or should output registration happen only during activation under RFC 0005?
- Which managed-state checkpoint failures block switching, and which permit activation with a typed degraded receipt?
- Should a different location with the same `LibraryId` be reported as a moved library, a cloned library, or ordinary replacement?
- Is one per-library playback file preferable to a bounded application-global map keyed by UUID?
- How should retained sessions be pruned when a library is permanently removed from recent selection state?
- Does `NoLibrary` need a minimal runtime graph, or can the GTK shell operate without `AppRuntime` until a candidate is selected?
- Which GTK registration/presentation operations can be staged before selection commit, and which must be classified as post-commit issues?

## Promotion plan

If accepted and implemented, update the [interactive session lifecycle architecture](../architecture/interactive-session-lifecycle.md) with `ActiveLibraryHost`, typed pair/candidate ownership, location versus library identity, activation, rollback, and no-library state.
Replace current startup selection, same-root comparison, replacement, preparation, failure, observation, and shutdown behavior in the [GTK active-library lifecycle specification](../spec/linux-gtk/active-library-lifecycle.md) phase by phase.
Add an exact active-library host runtime surface reference if switch states, receipts, request ids, and selection fields need exhaustive documentation.

Update the [application managed-state surface](../reference/persistence/application-config.md) with canonical location, expected library UUID, selection generation, and any legacy path migration after implementation.
Update the [managed file locations reference](../reference/persistence/location.md) if playback sessions or ephemeral bootstrap paths move.
Update persistence architecture/specification owners only if active-library selection adds a new semantic command above the current candidate-save and atomic-replacement boundaries.

Update playback architecture, session specifications, and format reference with library-bound session association when implemented with RFC 0005.
Update RFC 0018's promoted lifecycle specification with candidate preparation, resume, activation, and shutdown gates.
Update reporting specifications with the switch outcome matrix under RFC 0013.

Add GTK development/testing guidance only for reusable candidate-host fixtures and platform identity tests; do not duplicate the lifecycle state machine outside its specification.
Record an ADR if prepared-pair rollback, UUID-bound session retention, or persisted canonical location plus logical identity becomes a durable design choice.
