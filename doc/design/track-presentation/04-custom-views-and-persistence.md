# Custom Views and Persistence

## Purpose

Custom views provide customization without putting group-by, sort-by, and column controls back into the main track-table toolbar.

Built-in presets are read-only. Users customize by duplicating a built-in or creating a new custom view.

## Concepts

```text
Built-in presentation
  - shipped by Aobus
  - read-only
  - can evolve in future releases
  - identified by stable ids such as songs/albums/artists

Custom view
  - user-created
  - editable
  - can be based on a built-in
  - stores group, sort, visible fields, redundant fields
```

## Persistence Location

Recommended first version:

- Store custom view definitions in app config or GTK UI state.
- Do not store them in the LMDB music library.
- Do not bump `LibraryVersion` for this feature unless definitions are moved into the library database.

Rationale:

- Custom views are UI/presentation preferences.
- The library database should not gain UI-specific state until there is a clear workspace/library-specific requirement.

## Runtime Definition Shape

Runtime can use the same `TrackPresentationSpec` for built-ins and custom views. The app/config layer needs label and metadata.

```cpp
struct CustomTrackPresentationDefinition final
{
  std::string id;
  std::string label;
  std::string basePresetId;
  rt::TrackGroupKey groupBy = rt::TrackGroupKey::None;
  std::vector<rt::TrackSortTerm> sortBy{};
  std::vector<rt::TrackPresentationField> visibleFields{};
  std::vector<rt::TrackPresentationField> redundantFields{};
};
```

Conversion:

```cpp
rt::TrackPresentationSpec specFromCustomDefinition(CustomTrackPresentationDefinition const& definition);
CustomTrackPresentationDefinition customDefinitionFromSpec(rt::TrackPresentationSpec const& spec,
                                                           std::string label,
                                                           std::string basePresetId);
```

## App UI State Skeleton

Current app UI state in `app/linux-gtk/app/UIState.h` stores global track column state. Add presentation definitions without putting GTK columns into runtime.

Possible transitional skeleton:

```cpp
namespace ao::gtk
{
  struct TrackPresentationSortTermState final
  {
    std::string field;
    bool ascending = true;
  };

  struct CustomTrackPresentationState final
  {
    std::string id;
    std::string label;
    std::string basePresetId;
    std::string groupBy;
    std::vector<TrackPresentationSortTermState> sortBy;
    std::vector<std::string> visibleFields;
    std::vector<std::string> redundantFields;
  };

  struct TrackPresentationColumnState final
  {
    std::string presentationId;
    std::map<std::string, std::int32_t, std::less<>> fieldWidths;
  };

  struct TrackViewState final
  {
    std::string activePresentationId = "songs";

    // Existing global column state can remain during migration.
    std::vector<std::string> columnOrder;
    std::vector<std::string> hiddenColumns;
    std::map<std::string, std::int32_t, std::less<>> columnWidths;

    // Target custom view state.
    std::vector<CustomTrackPresentationState> customPresentations;
    std::vector<TrackPresentationColumnState> presentationColumnStates;
  };
}
```

This is UI/config state, not runtime service state. Runtime receives normalized `TrackPresentationSpec` values.

## Field and Sort Serialization

Use stable string ids:

- `trackPresentationFieldId()` / `trackPresentationFieldFromId()`
- `trackSortFieldId()` / `trackSortFieldFromId()` if not already present
- `trackGroupKeyId()` / `trackGroupKeyFromId()` if not already present

Validation rules:

1. Unknown presentation id: fall back to `songs`.
2. Unknown field id: drop it.
3. Unknown sort field id: drop it.
4. Duplicate visible fields: remove duplicates, preserve first occurrence.
5. Empty visible fields: fall back to the base preset or `songs`.
6. Empty sort terms: fall back to the base preset or `songs`.
7. Empty label: use `Custom View` or the base preset label plus ` Copy`.

## Presentation Registry With Custom Views

