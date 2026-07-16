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
- **Interactive admission** accepts a command only while authoring is `Available` and no earlier commit is awaiting publication completion.
- **Dictionary overlay** is the write-transaction-local text/id delta used while serializing records; it is not visible through committed dictionary reads.
- **Stored order** is the complete explicit membership order of a manual list, including ids hidden by an upstream source.

## Invariants

- One reader observes one coherent committed snapshot for its complete lifetime.
- Read and write capabilities are accepted only by stores from the same `MusicLibrary`; cross-library use fails before native database access.
- One `LibraryMutationService` exclusively owns live-runtime write authority; public runtime consumers cannot create a committing transaction.
- One writer command owns at most one write transaction and is independently atomic.
- A sequence of writer calls is a sequence of commits; the API exposes no caller-controlled multi-command transaction.
- An effective command commits one revision and publishes exactly one matching changeset through the coordinator before authoring becomes available at that revision.
- Interactive commands are rejected throughout import, scan-apply, and audio-identity maintenance.
- Metadata and tag commits require a current target binding and revalidate runtime identity, availability, revision, and every target while holding coordinator writer ownership.
- A preview returns the same classifications and report values as its committing counterpart from the same starting state, except it returns no allocated durable id.
- Dictionary rows and every record that references them commit in the same native transaction; committed dictionary publication completes before application change delivery.
- A preview, abort, serialization failure, or commit failure leaves committed dictionary lookup, size, and generation unchanged.
- User-authored validation failure, storage failure, serialization failure, and commit failure leave library content unchanged.
- Runtime return values own their data and never retain transaction-bound `TrackView`, `ListView`, or manifest views.
- Native commit or abort makes store writers terminal without destroying their C++ transaction owner; retained writers fail on use and remain safe to destroy before the outer wrapper leaves scope.

## Read model

`Library::reader()` creates a movable `LibraryReader` with one read transaction.
Its track, dictionary, list, resource, and tag queries use that same snapshot.
The runtime retains the library wrapper rather than a native LMDB type; transaction-bound store views cannot escape through its public result types.

Pure misses use the value channel selected by the method: `false`, an empty value, an invalid id, or `std::nullopt`.
Selection-tag intersection treats a stale selected track id as contributing no tags, so the result becomes empty.
The all-tags query returns distinct tag text and usage counts ordered by descending frequency and then ascending name.

## Track commands

### Metadata and tags

`Library::bindTrackTargets` accepts a non-empty target sequence only while authoring is available, verifies every track in one read snapshot, and returns a `BoundTrackTargets` for that runtime instance and committed revision.
Binding from inside the matching `Available` notification is valid, but committing another mutation reentrantly from any publication or availability observer is rejected.

Metadata updates apply one patch to the complete bound target sequence.
Validation uses this precedence: a foreign runtime binding is `Stale`; maintenance or fault is `Unavailable`; a superseded revision is `Stale`; and any missing target returns `Missing` with the missing ids.
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

Deleting an existing track removes its hot and cold records and manifest row and removes every occurrence from manual lists in the same transaction.
The reply reports the deleted track and affected list memberships.
A missing track returns `NotFound`.

## List commands

### Kinds and drafts

A list draft is either smart or manual.
A smart draft stores a filter expression and computes membership; a manual draft stores explicit track ids and has no filter expression.

Creation and update validate the name, parent relationship, kind-specific fields, smart expression, membership ids, size bounds, and parent cycles before commit.
A non-empty Smart List expression must parse and compile under the [predicate contracts](../../query/predicate-evaluation.md) before the transaction commits.
A stale update target returns `NotFound`.

Full manual drafts canonicalize duplicate ids to first occurrence and reject missing track ids atomically.
An unchanged update is a successful no-op.

### Manual insert

Insertion uses a gap index in the current stored order, from zero through the current size.
Requested ids are classified in request order with this precedence: duplicate request, already present, missing track, inserted.
Only inserted ids enter the list, in their first request order.
An all-skipped request is a successful no-op.

