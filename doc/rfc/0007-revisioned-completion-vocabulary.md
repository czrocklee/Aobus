---
id: rfc.0007.revisioned-completion-vocabulary
type: rfc
status: draft
domain: presentation
summary: Proposes deletion-correct, revisioned, asynchronously maintained completion vocabulary snapshots.
depends-on: none
---
# RFC 0007: Revisioned completion vocabulary

## Problem

`CompletionService` owns tag, custom-key, and metadata-value vocabularies derived from the active library.
Its change subscription marks caches dirty for inserted and mutated tracks but omits `tracksDeleted`.
Deleting the last track that contributes a tag, key, artist, genre, or other value can therefore leave a stale suggestion until another mutation or process restart.

Dirty caches rebuild lazily on the owner thread.
The first completion request after a change scans the complete hot or cold track store, counts values, sorts them, and only then returns suggestions.
The implementation coalesces fields by storage tier, but large-library I/O and counting still occur on the interactive completion path.

Thread safety is documented through owner-thread confinement and guarded with the C `assert` macro.
Release builds may compile that check out, turning a future off-executor delivery change into an unguarded data race rather than a deterministic contract failure.

The change event includes affected track ids, but `markDirty()` ignores them.
The service has no retained per-track contribution state with which to subtract deleted or previous mutated values.

## Dependencies

- Hard: None.
- Conditional: None.
- Integration: [RFC 0008](0008-declarative-track-capability-bridge.md) must agree with the indexed value-completion capability set, and [RFC 0003](0003-library-mutation-pipeline.md) must preserve the ordered change input if both proposals are implemented.

## Goals

- Remove deleted values immediately and correctly from every vocabulary.
- Keep completion reads bounded and free of full-library scans on the interactive thread.
- Maintain one immutable vocabulary snapshot correlated to a library revision.
- Update inserts, mutations, and deletions symmetrically.
- Preserve current frequency ordering and ASCII-insensitive prefix behavior.
- Keep query and metadata editors on one shared runtime vocabulary authority.
- Enforce executor/lifetime rules in release builds.
- Bound memory and worker concurrency for production-scale libraries.

## Non-goals

- Changing query completion syntax or operator suggestions.
- Making prefix comparison Unicode-aware independently of query comparison semantics.
- Moving completion vocabulary into core `ao_query`.
- Persisting the derived index as authoritative library truth in the first implementation.
- Replacing library mutation publication; RFC 0003 remains the proposal for that pipeline.
- Defining query-to-TrackField mappings; [RFC 0008](0008-declarative-track-capability-bridge.md) owns that bridge.

## Proposed design

### Completion index owner

Replace lazy mutable caches with a runtime-owned `CompletionIndex` composed by `CoreRuntime`.
It observes ordered `LibraryChangeSet` revisions and publishes immutable snapshots:

```cpp
struct CompletionVocabularySnapshot final
{
  LibraryRevision libraryRevision;
  CompletionIndexRevision indexRevision;
  CompletionIndexPhase phase;
  Vocabulary tags;
  Vocabulary customKeys;
  TrackFieldVocabularyMap values;
  std::optional<Error> error;
};
```

`CompletionService` becomes a narrow query facade over the latest installed snapshot.
`QueryExpressionCompleter` and `MetadataValueCompleter` retain their present result and replacement responsibilities.

Readers obtain immutable spans or shared snapshot ownership and do not trigger storage I/O.
System variable and operator completion remains available even while live vocabulary is building.

### Per-track contributions

The index retains one compact contribution record for every indexed track:

```cpp
struct TrackCompletionContribution final
{
  TrackId trackId;
  small_vector<DictionaryId> tags;
  small_vector<DictionaryId> customKeys;
  array<DictionaryId, valueFieldCount> values;
};
```

The concrete representation may use sparse entries, but it stores ids rather than duplicate strings.
This state allows symmetric updates:

- insert: read the new track, add its contributions, retain the record;
- mutate: subtract the retained record, read and add the new record;
- delete: subtract and erase the retained record without rereading a deleted store row.

Frequency maps retain counts by dictionary id.
An entry disappears from a published vocabulary when its count reaches zero.
String resolution and final ordering happen when building the immutable snapshot.

### Startup and reset rebuild

Initial construction and library reset use a bounded worker job:

1. Capture target library revision and cancellation generation on the callback executor.
2. Scan required hot and cold records using a worker-owned read transaction.
3. Build contribution and frequency maps off-thread.
4. Sort immutable vocabulary values off-thread.
5. Return the complete candidate snapshot.
6. Install only if its library revision/generation is still current; otherwise rebuild or apply queued ordered changes.

No partial index becomes public as `Ready`.
The service may publish `Building` with an empty live vocabulary while retaining core query suggestions.

