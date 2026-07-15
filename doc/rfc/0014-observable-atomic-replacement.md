---
id: rfc.0014.observable-atomic-replacement
type: rfc
status: rejected
domain: persistence
summary: Rejected durability receipts and recovery propagation after narrower private-file and RAII hardening closed the verified replacement gaps.
depends-on: none
---
# RFC 0014: Observable atomic replacement

## Disposition

Rejected on 2026-07-15.

The integrity and security problems were real, but the proposed applied-versus-barrier receipt, cleanup evidence, public security-policy enum, and propagation through every semantic store were broader than the verified product need.
A smaller implementation hardened the existing `Result<>` boundary:

- `writeAtomically(path, data)` always creates a private-user same-directory temporary file, so the unused and misleading permission selector is gone;
- POSIX applies mode `0600`, while Windows creates a protected DACL granting full control to the current process user and Local System before any payload byte is written;
- one source-private state machine gives every created temporary file a move-only RAII cleanup owner until replacement succeeds;
- complete write, data synchronization, close, and platform replacement remain mandatory and ordered;
- zero-progress writes are rejected, and close ownership cannot accidentally target a reused POSIX descriptor;
- deterministic tests inject normalization, parent, creation, partial-write, synchronization, close, replacement, and cleanup failures and prove that every returned pre-replacement error preserves the old target; and
- `ConfigStore` tests prove that malformed-file loading does not modify or automatically overwrite the original bytes.

The operation deliberately keeps one `Result<>` meaning: an error is returned before replacement, while success means the operating-system replacement call succeeded.
POSIX parent-directory synchronization remains best effort after replacement, and neither platform claims absolute power-loss durability.

The [atomic replacement specification](../spec/persistence/atomic-replacement.md) and [persistence architecture](../architecture/persistence-and-managed-state.md) are the current authorities.
They supersede this proposal; this RFC remains the record of why the larger observable-receipt and recovery design was not adopted.

## Problem

At proposal time, temporary-file ownership was inconsistent.
The POSIX permission-failure path could close without removing its temporary path, Windows repeated manual close/remove branches, and an ambiguous Windows close could defeat later deletion.
Cleanup failures were discarded, while the tests could not deterministically reach the rare write, barrier, close, replacement, or cleanup transitions.

The public permission vocabulary was also untruthful.
`OwnerReadWrite` enforced mode `0600` on POSIX but was ignored on Windows, where a replacement inherited the destination directory ACL.
Both production callers requested owner-private state, and no caller needed a platform-default file.

The proposal coupled those concrete issues to a larger ambiguity.
POSIX replacement becomes visible before the best-effort parent-directory barrier, so `Result<>` cannot separately report “visible but namespace barrier unconfirmed” without either returning an applied error or introducing a receipt.
The draft therefore proposed propagating lower barrier evidence through grouped-store and semantic-owner acknowledgements.

## Dependencies

- Hard: None.
- Conditional: None.
- Integration: [RFC 0005](0005-coherent-playback-boundary.md), [RFC 0010](0010-versioned-presentation-state.md), [RFC 0013](0013-coherent-application-reporting-policy.md), [RFC 0015](0015-fail-closed-config-store.md).

Those integration relationships described the rejected receipt propagation.
The implemented lower hardening is independent and does not require any of those RFCs.

## Goals

The proposal sought to:

- prevent a returned pre-replacement failure from exposing partial target bytes;
- make temporary ownership and cleanup uniform;
- enforce truthful private-file security on every supported platform;
- distinguish visible replacement from a completed namespace-barrier sequence;
- carry that evidence through semantic save acknowledgement and reporting; and
- deterministically test rare filesystem transitions.

The visibility, ownership, security, and deterministic-test goals were achieved by the narrower implementation.
The receipt propagation and application recovery goals were deliberately not adopted.

## Non-goals

- Transactions across several files, groups, stores, or directories.
- A journal, write-ahead log, backup generation, checksum, or automatic recovery protocol.
- Coordination between processes or semantic ordering for same-target writers.
- Preservation of arbitrary metadata from the replaced file object.
- Automatic scavenging of process-crash artifacts.
- Uniform durability behavior across local, network, virtual, and removable filesystems.

## Proposed design

The rejected design introduced `AtomicReplacementReceipt` with `VisibilityOnly` and `PlatformBarrierCompleted` outcomes, a typed post-replacement issue, and an `AtomicFileSecurityPolicy` choice between `PrivateUser` and `PlatformDefault`.
`ConfigStore` and semantic owners would have propagated the receipt, retained dirty revisions after visibility-only replacement, scheduled barrier-confirming retries, and reported the degraded state according to payload policy.

