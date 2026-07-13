---
id: persistence.atomic-replacement
type: spec
status: current
domain: persistence
summary: Defines complete-file temporary writing, synchronization, replacement, permissions, cleanup, and platform failure behavior.
---
# Atomic file replacement

## Scope

This specification owns the current observable behavior of `ao::utility::writeAtomically`: parent-directory creation, same-directory temporary files, complete synchronous writes, data synchronization, target replacement, permission selection, recoverable failures, cleanup, and platform differences.

It does not own serialization, payload validation, path selection, semantic transactions, dirty-state acknowledgement, save scheduling, retries, reporting, schema versions, or recovery of abandoned temporary files.
Those policies remain with the store or semantic owner above this Core mechanism.

The word “atomic” in this contract refers to replacing the target path with one complete temporary file through the operating-system replacement primitive.
It does not mean that several callers form a transaction, that competing writers are serialized, or that every filesystem provides identical crash durability.

## Code boundary

The [system architecture](../../architecture/system-overview.md) places reusable filesystem mechanisms in Core libraries.
The [persistence and managed-state architecture](../../architecture/persistence-and-managed-state.md) places atomic replacement below runtime, UIModel, frontend stores, schemas, and lifecycle owners.

The public boundary is [`include/ao/utility/AtomicFile.h`](../../../include/ao/utility/AtomicFile.h) in `ao_utility`.
[`lib/utility/CMakeLists.txt`](../../../lib/utility/CMakeLists.txt) selects [`AtomicFile.cpp`](../../../lib/utility/AtomicFile.cpp) for non-Windows builds and [`AtomicFileWindows.cpp`](../../../lib/utility/AtomicFileWindows.cpp) for Windows builds.
The mechanism cannot depend on application runtime, UIModel, a frontend, YAML, or an application payload type.

## Terminology

- The **target** is the caller-supplied path whose complete contents are to be created or replaced.
- The **parent** is the target's containing directory.
- The **temporary file** is a newly and exclusively created file in that same parent.
- The **replacement point** is the single POSIX `rename` or Windows `MoveFileExW` call that makes the temporary file the target.
- **Atomic visibility** means the helper does not intentionally expose a partially written target: before the replacement point readers address the previous target or absence, and after it they address the replacement.
- A **data barrier** flushes the temporary file's contents before replacement.
- A **namespace barrier** attempts to make the new directory entry survive a crash after replacement.
- **Cleanup** is the best-effort removal of a temporary path after a reported failure.

## Invariants

- The operation is synchronous and does not retain the target path or input `string_view` after return.
- Input is an opaque byte sequence; no text encoding, terminator, YAML rule, or schema is applied.
- The temporary file is created in the target's parent, so the helper never intentionally performs a cross-filesystem replacement.
- The complete byte sequence is written and a platform data barrier succeeds before the replacement point is attempted.
- Before the replacement point, the helper does not truncate or write through the target path.
- A successful return means the operating-system replacement call reported success; the platform sections define the additional durability work and its limits.
- Every expected filesystem or operating-system failure returned by the helper uses `Error::Code::IoError`.
- Diagnostic messages are not a machine-readable contract.
- The helper has no lock, compare-and-swap token, revision, merge, or same-target writer coordination.
- Replacing an existing file installs a new file object; existing permissions and other target metadata are not generally preserved.
- Parent directories created before a later failure are not rolled back.
- Temporary cleanup is best-effort and is not an invariant after failure or process termination.

## State model

The implementation advances through one linear replacement attempt:

| State | Filesystem effect | Target visibility |
|---|---|---|
| Initial | No helper-created path. | Existing target or absence. |
| Parent ready | Missing parent directories may have been created. | Existing target or absence. |
| Temporary open | A unique same-directory temporary path and handle exist. | Existing target or absence. |
| Data written | Every input byte has been handed to the temporary handle. | Existing target or absence. |
| Data synchronized | The platform data barrier has succeeded. | Existing target or absence. |
| Temporary closed | The temporary handle has been closed successfully. | Existing target or absence. |
| Replaced | The operating-system replacement call has succeeded. | Complete new target. |
| Returned success | Platform post-replacement work, if any, is complete or deliberately best-effort. | Complete new target. |

