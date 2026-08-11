---
id: library.mutation
type: spec
status: current
domain: library
summary: Defines coherent runtime reads and one-operation track and list mutation transactions.
---
# Library access and mutation

## Scope

This specification defines `ao::rt::LibraryReader` snapshot reads, coordinator admission, and `ao::rt::LibraryWriter` synchronous commands.
It owns transaction scope, previews, no-op behavior, authoring bindings, validation, atomicity, and the semantics of track and list mutations.

`ao::library::MusicLibrary` is the physical storage facade: it opens transactions and exposes specialized stores.
It does not own application commands such as metadata edits, list operations, file import, scanning, or relinking.
`ao::rt::Library` is the runtime facade that groups reader, writer, task, and change roles; callers select a role instead of adding unrelated public methods to `MusicLibrary`.

Change delivery belongs to [library change publication](change-publication.md), and exact entity fields belong to [library reference](../../../reference/library/README.md).

## Code boundary

This contract belongs to the **application runtime** layer in the [system architecture](../../../architecture/system-overview.md).
Its public boundary is `app/include/ao/rt/library/LibraryReader.h`, `LibraryWriter.h`, and `LibraryAuthoring.h`; its implementation is `app/runtime/library/`.
The private `LibraryMutationService` owns the one core writable capability and exposes no LMDB transaction or transaction-bound view to normal application consumers.

## Terminology

- **Read batch** is the lifetime of one `LibraryReader` and its one `ReadTransaction`, which owns one native LMDB read snapshot.
- **Command** is one public `LibraryWriter` mutator invocation.
- **Effective change** means serialized library state differs after applying a valid command.
- **Preview** executes the command path but leaves its write transaction uncommitted and publishes nothing.
- **Target binding** is runtime-created evidence containing one runtime instance id, committed library revision, and exact ordered track-id set.
- **Order binding** is runtime-created evidence containing one runtime instance id, committed library revision, saved List id, and complete effective source order.
- **Interactive admission** accepts a command only while authoring is `Available` and no earlier commit is awaiting publication completion.
- **Dictionary overlay** is the write-transaction-local text/id delta used while serializing records; it is not visible through committed dictionary reads.
- **Root operation boundary** is `WriteTransaction::apply()` and, for live writes, either `LibraryMutationService::Mutation::apply()` or `Mutation::execute()`; it owns rollback before a failed body can return or throw outward.
- **Operation outcome** is the runtime-private `Unchanged<Value>` or `Changed<Value>` classification returned inside `Result`; `Changed` carries the exact owning `LibraryChangeSet` that the coordinator commits and publishes.
- **Write operation context** is the callback-scoped `LibraryWrite` passed by that boundary; it exposes logical mutation ports but cannot commit or abort the transaction.
- **Logical mutation ports** are `LibraryWrite::tracks()` and `LibraryWrite::lists()`; runtime code composes user-facing behavior through them and never pairs physical Store writes itself.
- **Raw order** is a saved List's persisted rank sequence, including ids currently hidden by its parent or local expression.

## Invariants

