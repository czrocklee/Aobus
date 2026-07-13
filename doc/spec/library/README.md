---
id: library.spec-index
type: index
status: current
domain: library
summary: Routes the normative contracts for library reads, mutations, scans, changes, sources, projections, and transfers.
---
# Library specifications

## Purpose

These specifications define the observable behavior between the physical music library and frontend-neutral runtime consumers.
Start here when changing a library command, scan, revision event, source, projection, or transfer workflow.

## Responsibilities

- Point-in-time reads and one-operation mutation transactions.
- Track and list mutation semantics, previews, reports, and failures.
- Revisioned change publication and asynchronous task notifications.
- Scan planning, file reconciliation, audio identity, and identity backfill.
- Ordered source membership and live projection delta behavior.
- YAML export and import behavior.

## System context

The Library contract spans three application-runtime code boundaries below the core storage layer defined by [system architecture](../../architecture/system-overview.md):

| Boundary | Code | Owns here |
|---|---|---|
| Core library storage | `include/ao/library/`, `lib/library/` | Exact models and storage are delegated to [Library reference](../../reference/library/README.md). |
| Runtime library facade | `app/include/ao/rt/library/`, `app/runtime/library/` | Reads, commands, tasks, changes, scans, identity, and transfers. |
| Runtime sources | `app/include/ao/rt/source/`, `app/runtime/source/` | Ordered membership, leases, cache identity, and source deltas. |
| Runtime projections | `app/include/ao/rt/projection/`, `app/runtime/projection/` | Frontend-neutral list/detail snapshots and projection deltas. |

## Out of scope

Physical ownership and dependency direction belong to [library architecture](../../architecture/library.md).
Exact storage layouts and domain surfaces belong to [library reference](../../reference/library/README.md).
Query grammar, playback succession, and frontend rendering belong to their respective subsystem owners.

## Document map

### Runtime library facade

- [Library access and mutation](runtime/mutation.md) defines coherent reads, command transactions, previews, track/list mutations, and no-op behavior.
- [Library change publication](runtime/change-publication.md) defines revisions, changesets, task notifications, ordering, and executor delivery.
- [Library task execution](runtime/task-execution.md) defines worker/callback affinity, mutation serialization, progress, completion, and cancellation.
- [Library scan and audio identity](runtime/scan-and-identity.md) defines classification, reconciliation, relinking, cancellation, and identity backfill.
- [Library YAML transfer](runtime/yaml-transfer.md) defines export, restore, merge, preview, reporting, and change publication.

### Runtime sources

- [Track sources](source/track-source.md) defines ordered membership, leases, source deltas, caches, and dependency behavior.

### Runtime projections

- [Track-list projection](projection/track-list.md) defines row ordering, grouping, incremental deltas, invalidation, and arena rebasing.
- [Track-detail projection](projection/track-detail.md) defines selection targets, field aggregation, refresh, and snapshot publication.

## Recommended reading paths

- Command authors: mutation, then change publication.
- Long-running operation authors: task execution, then the operation-specific specification.
- Scan and recovery authors: scan and audio identity, then storage reference.
- View and playback authors: track source, then the relevant projection and presentation or playback specification.
- Import/export authors: YAML transfer, then YAML format reference.

## Implementation and test map

- Runtime library roles: `app/include/ao/rt/library/` and `app/runtime/library/`.
- Sources and projections: `app/include/ao/rt/source/`, `app/runtime/source/`, `app/include/ao/rt/projection/`, and `app/runtime/projection/`.
- Contract tests: `test/unit/runtime/library/`, `test/unit/runtime/source/`, and `test/unit/runtime/projection/`.
