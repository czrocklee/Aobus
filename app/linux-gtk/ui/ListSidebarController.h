// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/library/MusicLibrary.h>
#include <ao/model/ListDraft.h>
#include <gtkmm.h>
#include <runtime/TrackSource.h>

#include <functional>
#include <map>
#include <memory>
#include <string>

namespace ao::app
{
  class AppSession;
}

namespace ao::gtk
{
  class ListTreeNode;
  class TrackRowDataProvider;

  /**
   * ListSidebarController manages the sidebar tree model, selection, and list CRUD.
   */
  class ListSidebarController final
  {
  public:
    struct Callbacks final
    {
      std::function<void(ao::ListId)> onListSelected;
      std::function<ao::app::TrackSource*(ao::ListId)> getListMembership;
    };

    ListSidebarController(Gtk::Window& parent, ao::app::AppSession& session, Callbacks callbacks);
    ~ListSidebarController();

    Gtk::Widget& widget() { return _listScrolledWindow; }

    void rebuildTree(TrackRowDataProvider& dataProvider, ao::lmdb::ReadTransaction const& txn);
    void select(ao::ListId listId);
    void createSmartListFromExpression(ao::ListId parentListId, std::string expression);

    // Add to action group for menu access
    void addActionsTo(Gio::ActionMap& actionMap);

  private:
    static constexpr int kSidebarWidth = 200;

    void setupLayout();
    void setupSidebarListItem(Glib::RefPtr<Gtk::ListItem> const& listItem);
    void bindSidebarListItem(Glib::RefPtr<Gtk::ListItem> const& listItem);
    void onListSelectionChanged(std::uint32_t position, std::uint32_t nItems);

    // List context menu
    void showListContextMenu(Gtk::ListView& listView, Gdk::Rectangle const& rect);

    // List management - using ListDraft
    void openNewListDialog(ao::ListId parentListId, std::string initialExpression = {});
    void openNewSmartListDialog();
    void openEditListDialog(ao::ListId listId);
    bool listHasChildren(ao::ListId listId) const;

    void createList(ao::model::ListDraft const& draft);
    void updateList(ao::model::ListDraft const& draft);
    void onDeleteList();
    void onEditList();

    void buildListTree(ao::lmdb::ReadTransaction const& txn);
    void selectSidebarList(ao::ListId listId);

    Gtk::Window& _parent;
    Callbacks _callbacks;
    ao::app::AppSession& _session;
    TrackRowDataProvider* _dataProvider = nullptr;

    Gtk::ListView _listView;
    Gtk::ScrolledWindow _listScrolledWindow;
    Gtk::PopoverMenu _listContextMenu;

    // List model for sidebar - tree model
    Glib::RefPtr<Gio::ListStore<ListTreeNode>> _listTreeStore;
    Glib::RefPtr<Gtk::TreeListModel> _treeListModel;
    Glib::RefPtr<Gtk::SingleSelection> _listSelectionModel;
    std::map<ao::ListId, Glib::RefPtr<ListTreeNode>> _nodesById;

    // Actions
    Glib::RefPtr<Gio::SimpleAction> _newListAction;
    Glib::RefPtr<Gio::SimpleAction> _deleteListAction;
    Glib::RefPtr<Gio::SimpleAction> _editListAction;

    ao::ListId _pendingSelectId{0};
  };
} // namespace ao::gtk
