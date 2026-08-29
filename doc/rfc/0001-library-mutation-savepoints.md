---
id: rfc.0001.library-mutation-savepoints
type: rfc
status: draft
domain: library
summary: Proposes nested LMDB write savepoints and one transaction-chain dictionary journal on top of the current root Result boundary.
depends-on: none
---
# RFC 0001: Library mutation write savepoints

## Problem

The current [library architecture](../architecture/library.md), [outcome channel specification](../spec/failure/outcome-channel.md), and [runtime mutation specification](../spec/library/runtime/mutation.md) already define a non-nested root execution boundary.
`WriteTransaction::apply()` accepts a `Result<T>`-returning body, aborts the complete root on an error or private native transaction marker, and aborts before rethrowing an unrelated exception.
For live commits, `LibraryWriteLane::Mutation::executeAsync()` accepts one operation-owned `Changed` or `Unchanged` outcome, commits and publishes only `Changed`, and releases the sequencer turn on every terminal path.
`Mutation::apply()` is the noncommitting preview boundary, and offline scan/import owners use the same core `WriteTransaction::apply()` containment.
No runtime writer, task, scan, or importer catches `lmdb::detail::TransactionFailure`.
Root construction is deliberately outside that recoverable operation channel: native begin and candidate-revision construction occur before the wrapper is exposed, and failure unwinds writer ownership before aborting through the fatal facility.

That baseline makes whole-root commands safe and gives their callers one recoverable channel:

```text
root body returns Result error or a private mutation marker escapes a helper
  -> WriteTransaction aborts and terminalizes the root
  -> Mutation releases coordinator admission when present
  -> caller receives Result error

live root body returns Unchanged
  -> Mutation aborts the root and releases coordinator admission
  -> caller receives its owned value without a committed revision

live root body returns Changed
  -> Mutation commits and publishes its exact operation-owned change set
  -> caller receives its owned value and committed revision
```

It deliberately cannot preserve earlier root work after one already-mutated item fails.
A batch that is specified to skip an item and continue must therefore complete every recoverable item check before staging, or abort the entire batch.
Several lower helpers still use short-range transaction markers internally, but their current library-owned boundaries contain them; root containment does not by itself normalize every helper signature or enable item-local recovery.

The core LMDB adapter intentionally exposes only top-level write transactions today.
This proposal would introduce native child handles as a source-private mechanism owned exclusively by a callback-scoped library savepoint boundary, rather than restoring a freely managed low-level child transaction API.
Its root write wrapper owns one dictionary overlay, one native writer, one process writer gate, and one publication sequence, while the current specifications explicitly prohibit nested library transactions as item savepoints.

Naively adding a child `library::WriteTransaction` would not solve the problem.
It would attempt to reacquire the non-recursive writer gate, allocate dictionary IDs from committed state instead of parent-staged state, make a child blind to the parent's dictionary overlay, risk publishing a child delta before the root commit, and leave parent-bound cursor wrappers usable while the native parent is suspended.

The remaining proposal asks whether nested savepoints can preserve a usable parent after an item-local failure under one writer authority, without replacing the current dense dictionary, stable borrowed views, fast committed lookup, or atomic publication model.

## Dependencies

- Hard: None.
- Conditional: None.
- Integration: None.

Any implementation must preserve the synchronous root/preview kernel and sole live-runtime transaction owner established by [Decision 0015](../decision/0015-sequence-live-runtime-library-writes.md).

The implemented application runtime names the source-private sequencer
`LibraryWriteLane` and the public command surface `LibraryCommands`.
This proposal uses those current names throughout its design.

## Goals

