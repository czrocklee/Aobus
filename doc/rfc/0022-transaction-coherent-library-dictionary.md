---
id: rfc.0022.transaction-coherent-library-dictionary
type: rfc
status: implemented
domain: library
summary: Introduced transaction-local dictionary interning and atomic committed-index publication through the library write boundary.
depends-on: none
---
# RFC 0022: Transaction-coherent library dictionary

## Disposition

Implemented on 2026-07-15 together with the pure expression-binding foundation from [RFC 0009](0009-pure-expression-binding.md).

`MusicLibrary` now creates a move-only `WriteTransaction` that owns the native LMDB write transaction, a transaction-local `DictionaryStore::Writer`, and the process writer gate.
Dictionary interning writes the new row into that same LMDB transaction and records an owned overlay, while ordinary `DictionaryStore` readers continue to expose committed mappings only.
The wrapper prepares a no-throw in-memory publication under the dictionary's exclusive lock, holds that lock through LMDB commit, and then either advances the dictionary generation and unlocks or rolls the prepared state back before unlocking.
Library change callbacks therefore observe both the referencing records and their complete dictionary mappings.

The storage facade also exposes a distinct move-only `ReadTransaction` rather than leaking or aliasing the native LMDB type.
Stores are const handles whose readers accept library read or write capabilities and whose writers require a mutable `WriteTransaction`; raw LMDB access remains an implementation detail.
Transactions and stores carry one stable `MusicLibrary` identity, and a cross-library pairing is rejected before native DBI access.

Completing a write consumes its native handle but keeps the C++ native transaction and dictionary-writer objects alive until the outer wrapper is destroyed.
Store writers retained across the commit call therefore see a terminal transaction and destruct without closing an LMDB cursor twice.
The commit-failure seam is a data-only injected result that terminates the native transaction; it cannot re-enter library code while the writer and publication locks are held.

Dictionary ids are now dense, nonzero, and monotonically appended within committed storage.
An aborted id may be reused because it never became durable; committed ids are never reclaimed or rebound.
The implementation deliberately assumes a newly created host-local index.
It adds neither a schema/version change nor a legacy dictionary migration or integrity-validation path.

Query and format compilation is now pure and plans own their dictionary symbol text.
`PlanBinding` and `FormatBinding` resolve that text through an explicit bounded `DictionaryReadContext`, normally once per evaluation batch, so no expression path retains a global `getOrIntern()` escape hatch.

No dictionary integrity scan was added to ordinary library open.
An exhaustive administrative integrity audit remains future work under [RFC 0002](0002-durable-library-storage.md).
The [library architecture](../architecture/library.md), [library mutation specification](../spec/library/runtime/mutation.md), [database reference](../reference/library/storage/database.md), and query evaluation specifications now own current behavior and supersede the proposal wording below.

## Problem

Before this implementation, `DictionaryStore` combined two authorities with different transaction lifetimes:

- LMDB rows are durable only when the caller's write transaction commits; and
- `_idToStringStorage`, `_stringToId`, `_freeIds`, and `_reservedStrings` change immediately inside `put()` or `getOrIntern()`.

`put()` writes through the supplied LMDB transaction and then publishes the id and string into the process index before the caller commits.
If the caller returns an error, previews a mutation, or fails to commit, LMDB discards the row but the process index retains it.
The next `put()` of the same string can therefore return the cached id without recreating the missing row.
A later track record may commit a reference to an id that a reopened dictionary cannot resolve.

This is reachable through normal runtime behavior.
`LibraryWriter` serializes metadata and tags through `DictionaryStore` inside a library write transaction, and every preview path intentionally returns without committing that transaction.
The preview reply is correct, but a newly encountered metadata value, custom key, or tag can escape into the process dictionary.
Commit failure and any serialization failure after an earlier dictionary insertion have the same shape.

`getOrIntern()` makes the ownership problem broader.
Query and format compilation use it to allocate stable process-local ids for constants, tag names, and custom keys even when no durable library mutation is intended.
Those reservations share the same id space and cache as committed rows.
The current design consequently cannot answer whether a dictionary id proves durable existence, an uncommitted write, or only a compiled expression's process-local symbol.

The mutex protects container races but does not make the process index participate in the caller's LMDB transaction.
The database can remain internally valid while the live process observes a dictionary state that cannot be reconstructed after restart.

## Dependencies

- Hard: None.
- Conditional: None.
- Integration: [RFC 0003](0003-library-mutation-pipeline.md), [RFC 0009](0009-pure-expression-binding.md).

