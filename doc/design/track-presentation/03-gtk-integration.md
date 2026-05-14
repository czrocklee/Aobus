# GTK Integration for Track Presentation Presets

## Purpose

This document defines how GTK consumes runtime presentation specs while keeping GTK-specific columns, rendering, widths, and row value loading in the GTK layer.

## File Ownership

Recommended GTK files:

```text
app/linux-gtk/track/TrackPresentation.h/.cpp       # existing GTK column definitions and layout model
app/linux-gtk/track/TrackPresentationAdapter.h/.cpp # optional new file if mapping grows
app/linux-gtk/track/TrackViewPage.h/.cpp           # toolbar selector and applying presentation
app/linux-gtk/track/TrackColumnController.h/.cpp   # column widgets/factories remain here
app/linux-gtk/track/TrackRowCache.h/.cpp           # row value loading remains here
app/linux-gtk/track/TrackRowObject.h/.cpp          # cell value access remains here
```

If the existing `TrackPresentation.*` becomes too broad, create `TrackPresentationAdapter.*` for runtime-to-GTK mapping and leave column definitions in `TrackPresentation.*`.

## Runtime Field to GTK Column Mapping

Header skeleton:

```cpp
#pragma once

#include "track/TrackPresentation.h"

#include <runtime/TrackPresentationPreset.h>

#include <optional>

namespace ao::gtk
{
  std::optional<TrackColumn> trackColumnForPresentationField(rt::TrackPresentationField field);

  TrackColumnLayout trackColumnLayoutForPresentation(rt::TrackPresentationSpec const& presentation);

  TrackColumnLayout trackColumnLayoutForPresentation(rt::TrackListPresentationSnapshot const& presentation);
}
```

Implementation rule:

- `visibleFields` controls column order.
- Fields listed in `redundantFields` should be represented in the generated layout but marked hidden, or omitted from visible state depending on what works best with `TrackColumnController`.
- Unknown/unmapped fields are ignored safely.
- Widths come from existing `TrackColumnDefinition::defaultWidth`.

Suggested mapping:

```cpp
std::optional<TrackColumn> trackColumnForPresentationField(rt::TrackPresentationField field)
{
  switch (field)
  {
    case rt::TrackPresentationField::Title: return TrackColumn::Title;
    case rt::TrackPresentationField::Artist: return TrackColumn::Artist;
    case rt::TrackPresentationField::Album: return TrackColumn::Album;
    case rt::TrackPresentationField::AlbumArtist: return TrackColumn::AlbumArtist;
    case rt::TrackPresentationField::Genre: return TrackColumn::Genre;
    case rt::TrackPresentationField::Composer: return TrackColumn::Composer;
    case rt::TrackPresentationField::Work: return TrackColumn::Work;
    case rt::TrackPresentationField::Year: return TrackColumn::Year;
    case rt::TrackPresentationField::DiscNumber: return TrackColumn::DiscNumber;
    case rt::TrackPresentationField::TrackNumber: return TrackColumn::TrackNumber;
    case rt::TrackPresentationField::Duration: return TrackColumn::Duration;
    case rt::TrackPresentationField::Tags: return TrackColumn::Tags;
  }

  return std::nullopt;
}
```

## TrackColumnLayout Generation

Pseudo-implementation:

```cpp
TrackColumnLayout trackColumnLayoutForPresentation(rt::TrackPresentationSpec const& presentation)
{
  auto layout = TrackColumnLayout{};

  auto isRedundant = [&presentation](rt::TrackPresentationField field)
  { return std::ranges::contains(presentation.redundantFields, field); };

  for (auto const field : presentation.visibleFields)
  {
    auto const column = trackColumnForPresentationField(field);
    if (!column)
    {
      continue;
    }

    auto state = defaultTrackColumnState(*column); // expose this helper if needed
    state.visible = !isRedundant(field);
    layout.columns.push_back(state);
  }

  return normalizeTrackColumnLayout(layout);
}
```

If `defaultTrackColumnState()` is currently private in `TrackPresentation.cpp`, either expose a const helper or construct from `trackColumnDefinitions()`.

## TrackPresentation.h Adjustments

Current `TrackPresentation.h` owns GTK `TrackColumn`, `TrackColumnDefinition`, `TrackColumnState`, and `TrackColumnLayoutModel`.

Recommended additions:

```cpp
namespace ao::gtk
{
  TrackColumnDefinition const* trackColumnDefinition(TrackColumn column);
  TrackColumnState defaultTrackColumnState(TrackColumn column);
}
```

These helpers are useful for the presentation adapter and tests. They remain GTK-level helpers.

## TrackViewPage Target Header Skeleton

Replace group controls with presentation controls.

```cpp
class TrackViewPage final : public Gtk::Box
{
public:
  explicit TrackViewPage(ListId listId,
                         TrackListAdapter& adapter,
                         TrackColumnLayoutModel& columnLayoutModel,
                         rt::AppSession& session,
                         rt::ViewId viewId = rt::ViewId{});

private:
  void setupPresentationControls();
  void populatePresentationOptions();
  void selectPresentationOption(std::string_view presentationId);
  void onPresentationChanged();
  void applyPresentation(rt::TrackListPresentationSnapshot const& presentation);
  void applyPresentation(rt::TrackPresentationSpec const& presentation);

  void setupHeaderFactory();
  void setupStatusBar();
  void updateSectionHeaders();
  void onModelChanged();

  Gtk::Box _controlsBar{Gtk::Orientation::HORIZONTAL};
  Gtk::Entry _filterEntry;

  Gtk::MenuButton _presentationButton;
  Gtk::Popover _presentationPopover;
  Gtk::Box _presentationMenuBox{Gtk::Orientation::VERTICAL};
  std::vector<std::string> _presentationIds;

  Gtk::Label _statusLabel;
  Gtk::ScrolledWindow _scrolledWindow;
  Gtk::ColumnView _columnView;
  Gtk::Popover _contextPopover;

  rt::TrackPresentationSpec _activePresentation;

  // Existing members remain: adapter, session, group model, selection model,
  // column layout model, controllers, signals.
};
```

