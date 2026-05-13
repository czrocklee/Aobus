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
      std::function<void(ao::ListId)> onSelectionChanged;
      std::function<void(ao::ListId, Gdk::Rectangle const&)> onContextMenuRequested;
    };

    ListSidebarPanel(Callbacks callbacks);
    ~ListSidebarPanel();

    Gtk::Widget& widget() { return _listScrolledWindow; }

    void rebuildTree(ao::rt::AppSession& session, ao::lmdb::ReadTransaction const& txn);
    void selectList(ao::ListId listId);
    bool listHasChildren(ao::ListId listId) const;
    ao::ListId selectedListId() const;

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
    std::map<ao::ListId, Glib::RefPtr<ListTreeItem>> _nodesById;
  };
} // namespace ao::gtk
