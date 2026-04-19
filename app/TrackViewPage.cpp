// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#include "TrackViewPage.h"

#include <glibmm/wrap.h>
#include <gtk/gtk.h>

#include <gdk/gdk.h>

#include <gtkmm/columnviewcolumn.h>
#include <gtkmm/label.h>
#include <gtkmm/listheader.h>
#include <gtkmm/listitem.h>
#include <gtkmm/signallistitemfactory.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace
{
  using RowCompareFn = std::function<int(TrackRow const&, TrackRow const&)>;
  constexpr auto kTagsCellWidgetName = "track-tags-cell";

  auto trackRowFromItem(gconstpointer item) -> TrackRow const*
  {
    if (!item)
    {
      return nullptr;
    }

    auto* object = Glib::wrap_auto(reinterpret_cast<GObject*>(const_cast<void*>(item)), false);
    return dynamic_cast<TrackRow const*>(object);
  }

  auto createRowSorter(RowCompareFn compare) -> Glib::RefPtr<Gtk::Sorter>
  {
    auto* comparePtr = new RowCompareFn{std::move(compare)};
    auto* customSorter = gtk_custom_sorter_new(
      [](gconstpointer lhs, gconstpointer rhs, gpointer userData) -> int
      {
        auto* compareFn = static_cast<RowCompareFn*>(userData);
        auto const* leftRow = trackRowFromItem(lhs);
        auto const* rightRow = trackRowFromItem(rhs);

        if (!compareFn || !leftRow || !rightRow)
        {
          return 0;
        }

        return (*compareFn)(*leftRow, *rightRow);
      },
      comparePtr,
      [](gpointer userData) { delete static_cast<RowCompareFn*>(userData); });

    return Glib::wrap(GTK_SORTER(customSorter), false);
  }

  auto isTagsCellWidget(Gtk::Widget const* widget) -> bool
  {
    for (auto current = widget; current != nullptr; current = current->get_parent())
    {
      if (current->has_css_class(kTagsCellWidgetName))
      {
        return true;
      }
    }

    return false;
  }

  auto dropdownPositionFor(TrackGroupBy groupBy) -> std::uint32_t
  {
    switch (groupBy)
    {
      case TrackGroupBy::None: return 0;
      case TrackGroupBy::Artist: return 1;
      case TrackGroupBy::Album: return 2;
      case TrackGroupBy::AlbumArtist: return 3;
      case TrackGroupBy::Genre: return 4;
      case TrackGroupBy::Year: return 5;
    }

    return 0;
  }

  auto groupByFromDropdownPosition(std::uint32_t position) -> TrackGroupBy
  {
    switch (position)
    {
      case 1: return TrackGroupBy::Artist;
      case 2: return TrackGroupBy::Album;
      case 3: return TrackGroupBy::AlbumArtist;
      case 4: return TrackGroupBy::Genre;
      case 5: return TrackGroupBy::Year;
      default: return TrackGroupBy::None;
    }
  }

  auto trackCountLabel(guint count) -> std::string
  {
    auto label = std::to_string(count);
    label += count == 1 ? " track" : " tracks";
    return label;
  }
}

TrackViewPage::TrackViewPage(Glib::RefPtr<TrackListAdapter> const& adapter)
  : Gtk::Box(Gtk::Orientation::VERTICAL)
  , _adapter(adapter)
  , _sortModel(Gtk::SortListModel::create(adapter->getModel(), Glib::RefPtr<Gtk::Sorter>{}))
  , _presentationSpec(presentationSpecForGroup(TrackGroupBy::None))
{
  // Create multi-selection model to allow bulk operations
  _selectionModel = Gtk::MultiSelection::create(_sortModel);

  setupPresentationControls();

  setupHeaderFactory();

  // Set up column view
  _columnView.set_model(_selectionModel);
  _contextPopover.set_has_arrow(false);
  _contextPopover.set_parent(_columnView);

  // Show row separators (horizontal lines between rows)
  _columnView.set_show_row_separators(true);

  // Connect selection signal - takes (position, nItems) parameters
  _selectionModel->signal_selection_changed().connect(sigc::mem_fun(*this, &TrackViewPage::onSelectionChanged));

  // Set up columns
  setupColumns();

  // Set up activation (double-click, Enter key)
  setupActivation();

  // Set up scrolled window
  _scrolledWindow.set_child(_columnView);
  _scrolledWindow.set_vexpand(true);
  _scrolledWindow.set_hexpand(true);

  applyPresentationSpec();

  // Add to box (order: controls, scroll)
  append(_controlsBar);
  append(_scrolledWindow);
}

