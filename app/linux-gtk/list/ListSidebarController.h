// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/library/MusicLibrary.h>
#include <ao/model/ListDraft.h>
#include <gtkmm.h>
#include <runtime/TrackSource.h>

#include <functional>
#include <memory>
#include <string>

namespace ao::rt
{
  class AppSession;
}

namespace ao::gtk
{
  class TrackRowCache;
  class ListSidebarPanel;

  class ListSidebarController final
  {
  public:
    struct Callbacks final
    {
      std::function<void(ao::ListId)> onListSelected;
      std::function<ao::rt::TrackSource*(ao::ListId)> getListMembership;
    };

    ListSidebarController(Gtk::Window& parent, ao::rt::AppSession& session, Callbacks callbacks);
    ~ListSidebarController();

    Gtk::Widget& widget();

    void rebuildTree(TrackRowCache& dataProvider, ao::lmdb::ReadTransaction const& txn);
    void select(ao::ListId listId);
    void createSmartListFromExpression(ao::ListId parentListId, std::string expression);

    void addActionsTo(Gio::ActionMap& actionMap);

  private:
    void setupActions();
    void onContextMenuRequested(ao::ListId listId, Gdk::Rectangle const& rect);
    void onSelectionChanged(ao::ListId listId);

    void openNewListDialog(ao::ListId parentListId, std::string initialExpression = {});
    void openNewSmartListDialog();
    void openEditListDialog(ao::ListId listId);

    void createList(ao::model::ListDraft const& draft);
    void updateList(ao::model::ListDraft const& draft);
    void onDeleteList();
    void onEditList();

    Gtk::Window& _parent;
    Callbacks _callbacks;
    ao::rt::AppSession& _session;
    TrackRowCache* _dataProvider = nullptr;

    std::unique_ptr<ListSidebarPanel> _panel;

    Glib::RefPtr<Gio::SimpleAction> _newListAction;
    Glib::RefPtr<Gio::SimpleAction> _deleteListAction;
    Glib::RefPtr<Gio::SimpleAction> _editListAction;

    ao::ListId _pendingSelectId{0};
  };
} // namespace ao::gtk
