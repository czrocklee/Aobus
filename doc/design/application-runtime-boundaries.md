# Application Runtime Boundary Design

## Purpose

This document defines the intended object-oriented boundaries between the Aobus
core library, the private application runtime, and the Linux GTK frontend. It is
a design contract for future refactors: names, ownership, and dependencies
should make the layer boundary obvious before a class grows into a cross-layer
coordinator.

## Goals

- Keep the public core library frontend-neutral and reusable by third-party
  applications.
- Make the private application runtime the home for frontend-neutral use cases,
  projections, and application state orchestration.
- Keep GTK-specific widgets, caches, adapters, and event wiring in the GTK
  frontend.
- Use class names that describe the role a class actually plays.
- Prefer small, explicit coordinators over broad service bags and implicit
  initialization order.

## Non-Goals

- Do not introduce a full hexagonal architecture framework.
- Do not move GTK-specific UI behavior into `include/ao` or `lib/`.
- Do not force the CLI to depend on a GUI-oriented workspace runtime.
- Do not rename large parts of the tree without either tightening a boundary or
  reducing a misleading name.

## Layer Model

```text
app/linux-gtk
  GTK widgets, pages, panels, dialogs, adapters, caches, hosts, controllers,
  and platform bootstrap code.

app/runtime or app/application
  Private Aobus application layer: use cases, projections, session/workspace
  state, command/query services, and frontend-neutral orchestration.

include/ao + lib
  Public core library: music library storage, audio playback engine, decoders,
  tag parsing, query evaluation, LMDB wrappers, and stable public types.
```

Dependencies must point downward:

```text
app/linux-gtk  ->  app/runtime  ->  include/ao + lib
app/cli        ->  app/application or include/ao + lib
```

The core library must not depend on `app/runtime`, GTK, CLI, or frontend state.

## Public Core Library Boundary

The public core library is the stable domain and infrastructure surface. It may
own classes such as:

- `library::MusicLibrary`
- `library::TrackStore`, `ListStore`, `ResourceStore`, `DictionaryStore`
- `audio::Player`, `audio::Engine`, backend provider interfaces
- tag, media, query, LMDB, and utility primitives

The core library should not own:

- GTK row objects or list models
- window, layout, or presentation preferences
- import/export dialogs or progress widgets
- application workspace state
- frontend-specific caches such as pixbuf cover-art caches

Moving a type into `include/ao` is allowed only when the type is stable,
frontend-neutral, and useful without Aobus's private application runtime.

## Application Runtime Boundary

The private runtime owns Aobus application semantics that are not part of the
public library API. It may own:

- command/query services for library operations
- playback orchestration above `audio::Player`
- mutation services and notifications
- track sources and projections
- workspace/view state for GUI frontends
- configuration persistence for application state

Runtime types should avoid GTK includes and GTK object ownership. They may expose
small snapshots, IDs, deltas, subscriptions, and command results that a frontend
can adapt to its own widgets.

### Runtime Sub-boundaries

The current `app/runtime` contains both frontend-neutral services and GUI-like
workspace/view concepts. Future work should make this distinction explicit:

```text
app/application
  Frontend-neutral command/query services used by both CLI and GTK.

app/runtime/workspace
  GUI-oriented workspace, view, projection, selection, and session state.
```

This split can be implemented as directories later. The important rule is that
CLI commands should reuse frontend-neutral application services, not a full GUI
workspace runtime.

## GTK Frontend Boundary

The GTK frontend owns presentation and platform adaptation. It may own:

- `Gtk::Window`, `Gtk::Box`, `Gtk::Stack`, `Gtk::ColumnView`, dialogs, popovers
- row objects and GTK list-model adapters
- page hosts and view controllers
- GTK-specific layout components
- pixbuf/image caches
- Linux GTK platform bootstrap such as PipeWire/ALSA provider registration

GTK widgets and controllers should prefer runtime services for application
operations. Direct `MusicLibrary` access is acceptable only in adapter/cache
classes that explicitly bridge runtime IDs to GTK display data.

## Naming Rules

Names should communicate ownership and responsibility:

| Suffix | Intended meaning |
| --- | --- |
| `Runtime` | Composition root or runtime container for application services. |
| `Context` | Runtime state passed through a subsystem for one operation or host. |
| `Service` | Application use case or state orchestration API. |
| `Store` | Persistent or in-memory collection with identity-based lookup. |
| `Source` | Ordered or observable source of domain IDs. |
| `Projection` | Read model derived from sources and state, often observable. |
| `Coordinator` | High-level wiring of multiple controllers/services for a screen. |
| `Controller` | Event glue for a specific user interaction or feature area. |
| `Host` | Owner of a widget tree or child view lifecycle. |
| `Adapter` | Converts between runtime/domain APIs and frontend model APIs. |
| `Cache` | Local performance cache, usually frontend-specific unless proven general. |
| `Widget`, `Page`, `Panel`, `Dialog`, `Window` | Concrete GTK UI objects. |

