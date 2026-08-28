---
id: library.mutation
type: spec
status: current
domain: library
summary: Defines coherent runtime reads and one-operation track and list mutation transactions.
---
# Library access and mutation

## Scope

This specification defines `ao::rt::LibrarySnapshot` snapshot reads, sequencer admission, and `ao::rt::LibraryCommands` asynchronous commands.
It owns transaction scope, previews, no-op behavior, authoring bindings, validation, atomicity, and the semantics of track and list mutations.

`ao::library::MusicLibrary` is the physical storage facade: it opens transactions and exposes specialized stores.
It does not own application commands such as metadata edits, list operations, file import, scanning, or relinking.
`ao::rt::Library` is the runtime facade that groups reader, writer, task, and change roles; callers select a role instead of adding unrelated public methods to `MusicLibrary`.

Change delivery belongs to [library change publication](change-publication.md), and exact entity fields belong to [library reference](../../../reference/library/README.md).

## Code boundary

This contract belongs to the **application runtime** layer in the [system architecture](../../../architecture/system-overview.md).
Its public boundary is `app/include/ao/rt/library/LibrarySnapshot.h`, `LibraryCommands.h`, and `LibraryAuthoring.h`; its implementation is `app/runtime/library/`.
The private `LibraryWriteLane` owns the one core writable capability and exposes no LMDB transaction or transaction-bound view to normal application consumers.

## Terminology

- **Read batch** is the lifetime of one `LibrarySnapshot` and its one `ReadTransaction`, which owns one native LMDB read snapshot.
- **Command** is one owning `LibraryCommands` mutation or exact-preview Task submitted to the private live-runtime sequencer.
- **Command lane** is the per-library FIFO authority that grants one active runtime transaction or control transition at a time.
- **Effective change** means serialized library state differs after applying a valid command.
- **Preview** executes the command path but leaves its write transaction uncommitted and publishes nothing.
- **Target binding** is runtime-created evidence containing one runtime instance id, committed library revision, and exact ordered track-id set.
- **Order binding** is runtime-created evidence containing one runtime instance id, committed library revision, saved List id, and complete effective source order.
- **Authoring status** is the shared `AuthoringStatus` classification used by bound Track and saved-order commands: `Applied`, `NoOp`, `Busy`, `Stale`, or `Unavailable`.
- **Interactive admission** accepts a command only while authoring is `Available`, no Maintenance transition is pending, and the bounded interactive/background slots permit it.
- **Revision settlement** is completion of replica and changed-observer delivery plus the committed revision's applicable availability delivery; a committing command retains its lane turn through this point.
- **Dictionary overlay** is the write-transaction-local text/id delta used while serializing records; it is not visible through committed dictionary reads.
- **Root operation boundary** is `WriteTransaction::apply()` and, for live writes, either `LibraryWriteLane::Mutation::apply()` or `Mutation::executeAsync()`; it owns rollback before a failed body can return or throw outward.
- **Operation outcome** is the runtime-private `Unchanged<Value>` or `Changed<Value>` classification returned inside `Result`; `Changed` carries the exact owning `LibraryChangeSet` that the coordinator commits and publishes.
- **Write operation context** is the callback-scoped `LibraryWrite` passed by that boundary; it exposes logical mutation ports but cannot commit or abort the transaction.
- **Logical mutation ports** are `LibraryWrite::tracks()` and `LibraryWrite::lists()`; runtime code composes user-facing behavior through them and never pairs physical Store writes itself.
- **Raw order** is a saved List's persisted rank sequence, including ids currently hidden by its parent or local expression.

## Invariants

