---
id: rfc.0015.fail-closed-config-store
type: rfc
status: draft
domain: persistence
summary: Proposes candidate-based decoding, explicit document transactions, and fail-closed commits for grouped managed configuration.
depends-on: none
---
# RFC 0015: Fail-closed grouped configuration transactions

## Problem

The current [`ConfigStore`](../../app/include/ao/rt/ConfigStore.h) separates in-memory mutation from `flush()`, but its public API does not preserve the precondition between them.
The void `save()` wrapper discards the `Result` from `saveResult()`, while [`flush()`](../../app/runtime/ConfigStore.cpp) emits the current ryml tree without first establishing or validating the backing document.

That creates a destructive path against an existing file:

1. A store is constructed and remains Unloaded.
2. `save()` tries to initialize it.
3. Path inspection, file reading, or YAML parsing fails.
4. `save()` discards that failure.
5. The caller invokes `flush()`.
6. `flush()` emits the unestablished live tree—default after inspection or read failure, and potentially partial after parser failure—and atomically replaces the unreadable or temporarily inaccessible file.

Calling `flush()` directly on a fresh store reaches the same final step.
Atomic file replacement prevents a partial file, but faithfully installing a complete empty or default document is still data loss.

This is a production path, not only an accidental low-level API combination.
`WorkspaceService`, `AppConfigStore`, `KeymapStore`, `GtkLayoutStateStore`, and `ShellLayoutStore` all use a void save followed by flush.
`PlaybackSessionPersistence` uses the result-returning mutation path and avoids that specific loss, but still depends on the same split mutable-document protocol.

Mutation is not transactionally isolated inside the store either.
`saveResult()` clears or creates the live group before invoking its codec.
An allocation or codec exception can therefore leave the cached tree cleared or partially encoded; a later successful save of another group can flush that damaged tree even though the first operation never reported success.
Two group saves intended as one document update can likewise leave the first staged when the second fails.

Read behavior has a parallel fail-open edge.
Ordinary aggregate decoding writes known fields directly into the caller's target before the complete group is known to be valid.
Vectors and maps clear their target, skip children that fail to decode, and still return success; arrays retain defaults or prior values for invalid children and ignore excess elements.
`loadExact()` is transactional only for reflected aggregates and vectors because its generic fallback delegates other types to ordinary decode.

These behaviors are documented in the current [grouped configuration store specification](../spec/persistence/config-store.md), but the test suite does not directly protect Unloaded flush, discarded initialization failure, partial encode, or a failed ordinary decode's exact target state.
The API makes destructive sequencing easy, safe sequencing repetitive, and schema-recovery policy implicit in codec templates.

## Dependencies

- Hard: None.
- Conditional: None.
- Integration: [RFC 0005](0005-coherent-playback-boundary.md), [RFC 0010](0010-versioned-presentation-state.md).

RFC 0005 introduces a serialized configuration writer for shared runtime stores; that writer should own the transactions proposed here rather than serialize the legacy mutation/flush split.
RFC 0010 introduces versioned presentation codecs and fail-closed object recovery; those codecs should target the candidate-read contract instead of a second store API.
The current [atomic replacement specification](../spec/persistence/atomic-replacement.md) supplies the lower byte-integrity boundary: an error is pre-replacement, while success means the platform replacement call succeeded without claiming absolute power-loss durability.

## Goals

- Make it impossible for an ordinary save path to replace a document that the store failed to inspect, read, or parse.
- Remove public operations that can emit an uninitialized tree or discard mutation failure.
- Decode every group into a candidate and leave the caller's value unchanged on failure.
- Reject malformed present container elements instead of silently salvaging them through a generic codec.
- Stage one or more group changes in an isolated complete-document transaction.
- Give every staged group operation a strong failure guarantee: failure leaves both the live store and transaction candidate unchanged.
- Emit and replace only a completely encoded transaction candidate.
- Return one commit receipt that identifies the committed store generation after successful file replacement.
- Let semantic owners choose missing-data, recovery, retry, acknowledgement, and reporting policy without message parsing.
- Preserve one writer authority and executor confinement rather than hiding concurrency policy in the YAML codec.

## Non-goals

- Define payload fields, defaults, versions, migrations, or semantic validation for every managed group.
- Provide a transaction across several files, databases, or independently owned stores.
- Turn `ConfigStore` into a background scheduler, retry loop, notification service, or application facade.
- Add cross-process semantic ordering or decide which competing application revision should win.
- Make external editing safe while a long-lived store is active without an explicit locking or reload contract.
- Guarantee filesystem durability beyond the atomic-replacement contract.
- Preserve source compatibility for `save()`, `saveResult()`, `flush()`, or target-mutating load overloads.
- Recover an unreadable document automatically merely because a payload has usable defaults.

