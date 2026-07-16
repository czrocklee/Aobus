// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/CoreIds.h>
#include <ao/rt/ListMutation.h>
#include <ao/rt/Subscription.h>
#include <ao/rt/ViewIds.h>

#include <gdkmm/rectangle.h>
#include <giomm/actionmap.h>
#include <giomm/simpleaction.h>
#include <glibmm/refptr.h>
#include <gtkmm/widget.h>
#include <gtkmm/window.h>

#include <cstdint>
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
      std::function<void(ListId)> onListSelected = {};
      std::function<void(ListId, std::string)> onListPresentationSaved = {};
      std::function<std::optional<std::string>(ListId)> listPresentationCallback = {};
    };

    ListNavigationController(Gtk::Window& parent,
                             rt::AppRuntime& runtime,
                             Callbacks callbacks,
                             ThemeCoordinator& themeCoordinator);
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
    ListId submitListDraft(rt::LibraryListDraft const& draft, std::string presentationId);

    void addActionsTo(Gio::ActionMap& actionMap);

  private:
    void createActions();
    void handleContextMenuRequested(ListId listId, Gdk::Rectangle const& rect);
    void handleSelectionChanged(ListId listId);

    void openNewListDialog(ListId parentListId, std::string initialExpression = {});
    void openNewSmartListDialog();
    void openEditListDialog(ListId listId);

    ListId createList(rt::LibraryListDraft const& draft);
    bool updateList(rt::LibraryListDraft const& draft);
    void handleDeleteListActivated();
    void handleEditListActivated();

    Gtk::Window& _parent;
    Callbacks _callbacks;
    rt::AppRuntime& _runtime;
    ThemeCoordinator& _themeCoordinator;
    TrackRowCache* _dataProvider = nullptr;

    std::unique_ptr<ListNavigationPanel> _panelPtr;

    Glib::RefPtr<Gio::SimpleAction> _newListActionPtr;
    Glib::RefPtr<Gio::SimpleAction> _deleteListActionPtr;
    Glib::RefPtr<Gio::SimpleAction> _editListActionPtr;

    ListId _pendingSelectId{0};
    rt::ViewId _observedViewId = rt::kInvalidViewId;
    std::uint64_t _observedWorkspaceRevision = 0;
    bool _syncingWorkspaceSelection = false;
    rt::Subscription _workspaceSub;
  };
} // namespace ao::gtk
