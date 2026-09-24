---
id: architecture.interactive-session-lifecycle
---
# Interactive session lifecycle architecture

This page owns final placement and lifetime of a library-bound `AppRuntime` in
GTK, WinUI, AppKit, and TUI. It explains which objects may borrow the graph,
when restoration and callback admission begin, and how each frontend proves
that callbacks cannot reach retired state.

Cross-desktop root selection and the complete process-restart transaction are
owned by [desktop library lifecycle](desktop-library-lifecycle.md). Exact GTK
adaptation is in
[GTK active-library lifecycle](frontend/gtk/active-library-lifecycle.md), and
exact WinUI adaptation is in the
[Windows desktop shell](frontend/windows.md).

## Interactive runtime composition

`AppRuntime` is the interactive lifetime root. Its pinned implementation owns a
`CoreRuntime` first, then resource delivery, views, playback transport and
succession, workspace state, the owning workspace `ConfigStore`, and playback
session persistence. Core-borrowing members are constructed only after Core has
reached its final implementation address and are destroyed before Core.

`AppRuntime::create()` requires an executor and owning workspace store. It first
opens and validates `CoreRuntime`, materializes initial library state, moves Core
into the final App implementation, and only then constructs interactive
borrowers. The public `AppRuntime` and `CoreRuntime` values are move-only PImpl
handles: moving a factory result transfers the implementation pointer rather
than relocating internal services. A moved-from destructor is inert.

The playback-session store is either the owned workspace store or an explicit
borrowed override. An override must outlive the runtime. `CoreRuntime` is the
smaller graph used by CLI and does not own interactive workspace/playback
lifecycle.

A frontend must place the returned value in its final session storage before it
registers providers, restores state, subscribes observers, or publishes any
borrow. This rule is independent of the PImpl's stable internal address: it
keeps composition-root ownership and destruction order explicit.

## Shared placement and teardown shape

```text
select and validate paths
  -> create callback executor and stores
  -> create AppRuntime factory value
  -> move once into final frontend storage
  -> register providers
  -> restore workspace and playback according to startup mode
  -> construct UIModel/frontend borrowers and callbacks
  -> run
  -> checkpoint or terminally retire persistence while state and stores exist
  -> retire observers and callback admission
  -> stop and join producers
  -> destroy runtime graph
  -> release stores, executor dependencies, and logger
```

One graph is bound to one music root and database for its whole lifetime. A
frontend never retargets a live `MusicLibrary`. Desktop replacement uses the
transaction in [desktop library lifecycle](desktop-library-lifecycle.md); this
page owns only the placement and teardown proofs on either side of that
transaction. TUI has no in-process or successor switch command.

## Frontend placement and teardown proofs

### GTK

GTK creates one `AppRuntime`, moves it into one `unique_ptr`, and constructs
`MainWindow` only after that placement. The raw runtime pointer is attached
directly to the window GObject with one delete notifier; there is no second heap
smart-pointer holder for the runtime. `MainWindow` itself has an explicit C++
`unique_ptr` owner. Deleting that owner releases frontend members and observers
before GTK base teardown releases the attached runtime. A GObject-only `RefPtr`
does not delete an unmanaged C++ window wrapper, so observing GObject finalization
alone does not prove that its C++ collaborators retired.

Preparation constructs library-backed pages, workspace, and shell layout.
Activation then chooses ordinary playback restoration or successor idle start,
starts process adapters such as MPRIS, and presents the window. Provider
registration and both phases occur after final runtime placement.

Lifecycle work admitted by a native callback is deferred until that callback
returns. `MainContextCallbackScope` rejects late chooser completion; teardown
closes callback admission and removes the one handoff idle source before the
window is released. GTK then releases dialogs and frontend observers, MPRIS,
audio and worker producers, runtime services, stores, style state, and
`Gtk::Application` through scoped composition unwind. Process signal sources
are removed after that composition returns. Native chooser cancellation and
main-loop exit alone are not lifetime proofs.

GTK's registration and process-launch adaptations, including the supported
replacement overlap, are specified by
[GTK active-library lifecycle](frontend/gtk/active-library-lifecycle.md).

### WinUI

`App` owns one `LibraryWindowSession`. Its heap-pinned session storage owns the
dispatcher, global stores, and one phase-local `optional<RuntimeGraph>`.
`RuntimeGraph` directly owns the final `AppRuntime`; a later
`optional<InteractiveBorrowers>` owns UIModel/runtime borrowers. The graph is
emplaced before provider registration, restoration, or callback binding and is
never replaced in place.

Destructive lifecycle work is posted to the dispatcher, so a picker coroutine
returns before its owners can be released. Shutdown first begins dispatcher
closing, retires the owner callback generation, clears outward callbacks, and
requests task cancellation. The window and interactive borrowers are released
before `AppRuntime::shutdown()`. After runtime producers join,
`completeClosing()` performs the bounded final executor drain. The graph is
then released while its stores and dispatcher still exist.

A callback token is only an admission generation: it never owns raw session
memory, and renewal never makes an old generation admissible. Cancellation,
window close, or a rejected dispatcher enqueue does not replace producer join
and the final drain. WinUI's deliberate release-exception and dispatcher-exit
rules are specified by the [Windows desktop shell](frontend/windows.md).

### AppKit

