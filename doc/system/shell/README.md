---
id: architecture.application-shell
---
# Application shell architecture

## Find the contract for your change

- [GTK layout loading, editing, rebuild, state, and teardown](layout-lifecycle.md)
- [GTK allocation adaptation](layout-adaptation.md)
- [Keyboard shortcut merge and frontend projection](keyboard-shortcut.md)
- [Layout document language](../../reference/shell/layout-document.md)
- [Shared component vocabulary](../../reference/shell/component-vocabulary.md)
- [GTK component and action schema](../../reference/shell/layout-schema.md)
- [Windows component and action schema](../../reference/windows/layout-schema.md)
- [Frontend-specific behavior](../frontend/README.md)

## Scope

This page explains where application-shell composition belongs and who owns its
lifetime. It routes exact schemas and focused behavior to their owning pages; it
is not a registry of components, fields, tests, or every frontend implementation
class.

Shells place and bind presentation components. They do not own the semantic track,
playback, workspace, status, or resource state those components render.

## Shared language, frontend-owned construction

UIModel owns toolkit-neutral shell values and policy:

- versioned layout documents, bounded template expansion, and `PreparedLayout`;
- inert component and action schemas, including canonical entries whose authored
  values mean the same thing in GTK and Windows;
- GTK layout-session snapshots, component-state guards, and promotion policy; and
- neutral key chords, defaults, and override merge behavior.

A schema authorizes document vocabulary; it does not own executable handlers.
Stable component, property, action, command, and shortcut ids are not localized.
Frontend registries attach widgets, handlers, native input, and localized labels.

Sharing the version 1 document language does not create a shared widget runtime.
GTK and Windows own separate presets, schema extensions, construction, responsive
policy, and teardown. TUI composes its terminal shell independently and adopts
only neutral actions and keymap values that its event protocol can represent.

## GTK composition

```text
LayoutDocument -> prepareLayout() -> PreparedLayout
  -> LayoutSession build snapshot
  -> ComponentRegistry + ActionRegistry
  -> detached LayoutHost candidate
  -> commit one active GTK generation
```

One `MainWindow` owns one `ShellLayoutController` for the window lifetime. The
controller owns preset selection, stores, `LayoutSession`, component and action
registries, the host, editor workflow, Gio export, and shell-lifetime callback
scope. Component factories capture the exact long-lived collaborators they need
at registration; `LayoutBuildContext` carries only one build's snapshot and
traversal state.

Every rebuild constructs a complete detached tree before replacing the active
generation. `LayoutSession` advances the component-state generation before the
old tree is destroyed, preventing retiring widgets from writing into successor
state. The candidate-only `SharedWidgetHandoff` protects the one shell-owned
track-page widget while a build is accepted or rolled back.

Component state is separate from authored layout. A stateful component resolves
state by stable expanded node id, type, entry version, and authored baseline hash.
Its binding can write only for the current generation, main surface, non-edit
build, active preset, and live store. Anonymous stateful nodes still render but
do not persist.

Actions remain owned by `ActionRegistry`. Layout bindings and keymaps are
references to stable ids. Scoped Gio registrations disconnect activation before
removing only the exact actions they installed, so an independently retained old
action is inert and a newer same-id replacement is not removed. Activation
contexts, window references, and anchors are synchronous borrows and must not be
retained.

The complete load, fallback, editor transaction, reset, promotion, and teardown
rules are in [Shell layout lifecycle](layout-lifecycle.md).

## Windows composition

```text
App -> one LibraryWindowSession
  -> one LibrarySession + MainWindow
  -> MainWindow frame and one layout host
  -> ShellBuilder schema/actions/state
  -> one generation from windows.modern or windows.classic
```

The Windows shell uses the shared parse-expand-validate path but owns its schema,
dialect, presets, XAML element construction, styles, themed surfaces, pane
settings, and responsive state. `MainWindow.xaml` supplies the frame, host, and
resource scope; it does not duplicate the preset shell tree.

`ShellBuilder` owns schema, action handlers, menus, observable shell state, pane
accessors, and generation construction. A mode change first builds and publishes
the required preset candidate, then commits changed shell state. Failure retains
the previous generation and state; failure of the first build leaves the frame's
minimal layout-error surface.

Generation components receive narrow callbacks or services, never the complete
`LibrarySession` or `AppRuntime`. Mutable sources expose a current value plus a
scoped `async::Subscription`; each component reads before subscribing and owns
the subscription. Sources outlive the host, so destroying a generation releases
subscriptions before their publishers.

Playback leaves own their XAML event tokens and UIModel binding. Bind starts with
idempotent unbind, and teardown stops the binding and revokes tokens while the
native element is still alive. `MainWindow::shutdown()` retires `ShellBuilder`,
then session callbacks and reveal routing, then window-owned consumers before the
borrowed session is released.

Opening another library is a destructive application-level restart. The parent
retires its window, session, runtime borrowers, and state writers before launching
the successor; shell-mode switching does not replace the session.

Exact Windows vocabulary and rejection behavior belong to the
[Windows layout schema](../../reference/windows/layout-schema.md); observable
frontend behavior belongs to the [Windows shell specification](../frontend/windows.md).

## Other frontend boundaries

### AppKit shell owner

The AppKit application delegate is the session and native-presentation
coordinator. It retires callback admission before detaching presentation owners
and releasing the session.