- One reader observes one coherent committed snapshot for its complete lifetime.
- Read and write capabilities are accepted only by stores from the same `MusicLibrary`; cross-library use fails before native database access.
- One `LibraryWriteLane` exclusively owns live-runtime write authority; public runtime consumers cannot create a committing transaction.
- One sequencer command is active at a time, and no later command enters its transaction phase until the prior command aborts, fails, completes a preview/no-op, or reaches revision settlement.
- One writer command owns at most one write transaction and is independently atomic.
- A sequence of writer calls is a sequence of commits; the API exposes no caller-controlled multi-command transaction.
- Every transaction-dependent command body runs through the root operation boundary and returns `Result<T>`.
- Logical mutation ports and library-identity restore are unavailable on the transaction owner and are valid only while its `apply()` callback is active.
- Any error returned by that body aborts and terminalizes the complete root before the error is exposed; an unexpected exception does the same before being rethrown.
- Private `lmdb::detail::TransactionFailure` and `library::detail::LibraryException` carriers may unwind lower mutation helpers only as far as `WriteTransaction`, which catches them exactly, aborts, and returns the carried error.
- Library code does not expose nested transactions or item savepoints, so no batch owner continues after a failed root body.
- Every live committing body classifies its staged result before returning: `Unchanged` aborts, while `Changed` supplies one owning value and the exact zero-revision changeset to commit.
- Only `Mutation::executeAsync()` consumes `Changed`; no caller can replace its changeset after the operation returns.
- A successful `Mutation::apply()` is noncommitting and makes that mutation ineligible for later `execute()`; preview owners explicitly abort after projecting their owning reply.
- An effective command commits one revision and publishes exactly one matching changeset through the coordinator before its Task completes.
- Every active transaction runs on a worker inside one ordinary non-coroutine kernel; a native write transaction never spans `co_await`.
- Values retained across submission are owned by the command. Caller spans, string views, path references, transaction views, and raw implementation pointers do not enter the queue.
- Import Maintenance rejects interactive commands for the complete workflow. Scan and audio-identity preparation leave availability open; once their background mutation is outstanding, later interactive authoring receives `Busy` and ordinary commands receive `ResourceBusy`.
- `Busy` is transient lane contention and does not invalidate a Track/List authoring binding or draft. `Unavailable` remains logical Maintenance/lifetime rejection, and `Stale` remains invalid evidence.
- Metadata, tag, and combined Properties commits require a current target binding and revalidate runtime identity, availability, revision, and every target inside the active transaction turn.
- Order commits require a current order binding and revalidate runtime identity, availability, revision, List identity, selection, and anchor inside the active transaction turn.
- A preview returns the same classifications and report values as its committing counterpart from the same starting state, except it returns no allocated durable id.
- Dictionary rows and every record that references them commit in the same native transaction; committed dictionary publication completes before application change delivery.
- Track and List preparation rejects malformed UTF-8 as `InvalidInput`, normalizes admitted library text to NFC, and applies storage limits to the normalized bytes before its first persistent effect; filesystem URIs remain outside that transformation.
- A preview, abort, serialization failure, or commit failure leaves committed dictionary lookup, size, and generation unchanged.
- User-authored validation failure, storage failure, serialization failure, and commit failure leave library content unchanged.
- Runtime return values own their data and never retain transaction-bound `TrackView`, `ListView`, or manifest views.
- Native commit or abort makes transaction-borrowing logical writers terminal without destroying their C++ transaction owner; retained writers fail on use and remain safe to destroy while that owner remains alive.
- `LibraryWrite` itself is callback-scoped, and every logical writer must be destroyed before its borrowed `WriteTransaction` owner; using either through a dangling reference is outside the API contract.

## Live operation handoff

`Mutation::executeAsync()` accepts exactly one ordinary callback returning `Result<OperationOutcome<Value>>`.
`Value` and `LibraryChangeSet` must own everything needed after the callback, and both must be nothrow movable across the pre-commit handoff.
The coordinator does not recursively inspect `Value` and has no reply traits.
The caller also supplies a diagnostic operation name when command-specific context is available; a native commit error retains its code and source location and prefixes its message with that operation.

An operation error or exception aborts and releases the command lane.
`Unchanged<Value>` moves out the value, explicitly aborts, returns no committed revision, and publishes nothing.
`Changed<Value>` must carry a changeset whose `libraryRevision` is zero; a pre-stamped value is an invariant failure.
The coordinator prepares the return value and every correctness-critical publication-settlement handoff before native commit.
It then commits the transaction, stamps the exact changeset with the candidate revision, records `SubmittingPublication`, and passes only already-prepared nothrow-movable values into the non-throwing publication boundary.
After submission returns it records `AwaitingPublication` and suspends on a one-shot event that supports both completion-before-wait and wait-before-completion ordering.
The completion is posted to the awaiter's associated worker executor; callback delivery cannot begin the next transaction inline.
The returned `MutationExecution<Value>` carries the operation value plus an optional committed revision; command-specific code uses that revision where its public reply or next binding needs it.
The command releases its lane turn only after the event reports `Published` or coordinated-Closing retirement; caller continuation is not part of lane ownership.

`Mutation::apply()` is the noncommitting counterpart used by preview.
It accepts the broader existing `Result<T>` shape, but it cannot be followed by `execute()` on the same mutation.
Preview wrappers project command-specific public values before calling `abort()`; create-track and create-list therefore discard provisional identifiers even though their shared staging kernels allocate them inside the aborted transaction.

