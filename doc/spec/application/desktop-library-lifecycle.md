---
id: application.desktop-library-lifecycle
type: spec
status: current
domain: application
summary: Defines the shared GTK and WinUI library selection, successor-process handoff, durable-root admission, and detached-launch contract.
---
# Desktop library lifecycle

## Scope

This specification owns the behavior shared by the GTK and WinUI desktop
frontends when they select a startup library, reuse the active root, or replace
the active library through a successor process. It covers root identity,
startup planning, the private successor request, durable-root and playback
admission, parent preparation, graph release, and detached launch.

It does not own GTK application registration, Windows dispatcher and XAML
lifetime, native picker behavior, platform state schemas, workspace payloads,
or playback-session serialization. Those remain with the platform and
subsystem owners linked below.

## Code boundary

The shared pure rules and detached-launch mechanism form `ao_desktop_launch`, an
application target under the [system architecture](../../architecture/system-overview.md)
and [interactive session lifecycle architecture](../../architecture/interactive-session-lifecycle.md).
Its public surface is under `app/include/ao/desktop/` and its implementation is
under `app/desktop/`.

GTK and WinUI depend on this target and supply composition-root effects. The
target does not depend on `AppRuntime`, UIModel, toolkit types, native picker
types, state stores, or frontend callbacks. TUI and CLI do not consume this
desktop lifecycle.

## Terminology

- **Successor request**: one normalized absolute library root plus optional
  post-open scan intent.
- **Ordinary startup**: a process invocation without the private successor
  request.
- **Successor startup**: a process invocation carrying the complete private
  request defined by the
  [desktop successor protocol reference](../../reference/application/desktop-successor-protocol.md).
- **Durable root**: the active library root successfully committed by the
  frontend's application-state owner.
- **Parent preparation**: an ordinary state checkpoint followed by terminal
  playback-session retirement while the old graph is still usable.
- **Terminal playback retirement**: physical removal of persisted listening
  intent followed by the runtime's permanent no-rearm persistence state.

## Invariants

- One desktop process owns at most one library-bound runtime graph. A different
  root is never installed into a live runtime.
- Startup and requested-root selection require an accessible directory.
  Returned roots and encoded successor roots are absolute and lexically
  normalized; a previously active path may disappear without blocking a switch
  to another valid directory.
- Same-root detection prefers filesystem identity, so aliases such as symlinks
  reuse the active graph. A normalized lexical comparison is the fallback when
  identity inspection fails.
- The parent neither constructs the target graph nor persists the requested
  root.
- A different-root request is admitted once by the frontend and destructive
  work is deferred until the picker callback or coroutine has returned.
- Parent preparation completes before graph release. If terminal playback
  retirement fails, the old graph remains active, no successor is launched,
  and the request may be retried.
- After preparation succeeds, every old library-bound observer, runtime, store,
  and writer is released before the successor process is created. Process
  creation has no rollback to the released graph.
- Successor startup treats its explicit root strictly and never substitutes the
  persisted or empty fallback root.
- Ordinary startup restores playback intent. Successor startup begins with
  playback persistence dormant and playback idle until the selected root is
  durable.
- The selected-root commit is candidate based: failure does not mutate the live
  root snapshot. Success admits playback observation; failure permanently seals
  playback writes for that process and cannot associate target-library playback
  identities with the prior durable root.
- The detached launcher receives UTF-8 arguments. Its native default does not
  inherit unrelated handles; GTK opts into parent standard streams explicitly.

## State model

The shared layer retains values, not a process-lifetime owner:

- `LibrarySwitchRequest` carries one normalized root and scan intent;
- `LibrarySwitchPlan` selects `ReuseActive` or `Restart`;
- `LibraryStartupPlan` selects `ExplicitSuccessor`, `Persisted`, or
  `EmptyLibraryFallback`, plus playback startup admission; and
- `DetachedProcessLaunch` carries the exact executable, UTF-8 arguments,
  optional environment, and standard-stream policy.

Frontend composition roots retain their toolkit-specific queued/running/exiting
phases and perform checkpoints, dialogs, graph release, and process exit.
`PlaybackSessionPersistence` inside `AppRuntime` retains the authoritative
`Dormant`, `Observing`, `WriteSealed`, `Retired`, and `Shutdown` lifecycle.

There is deliberately no shared `LibraryProcessSwitch` stateful service or
teardown token. A GTK main-loop return proves destruction of its scoped graph;
a WinUI dispatcher turn explicitly releases its window/session owner while the
XAML application remains alive. A common token could prove neither condition.
There is also no second persistence-gate owner above `AppRuntime`: frontends
decide when the selected root is durable, while runtime persistence alone owns
write sealing and terminal retirement.

## Commands and transitions

### Plan startup

The planner is side-effect free.

1. A successor request has first priority. Its root must still be an accessible
   directory. The plan is strict, carries scan intent, marks the root for a
   later commit, and selects `AwaitDurableRoot`.
2. Otherwise, an existing persisted directory is selected with `Restore`.
3. Otherwise, the normalized frontend-supplied empty-library path is selected
   with `Restore`. The frontend creates that directory after planning.

Database detection, store construction, runtime construction, playback restore,
and scanning occur after this plan and remain composition effects.

### Plan an Open Library request

The active path must normalize and the requested path must resolve to an
accessible directory. The planner compares filesystem identity when both still
exist and otherwise uses normalized lexical identity. A same-root plan
returns `ReuseActive` and preserves scan intent for the active frontend. A
different-root plan returns one `Restart` request.

### Prepare and release the parent

The frontend first checkpoints eligible state, then calls runtime terminal
playback retirement. Retirement failure reports against the still-active
window and stops the transition. Success closes new callback admission and
releases the old graph in the platform's owned teardown order.

