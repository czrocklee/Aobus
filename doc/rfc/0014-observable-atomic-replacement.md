---
id: rfc.0014.observable-atomic-replacement
type: rfc
status: draft
domain: persistence
summary: Proposes explicit applied-versus-barrier-completed outcomes, enforced private-file security, unified temporary ownership, and deterministic failure tests.
depends-on: none
---
# RFC 0014: Observable atomic replacement

## Problem

The current [`writeAtomically`](../../include/ao/utility/AtomicFile.h) mechanism protects complete-file visibility by writing and synchronizing a same-directory temporary file before replacing the target.
That is a useful lower boundary, but its single `Result<>` cannot express all states that matter to a persistence owner.

On non-Windows platforms, [`AtomicFile.cpp`](../../lib/utility/AtomicFile.cpp) returns success after `rename` even when opening or synchronizing the parent directory fails.
Returning an ordinary error at that point would be worse: the new target is already visible, so an error conventionally interpreted as “not applied” could make a caller retry, roll back, or publish the wrong revision.
The implementation therefore hides a real distinction between a visible replacement and a replacement whose namespace barrier completed.

That ambiguity crosses the runtime boundary.
[`ConfigStore::flush`](../../app/runtime/ConfigStore.cpp) can acknowledge only success or failure, so a semantic owner cannot retain a dirty revision when the new bytes are visible but crash durability is unconfirmed.
It also cannot report that condition without parsing a platform message.

Temporary-file ownership is not uniform.
Most pre-replacement failures attempt cleanup, but the non-Windows permission-application branch can leave its temporary path behind, and a failed Windows handle close can prevent removal.
Cleanup failures are discarded, so an operator cannot distinguish a clean rejection from one that left an artifact.
Process termination can also leave temporary files, but the naming schemes are not exposed as a governed operational contract.

The public permission vocabulary is misleading across platforms.
`OwnerReadWrite` means mode `0600` on non-Windows systems, but is ignored by the Windows implementation, where the target inherits the parent directory's ACL.
Both production callers explicitly request `OwnerReadWrite`, so the same request expresses an enforced privacy boundary on one platform and an advisory preference on another.

The tests protect final content, overwrite, basic failure, non-Windows mode, Windows long paths, and different-target concurrency.
They do not deterministically reach partial writes, interruption, data-barrier failure, close failure, replacement failure, namespace-barrier failure, permission failure, or cleanup failure.
They also do not prove the central visibility property with concurrent readers or establish the supported result of same-target races.

These are one design problem rather than independent patches: the mechanism lacks a precise outcome model, one temporary-artifact owner, a truthful security policy, and a test seam capable of proving their relationship.

## Dependencies

- Hard: None.
- Conditional: None.
- Integration: [RFC 0005](0005-coherent-playback-boundary.md), [RFC 0010](0010-versioned-presentation-state.md), [RFC 0013](0013-coherent-application-reporting-policy.md), [RFC 0015](0015-fail-closed-config-store.md).

RFC 0005 moves playback persistence away from the callback executor and introduces one writer domain; its acknowledgement must preserve the replacement receipt.
RFC 0010 requires atomic writes for versioned presentation state and must select policy for a visible but unconfirmed replacement.
RFC 0013 owns whether that condition is observed, reported, or diagnostic-only for each semantic save operation.
RFC 0015 replaces bare `ConfigStore::flush()` with a receipt-bearing grouped document commit.
This RFC can be implemented against the current callers, but joint implementations align at those boundaries.

## Goals

