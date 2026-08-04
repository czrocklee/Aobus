---
id: decision.0005.winui-process-library-restart
type: decision
status: accepted
domain: application-shell
summary: Uses a destructive successor-process restart for WinUI library switching instead of overlapping two library-bound graphs in one process.
---
# Decision 0005: use process restart for WinUI library switching

## Context

A WinUI library switch originally prepared a second `LibrarySession`,
`MainWindow`, and `AppRuntime` while the active graph remained live.
That preserved in-process rollback but made two sessions temporarily own stale
`ConfigStore` snapshots for the same Windows settings and playback files.
It also required exact callback, window, native-adapter, asynchronous-task, and
runtime release ordering while a folder-picker coroutine still retained the old
window.

The 2026-08-04 maintainer review accepted a different product contract: WinUI
may receive a new PID, HWND, taskbar window, and SMTC instance when opening a
different library; playback need not cross that boundary; and a failed target
startup need not reconstruct the old live process.
That review and the implementation review introducing this record are the
contemporaneous evidence for the decision.

## Decision

One WinUI process owns exactly one `LibraryWindowSession`, containing one
`MainWindow`, one `LibrarySession`, and one uniquely owned `AppRuntime`.
A different-library request is posted to the application dispatcher so the
folder-picker coroutine returns before teardown begins.
The parent checkpoints state best effort, retires and releases the window,
releases the session, runtime, and application-state stores, and only then
launches the exact current executable with the selected root as a private
startup argument.
The parent exits after the launch attempt whether process creation succeeds or
fails.

The successor validates and opens the explicit root as its only session.
It does not silently replace an invalid explicit root with the empty fallback.
The selected root remains pending during construction and is written to desktop
settings only after the successor window and process-wide adapters are active.
Initial scan begins after that activation boundary.
A successor startup failure presents its own startup diagnostic and exits;
because the parent never persisted the requested root, the next ordinary launch
can still select the prior durable root.

Same-root selection is a no-op.
Modern/Classic and theme generation rebuilds remain within one process and do
not participate in library restart.
Independently launched concurrent WinUI instances are not made a supported
multi-library mode by this decision.

## Alternatives considered

### Keep in-process candidate replacement

Rejected because rollback required overlapping two complete window/session
owners, process-wide adapters, and mutable snapshots of the same application
configuration files.
Unique runtime ownership made safe teardown possible, but did not remove the
cross-generation callback and single-writer coordination burden.

### Use a two-phase parent/child readiness handshake

Rejected because it retained parent and successor graphs concurrently and added
an IPC commit protocol solely to preserve the old live window on successor
failure.
The accepted product behavior does not require that rollback.

### Keep both processes as simultaneous library windows

Rejected because application-global desktop settings, playback persistence,
SMTC, audio ownership, external activation, and process-exit behavior would need
a broker or per-instance partitioning.
That is a separate multi-window product design, not a simpler library switch.

### Exit first and rely on an external launcher

Rejected because a helper executable or operating-system restart registration
would add another deployment and failure owner.
The minimal application bootstrap can release its complete library graph while
remaining alive long enough to call `CreateProcessW` directly.

## Consequences

- No WinUI process contains two live `LibrarySession` or `AppRuntime` graphs for
  a library switch.
- Parent and successor do not concurrently own `windows-settings.yaml` or
  `windows-playback.yaml` on the supported restart path.
- The picker callback cannot synchronously destroy its own window or callable.
- Target open, database, writer-lease, and initial-materialization failures are
  successor startup outcomes rather than parent replacement results.
- Process creation failure is still parent-owned; the retired parent shows a
  native diagnostic and exits without rollback.
- A different-library switch stops playback and replaces PID, HWND, taskbar,
  focus, and SMTC identity.
- Cold-start latency and focus transfer are user-visible and require native
  validation.
- Normal shutdown, shell-generation teardown, asynchronous cancellation, and
  native event revocation remain correctness requirements; process exit is not
  used to excuse leaks or stale callbacks.
- Reversing this decision requires restoring either an in-process replacement
  transaction or an explicit cross-process handoff protocol.

## Current authorities

- [System architecture](../architecture/system-overview.md)
- [Interactive session lifecycle architecture](../architecture/interactive-session-lifecycle.md)
- [Application shell architecture](../architecture/application-shell.md)
- [Persistence and managed-state architecture](../architecture/persistence-and-managed-state.md)
- [Windows desktop shell specification](../spec/shell/windows-desktop.md)
- [Windows desktop state reference](../reference/windows/desktop-state.md)

## Supersession

Not superseded.
