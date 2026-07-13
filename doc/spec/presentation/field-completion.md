---
id: presentation.field-completion
type: spec
status: current
domain: presentation
summary: Defines live track-field value vocabularies and metadata-editor completion behavior.
---
# Track-field value completion

## Scope

This specification defines live library vocabularies and their use while editing a single metadata field.
It also owns the shared cache behavior consumed by query-value completion.

Query cursor analysis, operators, and query-string insertion belong to [query expression completion](../query/expression-completion.md).
The exact application field capabilities belong to the [track field catalog](../../reference/library/model/track-field.md).

## Code boundary

This contract belongs primarily to the **application runtime** and frontend adaptation layers from the [system architecture](../../architecture/system-overview.md), as refined by the [library](../../architecture/library.md) and [presentation](../../architecture/presentation.md) architectures.
`CompletionService` and `MetadataValueCompleter` are public under `app/include/ao/rt/completion/`; frontends consume their value types without reading the library directly.

## Terminology

- **Vocabulary entry** is a distinct non-empty value plus its library frequency.
- **Value-completable field** is a runtime `TrackFieldDefinition` whose capability flag allows live-value suggestions.
- **Hot/cold rebuild** is one scan of the corresponding track-store tier for every dirty field in that tier.

## Invariants

- The runtime field catalog and the completion service name the same value-completable field set.
- Vocabulary values are distinct and non-empty.
- Entries sort by descending frequency and then ascending value.
- Prefix matching is ASCII-case-insensitive.
- Metadata value completion is unavailable for fields without the capability flag.
- Applying a metadata-editor suggestion replaces the entire entry, not only the prefix before the cursor.
- Query completion may reuse the vocabulary but serializes the selected value according to query syntax.
- Frontend adapters own popover and input behavior, not vocabulary state.

## State model

`CompletionService` owns separate tag, custom-key, and per-track-field caches.
All caches begin dirty.

Any committed track insertion or mutation received through `LibraryChanges` marks tag, custom-key, and every supported field cache dirty.
The accompanying track-id span is not currently used for an incremental cache update.

The next access rebuilds lazily:

- tag vocabulary scans hot track tags;
- custom-key vocabulary scans cold custom metadata keys;
- artist, album, album artist, genre, and composer share a hot-tier rebuild;
- conductor, ensemble, work, movement, and soloist share a cold-tier rebuild.

Only dirty fields in the requested tier are recomputed during that scan.

## Commands and transitions

`MetadataValueCompleter::complete(prefix, limit)` returns at most `limit` matching entries.
A zero limit or unsupported field returns an empty result.

Its frontend provider clamps the cursor to the input length and matches the text before that cursor.
When matches exist, the returned replacement range covers the complete original entry, including any text after the cursor.
When no matches exist, the provider returns no result.

Bulk vocabulary ordering is retained through item creation, and the displayed frequency is carried in item detail text.

## Failure and cancellation

Completion is synchronous and has no cancellation point.
Library reads use the active library transaction boundary; expected storage failures follow the runtime library error policy rather than becoming a second frontend storage path.

The caches contain no synchronization.
Construction records the owner thread, and every cache access, dirty notification, and lazy rebuild asserts that same thread.
`LibraryChanges` delivery must therefore remain marshalled onto the callback/owner executor before invalidation.

## Frontend observations

Metadata editors display the raw vocabulary value and insert the same value.
Frequency detail may be rendered as secondary text.

GTK's shared entry controller owns list model, popover, keyboard, pointer, and widget lifetime behavior.
The runtime provider contains no GTK types.

## Implementation map

- [`TrackField.h`](../../../app/include/ao/rt/TrackField.h) defines the public capability flag.
- [`CompletionService.h`](../../../app/include/ao/rt/completion/CompletionService.h) defines vocabulary ownership.
- [`CompletionService.cpp`](../../../app/runtime/completion/CompletionService.cpp) owns scans, ranking, caching, and thread confinement.
- [`MetadataValueCompleter.cpp`](../../../app/runtime/completion/MetadataValueCompleter.cpp) adapts one field to completion items.
- [`EntryCompletionController`](../../../app/linux-gtk/completion/EntryCompletionController.h) is the GTK entry adapter.

## Test map

- [`CompletionServiceTest.cpp`](../../../test/unit/runtime/completion/CompletionServiceTest.cpp) protects tag/custom/value caches, ranking, invalidation, and tier rebuilds.
- [`MetadataValueCompleterTest.cpp`](../../../test/unit/runtime/completion/MetadataValueCompleterTest.cpp) protects field gating, prefix matching, limits, and whole-entry replacement.
- GTK completion-controller tests protect frontend application of the neutral result.

## Related documents

- [Presentation architecture](../../architecture/presentation.md)
- [Library architecture](../../architecture/library.md)
- [Query expression completion](../query/expression-completion.md)
- [Track field catalog](../../reference/library/model/track-field.md)
