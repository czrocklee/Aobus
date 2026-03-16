// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#include "TrackViewPage.h"

#include <gtkmm/columnviewcolumn.h>
#include <gtkmm/listitem.h>
#include <gtkmm/signallistitemfactory.h>

#include <cstdint>
#include <vector>

TrackViewPage::TrackViewPage(Glib::RefPtr<TrackListAdapter> const& adapter)
  : Gtk::Box(Gtk::Orientation::VERTICAL)
  , _adapter(adapter)
{
  // Create multi-selection model to allow bulk operations
  _selectionModel = Gtk::MultiSelection::create(adapter->getModel());

  // Set up filter entry
  _filterEntry.set_placeholder_text("Filter tracks...");
  _filterEntry.signal_changed().connect(sigc::mem_fun(*this, &TrackViewPage::onFilterChanged));

  // Set up column view
  _columnView.set_model(_selectionModel);

  // Show row separators (horizontal lines between rows)
  _columnView.set_show_row_separators(true);

  // Connect selection signal - takes (position, nItems) parameters
  _selectionModel->signal_selection_changed().connect(sigc::mem_fun(*this, &TrackViewPage::onSelectionChanged));

  // Set up columns
  setupColumns();

  // Set up scrolled window
  _scrolledWindow.set_child(_columnView);
  _scrolledWindow.set_vexpand(true);
  _scrolledWindow.set_hexpand(true);

  // Add to box
  append(_filterEntry);
  append(_scrolledWindow);
}

TrackViewPage::~TrackViewPage() = default;

void TrackViewPage::setupColumns()
{
  // Artist column
  auto artistFactory = Gtk::SignalListItemFactory::create();
  artistFactory->signal_setup().connect([](Glib::RefPtr<Gtk::ListItem> const& listItem) {
    auto* label = Gtk::make_managed<Gtk::Label>("");
    label->set_halign(Gtk::Align::START);
    listItem->set_child(*label);
  });
  artistFactory->signal_bind().connect([](Glib::RefPtr<Gtk::ListItem> const& listItem) {
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
  albumFactory->signal_setup().connect([](Glib::RefPtr<Gtk::ListItem> const& listItem) {
    auto* label = Gtk::make_managed<Gtk::Label>("");
    label->set_halign(Gtk::Align::START);
    listItem->set_child(*label);
  });
  albumFactory->signal_bind().connect([](Glib::RefPtr<Gtk::ListItem> const& listItem) {
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
  titleFactory->signal_setup().connect([](Glib::RefPtr<Gtk::ListItem> const& listItem) {
    auto* label = Gtk::make_managed<Gtk::Label>("");
    label->set_halign(Gtk::Align::START);
    listItem->set_child(*label);
  });
  titleFactory->signal_bind().connect([](Glib::RefPtr<Gtk::ListItem> const& listItem) {
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
  tagsFactory->signal_setup().connect([](Glib::RefPtr<Gtk::ListItem> const& listItem) {
    auto* label = Gtk::make_managed<Gtk::Label>("");
    label->set_halign(Gtk::Align::START);
    listItem->set_child(*label);
  });
  tagsFactory->signal_bind().connect([](Glib::RefPtr<Gtk::ListItem> const& listItem) {
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

void TrackViewPage::onFilterChanged()
{
  auto filterText = _filterEntry.get_text();
  _adapter->setFilter(filterText);
}

void TrackViewPage::onSelectionChanged([[maybe_unused]] std::uint32_t position, [[maybe_unused]] std::uint32_t nItems)
{
  _selectionChanged.emit();
}

std::vector<TrackListAdapter::TrackId> TrackViewPage::getSelectedTrackIds() const
{
  std::vector<TrackListAdapter::TrackId> result;

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
      auto item = model->get_object(i);
      if (item)
      {
        auto row = std::dynamic_pointer_cast<TrackRow>(item);
        if (row)
        {
          result.push_back(row->getTrackId());
        }
      }
    }
  }

  return result;
}

sigc::signal<void()>& TrackViewPage::signalSelectionChanged() { return _selectionChanged; }