It also proposed explicit cleanup results, diagnostic exposure of abandoned paths, a platform-operation seam covering post-replacement barrier failures, and a documented same-target concurrent-reader/writer matrix.

That design was internally coherent, but it turned a small byte-integrity utility into the first layer of an application-wide recovery protocol.
No current consumer has a product decision it can make from a namespace-barrier receipt: it cannot restore the previous target after replacement, and retrying the same bytes can race a newer semantic revision unless the owner gains generation, dirty-state, and scheduling machinery.
Those mechanisms may be valid for a future durability-critical payload, but they are not justified merely to harden application configuration replacement.

## Implemented alternative

The common algorithm is a private template over platform operations rather than a public virtual or type-erased interface.
It normalizes the path, prepares the parent, acquires one private temporary owner, writes, synchronizes, closes, replaces, and finally invokes a no-throw platform post-step.
Tests supply a scripted in-memory implementation; production supplies direct POSIX or Windows operations.

The temporary owner is committed only after the replacement API returns success.
Every earlier return runs its no-throw destructor, which closes an owned handle and attempts to remove the path.
Cleanup remains best effort: the primary `IoError` is preserved, an ambiguous close or failed deletion can leave an artifact, and process termination can bypass cleanup.

Security is an invariant rather than a selectable policy.
Every production caller stores private managed state, so removing the unused `PlatformDefault` possibility keeps the public API smaller and makes the guarantee platform-truthful.

Post-replacement behavior stays deliberately unobservable at this layer.
POSIX attempts a parent-directory `fsync` but cannot turn its failure into an ordinary “not applied” error; Windows relies on the completed `MoveFileExW` request with write-through and performs no separate directory operation.
A future payload that truly requires crash recovery must define its own recovery evidence and protocol rather than reinterpret this `Result<>`.

## Alternatives

### Keep the prior implementation

Rejected because Windows ignored the requested privacy boundary, POSIX had a known artifact leak path, manual cleanup duplicated ownership, and rare failures were untested.

### Implement only manual cleanup fixes

Rejected because repeated close/remove branches had already diverged across platforms.
One move-only owner makes the pre-replacement rule structural and provides a deterministic common test seam.

### Keep a public platform-default security option

Rejected because no production caller needs it and it would make private managed-state storage an optional call-site convention.
A future public-output use case should use a separate API whose metadata contract matches that artifact.

### Return an ordinary error after replacement

Rejected because callers conventionally interpret an error as not applied.
Once `rename` or `MoveFileExW` succeeds, that interpretation is false and rollback would require a second transaction.

### Add one backup generation

Deferred because recovery policy depends on the payload.
A user-authored layout may eventually justify preserving one prior generation, while transient component state may not; generic `AtomicFile` cannot choose retention, validation, or user-consent policy for both.

## Compatibility and migration

The two production callers migrated from the explicit `OwnerReadWrite` argument to the two-argument operation.
There is no in-tree caller for the removed `Default` mode and no source-compatibility requirement.

Existing targets are replaced by newly secured file objects on their next successful save.
The implementation does not rewrite files merely to update permissions, does not migrate or delete old temporary artifacts, and does not change payload bytes or schema behavior.

## Validation

The narrower implementation is accepted with:

- scripted success plus normalization, parent, creation, partial-write, data-barrier, close, replacement, and cleanup failure tests;
- assertions that every returned pre-replacement failure preserves the old target and never runs the post-replacement step;
- real empty, embedded-null, creation, overwrite, directory-target, and cleanup tests;
- POSIX owner-only mode verification;
- native Windows protected-DACL, extended-path, and unique-temporary-name tests;
- malformed `ConfigStore` byte-preservation coverage;
- documentation checks; and
- the repository's full validation gate.

## Open questions

None for this RFC.
A concrete durability-critical payload, demonstrated failure mode, and recovery policy are prerequisites for a new receipt, backup-generation, or journal proposal.

## Promotion plan

No proposal promotion remains.
The implemented behavior is current in:

- [Persistence and managed-state architecture](../architecture/persistence-and-managed-state.md)
- [Atomic file replacement specification](../spec/persistence/atomic-replacement.md)
- [Grouped configuration store specification](../spec/persistence/config-store.md)
- [Managed file locations reference](../reference/persistence/location.md)