Public semantic outcomes use `AuthoringResult<Reply>` when no refreshed binding is needed.
Track commands use `TrackAuthoringResult<Reply>` because an effective commit also returns `optNextTargets`; command-specific aliases do not duplicate either wrapper.

## Read model

`Library::snapshot()` creates a movable `LibrarySnapshot` with one read transaction.
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
Submission then uses this precedence: transient lane contention is `Busy`; a foreign runtime binding is `Stale`; Maintenance/lifetime rejection is `Unavailable`; and a superseded revision is `Stale`.
Because the coordinator serializes mutation and publication, disappearance under an accepted exact-revision binding is an invariant violation rather than a recoverable authoring outcome.
None of these outcomes commits a subset.
Fields whose current value already equals the patch produce a semantic `NoOp`; no-op preserves the current binding and publishes nothing.
An effective update returns `Applied`, the mutation reply, the committed revision, and a next binding for the same target order at that revision.

Tag edit adds absent requested tags and removes present requested tags.
Duplicate and already-present/absent tag requests do not create an effective change.
Target binding and all-or-none outcomes are identical to metadata update, and one command updates all affected tracks atomically.

The combined Properties command applies one metadata patch and one tag edit through the same target binding, root operation, and write transaction.
Either both parts commit in one revision and one changeset or an error aborts every staged effect; a tag validation or storage failure cannot leave the metadata part committed.
If both parts are semantic no-ops, the command returns `NoOp` and retains the binding.

Raw-id metadata, tag, and combined Properties previews remain non-committing administrative inspection.
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

`ListDraft` carries definition fields only: id for update, parent, name, description, and local expression.
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

Order commands return `Applied`, `NoOp`, `Busy`, `Stale`, or `Unavailable`.
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
Create, update, and delete Tasks report success only after their committed change publication settles.
Interactive navigation therefore rebuilds from those published List ids; a preview or an optimistic native-node edit is never authoritative library state.

## Failure and cancellation

Ordinary interactive commands have no operation-specific cooperative checkpoint inside their synchronous transaction kernel.
All recoverable input and persistence failures use `Result`; malformed internal edit coordinates and impossible invariants remain programmer errors.
Native transaction begin and candidate-revision construction from that writer snapshot's durable metadata occur before a `Mutation` is exposed and have no recoverable authoring branch; failure releases the lane turn and aborts through the fatal facility.

Pure validation may return before a transaction is acquired.
Once a root exists, a command-body `Result` error is exposed only after the root operation boundary explicitly aborts and releases transaction ownership, even when the C++ transaction or mutation wrapper remains alive.
Storage mutation faults and recoverable library failures carried by the two private markers are translated at that same boundary after abort; runtime code never catches either marker.
The current non-nested model provides whole-root rollback only, so a multi-item owner cannot catch an item failure and continue writing later items.

No command publishes a change for a failed, previewed, or no-op transaction.
Scan cancellation and a prepared scan with no committable work are `Unchanged` outcomes, so both intentionally roll back staged work without advancing revision.
When commit fails, staged dictionary mappings are rolled back before readers resume, and allocated ids and prepared resources are not observable as successful command results.
The deterministic commit-result test seam is data-only: it terminates the native transaction and supplies an error without invoking application callbacks while writer and dictionary locks are held.

`Busy`, `Stale`, `Unavailable`, `NoOp`, and `Applied` are semantic metadata, tag, and combined Properties authoring outcomes.
`Busy` retains the authoring session and permits an explicit retry; it triggers neither automatic replay nor an availability notification.
Input, validation, serialization, and pre-commit storage failures remain `Result` errors.
After durable commit, a revision invariant or mandatory-publication admission/delivery failure in a live runtime terminates the process; it is never reported as an ordinary pre-commit error or exposed as a public authoring outcome.
Caller cancellation cannot reinterpret a durable commit before its internal terminal is `Published` or `RetiredByClosing`.
The latter is a Closing-only internal terminal; a still-live waiter observes the existing `OperationCancelled` control-flow channel without any claim that the commit rolled back.
Coordinated Closing privately seals later mutation admission, retires queued commands, requests stop from active pre-transaction work, and waits for lane quiescence before callback work stops.

## Persistence and versioning

Every effective command commits its records and one candidate library revision in the same LMDB transaction; the revision is written immediately before native commit and is not consumed on abort or failed commit.
The next command receives the active turn only after callback-executor publication of that revision settles or coordinated Closing retires it.
Exact records and identifier allocation belong to the [library database reference](../../../reference/library/storage/database.md).