### Manual remove

Removal selects each requested id at most once.
The reply distinguishes duplicate requests and ids not present in stored order.
Existing ids are removed atomically, and their reported order follows stored order rather than request order.

### Manual move

Move selects existing requested ids once, preserves their stored relative order, removes them, and inserts them at a gap measured after removal.
The reply distinguishes duplicate requests and absent ids.
Empty selection and a resulting identical order are successful no-ops.

### List deletion

Deleting an existing list removes its stored definition.
Source invalidation and dependent-list behavior follow the source specification after the committed deletion is published.
A missing list returns `NotFound`.

## Failure and cancellation

Synchronous commands are not cooperatively cancellable.
All recoverable input and persistence failures use `Result`; malformed internal edit coordinates and impossible invariants remain programmer errors.

No command publishes a change for a failed, previewed, or no-op transaction.
When commit fails, staged dictionary mappings are rolled back before readers resume, and allocated ids and prepared resources are not observable as successful command results.
The deterministic commit-result test seam is data-only: it terminates the native transaction and supplies an error without invoking application callbacks while writer and dictionary locks are held.

`Stale`, `Missing`, `Unavailable`, `NoOp`, and `Applied` are semantic metadata/tag authoring outcomes.
Input, validation, serialization, and pre-commit storage failures remain `Result` errors.
After durable commit, publication enqueue or observer failure faults the coordinator and propagates as a committed-publication failure; it is never reported as an ordinary pre-commit error, and the runtime rejects every later mutation.

## Persistence and versioning

Every effective command commits its records and one bumped library revision in the same LMDB transaction.
The next interactive command is admitted only after callback-executor publication of that revision completes.
Exact records and identifier allocation belong to the [library database reference](../../../reference/library/storage/database.md).

## Implementation map

- [`Library.h`](../../../../app/include/ao/rt/library/Library.h) composes the runtime roles.
- [`LibraryReader.h`](../../../../app/include/ao/rt/library/LibraryReader.h) defines the scoped read surface.
- [`LibraryWriter.h`](../../../../app/include/ao/rt/library/LibraryWriter.h) defines commands and reply values.
- [`LibraryWriter.cpp`](../../../../app/runtime/library/LibraryWriter.cpp) owns command validation and transaction orchestration.
- [`LibraryAuthoring.h`](../../../../app/include/ao/rt/library/LibraryAuthoring.h) defines availability, target bindings, and typed outcomes.
- [`LibraryMutationService.h`](../../../../app/runtime/library/LibraryMutationService.h) owns live-runtime admission, commit, and publication completion.
- [`MusicLibrary.h`](../../../../include/ao/library/MusicLibrary.h) defines the lower physical facade.
- [`ReadTransaction.h`](../../../../include/ao/library/ReadTransaction.h) defines read-snapshot ownership and the store-read capability.
- [`WriteTransaction.h`](../../../../include/ao/library/WriteTransaction.h) defines coherent native-write and dictionary-overlay ownership.

## Test map

- [`LibraryReaderTest.cpp`](../../../../test/unit/runtime/library/LibraryReaderTest.cpp) proves coherent runtime values.
- `LibraryWriter*Test.cpp` under [`test/unit/runtime/library/`](../../../../test/unit/runtime/library/) proves metadata, tags, lists, manual ordering, track creation/deletion, dictionary-neutral previews, errors, and publication boundaries.
- [`LibraryAuthoringTest.cpp`](../../../../test/unit/runtime/library/LibraryAuthoringTest.cpp) proves binding precedence, all-or-none target validation, no-op binding retention, and post-commit fault closure.

## Related documents

- [Library architecture](../../../architecture/library.md)
- [Library change publication](change-publication.md)
- [Track model](../../../reference/library/model/track.md)
- [List model](../../../reference/library/model/list.md)
- [Predicate evaluation](../../query/predicate-evaluation.md)
- [Predicate language](../../../reference/query/predicate-language.md)