- One reader observes one coherent committed snapshot for its complete lifetime.
- Read and write capabilities are accepted only by stores from the same `MusicLibrary`; cross-library use fails before native database access.
- One `LibraryMutationService` exclusively owns live-runtime write authority; public runtime consumers cannot create a committing transaction.
- One writer command owns at most one write transaction and is independently atomic.
- A sequence of writer calls is a sequence of commits; the API exposes no caller-controlled multi-command transaction.
- Every transaction-dependent command body runs through the root operation boundary and returns `Result<T>`.
- Logical mutation ports and library-identity restore are unavailable on the transaction owner and are valid only while its `apply()` callback is active.
- Any error returned by that body aborts and terminalizes the complete root before the error is exposed; an unexpected exception does the same before being rethrown.
- Private `lmdb::detail::TransactionFailure` and `library::detail::LibraryException` carriers may unwind lower mutation helpers only as far as `WriteTransaction`, which catches them exactly, aborts, and returns the carried error.
- Library code does not expose nested transactions or item savepoints, so no batch owner continues after a failed root body.
- Every live committing body classifies its staged result before returning: `Unchanged` aborts, while `Changed` supplies one owning value and the exact zero-revision changeset to commit.
- Only `Mutation::execute()` consumes `Changed`; no caller can replace its changeset after the operation returns.
- A successful `Mutation::apply()` is noncommitting and makes that mutation ineligible for later `execute()`; preview owners explicitly abort after projecting their owning reply.
- An effective command commits one revision and publishes exactly one matching changeset through the coordinator before authoring becomes available at that revision.
- Interactive commands are rejected throughout import, scan-apply, and audio-identity maintenance.
- Metadata and tag commits require a current target binding and revalidate runtime identity, availability, revision, and every target while holding coordinator writer ownership.
- Order commits require a current order binding and revalidate runtime identity, availability, revision, List identity, selection, and anchor while holding coordinator writer ownership.
- A preview returns the same classifications and report values as its committing counterpart from the same starting state, except it returns no allocated durable id.
- Dictionary rows and every record that references them commit in the same native transaction; committed dictionary publication completes before application change delivery.
- A preview, abort, serialization failure, or commit failure leaves committed dictionary lookup, size, and generation unchanged.
- User-authored validation failure, storage failure, serialization failure, and commit failure leave library content unchanged.
- Runtime return values own their data and never retain transaction-bound `TrackView`, `ListView`, or manifest views.
- Native commit or abort makes transaction-borrowing logical writers terminal without destroying their C++ transaction owner; retained writers fail on use and remain safe to destroy while that owner remains alive.
- `LibraryWrite` itself is callback-scoped, and every logical writer must be destroyed before its borrowed `WriteTransaction` owner; using either through a dangling reference is outside the API contract.

## Live operation handoff

`Mutation::execute()` accepts exactly one callback returning `Result<OperationOutcome<Value>>`.
`Value` and `LibraryChangeSet` must own everything needed after the callback, and both must be nothrow movable across the pre-commit handoff.
The coordinator does not recursively inspect `Value` and has no reply traits.
The caller also supplies a diagnostic operation name when command-specific context is available; a native commit error retains its code and source location and prefixes its message with that operation.

An operation error or exception aborts and releases writer admission.
`Unchanged<Value>` moves out the value, explicitly aborts, returns no committed revision, and publishes nothing.
`Changed<Value>` must carry a changeset whose `libraryRevision` is zero; a pre-stamped value is an invariant failure.
The coordinator prepares the return value and the type-erased publication-completion handoff before native commit.
It then commits the transaction, stamps the exact changeset with the candidate revision, arms publication admission while still holding writer ownership, releases the writer mutex, and passes only already-prepared nothrow-movable values into the non-throwing publication boundary.
The returned `MutationExecution<Value>` carries the operation value plus an optional committed revision; command-specific code uses that revision where its public reply or next binding needs it.

`Mutation::apply()` is the noncommitting counterpart used by preview.
It accepts the broader existing `Result<T>` shape, but it cannot be followed by `execute()` on the same mutation.
Preview wrappers project command-specific public values before calling `abort()`; create-track and create-list therefore discard provisional identifiers even though their shared staging kernels allocate them inside the aborted transaction.

## Read model

`Library::reader()` creates a movable `LibraryReader` with one read transaction.
Its track, dictionary, list, resource, and tag queries use that same snapshot.
The runtime retains the library wrapper rather than a native LMDB type; transaction-bound store views cannot escape through its public result types.

Pure misses use the value channel selected by the method: `false`, an empty value, an invalid id, or `std::nullopt`.
Selection-tag intersection treats a stale selected track id as contributing no tags, so the result becomes empty.
The all-tags query returns distinct tag text and usage counts ordered by descending frequency and then ascending name.

Runtime construction receives only a `MusicLibrary` whose persisted dictionary, paired Track records, dictionary references, List rows, and manifest rows passed the open-time integrity gate.
Persisted corruption safely detected during `MusicLibrary::open()` returns `CorruptData`.
After admission, a Store or cross-Store fact that supported logical writers preserve is never
collapsed to a miss or returned as an application error; it aborts through the fatal facility.
A List or manifest row that violates the established gate instead aborts through `AO_INVARIANT`; runtime does not catch a private carrier to return partial output.