Shipping and smoke startup disable native window restoration before creating
`NSApplication`. `disableWindowRestoration()` merges
`ApplePersistenceIgnoreState=YES` into the volatile `NSArgumentDomain`,
preserving unrelated launch defaults; the main window is also non-restorable.
`desktop.plist` is the sole persisted window-geometry owner, so AppKit
restoration cannot compete with library switching or crash recovery. This is a
shared startup contract, not a smoke-only override. The production helper is in
[`DesktopApplication.mm`](../../app/macos-appkit/DesktopApplication.mm); the
merge/preservation regression is in
[`AppKitSmokeMain.mm`](../../test/integration/macos/AppKitSmokeMain.mm).

`LibrarySession` emplaces its returned `AppRuntime` before providers, workspace
restore, playback restore, or UIModel borrowers. The runtime-owned
`MainRunLoopExecutor` targets the main run loop; session storage remains alive
through runtime join and the executor's final drain.

Quit, switch, and hide requests are admitted in native callbacks and serviced on
a later run-loop turn. Editor completion, sheets, menu tracking, executor
callbacks, and media deliveries must settle before graph release. A pending
editor close decision is retained and shared with a competing Quit; a Keep
choice rejects close. Closing the visible window only hides it and retains the
session.

Once close preparation succeeds, AppKit retires MediaPlayer admission,
unregisters native targets, and clears Now Playing. `LibrarySession::canClose()`
requires that neither the runtime executor nor media adapter is performing a
callback. Session close then requests scan cancellation, destroys the retired
media adapter and UIModel subscriptions, shuts down the runtime, drains final
main-run-loop callbacks, and releases remaining editor state. Cancellation alone
is not the lifetime proof.

For a library switch, the session and the application-state lease remain alive
through checkpoint and terminal playback retirement. Native components settle
and detach before the session is released; the application-state lease is then
released before launch. The complete transaction and successor durability gate
are specified by [desktop library lifecycle](desktop-library-lifecycle.md).

### TUI

TUI moves the factory result once into its final stack slot. `ScreenInteractive`
is declared before the runtime, so the runtime-owned executor's screen borrow
remains valid through shutdown and destruction. Providers, workspace restore,
layout/presentation restore, playback persistence, and controller construction
begin only after placement.

On exit, the composition root retires scan/edit/settings work, cancels transient
interaction and cover delivery, performs final layout/workspace/playback
checkpoints, and only then requests ordinary playback stop. Frontend controllers,
subscriptions, and callback targets are destroyed before the runtime shutdown
scope guard. The runtime and executor are destroyed while the screen still
exists. Ending the event loop is not itself runtime teardown.

## Reentrancy, cancellation, and destruction

A synchronous observer must not destroy its emitting owner or start
composition-root shutdown on the same callback stack. It posts a later lifecycle
turn. The same prohibition applies to native callbacks that own the object being
retired; GTK picker completion, WinUI dispatcher work, and AppKit service turns
all separate admission from destruction.

`AppRuntime::shutdown()` is idempotent. It stops playback-session scheduling and
quiesces audio callbacks before its first-member `CoreRuntime` shuts down.
`CoreRuntime::shutdown()` seals library mutation/publication admission, then
closes callback resumption, stops and joins workers while library and
notification collaborators remain alive. A queued callback that has lost
runtime admission is discarded and its suspended frame unwinds without entering
retired application state.

The required destruction direction is:

1. stop accepting new lifecycle and frontend work;
2. retire frontend subscriptions, callback generations, and native targets;
3. stop audio/device and other callback producers;
4. seal library commands and publication while callback delivery still exists;
5. close async callback resumption, request worker stop, and join;
6. perform only the frontend's explicitly permitted final executor drain;
7. destroy runtime collaborators and release the executor last;
8. unregister the fatal sink and shut down logging only after every fatal
   producer has quiesced.

A callback gate or weak token prevents new entry but does not prove quiescence.
A stop request is cooperative and does not prove that a blocking decoder or
foreign call has returned. Owner-last destruction therefore depends on join and,
where specified, a final owner-thread drain.

## Structural constraints

- Application-global and per-library stores have distinct owners and lifetimes.
- Every published runtime borrow targets the final graph placement.
- Frontend observers retire before the services they observe.
- Callback producers quiesce before their targets and borrowed executor are
  destroyed.
- GTK, WinUI, AppKit, and TUI keep distinct teardown proofs; shared planning and
  launch values do not justify a stateful common lifecycle service.
- Desktop graph replacement must additionally satisfy the transaction and
  persistence invariants in
  [desktop library lifecycle](desktop-library-lifecycle.md).

## Source and test anchors

`AppRuntime.cpp` and `CoreRuntime.cpp` own composition and idempotent shutdown.
Frontend roots are `LibraryWindowLifecycle.cpp` and `main.cpp` for GTK,
`app/tui/App.cpp`, WinUI `App.xaml.cpp`, `LibraryWindowSession.cpp`, and
`LibrarySession.cpp`, and AppKit `DesktopApplication.mm` plus
`LibrarySession.cpp`.

`AppRuntimeTest.cpp` protects stable identity and producer-first teardown. GTK
`MainWindowTest.cpp`, WinUI callback-admission and dispatcher-admission tests,
TUI controller tests, and the AppKit desktop/authoring/media scenarios protect
the frontend-specific gates. Native rendering and operating-system process
behavior still require their documented native validation; source inspection
alone does not certify them.

## Related documents

- [System architecture](overview.md)
- [Runtime execution](execution/README.md)
- [Desktop library lifecycle](desktop-library-lifecycle.md)
- [GTK active-library lifecycle](frontend/gtk/active-library-lifecycle.md)
- [Windows desktop shell](frontend/windows.md)
- [Desktop successor protocol](../reference/application/desktop-successor-protocol.md)
- [Workspace](workspace/README.md)
- [Playback](playback/README.md)
- [Persistence](persistence/README.md)