The session-lifetime `MediaPlayerAdapter` projects playback into MediaPlayer Now
Playing state and the remote command center. It is the sole owner of its native
registration tokens, playback snapshot subscription, and current artwork
request. It registers exactly these seven commands: Play, Pause, Play/Pause,
Stop, Previous, Next, and Change Playback Position. Their native enabled state
tracks the adapter's current command-availability snapshot. Every other native
remote command is disabled; native commands default to enabled even without a
handler, so leaving an unsupported command untouched is not acceptable.

A native callback first uses revocable admission without retaining the session.
Play, Pause, Play/Pause, Stop, Previous, and Next enqueue delivery to the main
thread and resolve the current transport and action availability when that
delivery executes. In particular, consecutive admitted Next commands operate on
the then-current subject rather than one subject captured by both callbacks.
Duplicate Play while active and duplicate Pause while paused are successful
idempotent operations that do not move the playback clock anchor.

Change Playback Position is different: native admission validates a finite,
nonnegative in-range position and captures the current playback occurrence.
Main-thread delivery rejects the request if that occurrence has been replaced,
including a same-track replay, then submits an occurrence-guarded queued final
seek. The runtime checks the captured occurrence and position again when the
seek reaches execution. Thus backlog cannot retarget a seek to a replacement,
while multiple valid seeks for one occurrence retain FIFO order. Once handed
off, the queued seek is runtime-owned and may finish after the adapter retires.
Retirement fences native deliveries that have not reached that handoff,
unregisters and disables all seven commands, cancels observation and artwork
work, and clears Now Playing state.

Now Playing publishes the current title, artist, album, duration, playback
state, rate, elapsed position, and matching decoded artwork. A transport,
position-revision, or duration change establishes a new elapsed clock anchor.
Metadata-only and artwork-only updates republish from the existing anchor, so
they must not reset elapsed time; asynchronous artwork can publish only for the
still-current resource. Idle state without a resumable subject and adapter
retirement clear the publication.

[`MediaPlayerAdapter.mm`](../../../app/macos-appkit/MediaPlayerAdapter.mm) owns
this native boundary; [`AppKitMediaScenario.mm`](../../../test/integration/macos/AppKitMediaScenario.mm)
protects command admission, guarded seeks, clock preservation, and retirement.
The [playback command contract](../playback/application-commit.md) owns the
runtime queue after handoff. Native scenario and OS-surface evidence have the
separate limits described in [macOS development](../../development/macos.md#appkit-smoke-scenarios).

### TUI shortcut projection

TUI keeps terminal protocol above its configurable root plan. Text editing,
completion, modal/list navigation, Ctrl-C, escape paths, notification input, and
mouse sequences retain local precedence. One immutable projected plan supplies
both root dispatch and configurable hints. See
[Keyboard shortcuts](keyboard-shortcut.md) for merge, collision, and save rules.

## Dependency and lifetime constraints

- UIModel shell code cannot depend on GTK, GDK, Gio, XAML, WinRT, AppKit, FTXUI,
  or frontend-local classes.
- Frontend construction depends on UIModel schema and semantic services; runtime
  services do not depend back on layout components.
- Only `PreparedLayout` enters GTK or Windows generation construction.
- A build context is generation-scoped and cannot become a service locator.
- Presentation owners define rendered semantic values; shell owns placement,
  native construction, binding, and component lifetime.
- Authored layout, component interaction state, selected preset, and shortcut
  overrides remain separate persistence domains.
- Teardown closes callback admission and native/action registrations before
  destroying their callback targets or borrowed services.

## Implementation authority

Shared document, schema, session, state, and keymap boundaries are under
[`app/include/ao/uimodel/`](../../../app/include/ao/uimodel/) with implementation
under [`app/uimodel/`](../../../app/uimodel/).

GTK composition is rooted at
[`ShellLayoutController`](../../../app/linux-gtk/app/ShellLayoutController.h),
[`LayoutHost`](../../../app/linux-gtk/layout/runtime/LayoutHost.h),
[`ComponentRegistry`](../../../app/linux-gtk/layout/runtime/ComponentRegistry.h),
and [`ActionRegistry`](../../../app/linux-gtk/layout/runtime/ActionRegistry.h).

Windows composition is rooted at
[`App`](../../../app/windows-winui/App.xaml.h),
[`LibraryWindowSession`](../../../app/windows-winui/app/LibraryWindowSession.h),
[`MainWindow`](../../../app/windows-winui/MainWindow.xaml), and
[`ShellBuilder`](../../../app/windows-winui/layout/ShellBuilder.h).

AppKit composition is rooted at
[`DesktopApplication.mm`](../../../app/macos-appkit/DesktopApplication.mm), and
TUI composition at [`app/tui/App.cpp`](../../../app/tui/App.cpp) and
[`app/tui/Keymap.cpp`](../../../app/tui/Keymap.cpp).

## Related documents

- [System architecture](../overview.md)
- [Presentation architecture](../presentation/README.md)
- [Interactive session lifecycle](../session-lifecycle.md)
- [Persistence architecture](../persistence/README.md)
- [Shell layout lifecycle](layout-lifecycle.md)
- [Keyboard shortcuts](keyboard-shortcut.md)
- [Windows desktop state](../../reference/windows/desktop-state.md)