## Track commands

### Metadata and tags

`Library::bindTrackTargets` accepts a non-empty target sequence only while authoring is available, verifies every track in one read snapshot, and returns a `BoundTrackTargets` for that runtime instance and committed revision.
Binding from inside the matching `Available` notification is valid, but committing another mutation reentrantly from any publication or availability observer is rejected.

Metadata updates apply one patch to the complete bound target sequence.
Binding first validates every requested id and returns `NotFound` when a target is absent.
Submission then uses this precedence: a foreign runtime binding is `Stale`; maintenance is `Unavailable`; and a superseded revision is `Stale`.
Because the coordinator serializes mutation and publication, disappearance under an accepted exact-revision binding is an invariant violation rather than a recoverable authoring outcome.
None of these outcomes commits a subset.
Fields whose current value already equals the patch produce a semantic `NoOp`; no-op preserves the current binding and publishes nothing.
An effective update returns `Applied`, the mutation reply, the committed revision, and a next binding for the same target order at that revision.

Tag edit adds absent requested tags and removes present requested tags.
Duplicate and already-present/absent tag requests do not create an effective change.
Target binding and all-or-none outcomes are identical to metadata update, and one command updates all affected tracks atomically.

Raw-id metadata/tag previews remain non-committing administrative inspection.
They may report the mutation that would affect currently existing ids, but they create no authoring binding and cannot be turned into a commit without a fresh binding.

### Create from file

Track creation accepts an absolute path or a path relative to the configured music root.
The resolved file must be a supported regular audio file inside that root and must not already have a manifest row.

The command parses tags and technical properties, stages or reuses dictionary mappings, creates hot and cold track records and cover resources, writes an available manifest row, and commits these facts together.
Missing or out-of-root paths, unsupported or malformed media, filesystem failures, record limits, and duplicate manifest rows return a recoverable `Result` error.

The preview validates and prepares the same import but does not expose a `TrackId`, because allocation is not durable before commit.

### Delete track

Deleting an existing track removes its hot and cold records and manifest row and removes every occurrence from every saved List raw order in the same transaction.
The reply reports the deleted track and affected List ids.
A missing track returns `NotFound`.

## List commands

### Definitions and drafts

A List draft carries definition fields only: id for update, parent, name, description, and local expression.
There is no Manual, Smart, Folder, or Playlist kind.
An empty expression is the identity predicate and a non-empty expression computes membership from the parent source.
The draft never carries order ids.

Creation and update validate the name, expression, and size bounds before mutation; the logical List writer independently validates parent existence and cycles against the live write snapshot before commit.
A non-empty List expression must parse and compile under the [predicate contracts](../../query/predicate-evaluation.md) before the transaction commits.
A stale update target returns `NotFound`.

Updating definition fields copies the existing raw order unchanged.
An unchanged update is a successful no-op.

### Saved order

`Library::bindListOrder` accepts one saved List id and its complete current effective source order only while authoring is available.
The returned `BoundListOrder` is authoritative evidence for one runtime instance and committed global library revision.
Any intervening effective library commit makes it stale, including a commit unrelated to that List.

`moveListOrder` accepts selected ids and an optional `beforeTrackId`; no anchor means the complete raw-order end.
The command deduplicates the selection and preserves its relative order from the bound effective sequence, never request or widget-selection order.
Every selected id and anchor must belong to the bound sequence, and the anchor must be unselected.

The writer first simulates the effective move.
An empty selection or unchanged effective sequence is `NoOp` and does not materialize or commit an order.
For an effective move, the writer appends every currently unranked member in bound order, retains hidden ranks, applies the move to the complete raw order, verifies that its visible projection equals the requested result, then writes one List value and revision.
This is lazy full materialization: creating, viewing, filtering, changing metadata, or merely selecting Manual Order never writes order ids.