TrackViewPage::~TrackViewPage() = default;

void TrackViewPage::setupPresentationControls()
{
  _controlsBar.set_spacing(8);
  _controlsBar.set_margin_start(4);
  _controlsBar.set_margin_end(4);
  _controlsBar.set_margin_top(4);
  _controlsBar.set_margin_bottom(4);

  _filterEntry.set_placeholder_text("Filter tracks...");
  _filterEntry.set_hexpand(true);
  _filterEntry.signal_changed().connect(sigc::mem_fun(*this, &TrackViewPage::onFilterChanged));

  _groupByLabel.set_text("Group");
  _groupByLabel.set_halign(Gtk::Align::START);
  _groupByLabel.set_valign(Gtk::Align::CENTER);

  _groupByOptions = Gtk::StringList::create({"None", "Artist", "Album", "Album Artist", "Genre", "Year"});
  _groupByDropdown.set_model(_groupByOptions);
  _groupByDropdown.set_selected(dropdownPositionFor(_presentationSpec.groupBy));
  _groupByDropdown.property_selected().signal_changed().connect(sigc::mem_fun(*this, &TrackViewPage::onGroupByChanged));

  _controlsBar.append(_filterEntry);
  _controlsBar.append(_groupByLabel);
  _controlsBar.append(_groupByDropdown);
}

void TrackViewPage::setupHeaderFactory()
{
  _sectionHeaderFactory = Gtk::SignalListItemFactory::create();

  _sectionHeaderFactory->signal_setup_obj().connect(
    [](Glib::RefPtr<Glib::Object> const& object)
    {
      auto header = std::dynamic_pointer_cast<Gtk::ListHeader>(object);
      if (!header)
      {
        return;
      }

      auto* label = Gtk::make_managed<Gtk::Label>("");
      label->set_halign(Gtk::Align::START);
      label->set_margin_start(8);
      label->set_margin_end(8);
      label->set_margin_top(8);
      label->set_margin_bottom(4);
      label->set_xalign(0.0F);
      header->set_child(*label);
    });

  _sectionHeaderFactory->signal_bind_obj().connect(
    [this](Glib::RefPtr<Glib::Object> const& object)
    {
      auto header = std::dynamic_pointer_cast<Gtk::ListHeader>(object);
      auto* label = header ? dynamic_cast<Gtk::Label*>(header->get_child()) : nullptr;

      if (!header || !label)
      {
        return;
      }

      auto item = header->get_item();
      auto row = std::dynamic_pointer_cast<TrackRow>(item);

      if (!row)
      {
        label->set_text("");
        return;
      }

      auto text = groupLabelFor(row->getPresentationKeys(), _presentationSpec.groupBy);
      if (!text.empty())
      {
        text += " ";
      }
      text += "(" + trackCountLabel(header->get_n_items()) + ")";
      label->set_text(text);
    });
}

void TrackViewPage::setupStatusBar()
{
  _statusLabel.set_visible(false);
  _statusLabel.set_halign(Gtk::Align::START);
  _statusLabel.set_valign(Gtk::Align::CENTER);
  _statusLabel.set_margin_start(4);
  _statusLabel.set_margin_end(4);
  _statusLabel.set_margin_top(2);
  _statusLabel.set_margin_bottom(2);
  // Style for error/info messages
  auto context = _statusLabel.get_style_context();
  context->add_class("dim-label");
}