## Implementation map

- [`Library.h`](../../../../app/include/ao/rt/library/Library.h) composes the runtime roles.
- [`LibrarySnapshot.h`](../../../../app/include/ao/rt/library/LibrarySnapshot.h) defines the scoped read surface.
- [`LibraryCommands.h`](../../../../app/include/ao/rt/library/LibraryCommands.h) defines commands and reply values.
- [`LibraryCommands.cpp`](../../../../app/runtime/library/LibraryCommands.cpp) owns command validation and transaction orchestration.
- [`LibraryAuthoring.h`](../../../../app/include/ao/rt/library/LibraryAuthoring.h) defines availability, target bindings, and typed outcomes.
- [`LibraryWriteLane.h`](../../../../app/runtime/library/LibraryWriteLane.h) owns live-runtime admission, root-body execution, terminalization, commit, and publication completion.
- [`MusicLibrary.h`](../../../../include/ao/library/MusicLibrary.h) defines the lower physical facade.
- [`ReadTransaction.h`](../../../../include/ao/library/ReadTransaction.h) defines read-snapshot ownership and the store-read capability.
- [`WriteTransaction.h`](../../../../include/ao/library/WriteTransaction.h) defines coherent native-write and dictionary-overlay ownership plus the non-nested root execution boundary.
- [`LibraryWrite.h`](../../../../include/ao/library/LibraryWrite.h) defines the callback-scoped logical mutation capability.
- [`TrackWriter.h`](../../../../include/ao/library/TrackWriter.h) and [`ListWriter.h`](../../../../include/ao/library/ListWriter.h) define the logical transaction-scoped mutation ports used by runtime commands.

## Test map

- [`LibrarySnapshotTest.cpp`](../../../../test/unit/runtime/library/LibrarySnapshotTest.cpp) proves coherent runtime values.
- [`WriteTransactionTest.cpp`](../../../../test/unit/library/WriteTransactionTest.cpp) proves root error containment, rollback, terminal state, and writer-gate reuse.
- [`TrackWriterTest.cpp`](../../../../test/unit/library/TrackWriterTest.cpp) and [`ListWriterTest.cpp`](../../../../test/unit/library/ListWriterTest.cpp) prove the logical port capability boundary and relationship-preserving mutations below the runtime facade.
- `LibraryCommands*Test.cpp` under [`test/unit/runtime/library/`](../../../../test/unit/runtime/library/) proves metadata, tags, Lists, saved ordering, track creation/deletion, dictionary-neutral previews, errors, and publication boundaries.
- [`LibraryCommandsTrackPropertiesTest.cpp`](../../../../test/unit/runtime/library/LibraryCommandsTrackPropertiesTest.cpp) proves combined metadata/tag publication and whole-command rollback after a later tag failure.
- [`LibraryCommandsListMembershipTest.cpp`](../../../../test/unit/runtime/library/LibraryCommandsListMembershipTest.cpp) additionally proves that an invalid stored parent expression returns contextual `FormatRejected` before mutation or publication.
- [`LibraryAuthoringTest.cpp`](../../../../test/unit/runtime/library/LibraryAuthoringTest.cpp) proves binding precedence, all-or-none target validation, failed-mutation admission release, no-op binding retention, and publication reentrancy closure.
- [`LibraryChangesTest.cpp`](../../../../test/unit/runtime/library/LibraryChangesTest.cpp) proves `executeAsync()` transitions, injected native commit failure, exact operation-owned publication, signal-before-await settlement, revision return, rollback, lane release, and Closing retirement.
- [`LibraryJobsTest.cpp`](../../../../test/unit/runtime/library/LibraryJobsTest.cpp) proves scan cancellation and no-committable-work outcomes leave storage, revision, and publication unchanged.
- [`RuntimeFatalProbeTest.cpp`](../../../../test/unit/runtime/library/RuntimeFatalProbeTest.cpp) proves that `executeAsync()` after `apply()` and a pre-stamped operation changeset fail at the invariant boundary.

## Related documents

- [Decision 0015: sequence live-runtime library writes](../../../decision/0015-sequence-live-runtime-library-writes.md)
- [Library architecture](../../../architecture/library.md)
- [Library change publication](change-publication.md)
- [Track model](../../../reference/library/model/track.md)
- [List model](../../../reference/library/model/list.md)
- [Predicate evaluation](../../query/predicate-evaluation.md)
- [Predicate language](../../../reference/query/predicate-language.md)
