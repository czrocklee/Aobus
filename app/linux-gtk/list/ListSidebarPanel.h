// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/library/MusicLibrary.h>

#include <gtkmm.h>

#include <functional>
#include <map>
#include <memory>

namespace ao::rt
{
  class AppSession;
}

namespace ao::lmdb
{
  class ReadTransaction;
}

namespace ao::gtk
{
  class ListTreeItem;

  class ListSidebarPanel final
  {
  public:
    struct Callbacks final
    {
      std::function<void(ListId)> onSelectionChanged;
      std::function<void(ListId, Gdk::Rectangle const&)> onContextMenuRequested;
    };

    ListSidebarPanel(Callbacks callbacks);
    ~ListSidebarPanel();

    // Not copyable or movable
    ListSidebarPanel(ListSidebarPanel const&) = delete;
    ListSidebarPanel& operator=(ListSidebarPanel const&) = delete;
    ListSidebarPanel(ListSidebarPanel&&) = delete;
    ListSidebarPanel& operator=(ListSidebarPanel&&) = delete;

    Gtk::Widget& widget() { return _listScrolledWindow; }

    void rebuildTree(rt::AppSession& session, lmdb::ReadTransaction const& txn);
    void selectList(ListId listId);
    bool listHasChildren(ListId listId) const;
    ListId selectedListId() const;

    void showContextMenu(Gdk::Rectangle const& rect);

  private:
    static constexpr int kSidebarWidth = 200;

    void setupSidebarListItem(Glib::RefPtr<Gtk::ListItem> const& listItem);
    void bindSidebarListItem(Glib::RefPtr<Gtk::ListItem> const& listItem);
    void onListSelectionChanged(std::uint32_t position, std::uint32_t nItems) const;

    Callbacks _callbacks;

    Gtk::ListView _listView;
    Gtk::ScrolledWindow _listScrolledWindow;
    Gtk::PopoverMenu _listContextMenu;

    Glib::RefPtr<Gio::ListStore<ListTreeItem>> _listTreeStore;
    Glib::RefPtr<Gtk::TreeListModel> _treeListModel;
    Glib::RefPtr<Gtk::SingleSelection> _listSelectionModel;
    std::map<ListId, Glib::RefPtr<ListTreeItem>> _nodesById;
  };
} // namespace ao::gtk
