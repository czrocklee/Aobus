// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/CoreIds.h>

#include <gdkmm/rectangle.h>
#include <glibmm/refptr.h>
#include <gtkmm/listitem.h>
#include <gtkmm/listview.h>
#include <gtkmm/popovermenu.h>
#include <gtkmm/scrolledwindow.h>
#include <gtkmm/singleselection.h>
#include <gtkmm/treelistmodel.h>
#include <gtkmm/widget.h>

#include <cstdint>
#include <functional>
#include <map>

namespace Gio
{
  template<typename T>
  class ListStore;
}

namespace ao::rt
{
  class Library;
}

namespace ao::gtk
{
  class ListTreeItem;

  class ListNavigationPanel final
  {
  public:
    struct Callbacks final
    {
      std::function<void(ListId)> onSelectionChanged;
      std::function<void(ListId, Gdk::Rectangle const&)> onContextMenuRequested;
    };

    ListNavigationPanel(Callbacks callbacks);
    ~ListNavigationPanel();

    // Not copyable or movable
    ListNavigationPanel(ListNavigationPanel const&) = delete;
    ListNavigationPanel& operator=(ListNavigationPanel const&) = delete;
    ListNavigationPanel(ListNavigationPanel&&) = delete;
    ListNavigationPanel& operator=(ListNavigationPanel&&) = delete;

    Gtk::Widget& widget() { return _listScrolledWindow; }

    void rebuildTree(rt::Library const& reads);
    void selectList(ListId listId);
    bool hasListChildren(ListId listId) const;
    ListId selectedListId() const;

    void openContextMenu(Gdk::Rectangle const& rect);

  private:
    void setupNavListItem(Glib::RefPtr<Gtk::ListItem> const& listItemPtr);
    void bindNavListItem(Glib::RefPtr<Gtk::ListItem> const& listItemPtr);
    void handleListSelectionChanged(std::uint32_t position, std::uint32_t nItems) const;

    Callbacks _callbacks;

    Gtk::ListView _listView;
    Gtk::ScrolledWindow _listScrolledWindow;
    Gtk::PopoverMenu _listContextMenu;

    Glib::RefPtr<Gio::ListStore<ListTreeItem>> _listTreeStorePtr;
    Glib::RefPtr<Gtk::TreeListModel> _treeListModelPtr;
    Glib::RefPtr<Gtk::SingleSelection> _listSelectionModelPtr;
    std::map<ListId, Glib::RefPtr<ListTreeItem>> _nodesById;
  };
} // namespace ao::gtk
