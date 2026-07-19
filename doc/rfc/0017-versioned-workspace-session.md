---
id: rfc.0017.versioned-workspace-session
type: rfc
status: draft
domain: workspace
summary: Proposes a library-bound versioned workspace session with exact active-view identity, strict validation, and bounded transactional restoration.
depends-on: rfc.0016.coherent-workspace-transactions
---
# RFC 0017: Versioned semantic workspace sessions

## Problem

The current workspace session does not identify the active view.
It persists only `activeListId`, even though `openViews` may contain several views over the same list with different filters or presentations.
Restore scans matching views and currently lets the first matching filtered view win over an unfiltered match.
The focused view after restart can therefore differ from the view that was focused when the session was saved while the file remains completely valid.

The root payload has no document kind, complete workspace schema version, or library identity.
Its `presentationVersion` gates only the stable nested presentation vocabulary implemented by RFC 0010.
It is stored under a per-library path in GTK, but every persisted `ListId` is interpreted directly against whichever `MusicLibrary` opens that path.
A copied, mismatched, stale, or incorrectly selected workspace file can resolve numeric ids against the wrong library; coincidental id reuse can produce a plausible but semantically unrelated view instead of a rejection.
The library already has a durable UUID, but workspace persistence does not bind to it.

The current private document schema now owns exact root field names and stable nested presentation values.
That closes the numeric enum hazard but does not version root workspace semantics, list identity, filter dialect, or active-view interpretation.

Structural deserialization is strict and nested presentation values are validated before view creation.
Workspace identity and resource validation remain incomplete, however.
There is no schema-level bound on view count, preset count, filter length, collection size, or nested presentation size.
A small managed-state file can therefore trigger unbounded view, source, projection, and query construction during startup.

Custom preset identity is internally inconsistent.
Runtime lookup and removal use `spec.id`, while `addCustomPreset()` replaces an existing entry by `label`.
The persisted payload accepts duplicate labels, duplicate ids, empty ids, collisions with built-in ids, and arbitrary unresolved base ids.
After permissive deserialize, first-match lookup order rather than a declared identity contract determines behavior.

Restore has good candidate cleanup for view-creation failure, but its public operation does not declare an empty-workspace precondition.
Calling it on a non-empty workspace appends restored views and replaces presets instead of replacing or rejecting the existing aggregate.
It emits ordinary focus and preset observations during commit and has no result distinguishing first run, restored state, incompatible library, unsupported future schema, rejected predecessor state, or intentionally empty state.

Focused tests lock the stable presentation vocabulary and strict root membership, but there is still no complete golden fixture for a library-bound workspace envelope, exact active identity, limits, and outcomes because that envelope does not yet exist.

## Dependencies

- Hard: [RFC 0016](0016-coherent-workspace-transactions.md) supplies the validated candidate-install boundary required for one atomic restored workspace revision.
- Conditional: None.
- Integration: [RFC 0010](0010-versioned-presentation-state.md) supplies the implemented stable nested presentation vocabulary; [RFC 0013](0013-coherent-application-reporting-policy.md) defines user-visible dispositions for incompatible, corrupt, recovered, and unsaved workspace sessions.

The stable nested presentation prerequisite is complete.
The remaining work is the semantic envelope, library binding, exact active-view reference, limits, preset identity, outcomes, and recovery ownership.
The current grouped store already provides candidate reads and result-bearing whole-document commits.
This RFC's workspace owner must retain unresolved schema state and suppress ordinary checkpoints because the generic store cannot infer that a syntactically valid group has an unsupported semantic version.

## Goals

- Bind every workspace session to the durable identity of the library whose ids it contains.
- Give the root payload an explicit document kind and schema version.
- Persist an exact session-local reference to the active open-view entry.
- Give every persisted view entry a unique identity within the session document without persisting runtime `ViewId`.
- Retain RFC 0010's stable nested presentation identifiers inside a fully versioned workspace envelope.
- Deserialize into an isolated semantic candidate and reject ambiguous or malformed identity-bearing state.
- Apply explicit bounds before allocating runtime views, sources, projections, expressions, or presets.
- Validate custom preset identity, built-in collisions, references, and complete nested presentation values.
- Install one complete candidate through one workspace revision or leave the live workspace unchanged.
- Distinguish first run, restored, intentionally empty, incompatible, unsupported, and recovered outcomes.
- Preserve unsupported future input until an explicit user or policy-authorized recovery commit.
- Lock predecessor rejection and current canonical output with golden fixtures.

## Non-goals