`resetListOrder` clears every visible and hidden rank.
`forgetHiddenListOrder` removes only raw-order ids absent from the bound effective membership and preserves the current visible order.
Both return `NoOp` when nothing would be forgotten.

Order commands return `Applied`, `NoOp`, `Stale`, or `Unavailable`.
Malformed selection or anchor input is a `Result` error rather than a partial mutation.

### Tag-backed membership editing

Add/Remove to Playlist is available only when the parsed local expression root is exactly one positive tag variable.
That is an operation capability derived from the AST, not a persisted List kind.
Compound, negated, invalid, or non-tag expressions have computed membership and reject direct membership commands.

Add validates the complete bound track selection and, for a nested List, requires every target to belong to the target List's parent source.
It adds the ordinary visible tag atomically, does not materialize raw order, and leaves a new member in the unranked tail.
Already-present tags are idempotent no-ops.
If evaluating that parent membership encounters an invalid stored expression anywhere in the parent chain, Add returns contextual `FormatRejected` naming the originating parent List before changing tags, revision, or publication state.

Explicit Remove removes the ordinary tag and removes each selected id from the target List raw order in the same transaction.
The reply identifies every forgotten position, and the changeset carries both track mutation and raw-order evidence.
Its user-facing result must name the visible tag and explicitly say that saved positions were forgotten; reporting only membership removal is incomplete.
Removing the same tag through the generic tag editor does not alter raw order; a later re-entry therefore restores the hidden rank.

### List deletion

Ordinary deletion rejects a List with direct dependents and reports their identities; it never reparents or silently cascades.
`previewDeleteListAndDescendants` returns the complete subtree before the separate cascade command asks the logical List writer to rediscover that live subtree and remove every row children-first and atomically.

Deletion of a directly editable single-tag List preserves ordinary track tags by default.
The preview reports the tag, tagged-track count, and other remaining List expressions that reference it.
An explicit `removeWritableTagFromTracks` option removes that tag from all affected tracks in the same transaction as the single or cascade List deletion.

Every deleted List id is published so shared presentation-preference lifecycle code can remove all corresponding frontend state.
A missing list returns `NotFound`.

## Failure and cancellation

Synchronous commands are not cooperatively cancellable.
All recoverable input and persistence failures use `Result`; malformed internal edit coordinates and impossible invariants remain programmer errors.
Native transaction begin and candidate-revision construction from that writer snapshot's durable metadata occur before a `Mutation` is exposed and have no recoverable authoring branch; failure releases writer admission and aborts through the fatal facility.

Pure validation may return before a transaction is acquired.
Once a root exists, a command-body `Result` error is exposed only after the root operation boundary explicitly aborts and releases writer ownership, even when the C++ transaction or mutation wrapper remains alive.
Storage mutation faults and recoverable library failures carried by the two private markers are translated at that same boundary after abort; runtime code never catches either marker.
The current non-nested model provides whole-root rollback only, so a multi-item owner cannot catch an item failure and continue writing later items.

No command publishes a change for a failed, previewed, or no-op transaction.
Scan cancellation and a prepared scan with no committable work are `Unchanged` outcomes, so both intentionally roll back staged work without advancing revision.
When commit fails, staged dictionary mappings are rolled back before readers resume, and allocated ids and prepared resources are not observable as successful command results.
The deterministic commit-result test seam is data-only: it terminates the native transaction and supplies an error without invoking application callbacks while writer and dictionary locks are held.

`Stale`, `Unavailable`, `NoOp`, and `Applied` are semantic metadata/tag authoring outcomes.
Input, validation, serialization, and pre-commit storage failures remain `Result` errors.
After durable commit, a revision invariant or mandatory-publication admission/delivery failure in a live runtime terminates the process; it is never reported as an ordinary pre-commit error or exposed as a public authoring outcome.
Coordinated Closing privately seals later mutation admission before callback work is retired.

## Persistence and versioning

Every effective command commits its records and one candidate library revision in the same LMDB transaction; the revision is written immediately before native commit and is not consumed on abort or failed commit.
The next interactive command is admitted only after callback-executor publication of that revision completes.
Exact records and identifier allocation belong to the [library database reference](../../../reference/library/storage/database.md).

