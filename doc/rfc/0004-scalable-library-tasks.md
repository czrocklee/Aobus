---
id: rfc.0004.scalable-library-tasks
type: rfc
status: draft
domain: library
summary: Proposes phased, cancellable, and bounded library scan and transfer tasks for large libraries.
depends-on: rfc.0003.library-mutation-pipeline
---
# RFC 0004: Scalable library task execution

## Problem

Scan application currently opens one LMDB write transaction for the complete plan and performs audio-file opening, tag parsing, serialization, and optional fingerprinting while that transaction remains active.
For a large library this holds the single writer for an unbounded interval, grows transaction pressure, delays unrelated mutations, and discards all completed work when cancellation aborts the plan.

The scan plan records file size and modification time before application.
New and changed items parse the live file but write the planned stat values without a general stable-file pre/post revalidation, so a file changed between planning and application can produce an internally stale snapshot until the next scan.

Task cancellation is also uneven.
YAML import and export have no checkpoint after synchronous transfer begins, and scan planning has no checkpoint during its filesystem walk.
Source and projection delivery are correct and incremental, but the subsystem has no explicit callback-latency, writer-hold, memory, or 10k/50k/100k library performance budgets.

The current behavior is defined by the [task-execution specification](../spec/library/runtime/task-execution.md), [scan and identity specification](../spec/library/runtime/scan-and-identity.md), and [YAML transfer specification](../spec/library/runtime/yaml-transfer.md).

## Dependencies

- Hard: [RFC 0003](0003-library-mutation-pipeline.md) supplies the unified coordinator required for bounded commit batches and publication.
- Conditional: [RFC 0002](0002-durable-library-storage.md) supplies checkpoint and activation facilities if destructive restore uses its proposed staged-database path.
- Integration: Transfer and restore execution must preserve the strict schema and preview-bound authorization owned by [RFC 0001](0001-safe-library-transfer.md) when both proposals are implemented.

## Goals

- Keep filesystem traversal, parsing, and hashing outside LMDB write transactions.
- Bound writer ownership, memory growth, callback work, and cancellation latency for large operations.
- Revalidate every prepared file against live stable file facts before committing it.
- Preserve committed progress when a cancellable scan or backfill stops.
- Preserve all-or-nothing semantics for destructive restore without performing slow input work under the target write transaction.
- Define measurable production budgets and deterministic scale tests.
- Keep progress and completion reports truthful under partial success, cancellation, and retry.

## Non-goals

- Change track-source or projection semantic delta contracts.
- Make non-cryptographic audio identity a security boundary.
- Run two LMDB write transactions concurrently.
- Hide per-file media corruption or permission failures.
- Define the strict YAML schema or destructive authorization workflow proposed by RFC 0001.

## Proposed design

### Common task phases

Long library operations use four explicit phases:

```text
discover
  -> prepare immutable work on workers
  -> revalidate and commit through bounded mutation batches
  -> publish progress and semantic changes on callback executor
```

Every phase accepts a stop token and reports a phase-specific progress unit.
Prepared values own their memory and do not retain transaction-scoped views, mapped file borrows, or mutable filesystem iterators.

The runtime limits outstanding prepared bytes and item count.
Backpressure pauses discovery or preparation before memory exceeds the configured task budget.

### Stable file snapshots

A shared stable-file helper captures canonical library URI, size, modification time, and any platform file identity needed before parsing or hashing.
After preparation it captures the facts again and accepts the result only when they match.
The commit phase rechecks the current manifest generation and rejects or replans a stale prepared item.

The same helper is used by scan application, audio identity backfill, and YAML modes that read an audio-file baseline.
Manifest rows always record the accepted live snapshot rather than an earlier plan value.

### Bounded scan commits

Scan discovery produces classifications against one storage snapshot and assigns a scan generation.
Workers parse and fingerprint candidate items without a write transaction.
The mutation coordinator commits a bounded item and byte batch after revalidating its source manifest rows and stable files.

Each successful batch publishes one changeset and advances durable progress.
Cancellation stops new preparation, discards only unfinished prepared values, and retains already committed batches.
A later retry starts from current manifest state and does not require a special rollback log.

Move relinking remains atomic per item: old manifest key removal, track URI preservation, refreshed technical properties, and new manifest key creation commit together.
Ambiguous or stale move matches return to planning rather than degrading into an unsafe relink.

### Transfer execution