void TrackViewPage::applyPresentationSpec()
{
  updateColumnVisibility();

  if (_presentationSpec.sortBy.empty())
  {
    _sortModel->set_sorter(Glib::RefPtr<Gtk::Sorter>{});
  }
  else
  {
    _sortModel->set_sorter(
      createRowSorter([spec = _presentationSpec](TrackRow const& lhs, TrackRow const& rhs)
                      { return compareForSort(lhs.getPresentationKeys(), rhs.getPresentationKeys(), spec.sortBy); }));
  }

  if (_presentationSpec.groupBy == TrackGroupBy::None)
  {
    _sortModel->set_section_sorter(Glib::RefPtr<Gtk::Sorter>{});
    _columnView.set_header_factory(Glib::RefPtr<Gtk::ListItemFactory>{});
    return;
  }

  _sortModel->set_section_sorter(
    createRowSorter([groupBy = _presentationSpec.groupBy](TrackRow const& lhs, TrackRow const& rhs)
                    { return compareForGrouping(lhs.getPresentationKeys(), rhs.getPresentationKeys(), groupBy); }));
  _columnView.set_header_factory(_sectionHeaderFactory);
}

void TrackViewPage::setStatusMessage(std::string const& message)
{
  if (message.empty())
  {
    clearStatusMessage();
    return;
  }
  _statusLabel.set_text(message);
  _statusLabel.set_visible(true);
}

void TrackViewPage::clearStatusMessage()
{
  _statusLabel.set_text("");
  _statusLabel.set_visible(false);
}