## Implementation map

- [`Library.h`](../../../../app/include/ao/rt/library/Library.h) composes the runtime roles.
- [`LibraryReader.h`](../../../../app/include/ao/rt/library/LibraryReader.h) defines the scoped read surface.
- [`LibraryWriter.h`](../../../../app/include/ao/rt/library/LibraryWriter.h) defines commands and reply values.
- [`LibraryWriter.cpp`](../../../../app/runtime/library/LibraryWriter.cpp) owns command validation and transaction orchestration.
- [`LibraryAuthoring.h`](../../../../app/include/ao/rt/library/LibraryAuthoring.h) defines availability, target bindings, and typed outcomes.
- [`LibraryMutationService.h`](../../../../app/runtime/library/LibraryMutationService.h) owns live-runtime admission, root-body execution, terminalization, commit, and publication completion.
- [`MusicLibrary.h`](../../../../include/ao/library/MusicLibrary.h) defines the lower physical facade.
- [`ReadTransaction.h`](../../../../include/ao/library/ReadTransaction.h) defines read-snapshot ownership and the store-read capability.
- [`WriteTransaction.h`](../../../../include/ao/library/WriteTransaction.h) defines coherent native-write and dictionary-overlay ownership plus the non-nested root execution boundary.
- [`LibraryWrite.h`](../../../../include/ao/library/LibraryWrite.h) defines the callback-scoped logical mutation capability.
- [`TrackWriter.h`](../../../../include/ao/library/TrackWriter.h) and [`ListWriter.h`](../../../../include/ao/library/ListWriter.h) define the logical transaction-scoped mutation ports used by runtime commands.

## Test map

- [`LibraryReaderTest.cpp`](../../../../test/unit/runtime/library/LibraryReaderTest.cpp) proves coherent runtime values.
- [`WriteTransactionTest.cpp`](../../../../test/unit/library/WriteTransactionTest.cpp) proves root error containment, rollback, terminal state, and writer-gate reuse.
- [`TrackWriterTest.cpp`](../../../../test/unit/library/TrackWriterTest.cpp) and [`ListWriterTest.cpp`](../../../../test/unit/library/ListWriterTest.cpp) prove the logical port capability boundary and relationship-preserving mutations below the runtime facade.
- `LibraryWriter*Test.cpp` under [`test/unit/runtime/library/`](../../../../test/unit/runtime/library/) proves metadata, tags, Lists, saved ordering, track creation/deletion, dictionary-neutral previews, errors, and publication boundaries.
- [`LibraryWriterListMembershipTest.cpp`](../../../../test/unit/runtime/library/LibraryWriterListMembershipTest.cpp) additionally proves that an invalid stored parent expression returns contextual `FormatRejected` before mutation or publication.
- [`LibraryAuthoringTest.cpp`](../../../../test/unit/runtime/library/LibraryAuthoringTest.cpp) proves binding precedence, all-or-none target validation, failed-mutation admission release, no-op binding retention, and publication reentrancy closure.
- [`LibraryChangesTest.cpp`](../../../../test/unit/runtime/library/LibraryChangesTest.cpp) proves every `execute()` transition, including injected native commit failure, exact operation-owned publication, revision return, rollback, and writer-admission release.
- [`LibraryTaskServiceTest.cpp`](../../../../test/unit/runtime/library/LibraryTaskServiceTest.cpp) proves scan cancellation and no-committable-work outcomes leave storage, revision, and publication unchanged.
- [`RuntimeFatalProbeTest.cpp`](../../../../test/unit/runtime/library/RuntimeFatalProbeTest.cpp) proves that execute-after-apply and a pre-stamped operation changeset fail at the invariant boundary.

## Related documents

- [Library architecture](../../../architecture/library.md)
- [Library change publication](change-publication.md)
- [Track model](../../../reference/library/model/track.md)
- [List model](../../../reference/library/model/list.md)
- [Predicate evaluation](../../query/predicate-evaluation.md)
- [Predicate language](../../../reference/query/predicate-language.md)
