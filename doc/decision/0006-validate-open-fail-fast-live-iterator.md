---
id: decision.0006.validate-open-fail-fast-live-iterator
type: decision
status: accepted
domain: library
summary: Validates every structured iterable store before exposure and treats a later iterator integrity breach as an infrastructure fault.
---
# Decision 0006: validate before exposure and fail fast on live iterator faults

## Context

The August 2026 failure-channel follow-up for RFC 0001 found that raw LMDB iterators already have a value-or-throw contract: end of data is normal, while cursor construction, advance, key coercion, inactive-transaction use, and physical storage faults raise a general exception or contract failure.
The library's List and manifest adapters nevertheless used `library::detail::LibraryException` for malformed iterator rows, and runtime and CLI code caught that private type to manufacture operation-level `CorruptData` results.

That split made a private implementation type part of the application contract and implied a recoverable continuation that no iterator API provides.
It was also inconsistent because List records were not covered by the open-time integrity sweep, even though List writers already reject structurally invalid payloads before mutation.

## Decision

`MusicLibrary::open()` validates metadata, dictionary rows, paired Track records and references, List keys and records, and manifest keys and records before exposing the store graph.
A safely detected malformed record in that initial snapshot rejects the complete open with `CorruptData`.

After a successful open, List and manifest iteration relies on that validated-store invariant.
If an iterator later observes a structurally invalid row, it raises the general `ao::Exception` infrastructure channel and unwinds to the established application leaf; it does not yield an error variant, skip the row, return a partial result, or expose `library::detail::LibraryException` to runtime or CLI code.
Such a post-open breach requires external database mutation, physical corruption, or an Aobus invariant bug because all supported writers validate before mutation.

List point reads use the same general exception because their optional return shape represents absence only.
Result-returning manifest point reads retain their declared validation and `NotFound` channel, and the existing partial `TrackView` load contract is unchanged.
Private library error carriers remain permitted only across short implementation scopes and are translated by a library-owned boundary such as `MusicLibrary::open()` or `WriteTransaction::apply()`.

## Alternatives considered

### Return an error-bearing iterator item

Rejected because every consumer would need partial-result, resume, ordering, and duplicate-report policy even though the application cannot safely continue against a store whose validated invariant changed underneath it.

### Catch iterator exceptions in each runtime operation

Rejected because it leaks a private library type across the layer boundary and gives scan, backfill, export, and future consumers inconsistent recovery policy.

### Keep lazy List validation

Rejected because a malformed persisted List would then be ordinary startup input until the first unrelated List traversal, making the same physical state recoverable or fatal depending on access order.

### Terminate directly inside the iterator

Rejected because throwing the project's general invariant exception preserves RAII unwind and lets the established synchronous, asynchronous, or frontend leaf own diagnostics and termination.

## Consequences

- Opening an existing library adds one linear List validation pass.
- Every structured iterable store now establishes its row invariant before runtime exposure.
- Manifest and List iterator consumers cannot receive partial output after a bad row.
- Runtime and CLI production code no longer includes or catches the private library error carrier.
- External mutation or corruption after open is intentionally not an in-process repair or degraded-operation path.
- The raw LMDB cursor contract remains unchanged: only end is normal, and every other cursor fault throws.

## Current authorities

- [Failure and reporting architecture](../architecture/failure-and-reporting.md)
- [Library architecture](../architecture/library.md)
- [Outcome channel specification](../spec/failure/outcome-channel.md)
- [Library scan and audio identity](../spec/library/runtime/scan-and-identity.md)
- [Library database](../reference/library/storage/database.md)
- [LMDB operation specification](../spec/storage/lmdb-operation.md)

## Supersession

Not superseded.