### Toolbar behavior

`setupPresentationControls()` should:

1. Keep the existing filter entry behavior.
2. Add a `View: [preset ▼]` menu button with a popover.
3. Populate built-in presentations from runtime registry.
4. Later append custom views from custom definition storage.
5. Remove `_groupByLabel`, `_groupByDropdown`, and `_groupByOptions`.
6. Do not append `_columnController->columnsButton()` to the main toolbar in the target UI.

`Gtk::MenuButton` with a `Gtk::Popover` is used instead of `Gtk::DropDown` because the target menu requires separators, non-selectable action items ("Create Custom View...", "Manage Custom Views..."), and a custom views sub-section. `DropDown` cannot support these without heavy customization.

Pseudo-flow:

```cpp
void TrackViewPage::onPresentationSelected(std::string_view presentationId)
{
  _presentationPopover.popdown();
  _presentationButton.set_label(resolvePresentationLabel(presentationId));
  _session.views().setPresentation(_viewId, presentationId);
}
```

Do not leave `Gtk::ColumnView` model-less across main-loop turns while switching presentations. When column visibility or order must change, temporarily detach the model within the same callback, apply the projection/header/column changes while no live row/cell widgets are attached, then reconnect the selection model before returning to GTK. Reset the horizontal adjustment before applying a presentation that may shrink the visible column set.

## Applying Presentation to GTK Columns

When active presentation changes:

```cpp
void TrackViewPage::applyPresentation(rt::TrackPresentationSpec const& presentation)
{
  _activePresentation = presentation;

  updateSectionHeaders();
  _columnController->setLayoutAndApply(trackColumnLayoutForPresentation(presentation));
}
```

If projection snapshot is the source:

```cpp
void TrackViewPage::applyPresentation(rt::TrackListPresentationSnapshot const& snapshot)
{
  updateSectionHeaders();
  _columnController->setLayoutAndApply(trackColumnLayoutForPresentation(snapshot));
}
```

## TrackColumnController Role

`TrackColumnController` should remain the owner of:

- `Gtk::ColumnViewColumn` instances
- cell factories
- CSS provider
- title-position update logic
- width changes
- reorder tracking if still needed internally

But it should no longer be the main user customization surface in the toolbar.

Recommended incremental API:

```cpp
class TrackColumnController final
{
public:
  void setupColumns(FactoryProvider factoryProvider);
  void setupColumnControls(); // keep for custom editor reuse or remove from main toolbar only

  void applyColumnLayout();
  void setLayoutAndApply(TrackColumnLayout const& layout);
  void updateColumnVisibility();

  // Existing columnsButton() can remain temporarily, but TrackViewPage should stop appending it.
  Gtk::MenuButton& columnsButton();
};
```

Longer-term, `setupColumnControls()` can move into the custom view editor or become internal test/debug UI.

Column layout application should keep at least one visible column expandable. If a presentation hides the default expanding column, the controller should fall back to the Title column, or the first visible column if Title is hidden, so the `Gtk::ColumnView` continues to fill the scrolled window width.

## Redundant Fields

Current behavior hides columns that are redundant with grouping. Preserve that behavior, but drive it from runtime presentation spec instead of `presentationForGroup(groupBy)`.

Current redundancy provider maps runtime sort fields to GTK columns. Target provider should map runtime presentation fields:

```cpp
_columnController->setRedundancyProvider(
  [this]
  {
    auto redundant = std::unordered_set<TrackColumn>{};

    for (auto const field : _activePresentation.redundantFields)
    {
      if (auto const column = trackColumnForPresentationField(field))
      {
        redundant.insert(*column);
      }
    }

    return redundant;
  });
```

## Section Headers

`updateSectionHeaders()` can continue to inspect `projection()->presentation().groupBy`.

The section label text still comes from projection group sections.

Do not make GTK calculate groups from visible fields.

## Row Value Loading Remains Unchanged

Do not change these classes for the first preset implementation except for incidental compile adjustments:

- `TrackRowCache`
- `TrackRowObject`
- `TrackColumnFactoryBuilder`

Cell factories should continue using:

```cpp
row->getColumnText(definition.column)
```

The visible field list determines which factories are attached to visible columns; it does not determine where values are loaded.

## GTK Tests

Add tests for:

1. Every runtime `TrackPresentationField` maps to the expected `TrackColumn`.
2. `trackColumnLayoutForPresentation(albums)` yields Track, Title, Duration, Year, Tags in order.
3. Redundant fields become hidden or omitted according to the chosen implementation.
4. Duplicate visible fields are normalized away before or during layout generation.
5. The default layout for `songs` matches the product matrix.

Existing `TrackPresentationTest.cpp` is the likely home for these tests.
