// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 RockStudio Contributors

#pragma once

#include "platform/linux/ui/LibrarySession.h"

#include <gtkmm.h>
#include <rs/library/MusicLibrary.h>
#include <rs/model/ListDraft.h>

#include <functional>
#include <map>
#include <memory>
#include <string>

namespace app::ui
{
  class ListTreeNode;

  /**
   * ListSidebarController manages the sidebar tree model, selection, and list CRUD.
   */
  class ListSidebarController final
  {
  public:
    struct Callbacks final
    {
      std::function<void(rs::ListId)> onListSelected;
      std::function<void()> onListsChanged;
      std::function<void(rs::ListId)> onListCreatedAndSelected;
      std::function<rs::model::TrackIdList*(rs::ListId)> getListMembership;
    };

    ListSidebarController(Gtk::Window& parent, Callbacks callbacks);
    ~ListSidebarController();

    Gtk::Widget& widget() { return _listScrolledWindow; }

    void rebuildTree(LibrarySession& session, rs::lmdb::ReadTransaction& txn);
    void select(rs::ListId listId);
    
    // Add to action group for menu access
    void addActionsTo(Gio::ActionMap& actionMap);

  private:
    void setupLayout();
    void setupSidebarListItem(Glib::RefPtr<Gtk::ListItem> const& listItem);
    void bindSidebarListItem(Glib::RefPtr<Gtk::ListItem> const& listItem);
    void onListSelectionChanged(std::uint32_t position, std::uint32_t nItems);

    // List context menu
    void showListContextMenu(Gtk::ListView& listView, Gdk::Rectangle const& rect);

    // List management - using ListDraft
    void openNewListDialog(rs::ListId parentListId);
    void openNewSmartListDialog();
    void openEditListDialog(rs::ListId listId);
    bool listHasChildren(rs::ListId listId) const;

    void createList(rs::model::ListDraft const& draft);
    void updateList(rs::model::ListDraft const& draft);
    void onDeleteList();
    void onEditList();

    void buildListTree(rs::lmdb::ReadTransaction& txn);
    void selectSidebarList(rs::ListId listId);

    Gtk::Window& _parent;
    Callbacks _callbacks;
    LibrarySession* _currentSession = nullptr;

    Gtk::ListView _listView;
    Gtk::ScrolledWindow _listScrolledWindow;
    Gtk::PopoverMenu _listContextMenu;

    // List model for sidebar - tree model
    Glib::RefPtr<Gio::ListStore<ListTreeNode>> _listTreeStore;
    Glib::RefPtr<Gtk::TreeListModel> _treeListModel;
    Glib::RefPtr<Gtk::SingleSelection> _listSelectionModel;
    std::map<rs::ListId, Glib::RefPtr<ListTreeNode>> _nodesById;

    // Actions
    Glib::RefPtr<Gio::SimpleAction> _newListAction;
    Glib::RefPtr<Gio::SimpleAction> _deleteListAction;
    Glib::RefPtr<Gio::SimpleAction> _editListAction;
  };

} // namespace app::ui
