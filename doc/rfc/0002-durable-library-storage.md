---
id: rfc.0002.durable-library-storage
type: rfc
status: draft
domain: library
summary: Proposes explicit durable and regenerable library data classes with migration, recovery, and storage maintenance.
depends-on: none
---
# RFC 0002: Durable library storage lifecycle

## Problem

The current database reference describes the complete LMDB environment as a host-local rebuildable index and provides no in-place migration path.
Opening a database with a different physical version fails, after which reset and rescan are the documented recovery path.

The same environment also owns user-created lists, list membership, curated metadata, custom metadata, tags, selected cover resources, stable track identities, and the library identity.
The scan contract deliberately preserves edited metadata as database-authoritative state.
Reset and rescan therefore cannot reconstruct all data that the database currently owns.

The environment defaults to a fixed 1 GiB map.
Dictionary strings and content-addressed resources can outlive the records that once referenced them, but there is no production reachability collection, compaction workflow, map-usage telemetry, or automatic growth policy.
Page truncation and external corruption also lack an application recovery path beyond surfacing the underlying failure.

The current ownership is defined by the [persistence and managed-state architecture](../architecture/persistence-and-managed-state.md), and the exact physical behavior is defined by the [library database reference](../reference/library/storage/database.md).

## Dependencies

- Hard: None.
- Conditional: None.
- Integration: None.

## Goals

- Classify every persisted library fact as durable user/catalog truth, regenerable derived state, or shared storage supporting one of those classes.
- Preserve durable truth across supported schema upgrades without requiring a user-managed export.
- Create a verified recovery point before a migration can alter the active database.
- Detect unsupported, incomplete, or corrupt storage early and present an actionable recovery path.
- Reclaim unreachable dictionary and resource data safely.
- Grow and compact the LMDB environment according to observable limits rather than a hidden fixed ceiling.
- Keep YAML as portable interchange rather than the primary durability mechanism.

## Non-goals

- Provide cloud synchronization or multi-device conflict resolution.
- Make the host-local LMDB byte layout portable across architectures.
- Treat audio files as authoritative for every user edit.
- Guarantee recovery from arbitrary simultaneous loss of the active database and all checkpoints.
- Change track, list, or manifest application behavior except where migration and recovery require it.

## Proposed design

### Persistence classes

The persistence architecture classifies library state as follows:

| Class | Examples | Recovery authority |
|---|---|---|
| Durable catalog truth | Library id, stable track identity, curated metadata, custom metadata, tags, lists, list membership, user-selected covers. | Migrated database and verified checkpoints. |
| Regenerable derived state | File size and time observations, availability state, technical properties that can be reread, pending or calculated audio identity. | Audio files plus a new scan. |
| Shared storage | Dictionary strings and cover blobs referenced by durable or derived records. | Reachability from live records; never assumed regenerable as a whole. |

Physical co-location in one LMDB environment remains allowed.
The classification controls migration, backup, verification, and garbage-collection policy rather than requiring a database split.

### Stepwise schema migration

`MusicLibrary::open` distinguishes a corrupt header, a newer unsupported version, a current version, and an older supported version.
Older supported versions are upgraded by a registry of explicit `N -> N+1` migrators.
Each migrator validates its input version, writes only the next version, and has fixture-based tests for success, rejection, and interruption.

Migration uses a staged database location:

```text
close active writers
  -> create and verify checkpoint
  -> copy or transform into staging environment
  -> deep-verify staging
  -> atomically select staging as active
  -> retain previous checkpoint for rollback
```

The active database is never migrated destructively in place.
Startup does not silently reset durable catalog truth when migration fails.

### Checkpoint and recovery

A library-local checkpoint manager owns consistent LMDB copies and their metadata.
Every checkpoint records the library id, physical version, revision, creation time, and integrity-verification result.
Automatic checkpoints run before migration and before an explicitly destructive maintenance operation.

On open failure, the application reports whether a verified checkpoint is available and offers an explicit restore path.
Restoring a checkpoint preserves the failed database for diagnosis until the user or maintenance policy removes it.
Portable YAML export remains an additional user-controlled backup but is not required for normal schema evolution.

