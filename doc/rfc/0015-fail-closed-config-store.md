---
id: rfc.0015.fail-closed-config-store
type: rfc
status: rejected
domain: persistence
summary: Rejected a blocked-store transaction state machine after candidate deserialization and one-shot candidate saves closed the verified destructive paths.
depends-on: none
---
# RFC 0015: Fail-closed grouped configuration transactions

## Disposition

Rejected on 2026-07-16.

The destructive persistence paths were real, but the proposed explicit store states, detached transactions, generations, recovery commands, and commit receipts were broader than the verified problem.
A smaller implementation makes the ordinary boundary fail closed:

- existing files are read and parsed into local candidates, including top-level mapping validation, before becoming the live document;
- `load()` invokes an explicit owner schema and assigns the caller's target only after that schema returns a complete accepted candidate;
- result-bearing `save()` and `saveTogether()` establish the backing document, clone the complete live document, and serialize every requested group into that private candidate;
- serialization, emission, or pre-replacement failure cannot expose the candidate as live state or backing bytes;
- successful atomic replacement precedes no-throw installation of the matching candidate as the live document;
- `removeGroup()` uses the same candidate/replace/install path and avoids writing when the group is absent;
- public `saveResult()` and `flush()` are gone, so callers cannot stage a partial live tree or emit an uninitialized one; and
- production callers use one result-bearing save boundary, including one `saveTogether()` operation for GTK layout and presentation state.

The implementation deliberately does not add a sticky `Blocked` state, `reload()`, generic corrupt-document recovery, detached transaction objects, store generations, conflict detection, or commit receipts.
One store instance remains externally confined to one serialized owner, and every mutation is already one synchronous complete-document candidate.
There is no current detached or competing transaction for a generation to disambiguate.

[RFC 0032](0032-explicit-managed-state-schemas.md) subsequently removed generic aggregate salvage and `loadExact()` while preserving the candidate/replace/install boundary established here.
Strict schema, version, validation, defaults, and recovery decisions now belong entirely to each explicitly selected payload schema.

The [grouped configuration store specification](../spec/persistence/config-store.md) and [persistence architecture](../architecture/persistence-and-managed-state.md) are the current authorities.
They supersede this proposal; this RFC remains the record of why the larger transaction and recovery system was not adopted.

## Problem

At proposal time, `ConfigStore` separated in-memory mutation from `flush()` without preserving the precondition between them.
The void `save()` wrapper discarded the result of `saveResult()`, while `flush()` could emit the current ryml tree without first proving that the backing document had been inspected, read, and parsed.

That enabled a destructive sequence:

1. a fresh store attempted to save;
2. inspection, reading, or parsing failed;
3. the void wrapper discarded the failure; and
4. the following `flush()` replaced the original file with an unestablished empty or partial tree.

Production workspace, GTK configuration, keymap, presentation, and layout writers used this save-then-flush pattern.
Atomic file replacement prevented partial output bytes, but it could still faithfully install the wrong complete document.

`saveResult()` also cleared or created a group in the live tree before invoking its schema.
A schema exception could therefore leave damaged staged state that a later successful operation flushed.
Sequential saves intended as one document update could similarly stage the first group before the second failed.

Reads had the matching target-mutation hazard: ordinary aggregate deserialization could modify known fields before a later field failed.
The proposal combined these concrete bugs with a broader strict-container, blocked-store, recovery, transaction-generation, and receipt design.

## Dependencies

- Hard: None.
- Conditional: None.
- Integration: [RFC 0005](0005-coherent-playback-boundary.md), [RFC 0010](0010-versioned-presentation-state.md).

Those integration edges described the rejected writer, receipt, and strict-version propagation.
The implemented candidate boundary is independent of both RFCs.
The [atomic replacement specification](../spec/persistence/atomic-replacement.md) remains its lower byte-integrity contract.

## Goals

The proposal sought to:

- prevent an ordinary save from replacing a document that initialization rejected;
- remove operations that emit an uninitialized or partially serialized live tree;
- leave caller targets unchanged when group deserialization fails;
- isolate one or more group updates in a complete-document candidate;
- reject malformed present elements through a universally strict generic schema;
- expose blocked/reload/recovery states explicitly;
- detect stale detached transactions through a store generation; and
- return an applied-generation commit receipt.

The initialization, candidate deserialization, candidate write, multi-group atomicity, result propagation, and destructive-API removal goals were achieved by the narrower implementation.
Universal strictness, explicit recovery, detached transactions, generations, and receipts were deliberately not adopted.

## Non-goals

- Define payload fields, defaults, versions, migrations, or semantic validation.
- Provide transactions across several files or independently owned stores.
- Add background scheduling, retries, notifications, or reporting policy to `ConfigStore`.
- Coordinate different processes or different store instances targeting the same path.
- Make external editing safe after a store has cached its backing document.
- Guarantee durability beyond the atomic-replacement contract.
- Infer authorization to replace rejected input with defaults.

## Rejected design

The rejected design replaced the lazy-load flag with `Uninitialized`, `ReadyNew`, `ReadyExisting`, and sticky `Blocked` states.
It added explicit reload and recovery commands, a candidate-returning read API with presence, strict overlay/exact schemas, and a move-only `ConfigTransaction` that owned a whole-document candidate.

Transactions would have captured a monotonically increasing store generation.
Commit would have rejected stale generations, atomically replaced the file, installed the candidate, advanced the generation, and returned a `ConfigCommitReceipt` identifying the applied store generation.
Semantic owners would then have propagated that receipt into playback revisions, workspace checkpoints, GTK state, reporting, retry, and recovery policy.