GTK completes its main-loop and scoped-composition unwind before launch. WinUI
releases `LibraryWindowSession` on its dispatcher turn before launch. These are
distinct lifetime proofs and remain platform owned.

### Launch and activate the successor

After release, the parent encodes the private request and launches the exact
current executable through the shared Boost.Process V2 adapter. The parent
then exits regardless of process-creation success.

The successor parses the request before constructing its frontend application,
opens only that root, activates one usable graph with idle playback, and then
commits a settings candidate containing the selected root. Commit success
starts playback persistence. Commit failure retains the previous live and
durable root snapshot, seals playback writes, and leaves unrelated settings and
workspace persistence available.

The successor starts a scan when the request carries scan intent or the
canonical database did not exist, according to the consuming frontend's scan
policy.

## Failure and cancellation

Empty, relative, inaccessible, or non-directory roots return typed errors
before destructive retirement. Malformed private arguments fail startup.
Failure to create an empty fallback directory, open storage, acquire a writer,
construct a runtime, or activate a window is reported by the process that owns
that step.

Parent preparation is the last recoverable boundary that retains the old graph.
After release, launch and successor-startup failures do not reconstruct it.
Detached-launch errors identify process creation as `InitFailed`.

The shared Windows launcher performs the documented Windows argv escaping
before passing a raw command line to Boost.Process V2. This preserves empty,
quoted, whitespace-containing, Unicode, and trailing-backslash arguments.
Boost.Process creates the process with handle inheritance disabled unless a
caller explicitly supplies an inheriting initializer; the shared native
default supplies none.

## Persistence and versioning

The private process protocol has no durable compatibility promise and is
defined exactly by its [reference](../../reference/application/desktop-successor-protocol.md).
The request exists only across one parent-to-successor handoff.

GTK commits its root through the application session store. WinUI commits a
copy of its desktop settings and replaces the live snapshot only after the
atomic candidate save succeeds. Both inject an application-global playback
store into `AppRuntime`; playback payload and lifecycle semantics belong to the
[playback session persistence specification](../playback/session-persistence.md).

## Frontend observations

A same-root request presents or retains the active window and may request the
carried scan. A rejected request leaves the active graph unchanged. A terminal
retirement failure is presented by the old window. After successful release,
launch failure is presented by the retiring parent when the platform can still
surface diagnostics; target startup failure is presented by the successor.

GTK alone owns GApplication replacement and activation tokens. WinUI alone owns
dispatcher phase admission, native application exit, and XAML diagnostics.

## Implementation map

- [`LibraryPath.h`](../../../app/include/ao/desktop/LibraryPath.h),
  [`LibrarySwitch.h`](../../../app/include/ao/desktop/LibrarySwitch.h), and
  [`LibraryStartupPlanner.h`](../../../app/include/ao/desktop/LibraryStartupPlanner.h)
  own root and startup planning.
- [`LibrarySuccessorProtocol.h`](../../../app/include/ao/desktop/LibrarySuccessorProtocol.h)
  owns private request parsing and encoding.
- [`DetachedProcessLauncher.h`](../../../app/include/ao/desktop/DetachedProcessLauncher.h)
  owns detached process policy; sources under
  [`app/desktop/`](../../../app/desktop/) implement the target.
- GTK [`main.cpp`](../../../app/linux-gtk/main.cpp) and
  [`SuccessorProcessLauncher.cpp`](../../../app/linux-gtk/platform/SuccessorProcessLauncher.cpp)
  adapt GTK registration, teardown, environment, standard streams, and
  activation.
- WinUI [`App.xaml.cpp`](../../../app/windows-winui/App.xaml.cpp),
  [`LibrarySession.cpp`](../../../app/windows-winui/app/LibrarySession.cpp), and
  [`ProcessLauncher.cpp`](../../../app/windows-winui/platform/ProcessLauncher.cpp)
  adapt dispatcher phases, state candidates, teardown, executable discovery,
  and exit.

## Test map

- Tests under [`test/unit/desktop/`](../../../test/unit/desktop/) protect the
  protocol, pure startup and switch rules, symlink identity, detach, Windows
  argv round trips, and the no-unrelated-handle-inheritance default on both
  host gates.
- [`GtkStartupPlanTest.cpp`](../../../test/unit/linux-gtk/app/GtkStartupPlanTest.cpp)
  and [`SuccessorProcessLauncherTest.cpp`](../../../test/unit/linux-gtk/platform/SuccessorProcessLauncherTest.cpp)
  protect GTK argument partitioning, environment, executable discovery, and
  activation adaptation.
- [`DestructiveLibraryRestartTest.cpp`](../../../test/unit/winui/app/DestructiveLibraryRestartTest.cpp)
  and [`SelectedRootCommitTest.cpp`](../../../test/unit/winui/app/SelectedRootCommitTest.cpp)
  protect WinUI preparation failure, release-before-launch ordering, and
  candidate root mutation.
- [`PlaybackSessionTest.cpp`](../../../test/unit/runtime/PlaybackSessionTest.cpp)
  and GTK [`MainWindowTest.cpp`](../../../test/unit/linux-gtk/app/MainWindowTest.cpp)
  protect the runtime write seal, terminal retirement, and durable-root
  admission behavior.

## Related documents

- [Interactive session lifecycle architecture](../../architecture/interactive-session-lifecycle.md)
- [GTK active-library lifecycle](../linux-gtk/active-library-lifecycle.md)
- [Windows desktop shell](../shell/windows-desktop.md)
- [Playback session persistence](../playback/session-persistence.md)
- [Desktop successor protocol reference](../../reference/application/desktop-successor-protocol.md)