Runtime can own built-ins only. The app layer can combine built-ins and custom views for GTK menus.

App-level model skeleton:

```cpp
class TrackPresentationStore final
{
public:
  using ChangedSignal = sigc::signal<void()>;

  std::vector<rt::TrackPresentationPreset> builtinPresets() const;
  std::vector<CustomTrackPresentationDefinition> const& customDefinitions() const;

  std::optional<rt::TrackPresentationSpec> specForId(std::string_view id) const;

  void setCustomDefinitions(std::vector<CustomTrackPresentationDefinition> definitions);
  void upsertCustomDefinition(CustomTrackPresentationDefinition definition);
  void removeCustomDefinition(std::string_view id);

  ChangedSignal& signalChanged();
};
```

This class can live in GTK/app code if custom views are app config. If custom views later become workspace runtime state, move an equivalent registry into runtime.

## Custom View Editor

First complete dialog skeleton:

```cpp
class TrackCustomViewDialog final : public Gtk::Dialog
{
public:
  struct Result final
  {
    CustomTrackPresentationDefinition definition;
    bool deleted = false;
  };

  TrackCustomViewDialog(Gtk::Window& parent,
                        rt::TrackPresentationSpec const& initialSpec,
                        std::span<rt::TrackPresentationPreset const> builtins);

  std::optional<Result> runDialog();

private:
  void setupNameSection();
  void setupBasePresetSection();
  void setupGroupSection();
  void setupSortSection();
  void setupVisibleFieldsSection();
  void populateFromSpec(rt::TrackPresentationSpec const& spec);
  rt::TrackPresentationSpec collectSpec() const;
  CustomTrackPresentationDefinition collectDefinition() const;

  Gtk::Entry _nameEntry;
  Gtk::DropDown _basePresetDropdown;
  Gtk::DropDown _groupDropdown;
  Gtk::ListBox _sortTermsList;
  Gtk::ListBox _visibleFieldsList;
};
```

### Editor behavior

Minimum supported operations:

1. Duplicate built-in preset.
2. Rename custom view.
3. Choose group field.
4. Choose ordered sort terms and direction.
5. Choose visible fields and order.
6. Delete custom view.
7. Reset from base preset.

Nice-to-have later:

- live preview
- drag-and-drop field reordering
- preset descriptions
- warnings when a visible field is hidden by grouping redundancy

## Main Menu Integration

View selector should list:

1. Built-ins.
2. Separator.
3. Custom views if any.
4. Separator.
5. Create Custom View.
6. Manage Custom Views.

Selection flow:

```text
User picks built-in/custom id
  -> TrackPresentationStore resolves spec
  -> ViewService::setPresentation(viewId, spec)
  -> GTK applies visibleFields to column layout
```

## Active Presentation Scope

Recommended first version: active presentation is per runtime track-list view.

Reasoning:

- Different lists can be viewed through different lenses.
- Smart lists and playlists often have different presentation needs.
- It fits current `ViewService` ownership of filter/sort/group/selection.

Global “last used view” can be added later as a convenience default.

## Migration From Existing State

Map old group-based state to built-in ids:

| Old groupBy | Suggested presentation |
| --- | --- |
| None | `songs` |
| Artist | `artists` |
| Album | `albums` |
| AlbumArtist | `album-artists` |
| Genre | `genres` |
| Composer | `classical-composers` |
| Work | `classical-works` |
| Year | `years` |

If mapping fails, fall back to `songs`.

## Width Persistence

Column widths are GTK rendering state. They should not be part of runtime presentation spec.

Options:

1. First version: ignore per-preset width persistence and use `TrackColumnDefinition` defaults.
2. Later: persist field widths keyed by presentation id in GTK UI state.

Recommended first version: option 1.

## Versioning

No `LibraryVersion` bump is required if custom views are stored outside the LMDB library.

If custom views later become part of persisted library/workspace data, revisit versioning and migration rules in `doc/design/versioning.md`.
