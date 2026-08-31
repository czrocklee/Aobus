// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include "common/ActionMapRegistration.h"
#include <ao/CoreIds.h>
#include <ao/async/LifetimeScope.h>
#include <ao/async/Subscription.h>
#include <ao/i18n/MessageCatalog.h>
#include <ao/rt/ListMutation.h>
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
#include <string_view>

namespace ao::rt
{
  class AppRuntime;
  struct ListDraft;
}

namespace ao::gtk
{
  class TrackRowCache;
  class ListNavigationPanel;
  class SmartListDialog;
  class ThemeCoordinator;

  class ListNavigationController final
  {
  public:
    struct Callbacks final
    {
      std::function<bool(ListId)> onListSelected = {};
      std::function<void(ListId, std::string)> onListPresentationSaved = {};
      std::function<std::optional<std::string>(ListId)> listPresentationCallback = {};
    };

    ListNavigationController(Gtk::Window& parent,
                             rt::AppRuntime& runtime,
                             i18n::MessageCatalog textCatalog,
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
    void openNewPlaylistDialog();

    ActionMapRegistration addActionsTo(Gio::ActionMap& actionMap);

  private:
    void createActions();
    void handleContextMenuRequested(ListId listId, Gdk::Rectangle const& rect);
    void handleSelectionChanged(ListId listId);
    bool notifyListSelected(ListId listId) const;
    void reconcilePendingSelection();
    void syncSelectionFromWorkspace(rt::ViewId viewId);
    void updateListActions(ListId listId);

    void openNewListDialog(ListId parentListId, std::string initialExpression = {});
    void openNewSmartListDialog();
    void openEditListDialog(ListId listId);
    void submitListDraftFromDialog(SmartListDialog& dialog, rt::ListDraft draft, std::string presentationId);

    void handleDeleteListActivated();
    void handleDeleteListSubtreeActivated();
    void presentDeleteConfirmation(ListId listId,
                                   bool deleteDescendants,
                                   std::string title,
                                   std::string message,
                                   std::optional<rt::DeleteListReply::TagImpact> optTagImpact);
    void commitDeleteList(ListId listId, bool deleteDescendants, bool removeWritableTag);
    void showDeleteError(ListId listId, std::string_view message);
    void handleEditListActivated();

    Gtk::Window& _parent;
    Callbacks _callbacks;
    rt::AppRuntime& _runtime;
    i18n::MessageCatalog _textCatalog;
    ThemeCoordinator& _themeCoordinator;
    TrackRowCache* _dataProvider = nullptr;

    std::unique_ptr<ListNavigationPanel> _panelPtr;

    Glib::RefPtr<Gio::SimpleAction> _newListActionPtr;
    Glib::RefPtr<Gio::SimpleAction> _newPlaylistActionPtr;
    Glib::RefPtr<Gio::SimpleAction> _deleteListActionPtr;
    Glib::RefPtr<Gio::SimpleAction> _deleteListSubtreeActionPtr;
    Glib::RefPtr<Gio::SimpleAction> _editListActionPtr;

    ListId _pendingSelectId{0};
    rt::ViewId _observedViewId = rt::kInvalidViewId;
    std::uint64_t _observedWorkspaceRevision = 0;
    bool _syncingWorkspaceSelection = false;
    async::Subscription _workspaceSub;
    async::LifetimeScope _tasks;
  };
} // namespace ao::gtk