RFC 0003 should make dictionary publication part of the same mutation receipt and committed revision as the track/list/resource write that uses it.
RFC 0009 should remove query compilation's need to allocate durable dictionary ids for absent constants; if both proposals proceed, the dictionary API must not retain a second mutating expression-binding path.

## Goals

- Make the live dictionary index an exact view of committed LMDB dictionary rows.
- Stage every dictionary insertion and id allocation in the caller's write-transaction scope.
- Publish staged strings to process readers only after the matching transaction commits.
- Drop all staged state on preview, explicit abort, serialization failure, commit failure, or exception unwinding.
- Prevent a committed track, list, tag, or custom-metadata record from referencing a missing dictionary row.
- Separate durable dictionary identity from process-local query symbols.
- Preserve concurrent read performance and stable string-view lifetime where the public contract requires it.
- Give tests deterministic commit, abort, preview, and controlled commit-result fault injection.

## Non-goals

- Change user-visible metadata, tag, custom-key, or predicate semantics.
- Assign portable dictionary ids across independent libraries.
- Persist query ASTs, execution plans, or process-local symbol tables.
- Turn `DictionaryStore` into the owner of complete library transactions or revision publication.
- Introduce multi-process cache coherence beyond LMDB's existing single-writer and transaction rules.
- Reclaim committed dictionary rows as part of this proposal.

## Proposed design

### Committed index and transaction overlay

Restrict `DictionaryStore` itself to committed state.
Its public read methods resolve only rows that exist in a committed LMDB snapshot, and its process index is changed only by construction/rebuild or successful commit publication.

A library write transaction obtains a `DictionaryWriteSession` from the store:

```text
Library write transaction
  -> DictionaryWriteSession
       committed DictionaryStore lookup
       transaction-local string -> id overlay
       transaction-local id -> owned string delta
       transaction-local id-allocation state
  -> TrackBuilder serialization refers to committed or overlay ids
```

Repeated insertion of one string in the same transaction reuses its overlay id.
Lookup consults the overlay first and the committed index second.
No other transaction or process reader sees the overlay.

The session is move-only and cannot outlive its LMDB write transaction.
Dropping it without a successful commit discards all process state automatically.
Library serialization APIs that can create dictionary entries accept the session or a narrower interning port rather than the global mutable store.

### Id allocation

Id allocation remains library-local and nonzero.
The implemented writer allocates densely from committed size plus its transaction-local delta, records every allocation in its overlay, and never returns the same id for two different strings in that transaction.

LMDB serializes write transactions, so allocation does not require speculative global reservations between concurrent writers.
Any future storage adapter that permits concurrent writers must provide equivalent allocation serialization below this contract.

Aborted ids may be reused by a later transaction because they never became durable identity.
Committed ids are never reclaimed or rebound to another string.

### Commit protocol

Dictionary publication participates in the caller's transaction outcome:

```text
prepare track/list/resource rows and dictionary delta
  -> acquire the dictionary publication lock
  -> prepare a no-fail in-memory delta hidden behind that lock
  -> commit the LMDB write transaction while readers remain blocked
  -> advance dictionary generation and release the publication lock
  -> publish the library revision/change receipt
```

The selected implementation prepares every potentially throwing allocation before LMDB commit while holding the exclusive dictionary lock.
No reader can observe that prepared state.
After commit, generation advance and unlock are non-throwing; after failure, the prepared set/deque delta is rolled back before unlock.

Commit orchestration belongs at the library mutation boundary, not in `DictionaryStore::put()`.
The transaction owner is the only component that knows whether all related records are ready and whether the commit succeeded.

The library revision committed with the mutation covers both the referencing record and its dictionary rows.
There is no independent dictionary revision exposed to application consumers.

### Preview, abort, and failure

Preview runs the ordinary serialization and validation path against a transaction overlay, constructs the ordinary mutation reply, and then drops the transaction and overlay.
It cannot change `contains()`, `lookupId()`, `get()`, `size()`, gap state, or later id allocation.

The same rollback rule applies when:

- a later track/resource/list write fails;
- LMDB rejects creation or commit;
- the operation is cancelled before commit; or
- an internal exception unwinds the mutation.

Fault reporting remains owned by the caller's library operation.
The dictionary does not log or publish a separate partial outcome.

### Read and binding APIs

Make the distinction between committed lookup and write-session interning visible in the API:

```text
DictionaryStore::findId(text) -> optional committed id
DictionaryStore::get(id)      -> committed text
DictionaryWriteSession::intern(text) -> committed or staged id
```