Avoid `Manager` for new classes unless no more precise role applies.

## Current Rename Targets

The following names should be treated as transitional because their current
names understate or misdescribe their responsibilities:

| Current name | Preferred name | Rationale |
| --- | --- | --- |
| `rt::AppSession` | `rt::AppRuntime` | It constructs and exposes the runtime service graph; it is not only a user session. |
| `rt::AppSessionDependencies` | `rt::AppRuntimeDependencies` | Matches the runtime container role. |
| `gtk::WindowController` | `gtk::MainWindowCoordinator` | It wires controllers, caches, subscriptions, persistence, and platform bootstrap. |
| `gtk::layout::LayoutDependencies` | `gtk::layout::LayoutContext` | It is a mutable runtime context/service bundle, not just a dependency declaration. |
| `gtk::TrackPageManager` | `gtk::TrackPageHost` | It owns track page lifecycle inside a GTK stack. |

Renames should be paired with small responsibility tightening when practical.
For example, renaming `WindowController` to `MainWindowCoordinator` should also
move at least one non-window concern out of the class.

## Responsibility Rules

### Runtime container

The runtime container may construct and expose services. It should not become a
home for arbitrary use cases. If a convenience method combines multiple
services, prefer moving it into a command service once it grows beyond a trivial
delegation.

### Main window coordinator

The main window coordinator may wire feature controllers and connect runtime
events to GTK hosts. It should not own platform bootstrap, generic persistence,
or business rules that belong in runtime services.

### Workspace service

The workspace service should own in-memory navigation state:

- open views
- focused view
- navigation targets
- close/open behavior

Session persistence should live in a separate persistence service or controller.
The workspace service may expose snapshots needed by that persistence layer.

### Layout context

The layout context should expose only services required by layout components. It
must not become the only way for unrelated GTK features to find each other. When
the number of fields grows, group them into an explicit `GtkUiServices` or
feature-specific context object created by the main window coordinator.

### Track page host

The track page host should own page lifecycle in the GTK stack. If binding logic
continues to grow, split a `TrackPageBinder` that wires runtime projections,
selection, playback reveal, and tag actions to a page instance.

## CLI Rule

The CLI should not depend on GUI workspace/view runtime. Shared CLI/GTK behavior
should be extracted as frontend-neutral application services. The CLI may use
`library::MusicLibrary` directly during transition, but new behavior that also
exists in GTK should be implemented through shared command/query services.

Recommended service direction:

```text
LibraryCommandService
LibraryQueryService
TrackCommandService
ListCommandService
TagCommandService
```

These services may use `MusicLibrary`, `LibraryMutationService`, or lower-level
stores internally, but their API should not mention GTK objects or workspace
views.

## Persistence Rule

Persistence should be owned by the layer whose state is persisted:

- library database state belongs in `MusicLibrary` and stores
- frontend-neutral application state belongs in runtime/application persistence
- GTK window/layout preferences belong in GTK persistence helpers
- UI-only presets or column widths should not be written to the public library
  database unless they become part of the library domain

`ConfigStore` can remain a runtime utility, but classes should avoid scattering
ad-hoc group names and serialization logic across broad coordinators.

## Review Checklist

Use this checklist when adding or moving classes:

- Does the class include GTK headers? If yes, it belongs under `app/linux-gtk`.
- Does the class expose a stable frontend-neutral domain API? If yes, it may
  belong under `include/ao` or `lib`.
- Does the class coordinate Aobus use cases but not own widgets? If yes, it
  belongs in the private application/runtime layer.
- Is the class named after what it owns, coordinates, or adapts?
- Can the class be tested without GTK? If not, do not move it into runtime.
- Does a new dependency point downward? If not, introduce an adapter or command
  service instead.
- Is a `Manager` hiding a more precise `Host`, `Coordinator`, `Service`,
  `Store`, or `Adapter` role?

## Migration Policy

Boundary cleanup should be incremental. Prefer small compile-safe steps:

1. Rename misleading classes.
2. Move one responsibility at a time.
3. Add focused tests for extracted frontend-neutral services.
4. Update design documents when behavior or boundary rules change.
5. Avoid broad directory moves until the ownership model is already clear in the
   code.
