// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/Type.h>
#include <ao/lmdb/Transaction.h>
#include <ao/rt/CorePrimitives.h>
#include <ao/rt/LibraryMutationService.h>
#include <ao/rt/TrackSource.h>

#include <gdkmm/rectangle.h>
#include <giomm/actionmap.h>
#include <giomm/simpleaction.h>
#include <glibmm/refptr.h>
#include <gtkmm/widget.h>
#include <gtkmm/window.h>

#include <functional>
#include <memory>
#include <string>

namespace ao::rt
{
  class AppRuntime;
}

namespace ao::gtk
{
  class TrackRowCache;
  class ListNavigationPanel;

  class ListNavigationController final
  {
  public:
    struct Callbacks final
    {
      std::function<void(ListId)> onListSelected;
      std::function<rt::TrackSource*(ListId)> getListMembership;
    };

    ListNavigationController(Gtk::Window& parent, rt::AppRuntime& runtime, Callbacks callbacks);
    ~ListNavigationController();

    // Not copyable or movable
    ListNavigationController(ListNavigationController const&) = delete;
    ListNavigationController& operator=(ListNavigationController const&) = delete;
    ListNavigationController(ListNavigationController&&) = delete;
    ListNavigationController& operator=(ListNavigationController&&) = delete;

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

    void createList(rt::LibraryMutationService::ListDraft const& draft);
    void updateList(rt::LibraryMutationService::ListDraft const& draft);
    void onDeleteList();
    void onEditList();

    friend class ListNavigationControllerTestPeer;

    Gtk::Window& _parent;
    Callbacks _callbacks;
    rt::AppRuntime& _runtime;
    TrackRowCache* _dataProvider = nullptr;

    std::unique_ptr<ListNavigationPanel> _panelPtr;

    Glib::RefPtr<Gio::SimpleAction> _newListActionPtr;
    Glib::RefPtr<Gio::SimpleAction> _deleteListActionPtr;
    Glib::RefPtr<Gio::SimpleAction> _editListActionPtr;

    ListId _pendingSelectId{0};
    rt::Subscription _focusSub;
  };
} // namespace ao::gtk
