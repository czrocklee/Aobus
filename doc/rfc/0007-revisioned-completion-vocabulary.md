---
id: rfc.0007.revisioned-completion-vocabulary
type: rfc
status: rejected
domain: presentation
summary: Rejected a revisioned asynchronous index after deletion-correct invalidation, a typed field bridge, and one shared lazy frequency snapshot closed the verified gaps without per-track contribution state.
depends-on: none
---
# RFC 0007: Revisioned completion vocabulary

## Disposition

Rejected on 2026-07-16.

The deletion correctness problem was real, but the proposed revisioned snapshot service, worker rebuild protocol, per-track contribution index, generation barriers, degraded modes, and production memory budgets were broader than the verified need.
A narrower implementation now:

- invalidates one shared live-frequency snapshot after every committed track insertion, mutation, deletion, or library reset;
- leaves list-only changes alone;
- derives the value-completable field set and dictionary extraction from the typed runtime-to-query bridge implemented with [RFC 0008](0008-declarative-track-capability-bridge.md);
- rebuilds source-preserving title, tag, custom-key, and dictionary-field frequencies in one lazy traversal with both track-store tiers;
- materializes sorted individual vocabularies and a caller-selected unordered aggregate from those retained frequencies without further track-store scans; and
- covers deletion of the final contributor for tags, custom keys, and every supported metadata field with one regression contract.

Before cross-field Quick-filter completion existed, a targeted Release measurement on a Ryzen 9 9950X with a warm tmpfs-backed library found that coalescing every dirty field in a tier made a representative 100k-track first read take about 13.9 ms for the hot tier and 20.9 ms for the cold tier.
Scanning only the requested artist or work field reduced those paths to about 4.2 ms and 9.6 ms; a 50k-track unique-value stress case fell from about 43 ms per tier to about 8 ms per requested field.
That evidence initially justified requested-field scans instead of eager final materialization for every field.

A follow-up Release experiment used 50k tracks, 93,670 distinct live Quick-filter values, and 60,170 interned dictionary values.
The production one-pass unordered aggregate rebuilt in about 5.7 ms median and 6.2 ms p95.
Cached top-eight lookup took about 0.21 ms for a miss, 0.52 ms for a narrow artist prefix, 0.70 ms for a narrow work prefix, and 1.05 ms for a broad title prefix.
In the preceding strategy comparison, pre-sorting by frequency raised rebuild cost to about 18–19 ms, while a prefix-ordered index raised it to about 55–56 ms without improving the broad-prefix case enough to justify either structure.
Once that cross-tier aggregate became the common GTK and TUI Quick-filter source, letting later structured completers rescan the same library generation was redundant.
The production path therefore retains source frequencies from one scan, keeps final vectors lazy, and selects top aggregate matches without a global aggregate sort.
With all ten dictionary-backed fields, tags, and 20 custom keys included, the revised 50k-track Release baseline contains 60,190 dictionary entries and rebuilds the shared snapshot plus 93,670-value aggregate in about 7.6 ms median and 8.8 ms p95.
Subsequent in-memory materialization takes about 0.24 ms for 5,000 artists, 1.29 ms for 25,000 works, 0.005 ms for 120 tags, and below the baseline's one-microsecond resolution for 20 custom keys.
The tracked log-only performance baseline measures the shared rebuild, in-memory materialization, and cached lookup without imposing a machine-dependent timing threshold.

The [track expression architecture](../architecture/track-expression.md), [track-field value completion specification](../spec/presentation/field-completion.md), and [runtime track field catalog](../reference/library/model/track-field.md) own current behavior.
Those authorities supersede this proposal; this RFC remains the record of why the larger completion index was not adopted.

## Problem

`CompletionService` owns tag, custom-key, and metadata-value vocabularies derived from the active library.
Its change subscription marks caches dirty for inserted and mutated tracks but omits `tracksDeleted`.
Deleting the last track that contributes a tag, key, artist, genre, or other value can therefore leave a stale suggestion until another mutation or process restart.

Dirty caches rebuild lazily on the owner thread.
The first completion request after a change scans the complete hot or cold track store, counts values, sorts them, and only then returns suggestions.
The implementation historically coalesced fields by storage tier, but large-library I/O and counting still occurred on the interactive completion path.

Thread safety is documented through owner-thread confinement and guarded with the C `assert` macro.
Release builds may compile that check out, turning a future off-executor delivery change into an unguarded data race rather than a deterministic contract failure.

The change event includes affected track ids, but `markDirty()` ignores them.
The service has no retained per-track contribution state with which to subtract deleted or previous mutated values.

## Dependencies

- Hard: None.
- Conditional: None.
- Integration: [RFC 0008](0008-declarative-track-capability-bridge.md) must agree with the indexed value-completion capability set, and [RFC 0003](0003-library-mutation-pipeline.md) must preserve the ordered change input if both proposals are implemented.

## Goals

The proposal sought to:

- Remove deleted values immediately and correctly from every vocabulary.
- Keep completion reads bounded and free of full-library scans on the interactive thread.
- Maintain one immutable vocabulary snapshot correlated to a library revision.
- Update inserts, mutations, and deletions symmetrically.
- Preserve current frequency ordering and ASCII-insensitive prefix behavior.
- Keep query and metadata editors on one shared runtime vocabulary authority.
- Enforce executor/lifetime rules in release builds.
- Bound memory and worker concurrency for production-scale libraries.