- Preserve the current root `Result<T>` containment and whole-root rollback contract.
- Preserve exception or contract failure for broken invariants, allocation failure, and unexpected storage or program faults.
- Keep one process-serialized root writer authority and exactly one usable active write-transaction leaf at any time.
- Add stack-disciplined native child transactions as savepoints rather than as concurrent writers.
- Let an explicitly recoverable sub-operation return an error after staging without leaving a committable partial sub-operation in its parent.
- Keep root commit as the only durability, dictionary publication, revision publication, and application change-publication boundary.
- Preserve dense append-only committed `DictionaryId` values, reuse of aborted tail IDs, store-lifetime dictionary text views, fast committed text lookup, batch binding, and all-or-none publication to concurrent readers.
- Keep `lmdb::detail::TransactionFailure` and any short-range library failure carrier inside transaction or operation owners.
- Make stale parent or child writers fail before native cursor access.
- Retain pure preflight where it improves failure reporting or avoids unnecessary savepoint work, while removing its role as the only protection against partial commit.
- Give batch owners an explicit policy choice between continuing after an item-local child rollback and aborting the complete root operation.

## Non-goals

- Eliminate every C++ exception from the library implementation.
- Convert `std::bad_alloc`, contract violations, impossible postconditions, arbitrary cursor corruption, or other invariant faults into recoverable `Error` values.
- Introduce concurrent write transactions, parallel child transactions, or another application writer authority.
- Change the public runtime command outcomes, notification policy, or frontend presentation contract.
- Make every scan, import, or batch failure skippable merely because a savepoint can roll it back safely.
- Turn a child commit into an independently durable commit.
- Add database salvage, live repair, or partial-open behavior for a corrupt library.
- Replace `DictionaryId` with content hashes, allow committed ID gaps, or remove the committed in-memory dictionary index.
- Redesign the intentional hot-only and cold-only `TrackView` loading model.
- Change the host-local database format as part of the selected design.

## Proposed design

### Outcome contract

The target contract separates observable outcomes from private unwinding mechanisms.

| Situation | Proposed channel | Visibility |
|---|---|---|
| Ordinary miss, no-op, or predicate | Value, `std::optional`, or typed status | Public |
| Recoverable command, parse, media, validation, storage, or commit failure | `Result<T>` | Public |
| Recoverable failure inside a savepoint | `Result<T>` returned to the savepoint owner | Internal operation API |
| Native mutation fault that still uses an exception internally | Exact private catch by the active transaction owner, followed by rollback and `Result` translation | LMDB/library implementation only |
| Root construction or revision-initialization failure | AO fatal handling after writer-ownership unwind | Library infrastructure boundary |
| Broken invariant or unexpected fault | AO contract or unchanged foreign exception transport | Reaches the established owning fatal boundary |
| Failure after durable root commit in mandatory revision or publication infrastructure | Terminal infrastructure handling | Runtime implementation only |

A function declared to return `Result<T>` must not let an exception carrying an ordinary recoverable `Error` cross its declared subsystem boundary.
It may still throw an invariant or unexpected exception.

`library::detail::LibraryException` must not remain a cross-module application contract.
Its current uses are reclassified by origin:

- Pure validation and external-data rejection return `Result`.
- A recoverable mutation rejection returns `Result` through a root or child execution boundary that has already rolled back the affected transaction.
- A canonical postcondition that is impossible after successful preflight and prepared-value construction is an invariant fault.
- Safely detected persisted corruption before library exposure retains the open-level `CorruptData` policy.
  After that gate establishes a Store invariant, a later row-integrity breach aborts through `AO_INVARIANT` rather than becoming a recoverable private carrier; runtime and CLI code name neither private type.

`lmdb::detail::TransactionFailure` currently remains inside the independent LMDB and library adapters because a native mutation can fail through an interface that cannot safely return into its active transaction.
The implemented root execution owner catches it, aborts the root, and returns the carried `Error`; no runtime writer, task, scan, or importer catches that type.
The proposed savepoint owner extends the same containment to a child, subject to the conservative root-terminal policy below.

### One writer authority and one active leaf

A nested write transaction is a savepoint in one writer chain, not another writer authority.

```text
root active
  -> child active; root suspended
  -> child commit or abort
  -> root active
  -> sibling child active; root suspended
  -> sibling child commit or abort
  -> root active
  -> root commit or abort
```

The following constraints apply:

- One `MusicLibrary` write session owns the process writer gate and writer-lease anchor.
- LMDB still admits only one top-level write transaction for the environment.
- The root opens and retains every required DBI before creating a child; a child reuses those tokens and never calls `mdb_dbi_open`.
- A parent native handle remains alive while its child exists, but parent operations are forbidden until that child ends.
- Only the deepest child is usable when nesting depth is greater than one.
- Sibling savepoints execute sequentially.
- Root commit is rejected while any child is active.
- A child commit merges into its parent and publishes nothing.
- A child abort discards only its child-staged native changes and transaction-chain journal tail.
- Root abort discards the complete chain.

The implementation uses “writer” consistently for two different concepts.
The root chain owns one logical writer authority, while store-specific typed LMDB database writers are cursor wrappers bound to the current active leaf.
Several Track, List, Manifest, Resource, Metadata, and Dictionary cursor wrappers may exist inside one leaf, but they confer no additional writer authority and cannot cross a savepoint transition as usable handles.

### Implemented root-operation prerequisite

Single-command mutation does not require a native child merely to hide an exception.
The current authorities cited in [Problem](#problem) own the implemented behavior; this RFC relies on the following live shape only as a prerequisite:

```cpp
auto execution = mutation.execute(
  [&](library::LibraryWrite& write) -> Result<OperationOutcome<Value>>
  {
    return Changed<Value>{.value = makeValue(write), .changeSet = makeChangeSet()};
  });
```

An operation error terminalizes the root, `Unchanged` explicitly aborts it, and `Changed` commits the exact supplied change set through the coordinator.
The callback cannot commit, and a successful `Mutation::apply()` used for preview makes that mutation ineligible for later `execute()`, so the current API does not imply a caller-controlled apply/commit gap.
This RFC adds children only inside a root operation where a parent must remain usable after a recoverable sub-operation failure.

### Savepoint execution API

Native child handles are not exposed as freely managed public transactions.
The only supported child entry point is stack-scoped callback execution through the current write context.

```cpp
auto itemResult = root.withSavepoint(
  [&](library::LibraryWrite& child) -> Result<ItemChange>
  {
    auto trackWriter = child.tracks();
    auto manifestWriter = child.manifest();
    auto dictionaryWriter = child.dictionary();
    return applyItem(trackWriter, manifestWriter, dictionaryWriter);
  });
```

The owner, not the callback, begins, commits, or aborts the native child.
The callback cannot publish dictionary state, commit the root, publish a revision, or submit an application change set.

The savepoint follows this transition table:

| Event | Native child | Dictionary journal | Parent/root | Returned channel |
|---|---|---|---|---|
| Child begin fails | Not created or immediately terminal | Unchanged | Remains active when native guarantees permit | `Result` error |
| Body succeeds and child commit succeeds | Merged into parent | Child tail retained | Resumes active | Value |
| Body returns `Result` error | Aborted | Rolled back to checkpoint | Resumes active | Same `Error` |
| Existing internal transaction marker escapes body | Aborted | Rolled back to checkpoint | Conservatively terminalized unless the lower adapter proves the fault child-local | `Result` error |
| Child commit fails | Terminal | Root journal is unpublished | Root is aborted and terminal | `Result` error |
| Unexpected exception escapes body | Aborted | Child tail rolled back | Root is aborted and terminal | Original exception rethrown |

The proposal deliberately treats failed child commit as a root-terminal event regardless of whether one LMDB version appears to leave the parent usable.
This is a conservative Aobus policy rather than a claim that every native implementation necessarily invalidates the parent, and the promoted decision record must preserve that rationale.
No application behavior depends on continuing a parent after a failed native child merge.

A savepoint body must return owned data.
It cannot return `TrackView`, `ListView`, manifest views, writable spans, cursor wrappers, or any object that borrows the child transaction.

### Batch continuation policy

A savepoint makes continuation safe; it does not decide whether continuation is correct.
The operation owner retains semantic recovery authority.

An operation may continue with a later sibling only when its specification classifies the failure as item-local and the child rollback completed successfully.
Examples may include a bounded user value, a malformed individual media item, or a stale item-specific assumption when the batch contract explicitly permits skipping that item.

An operation aborts the root for infrastructure storage faults, failed child commit, broken revision evidence, cancellation rules requiring whole-operation termination, or any error whose scope cannot be proven item-local.
Machine behavior branches on an `Error::Code` or a more specific typed outcome, never on message text.

The initial implementation does not broaden scan or import continuation behavior automatically.
Each changed continuation policy requires an update to its owning specification and focused tests.

### Dictionary ownership split

The selected design preserves the current persisted `dictionary` database and committed read model.
It splits the current `DictionaryStore::Writer` responsibilities into three objects.

#### Committed `DictionaryStore`

`DictionaryStore` continues to own only committed reader-visible state:

- Dense `id -> text` storage in stable-address memory.
- Fast committed `text -> id` lookup.
- The shared mutex protecting all-or-none committed publication.
- The committed generation.
- Batch binding and bounded read-cache support.

Committed reads never inspect a write-session journal.
They observe either the complete old generation or the complete generation published by a successful root commit.

#### Root-owned `DictionaryWriteSession`

One root write chain owns one transaction-local journal:

```cpp
class DictionaryWriteSession final
{
  DictionaryStore* committed;
  std::uint32_t committedBaseSize;
  std::map<std::string, DictionaryId, std::less<>> overlay;
  std::vector<Delta> journal;
};
```

`committedBaseSize` is captured while the root writer gate prevents another publication.
`overlay` resolves every uncommitted text added by the root or any successfully merged active-descendant chain.
`journal` records the dense append-only tail that the root may eventually publish.

The next ID is derived from `committedBaseSize + journal.size() + 1` rather than from a child-local snapshot of committed size.
There is no independently mergeable `nextId` per child.

#### Active-leaf `DictionaryWriter`

A lightweight Dictionary writer binds the shared `DictionaryWriteSession` to the native transaction of the current active leaf.
It owns or borrows only the cursor needed for that leaf.
It does not own publication state, another overlay, or another ID allocator.

Interning follows this order:

1. Look up the text in committed `DictionaryStore` state.
2. Look up the text in the chain-wide `overlay`.
3. Reject exhaustion before assigning a new nonzero ID.
4. Prepare the journal and overlay insertion so an allocation failure cannot leave one without the other.
5. Create the dense row in the current active native transaction.
6. If native creation returns a recoverable error, undo the local journal and overlay insertion before returning it.
7. Return the committed or newly staged ID.

The root writer gate means the journal is owner-thread state and needs no independent mutex.
Committed readers continue to synchronize only with root publication.

### Dictionary checkpoints

Each child captures one scalar checkpoint before it can stage dictionary state:

```cpp
using DictionaryCheckpoint = std::size_t;

auto const checkpoint = session.journal.size();
```

If a child begins with this journal:

```text
[A, B]
```

and stages two new mappings, the shared state becomes:

```text
[A, B, C, D]
```

A successful native child commit keeps `C` and `D` in both the native parent and the chain journal.
No C++ overlay merge or allocation occurs after native child commit.

A child abort first aborts the native child and then rolls the C++ state back to its checkpoint:

1. Erase each tail entry's text from `overlay` in reverse order.
2. Truncate `journal` to the checkpoint.
3. Derive the next dense ID from the shortened journal.

The next sibling can reuse the IDs formerly assigned to `C` and `D`.
No row is deleted from the native parent because LMDB child abort already discarded the child rows.

Nested grandchildren use the same LIFO rule.
A grandchild commit retains its tail in its parent child; a later abort of that parent truncates the complete parent-and-grandchild tail and native child abort discards the corresponding merged native state.

### Root dictionary publication

Only root commit prepares and publishes the dictionary journal.
The existing two-phase publication shape remains:

The dense native dictionary rows already exist in the root transaction because root interning wrote them there and successful child commits merged their rows there.
Root publication prepares only the corresponding committed in-memory state before the native root commit.

```text
acquire committed dictionary exclusive lock
  -> verify journal.front().id == committedBaseSize + 1 when the journal is nonempty
  -> reserve every potentially allocating committed-index insertion
  -> append the complete journal text tail to in-memory stable-address storage
  -> prepare the committed in-memory reverse index
  -> commit the already-staged native root transaction
       success -> advance generation and unlock
       failure -> roll back prepared in-memory publication and unlock
```

A first-ID mismatch is an invariant fault because the writer gate, dense allocator, and LIFO rollback should make it impossible.
No child acquires the committed dictionary publication lock or changes the committed generation.
A root preview, root abort, failed child merge, failed root commit, or unexpected exception leaves committed dictionary lookup, size, borrowed strings, and generation unchanged.

### Transaction-bound handles and cursors

The native parent remains allocated while a child is active, but every parent-bound operation is suspended.
The library wrapper adds an active-leaf epoch or equivalent identity check that is independent of native `isActive()`.

Each store writer records the leaf identity or epoch under which it was created.
Entering or leaving a child advances that identity.
A writer created before the transition fails before any native call and must be reacquired from the resumed `LibraryWrite`.
The same rule applies to transaction-bound readers and views: they are not dereferenced across a savepoint transition.

The callback API limits the normal lifetime of child writers to the callback body.
Debug contracts and unit tests additionally reject deliberately retained wrappers.
The implementation may recreate a cursor lazily instead of renewing it, but it cannot silently reuse a cached append maximum or cursor position from an earlier leaf epoch.

The root candidate revision is computed exactly once when the root transaction begins and is persisted only immediately before the outer native commit.
Child begin does not acquire the writer gate again, acquire another process lease, or create another candidate revision.
The root's lease anchor remains alive until every child and the root are terminal.

### Result-oriented mutation bodies

Mutation bodies return `Result<T>` for expected failures even after they have staged child state.
The savepoint or root owner, rather than each nested helper, determines whether the complete affected transaction is aborted.

Pure validation remains valuable for three reasons:

- It avoids opening a native child for obviously invalid input.
- It lets a batch report an item-local rejection before doing storage work.
- It keeps expensive or user-facing validation separate from the short active transaction phase.

Pure validation is no longer the atomicity mechanism.
A late recoverable error is safe because it returns to an owner that aborts the root or current child before exposing the error.

`TrackBuilder` may encode this phase split with a validated preparation value:

```cpp
Result<ValidatedTrackPreparation> TrackBuilder::preflight() const;
Result<PreparedTrack> ValidatedTrackPreparation::stage(LibraryWrite& context) const;
```

The exact type shape remains an implementation decision, but one `Result`-returning prepare function must not require callers to know that some recoverable errors instead throw after interning begins.

Methods whose only native outcomes are success or an invariant exception use `void` or their successful value rather than a ceremonial `Result<>`.
Methods with a genuine recoverable outcome retain `Result`.
Normal absence remains optional or boolean where absence and failure are not the same state.

A failed mutation `Result` must never be discarded.
The existing type-level `[[nodiscard]]` contract remains, and tests or lint rules may add targeted enforcement if explicit discard becomes a practical risk.

### Runtime commit and publication

A child commit changes only the root's uncommitted native and dictionary state.
It does not advance the library revision, publish `LibraryChanges`, update sources or projections, or release the command lane.

After the root mutation body succeeds, the existing `LibraryWriteLane` commit path validates the revision, commits the root, completes dictionary publication, and submits exactly one matching `LibraryChangeSet`.
A failed, skipped, previewed, or child-only operation publishes no application change.

Once native root commit succeeds, mandatory revision or publication failure remains an infrastructure fault under the current library architecture.
Nested savepoints do not manufacture a rollback channel after durability.

### Concurrency and lifetime model

The proposal introduces no concurrent writer execution.
The ownership assumptions are:

- The root write session owns the process writer gate, native transaction chain, dictionary journal, and writer-lease anchor.
- The callback or worker thread that owns the root is the only thread that accesses its savepoints, journal, or active-leaf contexts.
- Children form one LIFO chain and are never used in parallel.
- Committed dictionary readers may run concurrently and continue to use the current shared mutex.
- No external callback runs while the committed dictionary exclusive lock or writer gate is being transitioned for publication.
- Application change delivery begins only after root durability and dictionary publication are complete.
- Destruction of a child, root, or maintenance owner is idempotent with respect to native abort and journal rollback.

## Alternatives

### Keep the current root-only boundary

The current root `apply` and live `execute` boundaries already preserve atomicity and hide native mutation markers from ordinary application code.
Stopping there has the smallest further implementation risk and is sufficient for every command that abandons its complete root transaction on any error.
It retains preflight as the only way for a batch to reject one item and continue, and it cannot preserve a parent while rolling back only that already-mutated item.
It remains the fallback if savepoint performance or cursor safety cannot be demonstrated.

### Mark the root transaction as poisoned

Every mutating `Result` failure could set a sticky error on the root and make later writes and commit fail.
This would permit `Result` propagation without exceptions, but it would require every store writer to share and check poison state and would still need a distinction between a retryable no-effect outcome and a transaction-ending failure.
It also cannot provide item-local rollback and continuation.

### Give every child an independent overlay

Each child Dictionary writer could own an overlay and delta layered over its parent.
Lookup would walk the chain, child abort would drop the layer, and child commit would adopt its layer into the parent.

This preserves the current format but introduces a no-fail adoption requirement after native child commit.
Parent vector growth, map insertion, or another allocation cannot be allowed to fail after native state has merged.
The selected chain-wide journal avoids adoption entirely: successful child commit keeps an already-recorded tail, while abort truncates it.

### Persist a reverse dictionary index

A second LMDB database could map a fixed-size `XXH3-128(text)` plus collision-probe ordinal to `DictionaryId`.
The existing dense `id -> text` database would remain authoritative, and every hash candidate would be verified against its stored text before reuse.
Native child visibility would then replace the C++ transaction-local overlay, while outer commit would scan and publish the newly committed dense ID tail into the existing in-memory read index.

A full text key is not suitable because dictionary values may exceed LMDB's bounded key size.
A verified hash/probe index would be correct but would require a database-version change, another row per dictionary string, more write amplification, an increased named-database limit, collision-probe logic, and bidirectional open-time validation.
It remains credible if journal complexity proves larger than expected, but it is not the selected design.

### Use content-derived or gapped Dictionary IDs

Content hashes or a never-reused global counter would make child allocation independent of parent overlays.
They would break the dense `1..N` contract used by positional dictionary storage, Track reference validation, completion frequency arrays, CLI iteration, and tail-only publication.
They would also require rewriting every persisted Track dictionary reference.
The proposal rejects this tradeoff.

### Remove the committed in-memory dictionary index

Using LMDB for every committed text or ID lookup would simplify publication but would break store-lifetime borrowed `string_view` values and add transaction or copy requirements to query binding, projections, completion, playback, and CLI consumers.
It moves complexity and cost from one writer path to every read path.

### Use exceptions for every library failure

Exception-only core operations would naturally unwind native transactions and reduce manual `Result` propagation.
They would make expected user input, media, format, and storage rejection implicit at runtime and frontend boundaries and conflict with the established recoverable command contract.
The proposal instead confines exceptions to invariants and private lower implementation mechanics.

### Create one child for every low-level write

A child is a logical savepoint, not a wrapper for each individual database call.
Per-call children would add cursor churn and merge overhead without defining a useful recovery unit.
Commands use the root owner, and batch operations add children only around independently recoverable logical items or phases.

## Compatibility and migration

### Persistent data

The selected journal design does not change LMDB database names, keys, values, `DictionaryId` allocation, Track records, or the metadata version.
No database migration is required.
The reverse-index alternative would require a separate RFC or an accepted amendment with a format-version change.

### Source and behavior compatibility

The proposal intentionally changes private and core mutation APIs during heavy development.
The current root boundary and validated-iterator policy have removed application catches of both private carriers; there is no compatibility wrapper for code that tries to reintroduce either type above `ao::library`.
Runtime and CLI code consume `Result` at operation boundaries.

Existing command success, no-op, preview, stale, unavailable, and failure semantics remain unchanged unless an owning subsystem specification explicitly adopts a new item-continuation policy.
Existing post-commit infrastructure termination behavior remains unchanged.
The removed low-level `WriteTransaction::begin(parent)` surface has no compatibility wrapper; native child construction is introduced only together with the active-leaf owner described by this RFC.

### Incremental implementation

Root-owner containment is an implemented prerequisite rather than a remaining RFC phase.
The remaining implementation proceeds in reversible phases.

1. Add an explicit root `WriteSession`, active-leaf identity, parent suspension, and callback-scoped child execution together with a source-private native LMDB child mechanism.
2. Split committed Dictionary state from the root-owned `DictionaryWriteSession`, replace child-local allocation with the chain journal and checkpoints, and preserve root-only publication.
3. Add cursor/epoch rejection and reacquisition across child transitions for every store writer and transaction-bound reader surface.
4. Enable savepoints in one batch path whose existing semantics already distinguish item-local failure, initially the scan apply path or an isolated test operation.
5. Measure savepoint and cursor overhead before enabling additional item-level children.
6. Normalize remaining mutation signatures so recoverable errors return `Result`, ceremonial success-only results use their narrow success shape, and existing private-carrier confinement is preserved.
7. Update the current architecture, specifications, reference, and decision record only after the selected behavior is implemented and validated.

Each phase keeps the root transaction abort path available.
If nested enablement is rolled back, the current root-owner containment remains independently useful and authoritative.

### Platform impact

The logical savepoint model is platform-neutral because both native platforms use the same LMDB core and library transaction code.
POSIX and Windows writer-session lease implementations retain their current cross-process exclusion role.
No frontend API or toolkit lifetime changes.

## Validation

### Native transaction tests

- A successful child commit becomes visible to its parent but not to an independent read transaction before root commit.
- Child abort leaves parent-staged state intact and discards only the child delta.
- Sequential siblings observe successful earlier siblings and do not observe aborted siblings.
- Nested grandchildren obey LIFO commit and abort.
- A nested transaction's attempt to open the main database or a named DBI fails at the adapter contract before native access.
- Root commit or abort is rejected while a child is active.
- Parent operations fail while a child is active.
- A failed child commit terminalizes and aborts the root under the conservative policy.
- Repeated child and root abort are idempotent.

### Dictionary tests

- Child interning reuses committed and parent-staged text.
- Grandchildren see ancestor-staged mappings.
- Child commit preserves a dense journal tail for its parent.
- Root publication rejects a nonempty journal whose first ID is not `committedBaseSize + 1` as an invariant fault.
- Child abort truncates the journal and a sibling reuses the released tail ID.
- Aborting a parent discards successful descendant merges.
- Root commit publishes every descendant mapping exactly once and advances generation once.
- Root preview, child failure, root commit failure, and injected failure leave committed lookup, size, generation, and stable borrowed views unchanged.
- Concurrent readers observe either the complete old dictionary or complete root publication, never a child or mixed generation.
- Dictionary exhaustion remains a recoverable error with no zero ID issued.

### Cursor and lifetime tests

- A parent writer cannot be used while a child is active.
- A writer retained across child entry or exit is stale and fails before native access.
- Reacquiring a writer from the resumed parent observes successful child changes and correct append maxima.
- A child writer or transaction-bound view cannot be used after callback completion.
- Root and child destruction in every terminal state release native handles, the writer gate, and the writer-lease anchor exactly once.

### Failure-channel tests

- Existing root regression gates continue to prove recoverable body errors, native mutation faults, unexpected exceptions, terminal commit behavior, and immediate writer-admission reuse.
- No runtime or CLI production file includes or catches `library::detail::LibraryException` for an ordinary operation, and the current confinement of `lmdb::detail::TransactionFailure` is preserved.
- A recoverable child body error before and after staged writes returns the same structured `Error` after child rollback under the selected continuation policy.
- Unknown exceptions preserve their dynamic type and diagnostic location after the child and root are made non-committable.
- Post-commit revision or publication failures retain terminal infrastructure behavior.
- Message text is never used to choose child continuation or root abort.

### Batch behavior tests

- Existing preflight-skipped items remain neutral.
- An explicitly item-local late failure rolls back its child and permits the next sibling only when the owning specification allows it.
- Infrastructure failure aborts the root and prevents later siblings.
- Final reports and `LibraryChangeSet` values contain only successful child effects.
- Cancellation before child begin, during child work, after child merge, and before root commit follows the owning task contract without publishing an uncommitted result.

### Performance and concurrency validation

- Benchmark savepoint-per-item scan and import workloads against the current root-only implementation at representative and large library sizes before broad rollout.
- Measure dictionary interning, cursor reconstruction, child begin/merge, and root publication separately.
- Preserve or deliberately revise the existing performance baselines only after review.
- Run deterministic concurrent-reader publication tests and the governed concurrency suite.
- Run the full AddressSanitizer, UndefinedBehaviorSanitizer, ThreadSanitizer, and normal project gates required by the validation policy when implementation begins.
- Run `./ao docs check` for every documentation promotion.

### Acceptance criteria

- Ordinary runtime and CLI mutation code consumes one recoverable `Result` channel, names no private library exception, and preserves current native-marker confinement.
- No failed savepoint can leave a partial child effect committable in its parent.
- No child publishes dictionary or application state.
- Dense IDs, aborted-tail reuse, stable dictionary text views, and all-or-none concurrent publication remain proven.
- Parent suspension and stale cursor use fail before native mutation.
- Failed child commit cannot be followed by parent continuation.
- Measured batch overhead is reviewed and accepted before item-level savepoints become the default.

## Open questions

- Should the first production savepoint user be scan apply, YAML import, or a smaller isolated batch mutation?
- Which exact `Error::Code` values, if any, are sufficient to authorize sibling continuation, and which operations require a more specific typed item outcome?
- Should the public core API support arbitrary nested depth, or should only the internal mechanism support depth while product operations are limited to one child level?
- Should active-leaf invalidation use a monotonically increasing epoch, an explicit registered-cursor set, or only callback-scoped non-retainable writer wrappers?
- Should child `withSavepoint` be exposed only inside the root `execute`/preview callback, or should both use one lower internal execution primitive?
- Should `TrackBuilder` expose a durable `ValidatedTrackPreparation` type or retain one prepare call after recoverable exception leakage is removed?
- Which current success-only `Result<>` methods should become `void`, and which should retain `Result` for future or third-party failures?
- Should the LMDB adapter eventually replace internal transaction exceptions with a terminalizing result API for child-bound writers, or is exact owner containment sufficient?
- What measured child-per-item overhead is acceptable for a large consumer music library?

## Promotion plan

If accepted and implemented, this RFC updates the following current authorities:

- [Library architecture](../architecture/library.md): writer-chain ownership, one active leaf, child containment, and root-only publication.
- [Failure and reporting architecture](../architecture/failure-and-reporting.md): child transaction-marker containment without changing public recoverable and invariant channels.
- [Outcome channel specification](../spec/failure/outcome-channel.md): recoverable exception containment at savepoint owners.
- [LMDB operation specification](../spec/storage/lmdb-operation.md): parent suspension, child merge/abort, stale cursor behavior, and failed-child-commit policy.
- [Library access and mutation specification](../spec/library/runtime/mutation.md): savepoint behavior, item continuation authority, and removal of the first-effect exception rule where savepoints apply.
- [Library change publication specification](../spec/library/runtime/change-publication.md): root-only visibility and publication after child merges.
- [Library database reference](../reference/library/storage/database.md): transaction-chain and Dictionary write-session mechanics without changing the physical format.

Acceptance also creates a decision record for the durable rationale behind nested savepoints and the chain-wide Dictionary journal, including rejection of gapped IDs, content-derived IDs, a persistent reverse index, and child-overlay adoption.

Implementation and test maps in the promoted documents will link the final `WriteTransaction`, `LibraryWrite`, `DictionaryWriteSession`, `LibraryWriteLane`, LMDB transaction, and focused test symbols.
No user guide is expected because the proposal changes no user task or visible command surface.
After implementation or rejection, every retained fact and inbound link moves to its authoritative current document or decision record, the RFC index is updated, and this RFC is deleted.
