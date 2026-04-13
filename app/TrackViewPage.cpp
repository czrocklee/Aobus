// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#include "TrackViewPage.h"

#include <glibmm/wrap.h>
#include <gtk/gtk.h>

#include <gdk/gdk.h>

#include <gtkmm/label.h>
#include <gtkmm/listheader.h>
#include <gtkmm/columnviewcolumn.h>
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
      [](gpointer userData)
      {
        delete static_cast<RowCompareFn*>(userData);
      });

    return Glib::wrap(GTK_SORTER(customSorter), false);
  }

  auto dropdownPositionFor(TrackGroupBy groupBy) -> std::uint32_t
  {
    switch (groupBy)
    {
      case TrackGroupBy::None:
        return 0;
      case TrackGroupBy::Artist:
        return 1;
      case TrackGroupBy::Album:
        return 2;
      case TrackGroupBy::AlbumArtist:
        return 3;
      case TrackGroupBy::Genre:
        return 4;
      case TrackGroupBy::Year:
        return 5;
    }

    return 0;
  }

  auto groupByFromDropdownPosition(std::uint32_t position) -> TrackGroupBy
  {
    switch (position)
    {
      case 1:
        return TrackGroupBy::Artist;
      case 2:
        return TrackGroupBy::Album;
      case 3:
        return TrackGroupBy::AlbumArtist;
      case 4:
        return TrackGroupBy::Genre;
      case 5:
        return TrackGroupBy::Year;
      default:
        return TrackGroupBy::None;
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

  // Set up status bar (hidden by default)
  setupStatusBar();

  setupHeaderFactory();

  // Set up column view
  _columnView.set_model(_selectionModel);

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

  // Add to box (order: controls, status, scroll)
  append(_controlsBar);
  append(_statusLabel);
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

  _sectionHeaderFactory->signal_setup_obj().connect([](Glib::RefPtr<Glib::Object> const& object)
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

  _sectionHeaderFactory->signal_bind_obj().connect([this](Glib::RefPtr<Glib::Object> const& object)
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
  if (_presentationSpec.sortBy.empty())
  {
    _sortModel->set_sorter(Glib::RefPtr<Gtk::Sorter>{});
  }
  else
  {
    _sortModel->set_sorter(createRowSorter(
      [spec = _presentationSpec](TrackRow const& lhs, TrackRow const& rhs)
      {
        return compareForSort(lhs.getPresentationKeys(), rhs.getPresentationKeys(), spec.sortBy);
      }));
  }

  if (_presentationSpec.groupBy == TrackGroupBy::None)
  {
    _sortModel->set_section_sorter(Glib::RefPtr<Gtk::Sorter>{});
    _columnView.set_header_factory(Glib::RefPtr<Gtk::ListItemFactory>{});
    return;
  }

  _sortModel->set_section_sorter(createRowSorter(
    [groupBy = _presentationSpec.groupBy](TrackRow const& lhs, TrackRow const& rhs)
    {
      return compareForGrouping(lhs.getPresentationKeys(), rhs.getPresentationKeys(), groupBy);
    }));
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
  artistFactory->signal_setup().connect([](Glib::RefPtr<Gtk::ListItem> const& listItem)
  {
    auto* label = Gtk::make_managed<Gtk::Label>("");
    label->set_halign(Gtk::Align::START);
    listItem->set_child(*label);
  });
  artistFactory->signal_bind().connect([](Glib::RefPtr<Gtk::ListItem> const& listItem)
  {
    auto item = listItem->get_item();
    auto row = std::dynamic_pointer_cast<TrackRow>(item);
    auto label = dynamic_cast<Gtk::Label*>(listItem->get_child());
    if (row && label)
    {
      label->set_text(row->getArtist());
    }
  });

  auto artistColumn = Gtk::ColumnViewColumn::create("Artist", artistFactory);
  artistColumn->set_expand(true);
  artistColumn->set_resizable(true);
  _columnView.append_column(artistColumn);

  // Album column
  auto albumFactory = Gtk::SignalListItemFactory::create();
  albumFactory->signal_setup().connect([](Glib::RefPtr<Gtk::ListItem> const& listItem)
  {
    auto* label = Gtk::make_managed<Gtk::Label>("");
    label->set_halign(Gtk::Align::START);
    listItem->set_child(*label);
  });
  albumFactory->signal_bind().connect([](Glib::RefPtr<Gtk::ListItem> const& listItem)
  {
    auto item = listItem->get_item();
    auto row = std::dynamic_pointer_cast<TrackRow>(item);
    auto label = dynamic_cast<Gtk::Label*>(listItem->get_child());
    if (row && label)
    {
      label->set_text(row->getAlbum());
    }
  });

  auto albumColumn = Gtk::ColumnViewColumn::create("Album", albumFactory);
  albumColumn->set_expand(true);
  albumColumn->set_resizable(true);
  _columnView.append_column(albumColumn);

  // Title column
  auto titleFactory = Gtk::SignalListItemFactory::create();
  titleFactory->signal_setup().connect([](Glib::RefPtr<Gtk::ListItem> const& listItem)
  {
    auto* label = Gtk::make_managed<Gtk::Label>("");
    label->set_halign(Gtk::Align::START);
    listItem->set_child(*label);
  });
  titleFactory->signal_bind().connect([](Glib::RefPtr<Gtk::ListItem> const& listItem)
  {
    auto item = listItem->get_item();
    auto row = std::dynamic_pointer_cast<TrackRow>(item);
    auto label = dynamic_cast<Gtk::Label*>(listItem->get_child());
    if (row && label)
    {
      label->set_text(row->getTitle());
    }
  });

  auto titleColumn = Gtk::ColumnViewColumn::create("Title", titleFactory);
  titleColumn->set_expand(true);
  titleColumn->set_resizable(true);
  _columnView.append_column(titleColumn);

  // Tags column
  auto tagsFactory = Gtk::SignalListItemFactory::create();
  tagsFactory->signal_setup().connect([](Glib::RefPtr<Gtk::ListItem> const& listItem)
  {
    auto* label = Gtk::make_managed<Gtk::Label>("");
    label->set_halign(Gtk::Align::START);
    listItem->set_child(*label);
  });
  tagsFactory->signal_bind().connect([](Glib::RefPtr<Gtk::ListItem> const& listItem)
  {
    auto item = listItem->get_item();
    auto row = std::dynamic_pointer_cast<TrackRow>(item);
    auto label = dynamic_cast<Gtk::Label*>(listItem->get_child());
    if (row && label)
    {
      label->set_text(row->getTags());
    }
  });

  auto tagsColumn = Gtk::ColumnViewColumn::create("Tags", tagsFactory);
  tagsColumn->set_expand(true);
  tagsColumn->set_resizable(true);
  _columnView.append_column(tagsColumn);
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

sigc::signal<void()>& TrackViewPage::signalSelectionChanged()
{
  return _selectionChanged;
}

sigc::signal<void(TrackViewPage::TrackId)>& TrackViewPage::signalTrackActivated()
{
  return _trackActivated;
}

void TrackViewPage::setupActivation()
{
  _columnView.set_focusable(true);
  _columnView.set_focus_on_click(true);

  // Built-in activation carries the exact row position that GTK activated.
  _columnView.signal_activate().connect([this](std::uint32_t position)
  {
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
    [this](guint keyval, guint, Gdk::ModifierType)
  {
    if (keyval == GDK_KEY_Return || keyval == GDK_KEY_KP_Enter)
    {
      onActivateCurrentSelection();
      return true;
    }
    return false;
  },
    false);
  _columnView.add_controller(keyController);

  // Use release rather than press so GTK has already updated selection when we
  // inspect the currently selected row.
  auto clickController = Gtk::GestureClick::create();
  clickController->set_button(GDK_BUTTON_PRIMARY);
  clickController->signal_released().connect([this](int nPress, double, double)
  {
    if (nPress == 2)
    {
      onActivateCurrentSelection();
    }
  });
  _columnView.add_controller(clickController);
}

void TrackViewPage::onActivateCurrentSelection()
{
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