## Proposed design

### Store states

Replace the current boolean lazy-load flag with an explicit internal state:

| State | Meaning | Write eligibility |
|---|---|---|
| `Uninitialized` | Construction captured only path and open mode. | No commit. |
| `ReadyNew` | A missing read/write file was observed and an empty mapping is the established base. | Transactions allowed. |
| `ReadyExisting` | A complete existing file was read and parsed successfully. | Transactions allowed. |
| `Blocked` | Inspection, required-file presence, read, or parse failed; the failure evidence is retained. | Ordinary transactions and commits rejected. |

The first read or transaction request initializes the store.
Initialization failure transitions to `Blocked` instead of leaving an apparently writable default tree.

`reload()` explicitly retries initialization from `Blocked` or refreshes an otherwise quiescent store.
It never merges disk content with staged state and is rejected while a transaction is active.

Replacing an unreadable document requires a separately named recovery operation selected by the semantic owner.
Ordinary `beginTransaction()` cannot reinterpret “failed to load” as “create a new document.”
The recovery operation reports the original failure, makes document replacement explicit in review and tests, and gives the owner an opportunity to preserve, export, report, or ask before discarding the rejected bytes.

### Candidate group reads

Replace target-mutating `load()` and the separate `contains()` preflight with one result carrying presence and a value:

```cpp
enum class ConfigGroupPresence : std::uint8_t
{
  Missing,
  Present,
};

template<typename T>
struct ConfigGroupRead final
{
  ConfigGroupPresence presence;
  T value;
};

template<typename T, typename Codec>
Result<ConfigGroupRead<T>>
readGroup(std::string_view group, T seed, Codec const& codec);
```

The seed is copied or moved into an isolated candidate.
A missing group returns `Missing` with the seed unchanged.
A present group returns `Present` only after the selected codec has accepted the complete candidate.
Any read, parse, node-kind, scalar, schema, or codec failure returns an error and cannot modify a live caller object.

Presence is part of the same store snapshot as decoding.
Callers no longer need `contains()` followed by a second group operation to distinguish missing data.

### Explicit decode policies

The common codec exposes two structural policies with transactional candidate assignment:

- `Overlay` permits absent known aggregate fields to retain the supplied seed and ignores unknown aggregate fields, but every present known field and every present container element must decode successfully;
- `Exact` requires the complete declared field set, rejects unknown or missing fields, and recursively requires every value and element.

Neither policy silently skips a malformed vector or map entry.
Both reject duplicate mapping keys within the governed group rather than selecting an incidental first or last value.
Fixed arrays require an explicit size policy rather than implicitly ignoring excess children.
Invalid optional content rejects the candidate instead of clearing the optional and continuing.

Generic enum casting is not semantic validation.
A durable payload that uses an enum supplies an enum codec or validator; versioned payloads should use stable identifiers according to their format owner.
The existing numeric aggregate traits may remain temporarily behind an explicitly named legacy codec, but they are not the default for new managed state.

Domain-specific salvage remains possible only in a domain codec that returns a typed normalization or recovery result.
The generic store does not decide that dropping malformed children is acceptable.

### Document transaction

`beginTransaction()` first establishes a Ready store and returns a non-copyable transaction bound to the store's current generation.
The transaction owns an isolated complete-document candidate and never exposes mutable access to the store's live ryml tree.

Its conceptual command surface is:

```cpp
Result<ConfigTransaction> beginTransaction();

template<typename T, typename Codec>
Result<> ConfigTransaction::put(
  std::string_view group,
  T const& value,
  Codec const& codec);

Result<bool> ConfigTransaction::remove(std::string_view group);
Result<ConfigCommitReceipt> ConfigTransaction::commit();
```

`put()` encodes the payload into a detached temporary node or tree first.
Only after complete encoding succeeds does it replace the corresponding group in the transaction candidate.
Failure leaves the prior candidate group unchanged.

`remove()` changes only the transaction candidate and reports whether the group existed there.
Several puts and removals therefore form one whole-file candidate without mutating the live store between calls.

Transactions capture a monotonically increasing in-memory store generation.
Commit returns `Conflict` if another accepted transaction or explicit reload changed that generation before this transaction commits.
The generation prevents a stale transaction within one store from overwriting a newer accepted document; it is not a cross-process revision.

### Fail-closed commit

Commit performs one ordered state transition:

1. Verify the store remains Ready and the transaction generation is current.
2. Emit the complete candidate into an owned byte string.
3. Prepare every live-tree and receipt value needed after filesystem replacement.
4. Invoke atomic replacement once with the complete bytes.
5. If replacement was not applied, retain the live store and transaction candidate for caller-directed retry or abandonment.
6. If replacement was applied, install the already-prepared candidate into the live store without a fallible allocation, advance the store generation, update replacement evidence, and consume the transaction.

