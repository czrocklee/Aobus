// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "list/ListNavigationController.h"

#include "app/ThemeCoordinator.h"
#include "list/ListNavigationPanel.h"
#include "list/SmartListDialog.h"
#include "track/TrackRowCache.h"
#include <ao/Type.h>
#include <ao/rt/AppRuntime.h>
#include <ao/rt/CorePrimitives.h>
#include <ao/rt/ListNode.h>
#include <ao/rt/Log.h>
#include <ao/rt/ViewService.h>
#include <ao/rt/WorkspaceService.h>
#include <ao/rt/library/Library.h>
#include <ao/rt/library/LibraryReader.h>
#include <ao/rt/library/LibraryWriter.h>
#include <ao/uimodel/list/ListActionPolicy.h>

#include <gdkmm/rectangle.h>
#include <giomm/actionmap.h>
#include <giomm/simpleaction.h>
#include <glibmm/variant.h>
#include <gtkmm/dialog.h>
#include <gtkmm/object.h>
#include <gtkmm/widget.h>
#include <gtkmm/window.h>

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>

namespace ao::gtk
{
  ListNavigationController::ListNavigationController(Gtk::Window& parent,
                                                     rt::AppRuntime& runtime,
                                                     Callbacks callbacks,
                                                     ThemeCoordinator& themeController)
    : _parent{parent}, _callbacks{std::move(callbacks)}, _runtime{runtime}, _themeController{themeController}
  {
    auto panelCallbacks = ListNavigationPanel::Callbacks{
      .onSelectionChanged = [this](ListId listId) { onSelectionChanged(listId); },
      .onContextMenuRequested = [this](ListId listId, Gdk::Rectangle const& rect)
      { onContextMenuRequested(listId, rect); },
    };

    _panelPtr = std::make_unique<ListNavigationPanel>(std::move(panelCallbacks));
    setupActions();

    _focusSub = _runtime.workspace().onFocusedViewChanged(
      [this](rt::ViewId viewId)
      {
        if (viewId != rt::kInvalidViewId)
        {
          if (auto const state = _runtime.views().trackListState(viewId); state.listId != kInvalidListId)
          {
            select(state.listId);
          }
        }
      });
  }

  ListNavigationController::~ListNavigationController() = default;

  Gtk::Widget& ListNavigationController::widget()
  {
    return _panelPtr->widget();
  }

  void ListNavigationController::setupActions()
  {
    _newListActionPtr = Gio::SimpleAction::create("new");
    _newListActionPtr->signal_activate().connect([this](Glib::VariantBase const& /*variant*/)
                                                 { openNewSmartListDialog(); });
    _newListActionPtr->set_enabled(false);

    _deleteListActionPtr = Gio::SimpleAction::create("delete");
    _deleteListActionPtr->signal_activate().connect([this](Glib::VariantBase const& /*variant*/) { onDeleteList(); });
    _deleteListActionPtr->set_enabled(false);

    _editListActionPtr = Gio::SimpleAction::create("edit");
    _editListActionPtr->signal_activate().connect([this](Glib::VariantBase const& /*variant*/) { onEditList(); });
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

  void ListNavigationController::onSelectionChanged(ListId listId)
  {
    auto const state = ao::uimodel::list::ListActionPolicy::describeActions(listId, _panelPtr->listHasChildren(listId));

    _newListActionPtr->set_enabled(state.canCreate);
    _deleteListActionPtr->set_enabled(state.canDelete);
    _editListActionPtr->set_enabled(state.canEdit);

    if (_callbacks.onListSelected)
    {
      _callbacks.onListSelected(listId);
    }
  }

  void ListNavigationController::onContextMenuRequested(ListId listId, Gdk::Rectangle const& rect)
  {
    auto const state = ao::uimodel::list::ListActionPolicy::describeActions(listId, _panelPtr->listHasChildren(listId));

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

    _panelPtr->showContextMenu(rect);
  }

  void ListNavigationController::openNewSmartListDialog()
  {
    auto const parentListId = ao::uimodel::list::ListActionPolicy::parentForNewSmartList(_panelPtr->selectedListId());
    openNewListDialog(parentListId);
  }

  void ListNavigationController::openNewListDialog(ListId parentListId, std::string initialExpression)
  {
    if (_dataProvider == nullptr)
    {
      return;
    }

    auto* dialog = Gtk::make_managed<SmartListDialog>(_parent, _runtime, parentListId, *_dataProvider);
    auto tokenPtr = std::make_shared<ThemeRegistrationToken>(_themeController.registerToplevel(*dialog));

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

          submitListDraft(dialog->draft(), presId);
        }

        dialog->close();
      });

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
      auto const optPres = _callbacks.getListPresentation ? _callbacks.getListPresentation(listId) : std::nullopt;
      auto* dialog = Gtk::make_managed<SmartListDialog>(_parent, _runtime, optNode->parentId, *_dataProvider);
      auto tokenPtr = std::make_shared<ThemeRegistrationToken>(_themeController.registerToplevel(*dialog));
      dialog->populate(listId, *optNode, optPres);
      dialog->signal_response().connect(
        [this, dialog, tokenPtr](std::int32_t responseId)
        {
          if (responseId == Gtk::ResponseType::OK)
          {
            if (auto const draft = dialog->draft(); draft.listId != kInvalidListId)
            {
              submitListDraft(draft, dialog->presentationId());
            }
          }

          dialog->close();
        });

      dialog->present();
    }
  }

  ListId ListNavigationController::submitListDraft(rt::LibraryWriter::ListDraft const& draft,
                                                   std::string presentationId)
  {
    if (draft.listId != kInvalidListId)
    {
      updateList(draft);

      if (_callbacks.onListPresentationSaved)
      {
        _callbacks.onListPresentationSaved(draft.listId, std::move(presentationId));
      }

      return draft.listId;
    }

    auto const newListId = createList(draft);

    if (_callbacks.onListPresentationSaved && newListId != kInvalidListId)
    {
      _callbacks.onListPresentationSaved(newListId, std::move(presentationId));
    }

    return newListId;
  }

  ListId ListNavigationController::createList(rt::LibraryWriter::ListDraft const& draft)
  {
    auto listId = _runtime.library().writer().createList(draft);
    _pendingSelectId = listId;
    return listId;
  }

  void ListNavigationController::updateList(rt::LibraryWriter::ListDraft const& draft)
  {
    _runtime.library().writer().updateList(draft);
    _pendingSelectId = draft.listId;
  }

  void ListNavigationController::onEditList()
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

  void ListNavigationController::onDeleteList()
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

    if (_panelPtr->listHasChildren(listId))
    {
      APP_LOG_ERROR("Cannot delete a list that still has child lists");
      return;
    }

    _runtime.library().writer().deleteList(listId);

    _pendingSelectId = rt::kAllTracksListId;
  }
} // namespace ao::gtk