Remove or narrow the global `getOrIntern()` API.
Query and format compilation must not receive a durable write session.
Until RFC 0009 is implemented, expression binding may use a separate ephemeral symbol table whose ids cannot be serialized into library records or accepted by durable dictionary APIs.

Dictionary-backed query constants that are absent from committed storage compile to an explicit non-match representation rather than reserving a durable id.
Tag/custom-key existence plans use committed lookup or plan-owned text according to the pure-binding design.

### Recovery and integrity audit

On library open, the committed index is constructed from one snapshot under the clean-start invariant established by the current write path.
The implementation does not add legacy-layout checks or the proposed exhaustive walk of every durable dictionary reference.

A future audit would be diagnostic and administrative; it must not invent missing strings or rebind ids.

## Alternatives

### Rebuild the cache after every preview or failure

This can repair the known paths but makes correctness depend on every caller remembering a compensating action.
It also performs unnecessary database work and leaves a window in which readers can observe uncommitted state.
A transaction overlay makes rollback the default.

### Update the cache only in each caller after commit

Hand-written caller hooks would duplicate ordering and miss future mutation paths.
A session-owned delta with one transaction-owner commit hook keeps the mechanism reusable while preserving the caller's authority.

### Remove the in-memory index

Reading every string/id lookup from LMDB would naturally follow transaction snapshots but would change hot query and presentation costs and complicate returned string lifetimes.
The proposal retains a committed read index and fixes its publication boundary.

### Persist every query symbol

That makes read-only compilation mutate the library, grows the dictionary from arbitrary input, and still couples query lifetime to storage identity.
Query symbols are not durable library facts.

### Allocate ids monotonically and never reuse gaps

Selected together with the transaction overlay and pure expression binding.
Monotonic allocation alone would not have solved publication or process-local query reservations, but dense committed ids make abort reuse straightforward once those other boundaries exist.

## Compatibility and migration

The LMDB dictionary row shape and library database version remain unchanged.
This implementation starts from a freshly created host-local index and provides no migration or compatibility-validation path for arbitrary prior dictionary contents.
Dense append-only ids are guaranteed constructively by current writers rather than established by an open-time scan.

API migration affects `TrackBuilder` serialization, library writers/importers/scanners, query/format compilers, and focused tests.
Callers that currently retain `DictionaryStore&` for mutation move to a committed read view or transaction-scoped write session.

An exhaustive referential-integrity command remains outside this implemented boundary.

Preview results and committed user behavior remain the same except that previews no longer influence later ids or lookups.
Querying an absent value remains a valid non-match rather than creating a hidden dictionary reservation.

## Validation

- A preview that introduces new metadata, custom keys, or tags leaves committed dictionary lookup, size, generation, and reopen state unchanged.
- Serialization failures and an injected terminal commit-result failure leave committed lookup contents, size, and generation equivalent to the pre-operation state.
- Store writers and the transaction-local dictionary writer remain safe to destroy after successful or failed commit while rejecting every post-terminal operation.
- A transaction from one `MusicLibrary` is rejected by every store owned by another library.
- A successful mutation commits dictionary rows and referencing track/list rows in one transaction and publishes them before the matching library change event.
- Reopening after every success fixture resolves every stored dictionary reference to the same text.
- Repeated strings within one transaction reuse one id; two different strings never share an id.
- Aborted ids can be reused without rebinding a committed id.
- Concurrent readers observe either the old committed index or the complete new committed index, never a partial delta.
- Query and format compilation of absent values performs no durable or process-global dictionary mutation.
- Existing dictionary, TrackBuilder, LibraryWriter, query, scan, import, and CLI preview tests pass with the new API.
- TSAN-focused dictionary and mutation tests pass, followed by a full `./ao check`.

## Open questions

None for the implemented boundary.
Dictionary deletion/reclamation, multi-process live-cache coherence, nested library writes, and exhaustive cross-store integrity verification require separate evidence-backed work.

## Promotion plan

The implemented current behavior is owned by:

- [Library architecture](../architecture/library.md)
- [Library access and mutation](../spec/library/runtime/mutation.md)
- [Library change publication](../spec/library/runtime/change-publication.md)
- [Library database](../reference/library/storage/database.md)
- [Predicate evaluation](../spec/query/predicate-evaluation.md)
- [Format expression evaluation](../spec/query/format-evaluation.md)
- [Track expression architecture](../architecture/track-expression.md)