void TrackViewPage::setupColumns()
{
  // Artist column
  auto artistFactory = Gtk::SignalListItemFactory::create();
  artistFactory->signal_setup().connect(
    [](Glib::RefPtr<Gtk::ListItem> const& listItem)
    {
      auto* label = Gtk::make_managed<Gtk::Label>("");
      label->set_halign(Gtk::Align::START);
      label->set_ellipsize(Pango::EllipsizeMode::END);
      listItem->set_child(*label);
    });
  artistFactory->signal_bind().connect(
    [](Glib::RefPtr<Gtk::ListItem> const& listItem)
    {
      auto item = listItem->get_item();
      auto row = std::dynamic_pointer_cast<TrackRow>(item);
      auto label = dynamic_cast<Gtk::Label*>(listItem->get_child());
      if (row && label)
      {
        label->set_text(row->getArtist());
      }
    });

  _artistColumn = Gtk::ColumnViewColumn::create("Artist", artistFactory);
  _artistColumn->set_expand(false);
  _artistColumn->set_resizable(true);
  _artistColumn->set_fixed_width(150);
  _columnView.append_column(_artistColumn);

  // Album column
  auto albumFactory = Gtk::SignalListItemFactory::create();
  albumFactory->signal_setup().connect(
    [](Glib::RefPtr<Gtk::ListItem> const& listItem)
    {
      auto* label = Gtk::make_managed<Gtk::Label>("");
      label->set_halign(Gtk::Align::START);
      label->set_ellipsize(Pango::EllipsizeMode::END);
      listItem->set_child(*label);
    });
  albumFactory->signal_bind().connect(
    [](Glib::RefPtr<Gtk::ListItem> const& listItem)
    {
      auto item = listItem->get_item();
      auto row = std::dynamic_pointer_cast<TrackRow>(item);
      auto label = dynamic_cast<Gtk::Label*>(listItem->get_child());
      if (row && label)
      {
        label->set_text(row->getAlbum());
      }
    });

  _albumColumn = Gtk::ColumnViewColumn::create("Album", albumFactory);
  _albumColumn->set_expand(false);
  _albumColumn->set_resizable(true);
  _albumColumn->set_fixed_width(200);
  _columnView.append_column(_albumColumn);

  // Track number column
  auto trackNumberFactory = Gtk::SignalListItemFactory::create();
  trackNumberFactory->signal_setup().connect(
    [](Glib::RefPtr<Gtk::ListItem> const& listItem)
    {
      auto* label = Gtk::make_managed<Gtk::Label>("");
      label->set_halign(Gtk::Align::END);
      label->set_xalign(1.0F);
      listItem->set_child(*label);
    });
  trackNumberFactory->signal_bind().connect(
    [](Glib::RefPtr<Gtk::ListItem> const& listItem)
    {
      auto item = listItem->get_item();
      auto row = std::dynamic_pointer_cast<TrackRow>(item);
      auto label = dynamic_cast<Gtk::Label*>(listItem->get_child());
      if (row && label)
      {
        label->set_text(row->getDisplayNumber());
      }
    });

  _trackNumberColumn = Gtk::ColumnViewColumn::create("No.", trackNumberFactory);
  _trackNumberColumn->set_expand(false);
  _trackNumberColumn->set_resizable(true);
  _trackNumberColumn->set_fixed_width(72);
  _columnView.append_column(_trackNumberColumn);

  // Title column
  auto titleFactory = Gtk::SignalListItemFactory::create();
  titleFactory->signal_setup().connect(
    [](Glib::RefPtr<Gtk::ListItem> const& listItem)
    {
      auto* label = Gtk::make_managed<Gtk::Label>("");
      label->set_halign(Gtk::Align::START);
      label->set_ellipsize(Pango::EllipsizeMode::END);
      listItem->set_child(*label);
    });
  titleFactory->signal_bind().connect(
    [](Glib::RefPtr<Gtk::ListItem> const& listItem)
    {
      auto item = listItem->get_item();
      auto row = std::dynamic_pointer_cast<TrackRow>(item);
      auto label = dynamic_cast<Gtk::Label*>(listItem->get_child());
      if (row && label)
      {
        label->set_text(row->getTitle());
      }
    });

  _titleColumn = Gtk::ColumnViewColumn::create("Title", titleFactory);
  // Keep one flexible column so row selection does not cause GTK to rebalance
  // spare width across every column.
  _titleColumn->set_expand(false);
  _titleColumn->set_resizable(true);
  _titleColumn->set_fixed_width(-1);
  _columnView.append_column(_titleColumn);

  // Tags column
  auto tagsFactory = Gtk::SignalListItemFactory::create();
  tagsFactory->signal_setup().connect(
    [](Glib::RefPtr<Gtk::ListItem> const& listItem)
    {
      auto* box = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 0);
      box->set_halign(Gtk::Align::FILL);
      box->set_hexpand(true);
      box->add_css_class(kTagsCellWidgetName);

      auto* label = Gtk::make_managed<Gtk::Label>("");
      label->set_halign(Gtk::Align::START);
      label->set_ellipsize(Pango::EllipsizeMode::END);
      label->set_hexpand(true);

      box->append(*label);
      listItem->set_child(*box);
    });
  tagsFactory->signal_bind().connect(
    [](Glib::RefPtr<Gtk::ListItem> const& listItem)
    {
      auto item = listItem->get_item();
      auto row = std::dynamic_pointer_cast<TrackRow>(item);
      auto* box = dynamic_cast<Gtk::Box*>(listItem->get_child());
      if (row && box)
      {
        auto* label = dynamic_cast<Gtk::Label*>(box->get_first_child());
        if (label)
        {
          label->set_text(row->getTags());
        }
      }
    });

  _tagsColumn = Gtk::ColumnViewColumn::create("Tags", tagsFactory);
  _tagsColumn->set_expand(true);
  _tagsColumn->set_resizable(true);
  _tagsColumn->set_fixed_width(-1);
  _columnView.append_column(_tagsColumn);
}

void TrackViewPage::updateColumnVisibility()
{
  if (_artistColumn)
  {
    _artistColumn->set_visible(shouldShowColumn(_presentationSpec.groupBy, TrackColumn::Artist));
  }

  if (_albumColumn)
  {
    _albumColumn->set_visible(shouldShowColumn(_presentationSpec.groupBy, TrackColumn::Album));
  }

  if (_trackNumberColumn)
  {
    _trackNumberColumn->set_visible(shouldShowColumn(_presentationSpec.groupBy, TrackColumn::TrackNumber));
  }

  if (_titleColumn)
  {
    _titleColumn->set_visible(shouldShowColumn(_presentationSpec.groupBy, TrackColumn::Title));
  }

  if (_tagsColumn)
  {
    _tagsColumn->set_visible(shouldShowColumn(_presentationSpec.groupBy, TrackColumn::Tags));
  }
}

