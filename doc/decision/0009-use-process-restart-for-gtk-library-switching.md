---
id: decision.0009.gtk-process-library-restart
type: decision
status: accepted
domain: application-shell
summary: Uses a destructive successor-process restart for GTK library switching and GApplication replacement only for bus-name takeover.
---
# Decision 0009: use process restart for GTK library switching

## Context

GTK library switching originally prepared a second `MainWindow` and
`AppRuntime` while the active pair remained live. That preserved rollback when
target construction failed, but required a candidate transaction spanning two
complete library graphs, GTK callback admission, window membership, playback
persistence, MPRIS, and exact observer-before-runtime destruction.

WinUI already uses a destructive successor process for the same product
operation. GTK can accept the same user-visible contract: opening a different
library stops playback, replaces the desktop process and window, and treats a
target-open failure as a new startup failure rather than reconstructing the old
live graph.

GTK also has a fixed `GApplication` ID. Merely waiting for the parent
`Gtk::Application` object to be destroyed does not order the D-Bus daemon's
name release before a successor's registration. A successor can otherwise be
classified as a remote invocation and disappear while activating the exiting
parent.

## Decision

One GTK process owns at most one active `MainWindow` and one window-owned
`AppRuntime`. Selecting the same normalized root reuses and presents that pair.
Selecting a different valid root schedules destructive work through the GTK
main context so the native chooser callback returns before its owning graph is
retired.

The parent checkpoints the active session, terminally retires playback-session
persistence, records an in-memory restart request, and quits the application.
Terminal retirement deletes the restorable playback payload and permanently
rejects later playback saves in that process. The callback scope and pending
idle source are closed before the window; the window, MPRIS bridge, runtime,
audio producers, workers, stores, style runtime, and `Gtk::Application` then
finish their normal destruction. Process exit is not used as a substitute for
that teardown.

Only after the GTK composition function has completely unwound does `main()`
remove its process signal sources and directly use Boost.Process V2 to launch
the exact current executable. Standard streams are inherited and the child is
detached after successful creation. An available new desktop activation token
is forwarded; any inherited token is first removed. Launch failure cancels that
activation token, presents a native diagnostic through a fresh non-unique
diagnostic application, and exits without reconstructing the retired graph.

Every Aobus GTK application uses
`Gio::Application::Flags::ALLOW_REPLACEMENT`. A restart invocation contains a
paired private `--aobus-successor` marker and
`--library-root <absolute-path>`; Aobus validates and consumes that pair before
GTK receives its argument vector. A valid successor uses `REPLACE` in addition
to `ALLOW_REPLACEMENT`, so it can take the fixed application ID even if the
D-Bus daemon still considers the old parent's connection its owner. The
standard `--gapplication-replace` option remains GTK-owned passthrough syntax;
it is not Aobus successor authorization and does not select strict-root or idle
startup behavior. An ordinary invocation with neither replacement mechanism
remains a remote activation while a primary owns the name.

The successor treats its explicit root strictly. It never falls back to the
empty bootstrap root when that target cannot be opened. It activates one
window with idle playback while playback persistence remains pending, then
persists the selected root best effort. Only a successful root commit admits
playback observation and later playback checkpoints. A failed commit keeps the
prior durable root, permanently seals playback writes, and excludes both the
selected root and playback from later window checkpoints. Ordinary window
geometry, output selection, column layout, and workspace saves continue, so the
failure loses resume intent rather than pairing new-library playback with the
old root. The successor then starts the requested bootstrap scan. A recoverable
typed startup failure presents its own native diagnostic and exits; unexpected
exceptions and invariant violations retain their fatal process-root handling.
Because the parent did not persist the requested root, a later ordinary launch
can still select the previous durable root.

This decision changes GTK only. WinUI retains its native launcher and startup
implementation, and TUI retains its single-runtime process lifecycle.

## Alternatives considered

### Keep in-process candidate replacement

