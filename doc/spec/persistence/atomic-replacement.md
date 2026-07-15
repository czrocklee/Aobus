---
id: persistence.atomic-replacement
type: spec
status: current
domain: persistence
summary: Defines private same-directory temporary writing, synchronization, atomic replacement, cleanup, and platform limits.
---
# Atomic file replacement

## Scope

This specification owns the current observable behavior of `ao::utility::writeAtomically`: parent-directory creation, private same-directory temporary files, complete synchronous writes, data synchronization, target replacement, recoverable failure, cleanup, and platform differences.

It does not own serialization, payload validation, path selection, semantic transactions, dirty-state acknowledgement, save scheduling, retries, reporting, backup generations, or recovery of corrupt targets and abandoned temporary files.
Those policies remain with the store or semantic owner above this Core mechanism.

The word “atomic” refers to replacing the target path with one complete temporary file through the operating-system replacement primitive.
It does not mean that several files or callers form a transaction, that competing writers are serialized, or that every successful call survives sudden power loss on every filesystem.

## Code boundary

The [system architecture](../../architecture/system-overview.md) places reusable filesystem mechanisms in Core libraries.
The [persistence and managed-state architecture](../../architecture/persistence-and-managed-state.md) places atomic replacement below runtime, UIModel, frontend stores, schemas, and lifecycle owners.

The public boundary is [`include/ao/utility/AtomicFile.h`](../../../include/ao/utility/AtomicFile.h) in `ao_utility`:

```cpp
Result<> writeAtomically(std::filesystem::path const& targetPath, std::string_view data);
```

[`lib/utility/CMakeLists.txt`](../../../lib/utility/CMakeLists.txt) selects [`AtomicFile.cpp`](../../../lib/utility/AtomicFile.cpp) for non-Windows builds and [`AtomicFileWindows.cpp`](../../../lib/utility/AtomicFileWindows.cpp) for Windows builds.
[`AtomicFileTransaction.h`](../../../lib/utility/AtomicFileTransaction.h) contains the source-private common state machine and its deterministic test seam.
None of these mechanisms can depend on application runtime, UIModel, a frontend, YAML, or an application payload type.

## Terminology

- The **target** is the caller-supplied path whose complete contents are to be created or replaced.
- The **parent** is the target's containing directory.
- The **temporary file** is a newly and exclusively created private file in that same parent.
- The **replacement point** is the POSIX `rename` or Windows `MoveFileExW` call that makes the temporary file the target.
- **Atomic visibility** means the helper does not intentionally expose a partially written target: before the replacement point readers address the previous target or absence, and after it they address the replacement.
- A **data barrier** synchronizes the temporary file's contents before replacement.
- A **namespace barrier** attempts to make the replaced directory entry survive a crash.
- **Cleanup** is the no-throw, best-effort close and removal of an uncommitted temporary file.

## Invariants

- The operation is synchronous and does not retain the target path or input `string_view` after return.
- Input is an opaque byte sequence; no text encoding, terminator, YAML rule, or schema is applied.
- The temporary file is created in the target's parent, so the helper never intentionally performs a cross-filesystem replacement.
- The temporary file is private to the current user before any payload byte is written.
- Every input byte is written and the platform data barrier succeeds before the replacement point is attempted.
- Before the replacement point, the helper does not truncate or write through the target path.
- Every returned `IoError` occurs before the replacement point; the helper has not deliberately changed the target through that attempt.
- A successful return means the mandatory pre-replacement sequence and the operating-system replacement call reported success.
- Success is not a receipt for absolute power-loss durability or for a separately observed parent-directory barrier.
- Every created temporary file has one RAII owner until replacement succeeds.
- Parent directories created before a later failure are not rolled back.
- Cleanup is attempted on every ordinary pre-replacement return after temporary creation, but removal failure and process termination can still leave an artifact.
- Diagnostic messages are not a machine-readable contract.
- The helper has no lock, compare-and-swap token, revision, merge, recovery state, or same-target writer coordination.
- Replacing an existing file installs a new file object; existing permissions and other target metadata are not preserved.

## State model

The implementation advances through one linear attempt:

| State | Filesystem effect | Target visibility |
|---|---|---|
| Initial | No helper-created path. | Existing target or absence. |
| Parent ready | Missing parent directories may have been created. | Existing target or absence. |
| Temporary owned | A unique private same-directory path and handle have one RAII owner. | Existing target or absence. |
| Data written | Every input byte has been handed to the temporary handle. | Existing target or absence. |
| Data synchronized | The platform data barrier has succeeded. | Existing target or absence. |
| Temporary closed | The temporary handle has closed successfully. | Existing target or absence. |
| Replaced | The operating-system replacement call has succeeded and temporary ownership is committed. | Complete new target. |
| Returned success | Platform post-replacement best effort has finished. | Complete new target. |

