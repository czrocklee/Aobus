// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "list/ListSidebarPanel.h"

#include "list/ListTreeItem.h"
#include "list/ListTreeModelBuilder.h"
#include <ao/Type.h>
#include <ao/lmdb/Transaction.h>
#include <ao/rt/AppRuntime.h>

#include <gdk/gdk.h>
#include <gdkmm/graphene_point.h>
#include <gdkmm/rectangle.h>
#include <giomm/menu.h>
#include <glib.h>
#include <glibmm/refptr.h>
#include <gtkmm/box.h>
#include <gtkmm/enums.h>
#include <gtkmm/gestureclick.h>
#include <gtkmm/label.h>
#include <gtkmm/listitem.h>
#include <gtkmm/object.h>
#include <gtkmm/signallistitemfactory.h>
#include <gtkmm/treeexpander.h>
#include <gtkmm/treelistrow.h>
#include <pangomm/layout.h>
#include <sigc++/functors/mem_fun.h>

#include <cstdint>
#include <format>
#include <limits>
#include <memory>
#include <utility>

namespace ao::gtk
{
  namespace
  {
    constexpr guint kInvalidListPosition = std::numeric_limits<guint>::max();
  }

  ListSidebarPanel::ListSidebarPanel(Callbacks callbacks)
    : _callbacks{std::move(callbacks)}
  {
    _listScrolledWindow.set_policy(Gtk::PolicyType::NEVER, Gtk::PolicyType::AUTOMATIC);
    _listScrolledWindow.set_child(_listView);
    _listScrolledWindow.set_size_request(kSidebarWidth, -1);

    auto factoryPtr = Gtk::SignalListItemFactory::create();
    factoryPtr->signal_setup().connect([this](Glib::RefPtr<Gtk::ListItem> const& listItem)
                                       { setupSidebarListItem(listItem); });
    factoryPtr->signal_bind().connect([this](Glib::RefPtr<Gtk::ListItem> const& listItem)
                                      { bindSidebarListItem(listItem); });

    _listView.set_factory(factoryPtr);

    auto menuModelPtr = Gio::Menu::create();
    menuModelPtr->append("New Smart List...", "win.new");
    menuModelPtr->append("Edit List...", "win.edit");
    menuModelPtr->append("Delete List", "win.delete");
    _listContextMenu.set_menu_model(menuModelPtr);
    _listContextMenu.set_parent(_listView);
  }

  ListSidebarPanel::~ListSidebarPanel()
  {
    _listContextMenu.unparent();
  }

  void ListSidebarPanel::rebuildTree(rt::AppRuntime& runtime, lmdb::ReadTransaction const& txn)
  {
    auto result = ListTreeModelBuilder::build(runtime, txn);
    _nodesById = std::move(result.nodesById);
    _listTreeStorePtr = std::move(result.storePtr);
    _treeListModelPtr = std::move(result.treeModelPtr);
    _listSelectionModelPtr = std::move(result.selectionModelPtr);
    _listSelectionModelPtr->signal_selection_changed().connect(
      sigc::mem_fun(*this, &ListSidebarPanel::onListSelectionChanged));
    _listView.set_model(_listSelectionModelPtr);
  }

  void ListSidebarPanel::selectList(ListId listId)
  {
    if (!_treeListModelPtr)
    {
      return;
    }

    auto const itemCount = _treeListModelPtr->get_n_items();

    for (guint idx = 0; idx < itemCount; ++idx)
    {
      auto itemPtr = _treeListModelPtr->get_object(idx);
      auto treeListRowPtr = std::dynamic_pointer_cast<Gtk::TreeListRow>(itemPtr);

      if (!treeListRowPtr)
      {
        continue;
      }

      auto nodePtr = std::dynamic_pointer_cast<ListTreeItem>(treeListRowPtr->get_item());

      if (nodePtr && nodePtr->listId() == listId)
      {
        _listSelectionModelPtr->set_selected(idx);
        break;
      }
    }
  }

  bool ListSidebarPanel::listHasChildren(ListId listId) const
  {
    if (auto it = _nodesById.find(listId); it != _nodesById.end())
    {
      return it->second->hasChildren();
    }

    return false;
  }