- Make every normal return identify whether the target was not applied, was replaced with visibility only, or was replaced after the configured platform barrier sequence completed.
- Guarantee that a returned error means the replacement point was not crossed.
- Keep atomic visibility distinct from platform barrier completion and from absolute power-loss durability.
- Let semantic stores acknowledge, retain, retry, and report a revision without inferring filesystem state from error text.
- Give every created temporary artifact one owner until successful replacement.
- Attempt cleanup on every pre-replacement failure and preserve cleanup evidence when removal fails.
- Replace advisory permission modes with enforceable, cross-platform security policies.
- Prove rare failure transitions through deterministic injection while retaining real-filesystem platform tests.
- Preserve one semantic writer above the mechanism rather than hiding lost-update policy in a process-local file lock.

## Non-goals

- Provide transactions across multiple files, keys, stores, or directories.
- Guarantee durability beyond the successfully completed operating-system barriers and the filesystem's own contract.
- Add a journal, write-ahead log, backup generation, or automatic recovery from media corruption.
- Turn this Core utility into a save scheduler, retry service, reporting service, or revision authority.
- Coordinate several processes or application components writing the same target.
- Preserve arbitrary metadata from the replaced inode or Windows file object.
- Delete process-crash artifacts opportunistically from a live write path.
- Make network and virtual filesystems emulate local-filesystem barriers they do not support.

## Proposed design

### Three observable outcomes

Change the operation to return a typed receipt:

```cpp
enum class ReplacementDurability : std::uint8_t
{
  VisibilityOnly,
  PlatformBarrierCompleted,
};

enum class ReplacementPostStage : std::uint8_t
{
  OpenNamespace,
  SynchronizeNamespace,
  CloseNamespace,
};

struct ReplacementPostIssue final
{
  ReplacementPostStage stage;
  std::error_code systemError;
};

struct AtomicReplacementReceipt final
{
  ReplacementDurability durability;
  std::optional<ReplacementPostIssue> issue;
};

Result<AtomicReplacementReceipt>
writeAtomically(const std::filesystem::path& path,
                std::string_view data,
                AtomicFileSecurityPolicy security);
```

The `Result` and receipt together form three outcomes:

| Outcome | Replacement point | Meaning |
|---|---|---|
| `Error` | Not crossed. | The requested target bytes were not applied through this attempt. Parent directories or a diagnosed temporary artifact may still exist. |
| `VisibilityOnly` | Crossed. | The complete target is visible, but the post-replacement namespace barrier was unavailable or failed. |
| `PlatformBarrierCompleted` | Crossed. | The complete target is visible and the configured platform barrier sequence returned success. |

`PlatformBarrierCompleted` deliberately does not use the word “durable.”
It records completed evidence: successful temporary-data synchronization followed by the platform's successful replacement and namespace-barrier sequence.
It does not promise behavior that a device, remote share, filesystem, hypervisor, or operating system does not provide.

`ReplacementPostIssue` carries a typed post-replacement stage and the platform `std::error_code`.
`VisibilityOnly` always carries the issue that prevented barrier completion.
`PlatformBarrierCompleted` normally has no issue but may carry a later namespace-handle close warning, so operators retain evidence without parsing a message and normal consumers can branch only on durability.

An error cannot be produced after the replacement point.
The implementation prepares result storage and diagnostic context before replacement and performs no fallible allocation after it.
Expected C++ and operating-system failures before replacement become `IoError`; no exception may escape and leave the caller unable to classify whether replacement occurred.

### Platform barrier mapping

On non-Windows systems, temporary-file `fsync`, close, and `rename` remain mandatory before a receipt can be returned.
After successful `rename`:

- successful parent-directory open and `fsync` return `PlatformBarrierCompleted`;
- inability to open or synchronize the directory returns `VisibilityOnly`;
- failure to close the directory after a successful `fsync` does not erase the completed barrier and is retained in `ReplacementPostIssue`.

On Windows, successful `FlushFileBuffers`, handle close, and `MoveFileExW` with `MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH` return `PlatformBarrierCompleted`.
The receipt describes completion of that API sequence only; it does not imply an undocumented directory-handle flush.
If supported Windows versions later gain a stronger explicit namespace barrier, it can replace this mapping without changing the receipt vocabulary.