Any ordinary failure before Replaced returns `IoError` and lets the temporary owner attempt cleanup.
There is no reported-failure transition after Replaced: non-Windows parent-directory synchronization is best effort, and Windows has no separate post-replacement operation.

## Commands and transitions

### Common operation

`writeAtomically(targetPath, data)` performs these steps:

1. Resolve the target form required by the platform.
2. Create missing parent directories.
3. Exclusively create a private temporary file in the parent and transfer it to one RAII owner.
4. Write the complete `data` range.
5. Apply the required temporary-file data barrier.
6. Close the temporary handle.
7. Replace the target through one platform call and commit the temporary owner.
8. Perform the platform's post-replacement best effort and return success.

An empty input installs a zero-length target.
Embedded null bytes are ordinary bytes because both implementations use the explicit `string_view` size.
An absent target is created on success, an existing regular file is replaced, and a directory target returns `IoError` when the platform rejects replacement.

The public API has no permission selector.
All current managed-state callers require private files, and exposing a platform-default option would weaken that invariant without an in-tree consumer.

### Non-Windows implementation

The non-Windows implementation uses this sequence:

1. `std::filesystem::create_directories` prepares the parent.
2. `mkstemp` creates a `.temp.XXXXXX` file in that parent and opens it exclusively.
3. `fchmod` establishes mode `0600` before payload writing.
4. `write` loops until all bytes are written, retries `EINTR`, and rejects zero progress.
5. `fsync` is the required temporary-file data barrier.
6. `close` is required before replacement; ownership is released before the call so an ambiguous close failure cannot close a later reused descriptor.
7. POSIX `rename` replaces the target.
8. The parent is opened as a directory and `fsync` is attempted as a best-effort namespace barrier; open, synchronization, and close outcomes are not returned.

Failure of the temporary-file barrier prevents replacement.
Failure of the parent-directory barrier cannot become an ordinary error because the replacement is already visible and `Result<>` errors mean not applied.

### Windows implementation

The Windows implementation converts the target to an absolute extended-length path, including the extended UNC form when applicable.
It then uses this sequence:

1. `std::filesystem::create_directories` prepares the extended-length parent path.
2. The process token supplies the current-user SID, and an SDDL security descriptor creates a protected DACL with full control entries for only that user and Local System.
3. `CreateFileW` with `CREATE_NEW`, no sharing, and those creation-time security attributes creates `.ao.tmp.<process>.<tick>.<counter>` in the parent.
4. Name collisions retry through a process-local atomic counter and stop with `IoError` after 128 candidates.
5. `WriteFile` writes chunks no larger than `0x7ffff000` bytes until complete and rejects zero progress.
6. `FlushFileBuffers` is the required temporary-file data barrier.
7. `CloseHandle` is required before replacement.
8. `MoveFileExW` uses `MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH` to replace the target.

Windows performs no separate directory-handle flush after `MoveFileExW`.
The contract records successful completion of the documented API sequence and does not infer stronger filesystem behavior from the flag name.

### Private-file security

The installed file is the secured temporary file, not the old target object:

| Platform | Enforced creation policy |
|---|---|
| POSIX-like | Effective owner read/write only through mode `0600`. |
| Windows | A protected DACL grants full control to the process-token user and Local System; broad inherited allow entries are excluded. |

The Windows policy assumes the supported application runs under its interactive process identity.
It does not claim protection from administrators, privileged processes, backup operators, inherited auditing or integrity policy, or offline access.
On either platform, replacement deliberately does not preserve a prior target's mode, DACL, ownership metadata, extended attributes, or alternate streams.

### Concurrent writers

Unique temporary creation prevents attempts from sharing one intermediate file.
Calls for different targets otherwise operate independently.

Calls for the same target are not serialized.
Each successful replacement installs one complete caller payload, and a later successful replacement determines the final contents.
There is no semantic ordering, fairness, lost-update detection, compare-and-swap behavior, or promised relationship between call completion order and final revision.
Runtime and frontend stores must assign one semantic writer per managed target.

## Failure and cancellation

| Failure point | Result | Existing target before replacement |
|---|---|---|
| Windows target normalization | `IoError`. | Unchanged. |
| Parent-directory creation | `IoError`. | Unchanged; some new ancestors may remain. |
| Private security preparation, unique temporary creation, or collision exhaustion | `IoError`. | Unchanged. |
| POSIX permission application | `IoError`; RAII cleanup attempted. | Unchanged. |
| Write, interrupted-write retry exhaustion through another error, or zero progress | `IoError`; RAII cleanup attempted. | Unchanged. |
| `fsync` or `FlushFileBuffers` | `IoError`; RAII cleanup attempted. | Unchanged. |
| Temporary-handle close | `IoError`; path removal attempted. | Unchanged. |
| `rename` or `MoveFileExW` | `IoError`; path removal attempted. | The helper has not deliberately written the target. |
| Non-Windows parent-directory open, `fsync`, or close after replacement | Ignored; operation returns success. | Complete new target is already visible. |