Any reported failure before Replaced returns without deliberately modifying the existing target.
It may leave created parent directories or a temporary artifact.
There is no reported-failure transition after Replaced in the current implementations: non-Windows namespace-barrier failure is ignored, and Windows returns the result of its replacement call.

## Commands and transitions

### Common operation

`writeAtomically(targetPath, data, permissions)` performs these common steps:

1. Resolve the target form required by the platform.
2. Create missing parent directories.
3. Exclusively create a temporary file in the parent.
4. Write the complete `data` range.
5. Apply a data barrier.
6. Close the temporary handle.
7. Replace the target through one platform call.
8. Perform any platform post-replacement durability attempt and return.

An empty input still creates, synchronizes, and installs a zero-length target.
Embedded null bytes are ordinary input bytes because both implementations use the explicit `string_view` size.

If the target is absent, successful replacement creates it.
If the target is an existing regular file, successful replacement overwrites it as a complete file.
Replacing a directory is not supported and returns `IoError` when the operating system rejects the replacement.

### Non-Windows implementation

The non-Windows implementation uses this sequence:

1. `std::filesystem::create_directories` prepares the parent.
2. `mkstemp` creates a `.temp.XXXXXX` file in that parent and opens it exclusively.
3. The selected permission mode is applied with `fchmod` unless `Default` was selected.
4. `write` loops until all bytes are written and retries an interrupted call when `errno == EINTR`.
5. `fsync` is a required temporary-file data barrier.
6. `close` is required before replacement.
7. POSIX `rename` replaces the target.
8. The parent is opened as a directory and `fsync` is attempted as a best-effort namespace barrier; open, sync, and close outcomes are not returned.

Failure of the temporary-file `fsync` prevents replacement and returns `IoError`.
Failure of the parent-directory barrier does not change a successful result because the replacement is already visible and the implementation chooses not to report an ambiguous “failed but applied” outcome.

### Windows implementation

The Windows implementation first converts the target to an absolute extended-length path, including the extended UNC form when applicable.
It then uses this sequence:

1. `std::filesystem::create_directories` prepares the extended-length parent path.
2. `CreateFileW` with `CREATE_NEW`, write access, and no sharing creates a temporary file named `.ao.tmp.<process>.<tick>.<counter>`.
3. Name collision retries use a process-local atomic counter and stop with `IoError` after 128 unsuccessful candidates.
4. `WriteFile` writes chunks no larger than `0x7ffff000` bytes until complete; a successful zero-byte write before completion is an error.
5. `FlushFileBuffers` is a required temporary-file data barrier.
6. `CloseHandle` is required before replacement.
7. `MoveFileExW` uses `MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH` to replace the target.

Windows performs no separate explicit parent-directory handle flush after `MoveFileExW` returns.
The contract records the API sequence rather than inferring stronger filesystem guarantees from the flag name.

### Permission modes

| Requested mode | Non-Windows result | Windows result |
|---|---|---|
| `OwnerReadWrite` | The temporary file is set to mode `0600`, and that new file becomes the target. | The parameter is advisory and ignored; the new file receives the destination directory's normal ACL inheritance. |
| `Default` | The `mkstemp` mode is retained, currently also `0600`. | Same behavior as `OwnerReadWrite`; the parameter is ignored. |

The function's default argument is `OwnerReadWrite`.
Current production callers pass `OwnerReadWrite` explicitly.

### Concurrent writers

Unique temporary creation prevents two attempts from intentionally sharing one temporary file.
Calls for different targets otherwise operate independently.

Calls for the same target are not serialized.
Each reports its own operating-system result; if multiple replacements succeed, the later successful replacement determines the target contents.
The helper supplies no lost-update detection, stable ordering, fairness, or relationship between return order and semantic revision.

## Failure and cancellation

| Failure point | Result | Existing target before replacement |
|---|---|---|
| Target normalization on Windows | `IoError`. | Unchanged. |
| Parent-directory creation | `IoError`. | Unchanged. |
| Unique temporary creation or collision exhaustion | `IoError`. | Unchanged. |
| Permission application on non-Windows | `IoError`. | Unchanged. |
| Write or zero-progress write | `IoError`. | Unchanged. |
| Temporary-file `fsync` or `FlushFileBuffers` | `IoError`. | Unchanged. |
| Temporary-handle close | `IoError`. | Unchanged. |
| `rename` or `MoveFileExW` replacement | `IoError`. | The helper has not deliberately written the target. |
| Non-Windows parent-directory open or `fsync` after replacement | Ignored; the operation returns success. | New target is already visible. |

