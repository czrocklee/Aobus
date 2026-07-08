// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/CoreIds.h>
#include <ao/rt/Subscription.h>
#include <ao/rt/library/LibraryWriter.h>
#include <ao/rt/source/TrackSource.h>

#include <gdkmm/rectangle.h>
#include <giomm/actionmap.h>
#include <giomm/simpleaction.h>
#include <glibmm/refptr.h>
#include <gtkmm/widget.h>
#include <gtkmm/window.h>

#include <functional>
#include <memory>
#include <optional>
#include <string>

namespace ao::rt
{
  class AppRuntime;
}

namespace ao::gtk
{
  class TrackRowCache;
  class ListNavigationPanel;
  class ThemeCoordinator;

  class ListNavigationController final
  {
  public:
    struct Callbacks final
    {
      std::function<void(ListId)> onListSelected;
      std::function<rt::TrackSource*(ListId)> getListMembership;
      std::function<void(ListId, std::string)> onListPresentationSaved;
      std::function<std::optional<std::string>(ListId)> listPresentationCallback;
    };

    ListNavigationController(Gtk::Window& parent,
                             rt::AppRuntime& runtime,
                             Callbacks callbacks,
                             ThemeCoordinator& themeController);
    ~ListNavigationController();

    // Not copyable or movable
    ListNavigationController(ListNavigationController const&) = delete;
    ListNavigationController& operator=(ListNavigationController const&) = delete;
    ListNavigationController(ListNavigationController&&) = delete;
    ListNavigationController& operator=(ListNavigationController&&) = delete;

    Gtk::Widget& widget();

    void rebuildTree(TrackRowCache& dataProvider);
    void select(ListId listId);
    void createSmartListFromExpression(ListId parentListId, std::string expression);
    ListId submitListDraft(rt::LibraryWriter::ListDraft const& draft, std::string presentationId);

    void addActionsTo(Gio::ActionMap& actionMap);

  private:
    void createActions();
    void onContextMenuRequested(ListId listId, Gdk::Rectangle const& rect);
    void onSelectionChanged(ListId listId);

    void openNewListDialog(ListId parentListId, std::string initialExpression = {});
    void openNewSmartListDialog();
    void openEditListDialog(ListId listId);

    ListId createList(rt::LibraryWriter::ListDraft const& draft);
    bool updateList(rt::LibraryWriter::ListDraft const& draft);
    void onDeleteList();
    void onEditList();

    friend class ListNavigationControllerTestPeer;

    Gtk::Window& _parent;
    Callbacks _callbacks;
    rt::AppRuntime& _runtime;
    ThemeCoordinator& _themeController;
    TrackRowCache* _dataProvider = nullptr;

    std::unique_ptr<ListNavigationPanel> _panelPtr;

    Glib::RefPtr<Gio::SimpleAction> _newListActionPtr;
    Glib::RefPtr<Gio::SimpleAction> _deleteListActionPtr;
    Glib::RefPtr<Gio::SimpleAction> _editListActionPtr;

    ListId _pendingSelectId{0};
    rt::Subscription _focusSub;
  };
} // namespace ao::gtk