There is no public bare `flush()`.
Only a transaction whose base document was established and whose complete candidate was emitted can reach file replacement.

`ConfigCommitReceipt` contains the accepted store generation after the current atomic replacement operation returns success.
It records the store transaction that was applied; it is not a stronger filesystem-durability receipt than the lower replacement contract.
A future payload-specific recovery protocol must define and integrate any additional evidence explicitly rather than silently widening this receipt.

An emission or pre-replacement I/O failure is not applied and leaves the live document unchanged.
A commit API may retain its transaction candidate for retry, but it cannot make retry automatic or acknowledge a semantic revision.

### Explicit recovery

A blocked store exposes its retained initialization error and path but no writable tree.
Recovery is a distinct semantic command, not a flag passed casually to `put()` or commit.

The recovery API establishes an empty or caller-supplied complete candidate only after the owner selects an explicit replacement policy.
It then uses the same transaction and commit path, so recovery cannot bypass encoding, atomic replacement, receipt propagation, or reporting ownership.

The store does not automatically delete or rename rejected input.
Payload specifications decide whether a corrupt preference is silently reset, backed up, exported, reported, or treated as a blocking failure.
Unsupported future versions are schema rejection, not generic permission to overwrite with defaults.

### Writer and executor ownership

The store remains unsynchronized internally.
One owner confines initialization, reads, transactions, reload, and commit to one executor or external serialization boundary.

When RFC 0005 is implemented, its serialized configuration writer owns `ConfigTransaction` values and is the only collaborator permitted to commit a shared store.
It serializes complete transaction commands, not independent `save()` and `flush()` work items.

The store generation rejects stale transactions but does not make concurrent calls data-race-safe.
Two `ConfigStore` instances targeting one path remain unsupported unless a later cross-process ownership protocol is introduced.

### Caller migration

Every current writer moves to a result-bearing operation boundary:

| Owner | Transaction boundary |
|---|---|
| `PlaybackSessionPersistence` | One playback-session put or removal and one revision-correlated commit receipt. |
| `WorkspaceService` | One complete workspace group put; save returns a typed result instead of logging inside a void command. |
| `AppConfigStore` | One explicit global group update, or a deliberate batch when several groups represent one lifecycle checkpoint. |
| `KeymapStore` | One delta group update with the result returned to the GTK workflow. |
| `GtkLayoutStateStore` | Column layout and list-presentation groups in one transaction. |
| `ShellLayoutStore` | One layout document transaction per preset file. |

Frontend and semantic wrappers no longer call raw `flush()` or discard a result.
They either return the commit outcome to their owner or deliberately classify it according to the owning operation specification and reporting policy.

### Current-spec transition

Until implementation changes, the [grouped configuration store specification](../spec/persistence/config-store.md) remains the authority for lazy loading, target-mutating decode, live-tree mutation, void save, and bare flush behavior.
The [YAML adapter specification](../spec/persistence/yaml-adapter.md) continues to own only the lower RapidYAML and scalar mechanism.
This RFC does not make proposed transaction behavior current by being linked from those documents.

## Alternatives

### Delete only the void `save()` wrapper

Requiring `saveResult()` would close the discarded initialization failure at current callers, but bare Unloaded `flush()`, live-tree partial encoding, multi-group partial staging, and target-mutating decode would remain.

### Make `flush()` call `ensureLoaded()`

This prevents fresh flush from skipping initialization but retains the unsafe split and still permits a partially encoded live tree to be flushed later.
It also cannot prove which successful mutations the caller intended to commit together.

### Add a dirty bit

A dirty bit can reject an untouched flush, but an attempted encode may mark or damage the document before failure.
It provides no candidate isolation, semantic revision, or multi-group transaction boundary.

### Clone the caller's object only during load

Candidate assignment fixes partial target mutation but does not change container codecs that silently accept malformed children or any write-side hazard.

### Keep generic salvage for resilience

Silently dropping invalid entries can make a corrupt keymap, view list, ordering, or identity collection look like a valid user choice and then persist the reduced value.
Salvage must be explicit and domain-owned so its normalization can be observed and tested.

### Add automatic reset after parse failure

Defaults are not evidence that replacing unreadable or newer-version data is authorized.
Automatic reset converts a recoverable load problem into irreversible data loss.

### Put all managed state in one database

A transactional database would solve a different scope and merge payloads with distinct path, sharing, portability, and lifecycle requirements.
Complete-file transactions remain appropriate for these small managed documents.

## Compatibility and migration

This proposal intentionally changes internal APIs and some current permissive decoding behavior.
It does not change file paths by itself.
Persisted compatibility changes only when a payload owner adopts a stricter or versioned codec, and that owner documents its migration or explicit lack of compatibility.