After temporary creation, both implementations remove temporary paths on most explicit write, data-barrier, close, and replacement failures, but removal errors are ignored.
The current non-Windows permission-application failure branch closes the file through its handle guard without removing the temporary path.
On Windows, a failed handle close can also make the subsequent best-effort removal ineffective.
Process termination can leave a temporary path on either platform.

These cleanup limitations do not expose a partial target through the target path, but they can accumulate hidden sibling files and require operational cleanup.
The public mechanism has no abandoned-temp discovery or garbage collection.

The function is not `noexcept` and does not broadly translate allocation, formatting, or other unrelated C++ exceptions.
All normal filesystem work is synchronous and exposes no cooperative cancellation point.

## Persistence and versioning

Atomic visibility and crash durability are separate guarantees.

On non-Windows platforms, successful temporary-file `fsync` establishes the required data barrier before `rename`.
The later parent-directory `fsync` is attempted but not checked, so success does not prove that the replacement directory entry survives sudden power loss on every filesystem.

On Windows, successful `FlushFileBuffers` precedes a successful `MoveFileExW` request with `MOVEFILE_WRITE_THROUGH`.
The helper performs no additional directory barrier and promises no behavior beyond successful completion of those APIs.

Filesystem, mount, network-share, and device behavior can weaken or reject the operating-system operations.
The helper does not probe filesystem capabilities or implement a recovery journal.

The byte payload has no format version at this layer.
Schema versioning and migration remain above the replacement mechanism.

## Frontend observations

The mechanism publishes no progress, event, notification, or retained state.
Its caller receives one synchronous `Result<>` and decides whether to acknowledge a snapshot, retain dirty state, retry, log, notify, or present a failure.

## Implementation map

- [`AtomicFile.h`](../../../include/ao/utility/AtomicFile.h) defines the public permission modes and result-returning operation.
- [`AtomicFile.cpp`](../../../lib/utility/AtomicFile.cpp) implements the non-Windows temporary write, barriers, replacement, and cleanup.
- [`AtomicFileWindows.cpp`](../../../lib/utility/AtomicFileWindows.cpp) implements extended paths, Windows temporary names, barriers, replacement, and cleanup.
- [`lib/utility/CMakeLists.txt`](../../../lib/utility/CMakeLists.txt) selects exactly one platform implementation in `ao_utility`.
- [`ConfigStore.cpp`](../../../app/runtime/ConfigStore.cpp) and [`ShellLayoutComponentStateStore.cpp`](../../../app/linux-gtk/app/ShellLayoutComponentStateStore.cpp) are the current production callers.

## Test map

- [`AtomicFileTest.cpp`](../../../test/unit/utility/AtomicFileTest.cpp) protects final content creation, overwrite, non-Windows owner-only permissions, unwritable-parent failure, and directory-target rejection.
- The Windows sections of the same test protect extended-length paths, concurrent unique temporary names for different targets, final contents, and successful temporary cleanup.
- [`ConfigStoreTest.cpp`](../../../test/unit/runtime/ConfigStoreTest.cpp) and [`ShellLayoutComponentStateStoreTest.cpp`](../../../test/unit/linux-gtk/app/ShellLayoutComponentStateStoreTest.cpp) protect integration through the two production callers.

Current tests do not directly prove concurrent-reader atomic visibility, empty or embedded-null payloads, same-target writer races, partial-write retry, interrupted writes, data-barrier failure, close failure, replacement failure cleanup, parent-directory barrier behavior, or the non-Windows permission-failure artifact.
Windows-specific sections require native Windows execution and are not exercised by a Linux test run.

## Related documents

- [Persistence and managed-state architecture](../../architecture/persistence-and-managed-state.md)
- [Grouped configuration store specification](config-store.md)
- [Application managed-state surface](../../reference/persistence/application-config.md)
- [Managed file locations reference](../../reference/persistence/location.md)
- [Outcome channel specification](../failure/outcome-channel.md)
- [RFC 0010: versioned presentation state](../../rfc/0010-versioned-presentation-state.md)
- [RFC 0014: observable atomic replacement](../../rfc/0014-observable-atomic-replacement.md)
