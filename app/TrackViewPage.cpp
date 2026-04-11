// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#include "TrackViewPage.h"

#include <gdk/gdk.h>

#include <gtkmm/columnviewcolumn.h>
#include <gtkmm/listitem.h>
#include <gtkmm/signallistitemfactory.h>

#include <cstdint>
#include <vector>

namespace
{
  std::optional<TrackListAdapter::TrackId> trackIdAtPosition(Glib::RefPtr<Gtk::MultiSelection> const& selectionModel,
                                                             std::uint32_t position)
  {
    if (!selectionModel)
    {
      return std::nullopt;
    }

    auto item = selectionModel->get_object(position);
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
}

TrackViewPage::TrackViewPage(Glib::RefPtr<TrackListAdapter> const& adapter)
  : Gtk::Box(Gtk::Orientation::VERTICAL), _adapter(adapter)
{
  // Create multi-selection model to allow bulk operations
  _selectionModel = Gtk::MultiSelection::create(adapter->getModel());

  // Set up filter entry
  _filterEntry.set_placeholder_text("Filter tracks...");
  _filterEntry.signal_changed().connect(sigc::mem_fun(*this, &TrackViewPage::onFilterChanged));

  // Set up status bar (hidden by default)
  setupStatusBar();

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

  // Add to box (order: filter, status, scroll)
  append(_filterEntry);
  append(_statusLabel);
  append(_scrolledWindow);
}

TrackViewPage::~TrackViewPage() = default;

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
    if (auto trackId = trackIdAtPosition(_selectionModel, position))
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
      auto item = model->get_object(i);
      if (item)
      {
        auto row = std::dynamic_pointer_cast<TrackRow>(item);
        if (row)
        {
          return row->getTrackId();
        }
      }
      // Found first selected, no need to continue
      break;
    }
  }

  return std::nullopt;
}