Unsupported or rejected pre-replacement data barriers remain errors.
The helper never installs bytes it was unable to synchronize first merely to obtain `VisibilityOnly`.

### Semantic acknowledgement

The current `ConfigStore::flush()` or RFC 0015's proposed document commit, plus other adapters, preserves the `AtomicReplacementReceipt` rather than collapsing it to `Result<>`.
Serialization and replacement remain separate stages: a serialization error is not applied, while a successful replacement yields one of the two applied receipts.

The semantic owner of each managed state declares how it treats `VisibilityOnly`:

- durability-critical state keeps the revision dirty and schedules a retry of the latest complete snapshot;
- best-effort preferences may accept visible state while emitting the specified diagnostic or report;
- neither policy may claim that the previous target is still installed;
- a retry never republishes an older semantic revision over a newer snapshot merely to confirm durability.

Receipt propagation stops at the semantic owner.
Frontends receive a domain outcome or report disposition, not raw platform barrier details, unless an operator-facing diagnostic surface explicitly owns them.

### Enforced security policy

Replace `AtomicFilePermissions` with a policy whose names describe observable intent:

```cpp
enum class AtomicFileSecurityPolicy : std::uint8_t
{
  PrivateUser,
  PlatformDefault,
};
```

`PrivateUser` is mandatory rather than advisory:

- on POSIX-like systems, the temporary file is mode `0600` before any payload is written;
- on Windows, creation uses a protected DACL granting the current user and `SYSTEM` the access required to replace and maintain the file, with no broad inherited allow entry;
- failure to establish that policy is a pre-replacement `IoError`;
- the contract does not claim protection from administrators, privileged processes, backup operators, or offline access.

`PlatformDefault` deliberately adopts the platform's ordinary new-file policy:

- POSIX creation uses the process creation mask and applicable default ACL rather than inheriting metadata from an existing target;
- Windows uses the parent directory's normal ACL inheritance;
- replacement still installs a new file object and does not preserve the old target's ACL or mode as a hidden side effect.

All current managed-state callers migrate to `PrivateUser`.
The security descriptor, mode, and policy selection are applied to the temporary file before payload bytes are written or the target can be replaced.

### One temporary-artifact owner

Each platform implementation creates a private `TemporaryArtifact` owner containing the path, open handle, and committed state.
From exclusive creation until successful replacement, exactly one live owner is responsible for closing and removing that artifact.

The owner provides explicit operations for write, barrier, close-for-replacement, and commit.
Its no-throw destructor is a last-resort cleanup path, while normal failure paths call cleanup explicitly so cleanup evidence can be attached to the primary error.
Successful replacement marks the artifact committed only after the operating-system replacement call returns success; its former temporary path must then be absent by definition.

If close or removal fails, the original operation failure remains primary and the diagnostic records:

- the cleanup stage;
- the operating-system error;
- the temporary path when it is safe to expose to operator logs;
- whether a later cleanup attempt could still remove the artifact.

Cleanup failure does not change a pre-replacement error into success.
It also does not weaken the guarantee that the target was not deliberately modified.

### Artifact naming and crash recovery boundary

Temporary names retain a recognizable, exclusively created Aobus prefix and sufficient unpredictable or monotonic uniqueness for the platform.
The exact suffix is not public API, but the prefix and same-directory placement are documented for diagnostics.

This RFC does not add automatic scavenging.
Deleting sibling temporary paths by age can race a slow process, a suspended writer, or another Aobus instance.
A future cleanup proposal must define ownership proof, minimum age, process identity limitations, symlink and reparse-point handling, and operator visibility before deletion becomes safe.

### Concurrency contract

The mechanism remains safe for independent targets and does not introduce a global or per-path mutex.
Unique temporary artifacts prevent attempts from sharing an intermediate file.

