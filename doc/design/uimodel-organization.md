# UIModel Organization

This is the standing convention for how `ao_app_uimodel` is namespaced, foldered,
and named. It describes the rules as they are, not how the layer reached them.

## Namespace

The public UI model namespace is flat:

```cpp
namespace ao::uimodel
{
}
```

Folder paths express feature ownership; they do not contribute to the namespace.
A type under `library/presentation/` is still spelled `ao::uimodel::TrackPresentationCatalog`:

```cpp
#include <ao/uimodel/library/presentation/TrackPresentationCatalog.h>

auto catalog = ao::uimodel::TrackPresentationCatalog{workspace};
```

The only permitted nested namespace in public headers is `ao::uimodel::detail`.
Tests use a flat `ao::uimodel::test` namespace and do not introduce
feature-specific test namespaces.

## Folder layout

Feature folders group related classes into capsules. The same layout is mirrored
under `app/include/ao/uimodel/`, `app/uimodel/`, and `test/unit/uimodel/`. Folder
names are singular.

```text
ao/uimodel/
├── input/
├── field/
├── layout/
│   ├── document/
│   ├── action/
│   ├── component/
│   └── shell/
├── library/
│   ├── list/
│   ├── presentation/
│   ├── track/
│   ├── detail/
│   └── property/
├── playback/
│   ├── now-playing/
│   ├── transport/
│   ├── seek/
│   ├── output/
│   ├── queue/
│   ├── quality/
│   └── soul/
├── preferences/
├── status/
│   └── activity/
└── FrameClock.h
```

The build enforces this layout and the namespace rules through the
`ao_uimodel_organization_guardrail` target (`cmake/AssertUimodelOrganization.cmake`).
Adding a new feature folder requires updating the allowed-path set in that file.

## Naming

Because public types live in a flat namespace, their names must be
self-describing without relying on a nested namespace for context.

Public names carry a feature/domain prefix. Avoid bare generic names:

```text
Avoid          Prefer
State          ActivityCompactState
Command        TrackPresentationApplyCommand
Context        LayoutActionActivationContext
Descriptor     LayoutActionDescriptor
ActionState    LayoutActionState
ColumnState    TrackColumnState
SelectionSummary  TrackSelectionSummary
```

## Role suffixes

A type's suffix declares what it is allowed to do. This keeps units small and
testable and prevents accidental God objects.

| Suffix | Responsibility | Examples |
|---|---|---|
| `ViewModel` | Long-lived adapter-facing object; subscribes to runtime/services, produces view state, exposes user-action methods, returns commands or updates UI-local stores. | `NowPlayingViewModel`, `TransportViewModel`, `TrackPresentationPickerViewModel` |
| `Model` | UI state machine or editor state holder; no platform widgets. | `ActivityStatusModel`, `SmartListEditorModel`, `TrackPropertiesFormModel` |
| `Workflow` | One user operation or operation family; stateless or short-lived. A type that owns subscriptions and renders state is a `ViewModel`, not a `Workflow`. | `TrackInlineEditWorkflow`, `TagEditWorkflow` |
| `Store` | Owns UI-local preference/session state and emits changes. Does not persist config unless named as a config/persistence store. | `ListPresentationPreferenceStore`, `TrackColumnLayoutStore`, `KeymapStore` |
| `Catalog` | Owns or exposes available options and lookups. Does not apply selections or navigate. | `TrackPresentationCatalog`, `LayoutActionCatalog`, `LayoutComponentCatalog` |
| `Policy` | Deterministic, platform-free decision logic. | `TrackColumnLayoutPolicy`, `TrackFieldEditPolicy`, `ListActionPolicy` |
| `Schema` | Static or definition-derived UI structure (rows/components), not current runtime values. | `TrackFieldGridSchema` |
| `Projection` | Maps a runtime/domain snapshot into UI-facing state or tree structure. | `ListTreeProjection` |
| `Formatter` | Converts values to display strings. | `TrackFieldFormatter`, `AudioQualityFormatter` |
| `Codec` | Converts text to typed edit values and back. | `TrackFieldEditCodec` |
| `Resolver` | Maps user/config references to concrete runtime/domain values. | `TrackFilterResolver` |
| `Recommender` | Picks a suitable default from context. | `TrackPresentationRecommender` |