void TrackViewPage::onGroupByChanged()
{
  _presentationSpec = presentationSpecForGroup(groupByFromDropdownPosition(_groupByDropdown.get_selected()));
  applyPresentationSpec();
}

void TrackViewPage::onFilterChanged()
{
  auto filterText = _filterEntry.get_text();
  _adapter->setFilter(filterText);
}

void TrackViewPage::onSelectionChanged([[maybe_unused]] std::uint32_t position, [[maybe_unused]] std::uint32_t nItems)
{
  _selectionChanged.emit();
}

std::size_t TrackViewPage::selectedTrackCount() const
{
  auto count = std::size_t{0};
  auto model = _selectionModel->get_model();

  if (!model)
  {
    return count;
  }

  auto const nItems = model->get_n_items();
  for (std::uint32_t i = 0; i < nItems; ++i)
  {
    if (_selectionModel->is_selected(i))
    {
      ++count;
    }
  }

  return count;
}

std::optional<TrackViewPage::TrackId> TrackViewPage::trackIdAtPosition(std::uint32_t position) const
{
  if (!_selectionModel)
  {
    return std::nullopt;
  }

  auto item = _selectionModel->get_object(position);
  if (!item)
  {
    return std::nullopt;
  }

  auto row = std::dynamic_pointer_cast<TrackRow>(item);
  if (!row)
  {
    return std::nullopt;
  }

  return row->getTrackId();
}

std::vector<TrackListAdapter::TrackId> TrackViewPage::getVisibleTrackIds() const
{
  auto result = std::vector<TrackListAdapter::TrackId>{};

  auto model = _selectionModel->get_model();
  if (!model)
  {
    return result;
  }

  auto const nItems = model->get_n_items();
  result.reserve(nItems);

  for (std::uint32_t i = 0; i < nItems; ++i)
  {
    if (auto trackId = trackIdAtPosition(i))
    {
      result.push_back(*trackId);
    }
  }

  return result;
}

std::vector<TrackListAdapter::TrackId> TrackViewPage::getSelectedTrackIds() const
{
  auto result = std::vector<TrackListAdapter::TrackId>{};

  auto model = _selectionModel->get_model();

  if (!model)
  {
    return result;
  }

  // Iterate through all items and check if selected
  auto nItems = model->get_n_items();
  for (std::uint32_t i = 0; i < nItems; ++i)
  {
    if (_selectionModel->is_selected(i))
    {
      if (auto trackId = trackIdAtPosition(i))
      {
        result.push_back(*trackId);
      }
    }
  }

  return result;
}

std::chrono::milliseconds TrackViewPage::getSelectedTracksDuration() const
{
  std::chrono::milliseconds totalDuration{0};

  auto model = _selectionModel->get_model();
  if (!model)
  {
    return std::chrono::milliseconds{0};
  }

  auto nItems = model->get_n_items();
  for (std::uint32_t i = 0; i < nItems; ++i)
  {
    if (_selectionModel->is_selected(i))
    {
      auto item = _selectionModel->get_object(i);
      if (auto row = std::dynamic_pointer_cast<TrackRow>(item))
      {
        totalDuration += row->getDuration();
      }
    }
  }

  return totalDuration;
}

sigc::signal<void()>& TrackViewPage::signalSelectionChanged()
{
  return _selectionChanged;
}

sigc::signal<void(TrackViewPage::TrackId)>& TrackViewPage::signalTrackActivated()
{
  return _trackActivated;
}

sigc::signal<void(double, double)>& TrackViewPage::signalContextMenuRequested()
{
  return _contextMenuRequested;
}

sigc::signal<void(std::vector<TrackViewPage::TrackId>, double, double)>& TrackViewPage::signalTagEditRequested()
{
  return _tagEditRequested;
}