The temporary owner closes any still-owned handle before attempting path removal.
Cleanup deliberately does not replace the primary error with a removal error.
A failed or ambiguous close can prevent removal on Windows, removal itself can fail on either platform, and termination can bypass destructors; therefore absence of abandoned artifacts is not guaranteed.
The helper performs no sibling scan or scavenging because age or name alone does not prove that another process has abandoned a file.

The function is not `noexcept` and does not broadly translate allocation, formatting, or other unrelated C++ exceptions.
All ordinary filesystem work is synchronous and exposes no cooperative cancellation point.

## Persistence and recovery boundary

Atomic visibility and crash durability are distinct.

On non-Windows platforms, successful temporary-file `fsync` precedes `rename`, but the later parent-directory `fsync` is best effort.
On Windows, successful `FlushFileBuffers` precedes a successful `MoveFileExW` request with write-through, with no additional directory barrier.
Filesystem, device, hypervisor, mount, and network-share behavior can weaken or reject those operations.

The helper has no journal, write-ahead log, backup generation, checksum, startup repair, or recovery protocol.
A format owner that needs recovery from an already corrupt target or a power-loss ambiguity must specify that separately rather than infer it from `writeAtomically` success.

## Frontend observations

The mechanism publishes no progress, event, notification, or retained state.
Its caller receives one synchronous `Result<>` and decides whether to acknowledge a snapshot, retain dirty state, retry, log, notify, or present a failure.
No platform barrier receipt crosses into `ConfigStore` or frontend state owners.

## Implementation map

- [`AtomicFile.h`](../../../include/ao/utility/AtomicFile.h) defines the two-argument public operation and private-file contract.
- [`AtomicFileTransaction.h`](../../../lib/utility/AtomicFileTransaction.h) owns the private common state sequence.
- [`AtomicFile.cpp`](../../../lib/utility/AtomicFile.cpp) owns POSIX temporary-file RAII, mode, write, barriers, replacement, and parent synchronization.
- [`AtomicFileWindows.cpp`](../../../lib/utility/AtomicFileWindows.cpp) owns extended paths, protected DACL creation, Windows temporary-file RAII, barriers, and replacement.
- [`lib/utility/CMakeLists.txt`](../../../lib/utility/CMakeLists.txt) selects one platform implementation and links the Windows security API.
- [`ConfigStore.cpp`](../../../app/runtime/ConfigStore.cpp) and [`ShellLayoutComponentStateStore.cpp`](../../../app/linux-gtk/app/ShellLayoutComponentStateStore.cpp) are the current production callers.

## Test map

- [`AtomicFileTest.cpp`](../../../test/unit/utility/AtomicFileTest.cpp) uses the private state-machine seam to inject normalization, parent, creation, partial-write, data-barrier, close, replacement, and cleanup failures; every pre-replacement case preserves the old target.
- Real-filesystem sections protect creation, overwrite, empty and embedded-null payloads, POSIX mode `0600`, unwritable-parent failure, directory-target rejection, and successful temporary cleanup.
- Native Windows sections protect the exact protected DACL, extended-length paths, distinct temporary names under concurrent different-target writes, final contents, and cleanup.
- [`ConfigStoreTest.cpp`](../../../test/unit/runtime/ConfigStoreTest.cpp) proves that malformed YAML loading returns `FormatRejected`, preserves the caller's seeded object, and does not modify or automatically overwrite the original bytes.
- [`ShellLayoutComponentStateStoreTest.cpp`](../../../test/unit/linux-gtk/app/ShellLayoutComponentStateStoreTest.cpp) protects integration through the other production caller.

The private failure seam proves state effects rather than platform error text.
Native platform tests remain necessary because the seam does not emulate filesystem, ACL, or kernel replacement semantics.

## Related documents

- [Persistence and managed-state architecture](../../architecture/persistence-and-managed-state.md)
- [Grouped configuration store specification](config-store.md)
- [Application managed-state surface](../../reference/persistence/application-config.md)
- [Managed file locations reference](../../reference/persistence/location.md)
- [Outcome channel specification](../failure/outcome-channel.md)
- [RFC 0010: versioned presentation state](../../rfc/0010-versioned-presentation-state.md)
- [RFC 0014: observable atomic replacement](../../rfc/0014-observable-atomic-replacement.md), rejected after this narrower hardening closed the verified integrity gaps
- [RFC 0015: fail-closed grouped configuration transactions](../../rfc/0015-fail-closed-config-store.md)