## Responsibility boundary

`uimodel` owns platform-neutral UI behavior. It may contain:

- View state projection and UI-local state machines.
- UI-local preference/session stores.
- Application preferences models that map user selections to persisted deltas
  and platform-supplied applier callbacks.
- Display formatting and editor parsing / edit-value codecs.
- UI action, visibility/enabled, and selection policy.
- User-operation workflows that produce runtime commands or metadata patches.
- Platform-neutral picker/menu/list/detail state.
- Runtime subscriptions inside platform-neutral ViewModels.

It must not contain:

- GTK widgets, GObject models, GTK signals or idle lifecycle.
- CSS providers or widget-bound CSS class mutation.
- Dialog/window/popover ownership, file dialogs, or portals.
- Direct LMDB transactions or storage store access.
- Direct audio backend/player/engine control.
- Any platform-specific include dependency.

Runtime and storage data enter `uimodel` only through runtime value types,
runtime service interfaces (for ViewModels), DTO snapshots, function
requests/hooks, and platform-neutral callbacks.

GTK adapters own the rest: widget creation and binding, emitting user actions
into `uimodel`, applying returned commands to runtime, dialog/window/popover
lifecycle, main-loop scheduling and cancellation, CSS, and GObject/Gio/Gtk list
model construction.

## Guardrails

The build enforces that `ao_app_uimodel` stays platform-neutral. Forbidden public
dependencies:

- GTK/GDK/Glibmm/Gio UI types; platform window/dialog/popover/widget types.
- Direct LMDB storage headers.
- Direct `ao::library` store/view headers, unless explicitly approved as a stable
  value-type dependency.
- Direct audio backend/player/engine control headers.

Allowed: stable core value types, runtime state/value types, runtime service
interfaces (for ViewModels), `ao::rt::Signal` / `Subscription`, and
platform-neutral standard/utility headers.

## Tests

Tests mirror the feature folders and use the flat `ao::uimodel::test` namespace.

```text
test/unit/uimodel/library/presentation/TrackPresentationCatalogTest.cpp
```

```cpp
namespace ao::uimodel::test
{
  TEST_CASE("TrackPresentationCatalog - labels prefer builtin and custom presets",
            "[uimodel][unit][library][presentation]")
  {
  }
}
```

Tags follow `[uimodel][unit][feature][component]`, e.g.
`[uimodel][unit][library][presentation]`, `[uimodel][unit][field][formatter]`,
`[uimodel][unit][playback][now-playing]`, `[uimodel][unit][status][activity]`.

## Feature capsules

- **`input`** — key chords, keymap model/store.
- **`field`** — the shared `TrackField` UI language: display formatting, edit
  text/value codec, field edit patch policy, inline edit workflow. Shared by track
  list, track detail, properties dialog, now playing, and presentation policy.
- **`layout`** — generic layout DSL and metadata: document model (`document/`),
  action catalog/validation (`action/`), component catalog and state
  (`component/`), shell session model (`shell/`). Track-specific selection or field
  grid policy does not belong here.
- **`library/presentation`** — track-list presentation: catalog, custom presets,
  per-list preferences, recommendation, picker view state, custom editor model,
  column layout store/policy, presentation field visibility. No GTK menus/dialogs.
- **`library/list`** — list tree projection, list action policy, smart list editor
  model. No GtkTreeListModel / Gio::ListStore.
- **`library/track`** — the main track-list surface: filtering, selection summary,
  selection region policy, page routing. No Gtk::ColumnView / row widgets.
- **`library/detail`** — track detail panel: field grid schema, field grid
  visibility policy, custom property add/edit/delete workflow. No GTK grid rows.
- **`library/property`** — track properties form: row spec, mixed-state model,
  patch generation from edited values. No GTK dialogs.
- **`playback`** — now-playing view model and action policy (`now-playing/`),
  transport (`transport/`), seek/time interpolation (`seek/`), output and volume
  (`output/`), queue (`queue/`), audio quality formatting (`quality/`), Aobus soul
  model (`soul/`).
- **`preferences`** — platform-neutral application preferences models that
  convert user selections into persisted deltas and platform-supplied applier
  callbacks. No GTK widgets or config-store ownership.
- **`status/activity`** — activity status model, detail/compact state types,
  activity action resolution policy.