Deletion correctness, one shared field authority, and one coherent lazy source snapshot were achieved by the narrower implementation.
The asynchronous snapshot, incremental contribution, release executor-contract, and production-scale budget goals were not adopted without measured need.

## Non-goals

- Changing query completion syntax or operator suggestions.
- Making prefix comparison Unicode-aware independently of query comparison semantics.
- Moving completion vocabulary into core `ao_query`.
- Persisting the derived index as authoritative library truth in the first implementation.
- Replacing library mutation publication; RFC 0003 remains the proposal for that pipeline.
- Defining query-to-TrackField mappings; [RFC 0008](0008-declarative-track-capability-bridge.md) owns that bridge.

## Rejected design

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

## Implemented alternative

`CompletionService` remains an owner-thread-confined lazy cache.
Its one `LibraryChanges` subscription invalidates only when a change set contains a library reset or inserted, mutated, or deleted tracks.
The next supported live-vocabulary reader rebuilds one shared source-frequency snapshot from a coherent library read transaction.

The runtime `TrackFieldDefinition` catalog is the sole authority for value-completion eligibility and carries an optional typed `query::Field` bridge.
For every dictionary-backed runtime field, `CompletionService` uses the core query field helper to extract the dictionary id while traversing hot and cold track data once.
This removes the former private completion-field table and dictionary-field switch without making query grammar part of the service's public API.

The change-set track ids remain intentionally unused because the service retains no per-track contribution state.
Invalidation followed by one complete lazy traversal is simple and deletion-correct.
The retained snapshot contains owned title frequencies and compressed dictionary-id frequencies separated by tag, custom-key, and track-field source.
Final tag, custom-key, and individual-field vectors resolve and sort only when requested, so the shared scan does not reintroduce eager sorting for every consumer.

For aggregate requests, the caller supplies a unique typed field set plus an optional tag contribution.
The service caches the last specification, combines only its selected retained sources, resolves live ids, and merges equal text without another track-store scan.
The resulting aggregate remains unordered; UIModel's `TrackFilterCompleter` performs ASCII-insensitive prefix matching and retains only the requested frequency-ranked top results.
This lets GTK and TUI share library-wide Quick-filter completion without moving the Quick-search field policy into runtime metadata or exposing unused historical dictionary entries.

## Alternatives

### Invalidate on deleted tracks

This is the implemented correction.
It fixes correctness on the next access while retaining one synchronous full-library scan for the new shared snapshot.
Measured shared-rebuild latency does not justify replacing it with the larger asynchronous index.

### Keep lazy scans but run them on a worker

Worker scans improve responsiveness, but without per-track contributions every mutation still invalidates the complete source snapshot and deletion correctness waits for a full rebuild.
Measured shared rebuild and aggregate lookup latency does not yet justify the additional lifetime, revision, and stale-candidate protocol.

### Derive vocabulary only from the dictionary store

The dictionary includes historical and reserved values that may no longer occur in any track.
It cannot provide current frequency or delete the last occurrence correctly.

### Persist a secondary completion database

Persistence can reduce startup cost but creates schema, recovery, and transaction-coherence obligations.
The first implementation keeps the index rebuildable and in memory; persistence can follow measured need.

### Add mutexes to the current service

Mutexes prevent races but do not fix deletion omission, full-scan latency, or mixed-revision publication.

## Compatibility and migration

No user configuration, library database, query syntax, or existing metadata/query completion ordering changes.
Deleted last-occurrence values now disappear on the next completion access instead of remaining cached until another insertion, mutation, or restart.
Interactive GTK and TUI Quick filters now add live cross-field value suggestions; applying one inserts a quoted Quick-filter term.
The runtime service remains synchronous and adds a parameterized aggregate vocabulary API without giving runtime ownership of the product field set.

## Validation

The narrower disposition is validated by:

- a regression that populates every tag, custom-key, and supported metadata vocabulary, deletes its only contributing track through `LibraryWriter`, and observes every vocabulary empty on its next access;
- exhaustive runtime tests proving every value-completable field has one dictionary-backed typed query bridge;
- one-snapshot coherence across tag, custom-key, field, and aggregate consumers;
- explicit insertion and reset invalidation, existing mutation invalidation, frequency ordering, query completion, and metadata completion tests;
- aggregate frequency merging, specification replacement, and insertion/mutation/deletion/reset invalidation tests;
- shared UIModel, TUI, and GTK Quick-filter completion contracts;
- a log-only 50k-track baseline for the shared rebuild, in-memory materialization, and cached Quick-filter lookup;
- documentation validation; and
- the repository full validation gate.

## Open questions

None for this RFC.
Any future asynchronous rebuild requires shared-snapshot latency on target hardware to violate the interaction budget and can choose the smallest design warranted by that evidence.

## Promotion plan

No proposal promotion remains.
The narrower current behavior is owned by:

- [Track expression architecture](../architecture/track-expression.md)
- [Track-field value completion](../spec/presentation/field-completion.md)
- [Query expression completion](../spec/query/expression-completion.md)
- [Runtime track field catalog](../reference/library/model/track-field.md)