Rejected because rollback requires two complete library-bound graphs and a
transaction over callback, persistence, adapter, window, and runtime ownership.
The complexity is intentional under the old rollback contract, but that
contract is no longer required for a different-root request.

### Spawn after local application destruction and wait or retry for name release

Rejected as the primary protocol because local destruction does not acknowledge
that the D-Bus daemon has processed the connection close. Polling or bounded
retry adds timing policy to every switch. `ALLOW_REPLACEMENT` and `REPLACE` are
the platform mechanism that removes this ordering precondition.

### Insert a temporary fixed-ID GApplication lease before launch

Rejected because a second registration does not create a continuous ownership
lease. There is a name-free interval before that registration and another after
it unregisters but before the successor registers. Successful child creation
also does not acknowledge successor registration. An independently launched
ordinary instance may become primary in either interval; the successor can then
replace it, but name-lost delivery does not wait for that displaced graph to
finish teardown. The extra application therefore adds transitions without
strengthening the parent-versus-successor lifetime proof.

### Coordinate name handoff with an inherited pipe

Rejected because pipe closure is not ordered with D-Bus name release and
therefore cannot prove the desired condition. A readiness handshake would also
retain two graphs concurrently if used to preserve rollback.

### Make all GTK instances non-unique

Rejected because it would turn a lifecycle implementation detail into a
multi-instance product mode with concurrent global-config writers, audio and
MPRIS ownership, and undefined external-activation behavior.

### Share the WinUI launcher immediately

Rejected for the initial change. WinUI already has tested Windows quoting and
handle-inheritance behavior. A cross-platform wrapper would enlarge the change
without reducing GTK's platform-specific GApplication and activation-token
work.

## Consequences

- A supported GTK library switch never contains old and target `AppRuntime`
  graphs in one process, and the original switch parent's library composition
  is fully torn down before the process launches its successor.
- Candidate construction, rollback callbacks, active-slot replacement, and
  their ordering tests are removed.
- A different-root switch stops playback and changes PID, window, focus, MPRIS,
  and audio identity. Cold-start latency is visible.
- Playback-discard failure remains recoverable before retirement: the parent
  presents the error and stays active. After terminal retirement succeeds,
  launch or target-startup failure has no old-window rollback.
- The requested root becomes durable only after successor activation. A
  best-effort persistence failure leaves the active successor usable and leaves
  the prior root as the next ordinary-startup choice; later checkpoints exclude
  the selected root and playback while retaining ordinary window, output,
  layout, and workspace saves.
- `Boost::process` becomes a real linked dependency of the GTK application,
  including its compiled support library.
- `ALLOW_REPLACEMENT` is permanent primary-instance policy. Another same-user
  process that independently starts during the name-free interval may become
  primary, and a process that deliberately requests GApplication replacement
  can make the current primary quit. The successor's later `REPLACE` request
  does not wait for either displaced graph to finish teardown, so the fixed-ID
  protocol does not guarantee serialization against independently launched
  instances.
- Standard streams and the controlling terminal remain inherited. A detached
  successor may still receive terminal-session signals such as `SIGHUP`.
- Exact executable discovery must account for packaged execution; an AppImage
  launcher path takes precedence over a procfs self-executable path.
- Reversing this decision requires restoring an in-process transaction or
  defining a separate brokered multi-process ownership protocol.

## Current authorities

- [Interactive session lifecycle architecture](../architecture/interactive-session-lifecycle.md)
- [GTK active-library lifecycle specification](../spec/linux-gtk/active-library-lifecycle.md)
- [Runtime execution architecture](../architecture/runtime-execution.md)
- [Playback architecture](../architecture/playback.md)
- [Persistence and managed-state architecture](../architecture/persistence-and-managed-state.md)
- [Decision 0005: use process restart for WinUI library switching](0005-use-process-restart-for-winui-library-switching.md)

## Supersession

Not superseded.