  ListId ListSidebarPanel::selectedListId() const
  {
    if (_listSelectionModelPtr == nullptr)
    {
      return kInvalidListId;
    }

    auto const selectedPosition = _listSelectionModelPtr->get_selected();

    if (selectedPosition == kInvalidListPosition)
    {
      return kInvalidListId;
    }

    auto const treeListRowPtr =
      std::dynamic_pointer_cast<Gtk::TreeListRow>(_listSelectionModelPtr->get_selected_item());

    if (treeListRowPtr == nullptr)
    {
      return kInvalidListId;
    }

    auto const nodePtr = std::dynamic_pointer_cast<ListTreeItem>(treeListRowPtr->get_item());

    if (nodePtr == nullptr)
    {
      return kInvalidListId;
    }

    return nodePtr->listId();
  }

  void ListSidebarPanel::showContextMenu(Gdk::Rectangle const& rect)
  {
    _listContextMenu.set_pointing_to(rect);
    _listContextMenu.popup();
  }

  void ListSidebarPanel::setupSidebarListItem(Glib::RefPtr<Gtk::ListItem> const& listItem)
  {
    auto* rowBox = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL);
    rowBox->set_halign(Gtk::Align::FILL);
    rowBox->set_hexpand(true);
    rowBox->add_css_class("ao-sidebar-row");

    auto* expander = Gtk::make_managed<Gtk::TreeExpander>();
    rowBox->append(*expander);

    auto* label = Gtk::make_managed<Gtk::Label>("");
    label->set_halign(Gtk::Align::START);
    rowBox->append(*label);

    auto* filterLabel = Gtk::make_managed<Gtk::Label>("");
    filterLabel->set_halign(Gtk::Align::START);
    filterLabel->add_css_class("dim-label");
    filterLabel->add_css_class("ao-sidebar-filter-label");
    filterLabel->set_ellipsize(Pango::EllipsizeMode::END);
    filterLabel->set_hexpand(true);
    rowBox->append(*filterLabel);

    auto clickControllerPtr = Gtk::GestureClick::create();
    clickControllerPtr->set_button(GDK_BUTTON_SECONDARY);
    clickControllerPtr->signal_pressed().connect(
      [this, listItem, rowBox](std::int32_t /*nPress*/, double xPos, double yPos)
      {
        if (auto const position = listItem->get_position(); position != kInvalidListPosition)
        {
          _listSelectionModelPtr->set_selected(position);
        }

        auto optPoint =
          rowBox->compute_point(_listView, Gdk::Graphene::Point{static_cast<float>(xPos), static_cast<float>(yPos)});

        if (!optPoint)
        {
          return;
        }

        auto rect = Gdk::Rectangle{
          static_cast<std::int32_t>(optPoint->get_x()), static_cast<std::int32_t>(optPoint->get_y()), 1, 1};

        if (_callbacks.onContextMenuRequested)
        {
          _callbacks.onContextMenuRequested(selectedListId(), rect);
        }
      });

    rowBox->add_controller(clickControllerPtr);
    listItem->set_child(*rowBox);
  }

  void ListSidebarPanel::bindSidebarListItem(Glib::RefPtr<Gtk::ListItem> const& listItem)
  {
    auto treeListRowPtr = std::dynamic_pointer_cast<Gtk::TreeListRow>(listItem->get_item());

    if (!treeListRowPtr)
    {
      return;
    }

    auto nodePtr = std::dynamic_pointer_cast<ListTreeItem>(treeListRowPtr->get_item());

    if (!nodePtr)
    {
      return;
    }

    auto* box = dynamic_cast<Gtk::Box*>(listItem->get_child());
    auto* expander = box != nullptr ? dynamic_cast<Gtk::TreeExpander*>(box->get_first_child()) : nullptr;
    auto* label = expander != nullptr ? dynamic_cast<Gtk::Label*>(expander->get_next_sibling()) : nullptr;
    auto* filterLabel = label != nullptr ? dynamic_cast<Gtk::Label*>(label->get_next_sibling()) : nullptr;

    if (expander != nullptr)
    {
      expander->set_list_row(treeListRowPtr);
    }

    if (label == nullptr)
    {
      return;
    }

    auto rowPtr = nodePtr->row();

    if (!rowPtr)
    {
      return;
    }

    label->set_text(rowPtr->name());

    if (filterLabel == nullptr)
    {
      return;
    }

    if (auto const filter = rowPtr->filter(); !filter.empty())
    {
      filterLabel->set_text(std::format("[{}]", filter.raw()));
      filterLabel->set_visible(true);
    }
    else
    {
      filterLabel->set_text("");
      filterLabel->set_visible(false);
    }
  }

  void ListSidebarPanel::onListSelectionChanged(std::uint32_t /*position*/, std::uint32_t /*nItems*/) const
  {
    if (_callbacks.onSelectionChanged)
    {
      _callbacks.onSelectionChanged(selectedListId());
    }
  }
} // namespace ao::gtk