YAML parsing, base64 decoding, validation, baseline reads, and record preparation happen before exclusive target mutation.
Merge commits bounded batches when its field and list-reference semantics can be made retry-safe.

Destructive restore retains all-or-nothing visibility.
It builds and verifies a staging database or equivalent shadow generation, then activates the complete result in one exclusive operation after preview authorization is revalidated.
It does not clear the active target and incrementally expose a partially restored catalog.

Export walks one LMDB read snapshot but emits output through bounded buffers and checks cancellation between records and encoded resource chunks.
Long-lived read snapshots expose elapsed time and retained-page metrics so operational tooling can diagnose storage pressure.

### Budgets and observability

The task specification defines configurable defaults and test ceilings for:

- maximum LMDB writer-hold duration per batch;
- maximum prepared item count and bytes;
- stop-request to task-completion latency outside an active system call;
- callback-executor work per progress or changeset turn;
- scan planning and application throughput at representative library sizes;
- peak memory and read-snapshot lifetime.

Progress events are coalesced to a bounded callback rate.
Completion reports distinguish discovered, prepared, committed, skipped-stale, failed, and cancelled counts.
Logs include phase, batch, elapsed time, and capacity metrics without exposing user metadata unnecessarily.

## Alternatives

### Keep one transaction and optimize file parsing

Faster parsing reduces average duration but leaves writer ownership and cancellation cost proportional to the complete plan.

### Commit each file independently

This minimizes rollback and writer duration but creates excessive transaction and publication overhead.
Bounded adaptive batches retain progress while amortizing fixed costs.

### Preserve whole-scan atomicity with a longer transaction

Whole-scan atomicity is simple to reason about, but scan state is already a reconciliation of an independently changing filesystem.
Per-item atomicity plus generation revalidation provides a better production boundary.

### Make restore incrementally visible

Incremental restore avoids a staging database but exposes a catalog that may temporarily lack tracks, lists, or references and complicates failure recovery.
Staged activation preserves the existing destructive-operation promise without slow work under the active writer.

## Compatibility and migration

Bounded scan commits change cancellation from whole-plan rollback to preservation of completed batches.
The promoted scan specification and frontend messaging must make that behavior explicit.
Track and list content remain semantically equivalent to a successful fresh scan or transfer.

Existing databases require no physical change for scan batching alone.
Staged restore may depend on checkpoint and activation facilities from [RFC 0002](0002-durable-library-storage.md).
Task report structures may gain counters and phase information; frontend adapters migrate with the runtime surface in the same change.

## Validation

- Deterministic tests mutate files between discovery, preparation, and commit and prove stale values never reach manifest or track records.
- Cancellation tests cover every phase and assert bounded latency and accurate committed counts.
- Concurrency tests prove hashing and parsing occur without the mutation coordinator or LMDB writer held.
- Scale tests exercise at least 10k, 50k, and 100k manifest entries with explicit time, memory, writer-hold, and callback-work budgets.
- Failure injection covers worker failure, batch commit failure, target revision conflict, staging activation failure, and shutdown during each phase.
- Oracle tests compare completed batched results with a fresh rebuild and compare cancelled results with current manifest truth.
- Restore tests prove the active database is either entirely old or entirely new across cancellation and injected failure.
- ThreadSanitizer tests cover task queues, bounded buffers, progress coalescing, and shutdown.

## Open questions

- Should batch size be fixed, byte-based, duration-adaptive, or a combination?
- Which scan classifications can be prepared concurrently without increasing duplicate parsing or stale work excessively?
- Should a stale item be retried within the same task or reported for the next scan?
- Can merge import become retry-safe per batch, or should it use the same staging mechanism as restore?
- What performance budgets should be release gates on shared CI hardware versus recorded benchmark baselines?

## Promotion plan

- Update [library architecture](../architecture/library.md) and [runtime execution architecture](../architecture/runtime-execution.md) with phased task and coordinator boundaries.
- Update the [task-execution specification](../spec/library/runtime/task-execution.md), [scan and identity specification](../spec/library/runtime/scan-and-identity.md), and [YAML transfer specification](../spec/library/runtime/yaml-transfer.md).
- Update the [library database reference](../reference/library/storage/database.md) only if scan generations or staged activation add physical fields.
- Add a decision record for batch policy or staged restore when the selected alternative has durable rationale.
- Add development guidance for scale budgets, failure injection, and representative performance fixtures.