- Persist `ViewId`, selection, projection rows, source leases, widget state, scroll positions, or navigation history.
- Turn a workspace session into a complete desktop-layout or playback-session document.
- Make a session portable across unrelated library identities merely because list ids happen to match.
- Preserve invalid individual views by silently dropping them from an otherwise accepted version-1 session.
- Define generic YAML, grouped-store, or atomic-file behavior.
- Define nested presentation stable ids independently of RFC 0010.
- Require compatibility with every unversioned development snapshot.
- Introduce a cross-file transaction among workspace, playback, presentation, and global application state.

## Proposed design

### Versioned library-bound envelope

Replace the reflected root aggregate with a domain schema and an explicit logical envelope:

```yaml
workspace:
  kind: aobus.workspace-session
  version: 1
  libraryId: 123e4567-e89b-12d3-a456-426614174000
  views:
    - key: view-1
      listId: 4294967295
      filter: ""
      presentation: {}
  activeView: view-1
  customPresets: []
```

The exact stable key spelling and scalar grammar are finalized in the promoted format reference.
The structural requirements are normative:

- `kind` prevents an unrelated group from being interpreted as a legacy session;
- `version` selects one explicit schema reader and migration policy;
- `libraryId` is the canonical UUID stored in library metadata, not a filesystem path;
- `views` is an ordered collection of complete semantic view entries;
- `activeView` is null only when `views` is empty and otherwise references exactly one entry key;
- custom presets form one separately validated catalog inside the same session candidate.

The runtime reads the active library UUID before deserializing identity-bearing view content.
A different UUID rejects the candidate before any view/source construction.
Changing root path without changing library identity remains valid; restoring a library backup that deliberately adopts another UUID invalidates the old workspace until explicit recovery.

### Session-local view keys

Every view entry carries a key unique within one session document.
The key is persistence identity for references inside that document only.
It is never passed to `ViewService`, treated as a cross-session user object, or confused with runtime `ViewId`.

Capture assigns keys to the complete snapshot before serialization and writes the active entry's exact key.
An implementation may retain a loaded key as non-semantic live metadata to improve diff stability, but correctness cannot depend on retaining it.
Rewriting a session may allocate new keys as long as all references in the new document are coherent.

Duplicate keys, an unknown active key, a non-null active key with no views, or a null active key with nonempty views reject the complete version-1 candidate.
There is no filter-based or list-based focus heuristic in version 1.

### Domain-owned stable schema

The workspace session schema owns literal field names and does not derive them from C++ member spelling or aggregate order.
It parses into an untrusted domain representation, validates it, and only then constructs typed runtime values.

List ids retain their exact governed numeric representation because they are durable library identities scoped by `libraryId`.
Closed presentation choices use RFC 0010's stable textual ids rather than C++ enum ordinals.
Filter expressions remain exact text and are compiled only after structural and size validation.

Unknown root or view fields are rejected in version 1 unless the format reference explicitly designates an extension mapping.
Unsupported future versions return a typed `UnsupportedVersion` outcome and preserve the original managed document.
They are never deserialized as version 1 by ignoring unknown fields.

### Resource and complexity limits

The format specification declares conservative limits for:

- total serialized group bytes before YAML/domain deserialize;
- number of open views;
- filter-expression bytes per view and in aggregate;
- custom preset count;
- presentation field, sort-term, and redundant-field counts;
- identifier and label lengths;
- total candidate resource budget where view construction can estimate it.

Limits are checked before creating runtime views.
The initial values are selected from real product scale tests rather than copied from container maxima.
A limit rejection identifies the violated category without echoing untrusted content into user-visible text.

The session owner may offer explicit reset or export for an oversized file.
It does not silently truncate views, expressions, presets, or fields and then save the reduced state as if it were the user's session.

### Semantic validation

Version-1 validation is fail closed for the complete workspace candidate.
It checks at least:

- document kind, supported version, and active library UUID;
- view-key uniqueness and exact active reference;
- valid, nonzero or governed virtual `ListId` values resolvable in the bound library;
- filter syntax and any current compilation prerequisites;
- complete presentation normalization and stable-id resolution;
- unique nonempty custom preset ids;
- custom ids that do not collide with built-in ids or reserved namespaces;
- base-preset references that resolve according to the presentation catalog contract;
- unique identity independent from user-editable labels;
- every declared resource limit.

Labels are presentation text and may be duplicate unless product behavior explicitly requires uniqueness.
Preset add/update/remove APIs key by stable preset id, never by label in one command and id in another.

Validation produces structured path and reason information for diagnostics.
It does not install partial vectors or skip malformed elements.

### Candidate preparation and atomic restore

After deserialize and semantic validation, restore prepares one `WorkspaceSessionCandidate`:

1. Verify the live workspace is in the documented admissible state, normally empty during startup.
2. Acquire or prepare every candidate view without publishing it.
3. Resolve the exact active candidate by session view key.
4. Prepare the custom preset catalog and initial navigation point.
5. Revalidate library and workspace revisions.
6. Install the complete aggregate through RFC 0016's no-fail candidate commit.
7. Publish one `WorkspaceChanged` event carrying one new revision and restore cause.

Any preparation failure releases all candidates and leaves live workspace, history, preset catalog, and revision unchanged.
Restore on a non-empty workspace is rejected unless an explicit `ReplaceCurrent` operation was selected.
`ReplaceCurrent` stages both the incoming candidate and release of the old aggregate under the same workspace transaction; it never appends implicitly.

An intentionally empty valid session installs an empty aggregate and returns `RestoredEmpty`.
A missing group returns `FirstRun` without creating or saving defaults.
Those outcomes are distinct even though both leave no open views.

### Restore and save outcomes

Expose domain results rather than a bare success flag:

```text
WorkspaceRestoreOutcome
  FirstRun
  Restored(revision, viewCount)
  RestoredEmpty(revision)
  RejectedIncompatibleLibrary(expected, actual)
  RejectedUnsupportedVersion(version)
  RejectedInvalid(diagnosticId)
```

The concrete C++ shape may use `Result` plus a successful outcome variant.
Errors preserve typed storage, parse, schema, library, and view-preparation evidence.

Save captures one canonical workspace revision and returns a commit receipt naming that revision.
If the workspace advances while serialization or writing, the receipt acknowledges only the captured revision and the owner remains dirty for a newer checkpoint.
The current grouped-store result proves one applied candidate replacement; this RFC owns correlation between that result and the captured workspace revision.

Reporting policy under RFC 0013 decides which outcomes are silent first-run behavior, visible recovery, actionable incompatibility, or diagnostic-only preference loss.
The schema and store never post notifications directly.

### Legacy input policy

Unversioned development snapshots are rejected rather than migrated.
The presentation-only transitional document introduced by RFC 0010 is likewise not a supported predecessor for the future root version 1 unless implementation evidence establishes a concrete compatibility requirement before this RFC is accepted.

This clean-start policy avoids freezing ambiguous `activeListId` focus heuristics, development-era root names, and unbounded documents as a permanent decoder.
Unsupported future input is rejected through its explicit version path, not treated as legacy.

### Recovery ownership

An incompatible library, unsupported version, malformed session, or exceeded limit leaves the stored group untouched.
Runtime may continue with a clean in-memory workspace so the application can open, but it marks the session unresolved and suppresses automatic checkpoint overwrite.

Replacing rejected input requires an explicit recovery choice owned by the application lifecycle:

- keep and export the rejected payload;
- reset the workspace session for this library;
- select the correct library;
- retry after a transient store failure.

Recovery authorization belongs to the workspace-session owner and may use the current candidate save only after that explicit choice.
Starting with defaults is not itself authorization to destroy the rejected session.

## Alternatives

### Persist the active view's list and filter

That distinguishes common same-list views but duplicates a view entry, still needs exact matching rules, and becomes ambiguous when duplicate equivalent views are intentionally open.
A session-local entry key is a direct reference.

### Persist the runtime `ViewId`

`ViewId` is allocated by one `ViewService` lifetime and has no meaning after restart.
Persisting it would turn execution identity into accidental durable identity.

### Use the active view's sequence index

An index is compact, but dropping or reordering one entry during migration changes its meaning.
Strict version-1 deserialization makes an index possible, yet an explicit key is clearer and supports future internal references without coupling them to display order.

### Rely on the per-library file path

Paths can be copied, roots can move, and application selection bugs can associate the wrong file with a runtime.
Binding to the existing durable library UUID turns that association into a checked invariant.

### Keep permissive salvage

Dropping one malformed view appears resilient but can change focus, ordering, and user intent, then overwrite the original file on checkpoint.
Explicit recovery preserves evidence and makes any loss a policy decision.

### Version only nested presentation state

RFC 0010 protects presentation meaning but does not fix active-view ambiguity, library binding, root field names, resource limits, or complete workspace-candidate validation.
The schemas integrate but retain separate owners.

### Store workspace state in the library database

That would provide library identity and transactions but couples per-user interactive state to shareable library content and backup/export semantics.
A library-bound managed document preserves separate ownership.

## Compatibility and migration

The repository has no source-compatibility requirement for the internal session types.
Version 1 is designed without preserving reflected aggregate layout or numeric presentation enums.