For concurrent calls targeting the same path, every successful replacement installs one complete caller payload and the final target equals one successful payload.
The mechanism promises no semantic ordering, fairness, compare-and-swap behavior, or relationship between call completion order and final content.

Runtime and frontend stores continue to assign one semantic writer per managed target.
A process-local lock would neither prevent another process from racing nor identify which semantic revision should win; it would conceal an ownership bug without solving the persistence problem.

### Deterministic platform-operation seam

Factor the state machine from direct system calls behind a private, platform-specific operations interface used only by the utility implementation and tests.
The production implementation delegates to POSIX or Windows APIs.
A scripted test implementation controls each result without becoming part of the public Aobus API.

The seam can inject:

- parent creation and temporary creation failure;
- permission or security-descriptor failure;
- short writes, interrupted writes, and zero progress;
- temporary data-barrier failure;
- handle close failure;
- replacement failure;
- post-replacement namespace-barrier failure;
- cleanup close and removal failure.

Tests assert state transitions and target effects, not only returned messages.
System-call messages remain diagnostic and are not used as machine-readable outcomes.

### Current-spec transition

Until implementation is complete, the [atomic replacement specification](../spec/persistence/atomic-replacement.md) remains the authority for current behavior, including best-effort cleanup, ignored non-Windows namespace-barrier failure, and advisory Windows permissions.
Implementation changes update that specification phase by phase; this RFC never silently upgrades current guarantees.

## Alternatives

### Keep `Result<>` and log namespace-barrier failure

This preserves the API but gives semantic owners no reliable acknowledgement or retry input.
Logs cannot tell a caller whether its revision may be considered settled.

### Return an ordinary error after replacement

This makes “error” mean both not applied and already applied.
Callers could overwrite a newer snapshot, duplicate reports, or claim the old target survived.

### Treat namespace-barrier failure as unconditional success

This is the current non-Windows behavior.
It is acceptable only for callers that explicitly choose best-effort persistence, and the lower mechanism cannot safely make that policy for every payload.

### Require every namespace barrier and fail before returning

The replacement point cannot be undone reliably after `rename` or `MoveFileExW` succeeds.
Reporting a hard error would still be an applied error, while attempting rollback would create a second race and require a journal.

### Add a global or per-path writer lock

A process-local lock does not cover other processes and has no semantic revision knowledge.
A cross-process lock adds stale-lock recovery and still cannot decide which application state should win.
One writer belongs at the store or domain owner.

### Preserve the old target's metadata

Copying mode, ACLs, ownership, extended attributes, and platform metadata creates a large, payload-dependent policy surface and can preserve accidentally broad access.
Explicit new-file security policies are smaller and reviewable.

### Move managed state into a database or journal

A transactional store can provide stronger multi-record recovery but is disproportionate for small complete-file preferences and snapshots.
Payloads that need that guarantee should select a different storage mechanism rather than expanding this utility invisibly.

## Compatibility and migration

The repository has no source-compatibility requirement for this internal API.
Persisted payload schemas and file locations do not change, but newly written file security may become stricter on Windows and may differ for the explicit `PlatformDefault` policy.

Implementation proceeds in reviewable phases:

1. Add the private platform-operation seam and deterministic tests around the current state machine.
2. Introduce `TemporaryArtifact` ownership and close the permission-failure and close-failure cleanup gaps without changing the public result.
3. Introduce `AtomicFileSecurityPolicy`, migrate production callers to `PrivateUser`, and add native permission/ACL tests.
4. Change `writeAtomically` to return `AtomicReplacementReceipt` and expose namespace-barrier completion.
5. Propagate the receipt through the current `ConfigStore` boundary or RFC 0015's document commit, plus the shell component-state store.
6. Give every semantic caller an explicit acknowledgement, retry, and reporting policy.
7. Remove legacy permission vocabulary and any message-based durability inference.

Each phase keeps the current specification accurate.
There is no eager rewrite of existing files solely to change their access policy; the next successful semantic save replaces them under the selected policy.

