// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "list/ListSidebarPanel.h"
#include "layout/LayoutConstants.h"
#include "list/ListRowObject.h"
#include "list/ListTreeItem.h"
#include "list/ListTreeModelBuilder.h"
#include <runtime/AppSession.h>

namespace ao::gtk
{
  ListSidebarPanel::ListSidebarPanel(Callbacks callbacks)
    : _callbacks{std::move(callbacks)}
  {
    _listScrolledWindow.set_policy(Gtk::PolicyType::NEVER, Gtk::PolicyType::AUTOMATIC);
    _listScrolledWindow.set_child(_listView);
    _listScrolledWindow.set_size_request(kSidebarWidth, -1);

    auto factory = Gtk::SignalListItemFactory::create();
    factory->signal_setup().connect([this](Glib::RefPtr<Gtk::ListItem> const& listItem)
                                    { setupSidebarListItem(listItem); });
    factory->signal_bind().connect([this](Glib::RefPtr<Gtk::ListItem> const& listItem)
                                   { bindSidebarListItem(listItem); });

    _listView.set_factory(factory);

    auto menuModel = Gio::Menu::create();
    menuModel->append("New Smart List...", "win.new");
    menuModel->append("Edit List...", "win.edit");
    menuModel->append("Delete List", "win.delete");
    _listContextMenu.set_menu_model(menuModel);
    _listContextMenu.set_parent(_listView);
  }

  ListSidebarPanel::~ListSidebarPanel() = default;

  void ListSidebarPanel::rebuildTree(ao::rt::AppSession& session, ao::lmdb::ReadTransaction const& txn)
  {
    auto result = ListTreeModelBuilder::build(session, txn);
    _nodesById = std::move(result.nodesById);
    _listTreeStore = std::move(result.store);
    _treeListModel = std::move(result.treeModel);
    _listSelectionModel = std::move(result.selectionModel);
    _listSelectionModel->signal_selection_changed().connect(
      sigc::mem_fun(*this, &ListSidebarPanel::onListSelectionChanged));
    _listView.set_model(_listSelectionModel);
  }

  void ListSidebarPanel::selectList(ListId listId)
  {
    if (!_treeListModel)
    {
      return;
    }

    auto const itemCount = _treeListModel->get_n_items();

    for (guint index = 0; index < itemCount; ++index)
    {
      auto item = _treeListModel->get_object(index);
      if (!item)
      {
        continue;
      }

      auto treeListRow = std::dynamic_pointer_cast<Gtk::TreeListRow>(item);
      if (!treeListRow)
      {
        continue;
      }

      auto node = std::dynamic_pointer_cast<ListTreeItem>(treeListRow->get_item());
      if (!node)
      {
        continue;
      }

      if (node->getListId() == listId)
      {
        _listSelectionModel->set_selected(index);
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
    if (_listSelectionModel == nullptr)
    {
      return ListId{0};
    }

    auto const selectedPosition = _listSelectionModel->get_selected();

    if (selectedPosition == GTK_INVALID_LIST_POSITION)
    {
      return ListId{0};
    }

    auto const treeListRow = std::dynamic_pointer_cast<Gtk::TreeListRow>(_listSelectionModel->get_selected_item());

    if (treeListRow == nullptr)
    {
      return ListId{0};
    }

    auto const node = std::dynamic_pointer_cast<ListTreeItem>(treeListRow->get_item());

    if (node == nullptr)
    {
      return ListId{0};
    }

    return node->getListId();
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
    rowBox->set_margin_start(layout::kMarginMedium);
    rowBox->set_margin_end(layout::kMarginMedium);
    rowBox->set_margin_top(layout::kMarginSmall);
    rowBox->set_margin_bottom(layout::kMarginSmall);

    auto* expander = Gtk::make_managed<Gtk::TreeExpander>();
    rowBox->append(*expander);

    auto* label = Gtk::make_managed<Gtk::Label>("");
    label->set_halign(Gtk::Align::START);
    rowBox->append(*label);

    auto* filterLabel = Gtk::make_managed<Gtk::Label>("");
    filterLabel->set_halign(Gtk::Align::START);
    filterLabel->add_css_class("dim-label");
    filterLabel->set_margin_start(layout::kMarginMedium);
    filterLabel->set_ellipsize(Pango::EllipsizeMode::END);
    filterLabel->set_hexpand(true);
    rowBox->append(*filterLabel);

    auto clickController = Gtk::GestureClick::create();
    clickController->set_button(GDK_BUTTON_SECONDARY);
    clickController->signal_pressed().connect(
      [this, listItem, rowBox](int /*nPress*/, double xPos, double yPos)
      {
        if (auto const position = listItem->get_position(); position != GTK_INVALID_LIST_POSITION)
        {
          _listSelectionModel->set_selected(position);
        }

        auto point =
          rowBox->compute_point(_listView, Gdk::Graphene::Point(static_cast<float>(xPos), static_cast<float>(yPos)));

        if (!point)
        {
          return;
        }

        auto rect = Gdk::Rectangle(static_cast<int>(point->get_x()), static_cast<int>(point->get_y()), 1, 1);

        if (_callbacks.onContextMenuRequested)
        {
          _callbacks.onContextMenuRequested(selectedListId(), rect);
        }
      });

    rowBox->add_controller(clickController);
    listItem->set_child(*rowBox);
  }

  void ListSidebarPanel::bindSidebarListItem(Glib::RefPtr<Gtk::ListItem> const& listItem)
  {
    auto treeListRow = std::dynamic_pointer_cast<Gtk::TreeListRow>(listItem->get_item());
    if (!treeListRow)
    {
      return;
    }

    auto node = std::dynamic_pointer_cast<ListTreeItem>(treeListRow->get_item());
    if (!node)
    {
      return;
    }

    auto* box = dynamic_cast<Gtk::Box*>(listItem->get_child());
    auto* expander = box != nullptr ? dynamic_cast<Gtk::TreeExpander*>(box->get_first_child()) : nullptr;
    auto* label = expander != nullptr ? dynamic_cast<Gtk::Label*>(expander->get_next_sibling()) : nullptr;
    auto* filterLabel = label != nullptr ? dynamic_cast<Gtk::Label*>(label->get_next_sibling()) : nullptr;

    if (expander != nullptr)
    {
      expander->set_list_row(treeListRow);
    }

    if (label == nullptr)
    {
      return;
    }

    auto row = node->getRow();
    if (!row)
    {
      return;
    }

    label->set_text(row->getName());

    if (filterLabel == nullptr)
    {
      return;
    }

    auto const filter = row->getFilter();
    if (!filter.empty())
    {
      filterLabel->set_text("[" + filter + "]");
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