Implementation proceeds in phases:

1. Lock rejection of unversioned and presentation-only transitional documents with read-only fixtures.
2. Add stable session view keys, exact active-view capture, library UUID binding, explicit limits, and domain validation in an isolated schema.
3. Add successful outcome variants and rejected-session suppression so startup defaults cannot overwrite unresolved input.
4. Implement candidate restore through RFC 0016 and remove implicit append behavior.
5. Reuse RFC 0010's nested stable presentation schema in the new envelope without duplicating its token catalog.
6. Integrate the current candidate read/save boundary with workspace revision acknowledgement and explicit owner-controlled recovery before enabling ordinary version-1 checkpoints.
7. Migrate preset mutation to stable-id identity and remove the transitional presentation-only workspace document.
8. Add reporting dispositions and reset/export workflows, then remove temporary compatibility adapters.

No automatic rewrite happens on inspection or rejected restore.
Rejected predecessor input is never rewritten as a side effect of inspection.
The first explicit successful checkpoint after an owner-authorized reset writes only canonical version 1.

## Validation

- Two or more views over the same list with different filters and presentations restore the exact previously active entry.
- Duplicate equivalent views remain distinct entries and the active key resolves one exact entry.
- Runtime `ViewId` values never appear in the serialized document.
- A workspace file whose `libraryId` differs from the active library is rejected before any source or view creation.
- Moving one library root while preserving its database identity does not invalidate its session.
- Duplicate view keys, dangling active keys, invalid nullability, duplicate preset ids, reserved-id collisions, and unresolved required references reject the complete candidate.
- Unknown stable presentation ids and invalid nested presentation objects follow RFC 0010's documented object/session policy without enum casting.
- Every view, expression, preset, identifier, and aggregate limit is tested at below, equal, and above boundary values.
- Oversized or malformed input creates no partial views and is not overwritten by startup defaults.
- Restore on a non-empty workspace rejects by default; explicit replacement commits one coherent workspace revision.
- Missing, intentionally empty, restored, incompatible, unsupported, invalid, and migrated outcomes are distinguishable without message parsing.
- A failed candidate preparation leaves workspace snapshot, preset catalog, history, view catalog, and revision unchanged.
- One successful restore publishes exactly one workspace observation after the complete aggregate is valid.
- A save receipt acknowledges the captured workspace revision only, and a newer revision remains dirty.
- Unsupported future versions and incompatible-library sessions remain byte-for-byte unchanged until explicit recovery.
- Golden fixtures lock predecessor rejection and every canonical version-1 root field and stable id.
- Repository searches reject production use of generic reflected serialization for the workspace root after implementation.
- Completed implementation passes `./ao check` and the documentation gate.

## Open questions

- Should version 1 use human-readable session view keys, UUIDs, or bounded unsigned integers?
- Is a valid session allowed to contain zero views and nonempty custom presets, and how should that outcome be presented?
- Which initial resource limits fit production libraries and custom preset workflows without making startup memory proportional to attacker-controlled input?
- Should list deletion mark the next session checkpoint dirty immediately, or is ordinary workspace closure sufficient evidence?
- Does a library restore that adopts a different `libraryId` offer an explicit option to rebind and validate the previous workspace, or always require reset/export?

## Promotion plan

If accepted and implemented, update the [workspace architecture](../architecture/workspace.md) with library binding, semantic session candidates, restore outcomes, and rejected-session ownership.
Replace current snapshot, restore, fallback, failure, observation, and versioning behavior in the [workspace session specification](../spec/workspace/session.md) phase by phase.
Replace the transitional field inventory in the [workspace session state reference](../reference/workspace/session-state.md) with the implemented versioned envelope, stable keys, UUID binding, limits, validation, canonical serialization, and explicit predecessor rejection.

Update the [persistence and managed-state architecture](../architecture/persistence-and-managed-state.md), [grouped configuration store specification](../spec/persistence/config-store.md), and [application managed-state surface](../reference/persistence/application-config.md) only if workspace recovery adds a new generic persistence contract; otherwise keep unresolved-session state and revision acknowledgement with the workspace owner.
RFC 0010's presentation architecture/spec/reference promotion is complete.
Reuse that stable vocabulary and do not duplicate its field catalog in the future workspace envelope reference.
Update library metadata reference only if workspace consumers need a new public library-identity accessor, not to redefine the UUID format.

Update the GTK active-library lifecycle and future frontend-neutral session lifecycle specifications with incompatible-library, unsupported-version, first-run, and explicit-reset dispositions.
Record a decision if library UUID binding, strict whole-session rejection, or session-local view keys become durable choices with credible alternatives.
