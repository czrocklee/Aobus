// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "list/ListNavigationController.h"

#include "app/AppDialog.h"
#include "app/ThemeCoordinator.h"
#include "list/ListNavigationPanel.h"
#include "list/SmartListDialog.h"
#include "track/TrackRowCache.h"
#include <ao/CoreIds.h>
#include <ao/Error.h>
#include <ao/rt/AppRuntime.h>
#include <ao/rt/ListNode.h>
#include <ao/rt/Log.h>
#include <ao/rt/ViewIds.h>
#include <ao/rt/ViewService.h>
#include <ao/rt/VirtualListIds.h>
#include <ao/rt/WorkspaceService.h>
#include <ao/rt/WorkspaceSnapshot.h>
#include <ao/rt/library/Library.h>
#include <ao/rt/library/LibraryReader.h>
#include <ao/uimodel/library/list/ListActionPolicy.h>

#include <gdkmm/rectangle.h>
#include <giomm/actionmap.h>
#include <giomm/simpleaction.h>
#include <glibmm/variant.h>
#include <gtkmm/dialog.h>
#include <gtkmm/object.h>
#include <gtkmm/widget.h>
#include <gtkmm/window.h>

#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <string>
#include <utility>

namespace ao::gtk
{
  ListNavigationController::ListNavigationController(Gtk::Window& parent,
                                                     rt::AppRuntime& runtime,
                                                     Callbacks callbacks,
                                                     ThemeCoordinator& themeCoordinator)
    : _parent{parent}, _callbacks{std::move(callbacks)}, _runtime{runtime}, _themeCoordinator{themeCoordinator}
  {
    auto panelCallbacks = ListNavigationPanel::Callbacks{
      .onSelectionChanged = [this](ListId listId) { handleSelectionChanged(listId); },
      .onContextMenuRequested = [this](ListId listId, Gdk::Rectangle const& rect)
      { handleContextMenuRequested(listId, rect); },
    };

    _panelPtr = std::make_unique<ListNavigationPanel>(std::move(panelCallbacks));
    createActions();
    auto const initialWorkspace = _runtime.workspace().snapshot();
    _observedViewId = initialWorkspace.activeViewId;
    _observedWorkspaceRevision = initialWorkspace.revision;

    _workspaceSub = _runtime.workspace().onChanged(
      [this](rt::WorkspaceChanged const& changed)
      {
        if (changed.snapshot.revision <= _observedWorkspaceRevision)
        {
          return;
        }

        _observedWorkspaceRevision = changed.snapshot.revision;
        auto const viewId = changed.snapshot.activeViewId;

        if (viewId == _observedViewId)
        {
          return;
        }

        _observedViewId = viewId;

        if (viewId != rt::kInvalidViewId)
        {
          if (auto const state = _runtime.views().trackListState(viewId); state.listId != kInvalidListId)
          {
            _syncingWorkspaceSelection = true;
            select(state.listId);
            _syncingWorkspaceSelection = false;
          }
        }
      });
  }

  ListNavigationController::~ListNavigationController() = default;

  Gtk::Widget& ListNavigationController::widget()
  {
    return _panelPtr->widget();
  }

  void ListNavigationController::createActions()
  {
    _newListActionPtr = Gio::SimpleAction::create("list-new-smart-list");
    _newListActionPtr->signal_activate().connect([this](Glib::VariantBase const& /*variant*/)
                                                 { openNewSmartListDialog(); });
    _newListActionPtr->set_enabled(false);

    _deleteListActionPtr = Gio::SimpleAction::create("list-delete");
    _deleteListActionPtr->signal_activate().connect([this](Glib::VariantBase const& /*variant*/)
                                                    { handleDeleteListActivated(); });
    _deleteListActionPtr->set_enabled(false);

    _editListActionPtr = Gio::SimpleAction::create("list-edit");
    _editListActionPtr->signal_activate().connect([this](Glib::VariantBase const& /*variant*/)
                                                  { handleEditListActivated(); });
    _editListActionPtr->set_enabled(false);
  }

  void ListNavigationController::addActionsTo(Gio::ActionMap& actionMap)
  {
    actionMap.add_action(_newListActionPtr);
    actionMap.add_action(_deleteListActionPtr);
    actionMap.add_action(_editListActionPtr);
  }

  void ListNavigationController::rebuildTree(TrackRowCache& dataProvider)
  {
    _dataProvider = &dataProvider;

    _panelPtr->rebuildTree(_runtime.library());

    if (_pendingSelectId != kInvalidListId)
    {
      _panelPtr->selectList(_pendingSelectId);

      if (_callbacks.onListSelected)
      {
        _callbacks.onListSelected(_pendingSelectId);
      }

      _pendingSelectId = kInvalidListId;
    }
  }

  void ListNavigationController::select(ListId listId)
  {
    _panelPtr->selectList(listId);
  }

  void ListNavigationController::handleSelectionChanged(ListId listId)
  {
    auto const state = ao::uimodel::describeListActions(listId, _panelPtr->hasListChildren(listId));

    _newListActionPtr->set_enabled(state.canCreate);
    _deleteListActionPtr->set_enabled(state.canDelete);
    _editListActionPtr->set_enabled(state.canEdit);

    if (!_syncingWorkspaceSelection && _callbacks.onListSelected)
    {
      _callbacks.onListSelected(listId);
    }
  }

  void ListNavigationController::handleContextMenuRequested(ListId listId, Gdk::Rectangle const& rect)
  {
    auto const state = ao::uimodel::describeListActions(listId, _panelPtr->hasListChildren(listId));

    if (_newListActionPtr)
    {
      _newListActionPtr->set_enabled(state.canCreate);
    }

    if (_deleteListActionPtr)
    {
      _deleteListActionPtr->set_enabled(state.canDelete);
    }

    if (_editListActionPtr)
    {
      _editListActionPtr->set_enabled(state.canEdit);
    }

    _panelPtr->openContextMenu(rect);
  }