Implementation proceeds in phases:

1. Add focused regression tests that freeze the current destructive and partial-mutation paths before changing them.
2. Add candidate-returning group reads and strict container behavior beside current load overloads.
3. Add isolated document transactions, detached group encoding, store generation, and commit tests while adapting to the current atomic replacement result.
4. Migrate `PlaybackSessionPersistence`, then multi-group GTK presentation state, workspace, global GTK groups, keymaps, and layout documents to result-bearing transactions.
5. Remove void `save()`, public `saveResult()`, bare `flush()`, target-mutating load overloads, and generic silent container salvage.
6. Integrate the serialized writer from RFC 0005 when that proposal is implemented.
7. Move legacy numeric enum payloads to their owning explicit codecs, coordinating presentation formats with RFC 0010.
8. Add reporting dispositions at semantic owners and remove lower wrapper logging where typed outcomes now propagate.

No automatic rewrite occurs merely when a document is read.
A compatible document is written only through a semantic save or explicit migration commit.

## Validation

- A fresh store cannot replace an existing file through commit without successful initialization.
- Inspection, read, and parse failure enter `Blocked`; ordinary read, transaction, and commit operations preserve the original file.
- Explicit reload can recover after a transient failure without exposing a writable default document in between.
- Explicit recovery is the only path from rejected input to replacement with defaults or a new candidate.
- Missing groups return `Missing` distinctly from a present successfully decoded group.
- Every failed group read leaves the seed and all live caller state unchanged.
- Overlay decode accepts missing known fields and unknown fields but rejects duplicate keys, every invalid present known field, and every malformed container element.
- Exact decode rejects missing, unknown, duplicate where observable, invalid, and malformed nested values without partial assignment.
- Detached encoding failure leaves the transaction's previous group and live store unchanged.
- Several successful puts followed by one failed put can be abandoned without changing the live tree or file.
- One transaction commits several group changes through one complete-file replacement.
- Stale same-store transactions return `Conflict` and cannot replace a newer accepted generation.
- Emission and pre-replacement write failure retain the old live document and backing file.
- An applied commit installs the matching candidate and returns its exact store generation.
- A successful commit receipt names the exact applied store generation without claiming stronger durability than atomic replacement provides.
- Playback revision tests acknowledge only the revision named by a matching applied commit.
- GTK presentation tests prove column and preference groups cannot be split across transactions.
- Workspace, keymap, global GTK, and layout tests prove their wrappers return or deliberately classify every commit outcome.
- Repository search and a boundary test reject production calls to removed `save()`, `saveResult()`, and `flush()` APIs.
- The completed implementation passes `./ao check` and the documentation gate.

## Open questions

- Should explicit corrupt-document recovery require preserving the rejected bytes under a sibling diagnostic name before replacement, or should that remain payload policy?
- Is one active transaction per store enforced dynamically, or are several generation-checked candidates useful enough to permit?
- Should a store capture and compare a complete-file fingerprint before commit to diagnose external edits, despite the remaining check-to-replace race?
- Which current payloads need an explicit legacy overlay codec during migration, and which can move directly to versioned domain codecs?
- Should the implementation retain the `ConfigStore` name after its public surface becomes a grouped document transaction rather than a mutable store-plus-flush API?

## Promotion plan

If accepted, update the [persistence and managed-state architecture](../architecture/persistence-and-managed-state.md) with the transaction owner, Ready/Blocked boundary, candidate flow, and commit receipt.
Replace the current mutation, decode, state, failure, and test contracts in the [grouped configuration store specification](../spec/persistence/config-store.md) phase by phase while leaving the [YAML adapter specification](../spec/persistence/yaml-adapter.md) focused on Core mechanisms.

Update the [application managed-state surface](../reference/persistence/application-config.md) only for implemented codec modes, document recovery markers, or group registry changes.
Update the [workspace session specification](../spec/workspace/session.md) with candidate, transaction, acknowledgement, and failure behavior, and update the [workspace session state reference](../reference/workspace/session-state.md) only for implemented codec or recovery markers.
Update the [interactive session lifecycle architecture](../architecture/interactive-session-lifecycle.md) and [GTK active-library lifecycle specification](../spec/linux-gtk/active-library-lifecycle.md) if result-bearing checkpoints change switch or shutdown ordering.
Add or update playback-session, keymap, presentation, and layout specifications with their missing, recovery, transaction, acknowledgement, and reporting policy.

Coordinate RFC 0005's serialized writer and RFC 0010's versioned presentation codecs when implemented together.
Update the failure/reporting documents only where new typed outcomes cross a semantic owner, and record an ADR if candidate transactions, blocked-store recovery, or removal of generic salvage represents a durable choice with credible alternatives.
