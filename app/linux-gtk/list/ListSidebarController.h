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
      std::function<void(ListId)> onListSelected;
      std::function<rt::TrackSource*(ListId)> getListMembership;
    };

    ListSidebarController(Gtk::Window& parent, rt::AppSession& session, Callbacks callbacks);
    ~ListSidebarController();

    Gtk::Widget& widget();

    void rebuildTree(TrackRowCache& dataProvider, lmdb::ReadTransaction const& txn);
    void select(ListId listId);
    void createSmartListFromExpression(ListId parentListId, std::string expression);

    void addActionsTo(Gio::ActionMap& actionMap);

  private:
    void setupActions();
    void onContextMenuRequested(ListId listId, Gdk::Rectangle const& rect);
    void onSelectionChanged(ListId listId);

    void openNewListDialog(ListId parentListId, std::string initialExpression = {});
    void openNewSmartListDialog();
    void openEditListDialog(ListId listId);

    void createList(model::ListDraft const& draft);
    void updateList(model::ListDraft const& draft);
    void onDeleteList();
    void onEditList();

    Gtk::Window& _parent;
    Callbacks _callbacks;
    rt::AppSession& _session;
    TrackRowCache* _dataProvider = nullptr;

    std::unique_ptr<ListSidebarPanel> _panel;

    Glib::RefPtr<Gio::SimpleAction> _newListAction;
    Glib::RefPtr<Gio::SimpleAction> _deleteListAction;
    Glib::RefPtr<Gio::SimpleAction> _editListAction;

    ListId _pendingSelectId{0};
  };
} // namespace ao::gtk