  void ListNavigationController::openNewSmartListDialog()
  {
    auto const parentListId = ao::uimodel::parentForNewSmartList(_panelPtr->selectedListId());
    openNewListDialog(parentListId);
  }

  void ListNavigationController::openNewListDialog(ListId parentListId, std::string initialExpression)
  {
    if (_dataProvider == nullptr)
    {
      return;
    }

    auto* dialog = Gtk::make_managed<SmartListDialog>(_parent, _runtime, parentListId, *_dataProvider);
    auto tokenPtr = std::make_shared<ThemeRegistrationToken>(_themeCoordinator.registerToplevel(*dialog));

    if (!initialExpression.empty())
    {
      dialog->setLocalExpression(std::move(initialExpression));
    }

    dialog->signal_response().connect(
      [this, dialog, tokenPtr](std::int32_t responseId)
      {
        if (responseId == Gtk::ResponseType::OK)
        {
          auto const presId = dialog->presentationId();

          if (auto const submitted = submitListDraft(dialog->draft(), presId); !submitted)
          {
            dialog->showError(submitted.error().message);
            return;
          }
        }

        dialog->close();
      });
    dialog->signal_hide().connect([tokenPtr] { (*tokenPtr).reset(); });

    dialog->present();
  }

  void ListNavigationController::createSmartListFromExpression(ListId parentListId, std::string expression)
  {
    openNewListDialog(parentListId, std::move(expression));
  }

  void ListNavigationController::openEditListDialog(ListId listId)
  {
    if (_dataProvider == nullptr)
    {
      return;
    }

    auto scope = _runtime.library().reader();

    if (auto const optNode = scope.listNode(listId); optNode)
    {
      auto const optPres =
        _callbacks.listPresentationCallback ? _callbacks.listPresentationCallback(listId) : std::nullopt;
      auto* dialog = Gtk::make_managed<SmartListDialog>(_parent, _runtime, optNode->parentId, *_dataProvider);
      auto tokenPtr = std::make_shared<ThemeRegistrationToken>(_themeCoordinator.registerToplevel(*dialog));
      dialog->populate(listId, *optNode, optPres);
      dialog->signal_response().connect(
        [this, dialog, tokenPtr](std::int32_t responseId)
        {
          if (responseId == Gtk::ResponseType::OK)
          {
            if (auto const draft = dialog->draft(); draft.listId != kInvalidListId)
            {
              if (auto const submitted = submitListDraft(draft, dialog->presentationId()); !submitted)
              {
                dialog->showError(submitted.error().message);
                return;
              }
            }
          }

          dialog->close();
        });
      dialog->signal_hide().connect([tokenPtr] { (*tokenPtr).reset(); });

      dialog->present();
    }
  }

  Result<ListId> ListNavigationController::submitListDraft(rt::LibraryListDraft const& draft,
                                                           std::string presentationId)
  {
    if (draft.listId != kInvalidListId)
    {
      if (auto const updateResult = _runtime.library().updateList(draft); !updateResult)
      {
        APP_LOG_ERROR("Failed to update list: {}", updateResult.error().message);
        return std::unexpected{updateResult.error()};
      }

      _pendingSelectId = draft.listId;

      if (_callbacks.onListPresentationSaved)
      {
        _callbacks.onListPresentationSaved(draft.listId, std::move(presentationId));
      }

      return draft.listId;
    }

    auto const listResult = _runtime.library().createList(draft);

    if (!listResult)
    {
      APP_LOG_ERROR("Failed to create list: {}", listResult.error().message);
      return std::unexpected{listResult.error()};
    }

    auto const newListId = *listResult;
    _pendingSelectId = newListId;

    if (_callbacks.onListPresentationSaved)
    {
      _callbacks.onListPresentationSaved(newListId, std::move(presentationId));
    }

    return newListId;
  }

  void ListNavigationController::handleEditListActivated()
  {
    if (_dataProvider == nullptr)
    {
      return;
    }

    auto const listId = _panelPtr->selectedListId();

    if (listId == kInvalidListId || listId == rt::kAllTracksListId)
    {
      return;
    }

    openEditListDialog(listId);
  }

  void ListNavigationController::handleDeleteListActivated()
  {
    if (_dataProvider == nullptr)
    {
      return;
    }

    auto const listId = _panelPtr->selectedListId();

    if (listId == kInvalidListId || listId == rt::kAllTracksListId)
    {
      return;
    }

    if (_panelPtr->hasListChildren(listId))
    {
      APP_LOG_ERROR("Cannot delete a list that still has child lists");
      return;
    }

    if (auto const deleteResult = _runtime.library().deleteList(listId); !deleteResult)
    {
      APP_LOG_ERROR("Failed to delete list {}: {}", listId, deleteResult.error().message);
      auto* const dialog = AppDialog::presentMessage(
        _parent,
        "Unable to Delete List",
        deleteResult.error().message,
        {AppDialogAction{
          .label = "Close", .responseId = Gtk::ResponseType::CLOSE, .role = AppDialogActionRole::Cancel}},
        Gtk::ResponseType::CLOSE);
      auto tokenPtr = std::make_shared<ThemeRegistrationToken>(_themeCoordinator.registerToplevel(*dialog));
      dialog->signal_hide().connect([tokenPtr] { (*tokenPtr).reset(); });
      return;
    }

    _pendingSelectId = rt::kAllTracksListId;
  }
} // namespace ao::gtk