### Verification and maintenance

A maintenance service provides read-only quick verification and offline deep verification.
Deep verification checks database structure, record semantics, cross-store identities, list parents and membership, manifest bindings, dictionary references, resource references, and metadata version consistency.

Reachability collection scans current track and list records, marks referenced dictionary and resource ids, and removes only unmarked shared rows inside a maintenance transaction.
Collection emits reclaimed row and byte counts and is preceded by a verified checkpoint until the implementation has sufficient operational history.

The runtime records current map size, used pages, free pages, and growth attempts.
Before a write reaches the configured safety margin, the storage owner grows the map geometrically up to a configurable ceiling.
An offline compaction operation rewrites live pages into a staged environment and uses the same verification and activation protocol as migration.

### Ownership

Core `ao::library` owns physical version inspection, migrators, verification primitives, reachability, and map resizing.
Application runtime owns maintenance scheduling, progress, cancellation, user authorization, checkpoint retention, and recovery presentation.
Frontends invoke runtime operations and display results; they do not manipulate LMDB paths directly.

## Alternatives

### Continue requiring YAML export and reset

This keeps storage code small but makes preservation of database-authoritative user data depend on a manual action performed before failure or upgrade.
It is not a sufficient production durability contract.

### Write every curated edit back into audio files

File write-back improves portability for supported tags but cannot represent every list, identity, custom value, or application-specific choice.
It may complement checkpoints but cannot replace the durable catalog.

### Split durable and derived state into separate databases immediately

Physical separation makes rebuild boundaries obvious but introduces identity coordination, two-store commit, and migration complexity.
The proposed logical classification captures the required policy before committing to a physical split.

### Grow the map only after `MDB_MAP_FULL`

Reactive growth is simpler but turns a predictable capacity condition into a failed user mutation.
Proactive growth provides better observability and permits a clear maximum-size policy.

## Compatibility and migration

The first implementation introduces migration support while the current physical version remains readable.
Before the first new physical version is activated, fixture databases for every supported prior version become test assets.

Libraries from newer unsupported Aobus versions remain read-protected and are never downgraded automatically.
Checkpoint metadata is versioned independently from the library records so a newer application can inspect older recovery points.
No current database is relabeled as fully rebuildable after this RFC is promoted.

## Validation

- Golden fixture tests migrate every supported version step by step and compare semantic content rather than raw host bytes.
- Failure injection at every staging and activation boundary proves either the previous active database or a verified replacement remains selectable.
- Tests prove lists, stable identities, metadata, tags, custom fields, covers, and library id survive migration and checkpoint restore.
- Deep-verification tests cover corrupt headers, invalid record slices, dangling cross-store ids, list cycles, missing resources, and manifest mismatches.
- Reachability tests retain shared resources, reclaim unreferenced resources and dictionary rows, and remain idempotent.
- Capacity tests exercise proactive growth, configured ceilings, growth failure, and post-growth readers.
- Compaction tests prove semantic equivalence and reduced allocated size on a churned fixture.
- Runtime tests prove maintenance progress, cancellation boundaries, and shutdown lifetime safety.

## Open questions

- How many prior physical versions must one release support directly?
- What checkpoint retention policy balances recovery value against large music-library storage cost?
- Should durable catalog truth eventually live in a physically separate environment from scan-derived state?
- Which verification checks may run online under an LMDB snapshot, and which require exclusive offline access?
- Can dictionary ids be rewritten during compaction, or must maintenance preserve them for diagnostic and compatibility reasons?

## Promotion plan

- Update [persistence and managed-state architecture](../architecture/persistence-and-managed-state.md) with durable, derived, checkpoint, and maintenance ownership.
- Update [library architecture](../architecture/library.md) with the core/runtime maintenance boundary.
- Add migration, checkpoint, recovery, verification, and maintenance specifications under `doc/spec/library/`.
- Update the [library database reference](../reference/library/storage/database.md) with migration compatibility, checkpoint metadata, map policy, and any new physical version.
- Add a decision record if physical co-location versus separation or checkpoint retention has durable rationale.
- Add user and development recovery guides before exposing maintenance operations outside tests.
