---
id: presentation.field-completion
type: spec
status: current
domain: presentation
summary: Defines live single-field and aggregate value vocabularies and their completion consumers.
---
# Track-field value completion

## Scope

This specification defines live library vocabularies, their use while editing a single metadata field, and the aggregate vocabulary used by Quick-filter completion.
It also owns the shared cache behavior consumed by query-value completion.

Query cursor analysis, operators, and query-string insertion belong to [query expression completion](../query/expression-completion.md).
Quick-filter field selection, ranking, replacement, and insertion belong to [track filtering](track-filter.md).
The exact application field capabilities belong to the [track field catalog](../../reference/library/model/track-field.md).

## Code boundary

This contract belongs primarily to the **application runtime** and frontend adaptation layers from the [system architecture](../../architecture/system-overview.md), as refined by the [library](../../architecture/library.md) and [presentation](../../architecture/presentation.md) architectures.
`CompletionService` and `MetadataValueCompleter` are public under `app/include/ao/rt/completion/`; frontends consume their value types without reading the library directly.

## Terminology

- **Vocabulary entry** is a distinct non-empty value plus its library frequency.
- **Value-completable field** is a runtime `TrackFieldDefinition` whose capability flag and dictionary-backed typed query bridge allow live-value suggestions.
- **Vocabulary snapshot** is the source-preserving title, tag, custom-key, and dictionary-field frequency state captured by one track-store traversal.
- **Aggregate specification** is a caller-provided unique set of title or dictionary-backed `TrackField` values plus optional tags.
- **Materialization** converts selected snapshot frequencies into text entries and applies the ordering required by that consumer without reading track storage.

## Invariants

- The runtime field catalog is the only authority for the value-completable field set.
- Every field carrying the value-completion flag has a resolvable typed query-field bridge to a dictionary-backed field; the completion service enforces that contract and derives value extraction from the bridge.
- After one invalidation, every live vocabulary is derived from one shared snapshot until the next qualifying library change.
- Vocabulary values are distinct and non-empty.
- Tag, custom-key, and individual-field entries sort by descending frequency and then ascending value.
- Aggregate entries are intentionally unordered; their consumer selects and ranks only the matching top results.
- Prefix matching is ASCII-case-insensitive.
- Metadata value completion is unavailable for fields without the capability flag.
- Applying a metadata-editor suggestion replaces the entire entry, not only the prefix before the cursor.
- Query completion may reuse the vocabulary but serializes the selected value according to query syntax.
- Aggregate values include only live track contributions, merge identical text across requested fields and tags, and never expose unused historical dictionary entries.
- Frontend adapters own popover and input behavior, not vocabulary state.

## State model

`CompletionService` owns one source-preserving frequency snapshot plus separately materialized tag, custom-key, per-track-field, and aggregate results.
The snapshot begins dirty and materialized results begin unavailable.

Any committed track insertion, mutation, deletion, or library reset received through `LibraryChanges` marks the shared snapshot dirty.
List-only changes leave the caches unchanged.

The next non-empty supported live-vocabulary access rebuilds lazily by traversing `TrackStore` once with both hot and cold data available.
That traversal counts:

- inline titles by owned text;
- tags and custom metadata keys by dictionary id; and
- every dictionary-backed runtime track field by its typed query-field extractor.

The service compresses dictionary-id counts by source and discards the traversal working storage.
No tag, custom-key, field, or aggregate access scans track storage again until another qualifying library change invalidates the snapshot.
Individual result vectors remain lazy: tags, custom keys, and requested fields resolve their retained ids and sort in memory only when consumed.

The aggregate cache retains only the most recently requested specification and copies its field identities rather than borrowing the caller's span.
Changing that specification replaces only the materialized aggregate, not the shared frequency snapshot.
Aggregate materialization combines the retained sources selected by the specification, resolves only ids with live contributions, and merges equal title/dictionary text.
Materialization does not sort the complete aggregate because Quick-filter completion scans it once and retains only the requested top matches.
An empty aggregate specification returns an empty result without forcing a snapshot rebuild.

## Commands and transitions

`MetadataValueCompleter::complete(prefix, limit)` returns at most `limit` matching entries.
A zero limit or unsupported field returns an empty result.

Its frontend provider clamps the cursor to the input length and matches the text before that cursor.
When matches exist, the returned replacement range covers the complete original entry, including any text after the cursor.
When no matches exist, the provider returns no result.

Bulk vocabulary ordering is retained through item creation, and the displayed frequency is carried in item detail text.

`CompletionService::aggregateValues(spec)` returns the cached live aggregate for a validated specification.
The service does not decide which fields form a product search surface or how aggregate values are matched, ranked, or inserted.

## Failure and cancellation

Completion is synchronous and has no cancellation point.
The shared rebuild uses one active library read transaction; expected storage failures follow the runtime library error policy rather than becoming a second frontend storage path.

The caches contain no synchronization.
Construction records the owner thread, and every cache access, dirty notification, and lazy rebuild asserts that same thread.
`LibraryChanges` delivery must therefore remain marshalled onto the callback/owner executor before invalidation.

## Frontend observations

Metadata editors display the raw vocabulary value and insert the same value.
Frequency detail may be rendered as secondary text.

Interactive Quick filters consume aggregate values through the UIModel `TrackFilterCompleter`; frontends do not request storage fields independently.

GTK's shared entry controller owns list model, popover, keyboard, pointer, and widget lifetime behavior.
The runtime provider contains no GTK types.

## Implementation map

- [`FieldCatalog.h`](../../../include/ao/query/FieldCatalog.h) defines typed query-variable descriptors.
- [`TrackField.h`](../../../app/include/ao/rt/TrackField.h) defines the public capability flag and typed query bridge.
- [`CompletionService.h`](../../../app/include/ao/rt/completion/CompletionService.h) defines vocabulary ownership.
- [`CompletionService.cpp`](../../../app/runtime/completion/CompletionService.cpp) owns the shared scan, source frequencies, materialization, caching, and thread confinement.
- [`MetadataValueCompleter.cpp`](../../../app/runtime/completion/MetadataValueCompleter.cpp) adapts one field to completion items.
- [`TrackFilterCompleter`](../../../app/include/ao/uimodel/library/track/TrackFilterCompleter.h) adapts an aggregate vocabulary according to Quick-filter policy.
- [`EntryCompletionController`](../../../app/linux-gtk/completion/EntryCompletionController.h) is the GTK entry adapter.

## Test map

- [`CompletionServiceTest.cpp`](../../../test/unit/runtime/completion/CompletionServiceTest.cpp) protects shared-snapshot coherence, tag/custom/field/aggregate materialization, frequency merging, specification replacement, and insertion/mutation/deletion/reset invalidation.
- [`MetadataValueCompleterTest.cpp`](../../../test/unit/runtime/completion/MetadataValueCompleterTest.cpp) protects field gating, prefix matching, limits, and whole-entry replacement.
- [`CompletionVocabularyBaselineTest.cpp`](../../../test/perf/CompletionVocabularyBaselineTest.cpp) records shared rebuild, in-memory materialization, and cached Quick-filter lookup latency at representative cardinalities without a machine-dependent pass threshold.
- GTK completion-controller tests protect frontend application of the neutral result.

## Related documents

- [Presentation architecture](../../architecture/presentation.md)
- [Library architecture](../../architecture/library.md)
- [Query expression completion](../query/expression-completion.md)
- [Track field catalog](../../reference/library/model/track-field.md)