That design was internally coherent, but most machinery existed only to support other proposed machinery.
Without detached transactions, an externally serialized store has no same-instance stale candidate.
Without a concrete recovery workflow, a sticky blocked state is less useful than retrying initialization after a transient failure.
Without a product action keyed to a store generation, a receipt merely widens every wrapper while the lower atomic replacement result already reports whether replacement was applied.

Strict generic containers were also a separate compatibility decision rather than a prerequisite for write safety.
Changing permissive restore globally could silently alter existing payload recovery behavior; each versioned format should select and test its own strictness.

## Implemented alternative

The store keeps one small two-state model: Unloaded or Loaded.
Initialization reads bytes and parses a local tree, validates that its root is a mapping, and installs both tree and input buffer only after success.
Failure leaves the store Unloaded, so later operations may retry without ever exposing a writable default document for an existing rejected file.

Present-group loads use a copy of the seeded target:

```cpp
Result<> load(std::string_view group, T& target);
Result<> loadExact(std::string_view group, T& target);
```

The selected schema runs against the candidate.
Only complete deserialize success moves that candidate into the caller's target.
Missing groups retain the existing successful no-change behavior; callers use `contains()` when presence matters.

Writes expose one operation rather than staging plus flush:

```cpp
auto workspaceResult = store.save("workspace", workspace);
auto presentationResult = store.save("trackView.columnLayouts", layouts,
                                     "trackView.presentations", presentations);
auto removalResult = store.removeGroup("playbackSession");
```

Each effective write:

1. establishes a valid live document;
2. copies the complete document;
3. applies every requested group change to the copy;
4. emits the complete candidate;
5. atomically replaces the backing file; and
6. moves the candidate into live state only after replacement succeeds.

An serialization or emitter exception unwinds the isolated candidate.
A returned atomic-replacement error also leaves live state unchanged.
The next successful operation therefore cannot accidentally include a group from the failed attempt.

`removeGroup()` follows the same ordering when its group exists.
An absent group is an idempotent success and does not create or rewrite the backing file.

The store remains intentionally unsynchronized.
Its owner confines all operations to one executor or external serialization boundary, while different instances targeting one path remain unsupported competing whole-file writers.

## Caller migration

All production split writers moved to the one-shot boundary:

| Owner | Current behavior |
|---|---|
| `PlaybackSessionPersistence` | Saves or removes its group through one result-bearing replacement. |
| `WorkspaceService` | Saves the workspace group once and logs a returned failure. |
| `AppConfigStore` | Saves one global group once and classifies failure in the wrapper. |
| `KeymapStore` | Saves the shortcut delta once and logs failure. |
| `GtkLayoutStateStore` | Saves column layouts and presentation preferences in one multi-group operation. |
| `ShellLayoutStore` | Uses the one-shot layout-document result and currently classifies failure through logging. |

These wrappers do not all provide semantic acknowledgement to their UI workflows.
That remaining product behavior belongs to each workflow and reporting policy; it does not justify reintroducing a generic store transaction receipt.

## Alternatives

### Remove only the void wrapper

Rejected because a public bare `flush()` and live-tree mutation would still permit uninitialized or partially serialized state to reach disk.

### Make `flush()` initialize first

Rejected because initialization alone would not isolate failed serialization or combine related group updates.
The split API would continue to make correctness a caller sequencing rule.

### Require an explicit transaction object

Rejected for the current synchronous store.
A complete candidate per save and optional multi-group arguments provide the needed atomic boundary without transaction lifetime, commit, abandonment, or generation APIs.

### Add a sticky blocked state and generic recovery

Deferred to a concrete payload that needs it.
Current failures preserve the original bytes and retry initialization; only the semantic owner can decide whether rejected data should be preserved, exported, reset, migrated, or treated as fatal.

### Make every generic container strict

Deferred because this is persisted-format compatibility and recovery policy, not required write isolation.
Versioned owners can use exact or domain-specific deserialization and validation.

## Compatibility and migration

The internal source API intentionally changed.
The void `save()`, public `saveResult()`, and public `flush()` operations were replaced by result-bearing one-shot `save()` and `removeGroup()` operations, and all in-tree callers migrated.
Candidate loads now require copy-constructible, move-assignable payload values, and `removeGroup()` treats absence as success rather than returning a separate removed/not-removed boolean.

No path, group name, or payload schema changed.
Ordinary and exact schema meanings remain as documented, including ordinary container salvage.
The store does not rewrite a file merely because it loads successfully, and rejected files remain byte-identical until an explicit successful save is authorized.

## Validation

The narrower implementation is accepted with tests proving that:

- malformed and non-mapping backing documents are rejected and preserved byte-for-byte;
- a failed batch serializer changes neither disk nor later live-state commits;
- a pre-replacement write failure preserves the previous bytes and does not enter later live-state commits;
- a failed ordinary deserialize leaves a seeded target unchanged;
- one save commits several groups together and preserves unrelated groups;
- removal is durable, exact, idempotent, and does not create a missing file;
- read-only stores reject writes and preserve missing-file behavior;
- callers compile without `saveResult()` or `flush()`; and
- the documentation and full repository validation gates pass.

## Open questions

None for this RFC.
A concrete payload, demonstrated recovery or conflict need, and owner-level workflow are prerequisites for a new blocked-store, receipt, backup-generation, or detached-transaction proposal.

## Promotion plan

No proposal promotion remains.
The implemented behavior is current in:

- [Persistence and managed-state architecture](../architecture/persistence-and-managed-state.md)
- [Grouped configuration store specification](../spec/persistence/config-store.md)
- [Atomic replacement specification](../spec/persistence/atomic-replacement.md)
- [Application managed-state surface](../reference/persistence/application-config.md)