void TrackViewPage::showTagPopover(TagPopover& popover, double x, double y)
{
  auto rect = Gdk::Rectangle{static_cast<int>(x), static_cast<int>(y), 1, 1};
  popover.set_parent(_columnView);
  popover.set_pointing_to(rect);
  popover.popup();
}

void TrackViewPage::setupActivation()
{
  _columnView.set_focusable(true);
  _columnView.set_focus_on_click(true);

  // Built-in activation carries the exact row position that GTK activated.
  _columnView.signal_activate().connect(
    [this](std::uint32_t position)
    {
      if (_suppressNextTrackActivation)
      {
        _suppressNextTrackActivation = false;
        return;
      }

      if (auto trackId = trackIdAtPosition(position))
      {
        _trackActivated.emit(*trackId);
        return;
      }

      onActivateCurrentSelection();
    });

  // Keep an explicit Enter handler so activation still works when GTK focus is
  // on the view but no activate action is emitted automatically.
  auto keyController = Gtk::EventControllerKey::create();
  keyController->signal_key_pressed().connect(
    [this](guint keyval, guint, Gdk::ModifierType modifiers)
    {
      if (keyval == GDK_KEY_Return || keyval == GDK_KEY_KP_Enter)
      {
        onActivateCurrentSelection();
        return true;
      }
      // Ctrl+T opens tag edit popover
      if (keyval == GDK_KEY_t || keyval == GDK_KEY_T)
      {
        if (bool(modifiers & Gdk::ModifierType::CONTROL_MASK))
        {
          auto selectedIds = getSelectedTrackIds();
          if (!selectedIds.empty())
          {
            _tagEditRequested.emit(selectedIds, 0, 0);
          }
          return true;
        }
      }
      return false;
    },
    false);
  _columnView.add_controller(keyController);

  auto primaryClickController = Gtk::GestureClick::create();
  primaryClickController->set_button(GDK_BUTTON_PRIMARY);
  primaryClickController->set_propagation_phase(Gtk::PropagationPhase::CAPTURE);
  primaryClickController->signal_pressed().connect(
    [this, primaryClickController](int nPress, double x, double y)
    {
      if (nPress != 2)
      {
        return;
      }

      auto* target = _columnView.pick(x, y, Gtk::PickFlags::NON_TARGETABLE);
      if (!isTagsCellWidget(target))
      {
        return;
      }

      auto selectedIds = getSelectedTrackIds();
      if (selectedIds.empty())
      {
        return;
      }

      primaryClickController->set_state(Gtk::EventSequenceState::CLAIMED);
      _suppressNextTrackActivation = true;
      _tagEditRequested.emit(selectedIds, x, y);
    });
  _columnView.add_controller(primaryClickController);

  auto secondaryClickController = Gtk::GestureClick::create();
  secondaryClickController->set_button(GDK_BUTTON_SECONDARY);
  secondaryClickController->signal_released().connect(
    [this](int, double x, double y)
    {
      if (selectedTrackCount() == 0)
      {
        return;
      }

      _contextMenuRequested.emit(x, y);
    });
  _columnView.add_controller(secondaryClickController);
}

void TrackViewPage::onActivateCurrentSelection()
{
  if (_suppressNextTrackActivation)
  {
    _suppressNextTrackActivation = false;
    return;
  }

  auto trackId = getPrimarySelectedTrackId();
  if (trackId)
  {
    _trackActivated.emit(*trackId);
  }
}

std::optional<TrackViewPage::TrackId> TrackViewPage::getPrimarySelectedTrackId() const
{
  auto model = _selectionModel->get_model();

  if (!model)
  {
    return std::nullopt;
  }

  // Find first selected item
  auto nItems = model->get_n_items();
  for (std::uint32_t i = 0; i < nItems; ++i)
  {
    if (_selectionModel->is_selected(i))
    {
      if (auto trackId = trackIdAtPosition(i))
      {
        return trackId;
      }
      // Found first selected, no need to continue
      break;
    }
  }

  return std::nullopt;
}