### Incremental update serialization

Ordered `LibraryChangeSet` delivery remains the input authority.
The completion index serializes update application by library revision on its callback executor.

Small batches may update contribution maps and build the next immutable snapshot in one callback turn when a measured budget permits.
Larger batches or expensive cold reads use a worker candidate with a generation barrier.
Only one candidate per index is active; later revisions coalesce into ordered pending changes or supersede a full rebuild.

Every installed snapshot states the exact highest library revision it incorporates.
Completion consumers never combine values from separately published tag, key, and field revisions.

### Thread and lifetime contract

Mutable contribution and frequency state has one serialized owner.
Public readers see immutable snapshot ownership and need no mutation lock.

Executor-affinity violations use repository contracts that remain active in production configuration, not only `assert`.
Worker jobs own explicit library/task lifetime tokens and return through the callback executor.
Shutdown closes admission, cancels rebuilds, waits for workers, and then releases snapshots and the library.

### Memory and scale budgets

The first implementation stores dictionary ids only for the currently supported field set.
Acceptance establishes budgets for contribution bytes per track, startup rebuild time, incremental batch time, snapshot allocation, and query latency.

If the contribution index exceeds its memory budget, an explicit degraded mode may retain only tags/custom keys or fall back to scheduled full rebuilds.
It cannot silently return stale deleted values.

## Alternatives

### Add `tracksDeleted` to `markDirty()`

This fixes correctness after the next access but still moves a full scan onto the interactive completion path.
It should be applied as an immediate bug fix if the full RFC is delayed.

### Keep lazy scans but run them on a worker

Worker scans improve responsiveness, but without per-track contributions every mutation still invalidates an entire tier and deletion correctness waits for a full rebuild.
The selected design makes ordinary changes incremental.

### Derive vocabulary only from the dictionary store

The dictionary includes historical and reserved values that may no longer occur in any track.
It cannot provide current frequency or delete the last occurrence correctly.

### Persist a secondary completion database

Persistence can reduce startup cost but creates schema, recovery, and transaction-coherence obligations.
The first implementation keeps the index rebuildable and in memory; persistence can follow measured need.

### Add mutexes to the current service

Mutexes prevent races but do not fix deletion omission, full-scan latency, or mixed-revision publication.

## Compatibility and migration

No user configuration or library database format change is required.
Suggestion ordering remains frequency descending and value ascending; deleted values disappear sooner, which is a correctness change.

Implementation phases are:

1. Include deletions in invalidation and add last-occurrence deletion regression tests.
2. Introduce immutable snapshots and make completion reads I/O-free while rebuild remains full-scan.
3. Move startup/reset rebuild to worker execution with revisioned installation.
4. Add per-track contributions and symmetric incremental updates.
5. Replace debug-only thread checks with the production executor contract and add shutdown/concurrency tests.

The existing `CompletionService` API may remain as a facade during migration.

## Validation

- Deleting the last occurrence of every supported tag, custom key, and metadata value removes it from the next installed snapshot.
- Mutating one track from value A to B decrements A, increments B, and preserves deterministic frequency ordering.
- Insert, mutate, and delete storms produce the same snapshot as a clean full rebuild at the same library revision.
- Completion reads perform no library transaction and remain within a defined latency budget while a rebuild is active.
- A slow startup scan does not block callback-executor heartbeat or ordinary commands.
- A stale rebuild cannot overwrite a snapshot that incorporates a newer revision.
- Query and metadata completion observe the same vocabulary revision.
- System fields and operators remain completable while live values are `Building` or failed.
- Shutdown and library switching produce no callback or read after library destruction.
- ThreadSanitizer and repeated mutation tests report no race.
- Memory and rebuild benchmarks cover representative small, large, and high-cardinality libraries.
- Completed implementation passes `./ao check`.

## Open questions

- Should incremental snapshot publication occur after every library revision or coalesce within one callback turn?
- Is a persistent rebuildable completion cache justified after measuring startup cost?
- What memory budget and degraded behavior are acceptable for high-cardinality custom metadata?
- Should vocabulary matching remain ASCII-insensitive until the predicate evaluator adopts a shared Unicode normalization contract?

## Promotion plan

If accepted and implemented, update:

- [Library architecture](../architecture/library.md) with completion-index ownership and lifetime;
- [Track expression architecture](../architecture/track-expression.md) with immutable vocabulary composition;
- [Track-field value completion](../spec/presentation/field-completion.md) with revision, phases, and incremental behavior;
- [Query expression completion](../spec/query/expression-completion.md) with snapshot availability behavior;
- [Runtime track field catalog](../reference/library/model/track-field.md) if the indexed capability surface changes;
- runtime execution and concurrency development guidance for the worker/callback snapshot pattern.