## Validation

- Scripted operation tests reach every pre-replacement failure and prove that `Error` never crosses the replacement point.
- Namespace-barrier failure after replacement returns `VisibilityOnly` with the complete new target installed.
- Completed platform sequences return `PlatformBarrierCompleted` without claiming stronger device durability.
- Short writes complete, interrupted POSIX writes retry, and zero-progress writes fail without replacing the target.
- Data-barrier, close, and replacement failures preserve the previous target and invoke cleanup exactly once through the artifact owner.
- Permission/security failure leaves no temporary artifact when removal succeeds; an injected removal failure preserves structured cleanup evidence.
- Empty and embedded-null payloads round-trip exactly.
- A concurrent-reader stress test observes only the complete old payload or complete new payload, never an intentional partial target.
- Same-target writer tests accept any complete successful payload as the winner and reject partial or mixed content assumptions.
- Different-target concurrent writers retain independent results and temporary ownership.
- POSIX tests verify `PrivateUser` mode and the documented `PlatformDefault` creation policy.
- Native Windows tests verify extended paths, the `PrivateUser` DACL, inherited `PlatformDefault`, replacement, and cleanup.
- `ConfigStore` and shell state-store tests prove receipt propagation without parsing error messages.
- Semantic persistence tests prove that critical state remains dirty after `VisibilityOnly`, retries only the latest revision, and acknowledges the matching revision after barrier completion.
- Reporting tests prove the selected disposition for best-effort and critical payloads when RFC 0013 is implemented.
- The completed implementation passes native Linux and Windows test matrices, `./ao check`, and the documentation gate.

## Open questions

- Should filesystems whose replacement RPC can report an indeterminate completion be rejected for managed state, or should the outcome vocabulary add `ReplacementIndeterminate` instead of treating replacement-call failure as not applied?
- Which existing managed-state payloads are durability-critical enough to retain dirty state after `VisibilityOnly`, and which explicitly accept best-effort visibility?
- Does the Windows `PrivateUser` DACL require any application-service principal beyond the current user and `SYSTEM` in supported deployment models?
- Should the mechanism itself be `noexcept`, or should a narrower internal no-throw boundary guarantee classification across the replacement point while preserving allocation-failure behavior before it?
- Is platform barrier completion sufficient for playback-session recovery, or should that payload adopt a separate journal or generation backup?

## Promotion plan

If accepted, update the [persistence and managed-state architecture](../architecture/persistence-and-managed-state.md) with receipt propagation, semantic acknowledgement ownership, and the platform-security boundary.
Update the [atomic replacement specification](../spec/persistence/atomic-replacement.md) after each implemented phase with the exact result state machine, barrier mapping, artifact lifecycle, concurrency contract, and security policies.
Update the [grouped configuration store specification](../spec/persistence/config-store.md) with receipt propagation and flush acknowledgement behavior.

Update the [application managed-state surface](../reference/persistence/application-config.md) and [managed file locations reference](../reference/persistence/location.md) only where implemented security, artifact naming, or operational cleanup becomes externally useful reference data.
Add semantic acknowledgement and reporting matrices to the owning playback, [workspace session](../spec/workspace/session.md), presentation, and frontend-store specifications rather than centralizing those decisions in the Core utility.
Update the [interactive session lifecycle architecture](../architecture/interactive-session-lifecycle.md) and [GTK active-library lifecycle specification](../spec/linux-gtk/active-library-lifecycle.md) only if receipt handling changes final-checkpoint, switch, or shutdown ownership.

Coordinate RFC 0005's persistence writer and acknowledgement, RFC 0010's versioned-state writes, RFC 0013's reporting dispositions, and RFC 0015's document transaction when those proposals are implemented together.
Record an ADR if the accepted receipt vocabulary, Windows private-file ACL, or decision not to serialize same-target writers represents a durable choice with credible alternatives.
