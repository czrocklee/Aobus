// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "list/ListSidebarController.h"
#include "list/ListSidebarPanel.h"
#include "list/SmartListDialog.h"
#include "track/TrackRowCache.h"
#include <ao/library/ListStore.h>
#include <ao/utility/Log.h>
#include <runtime/AppSession.h>
#include <runtime/LibraryMutationService.h>
#include <runtime/ViewService.h>
#include <runtime/WorkspaceService.h>

namespace ao::gtk
{
  namespace
  {
    ListId allTracksListId()
    {
      return ListId{std::numeric_limits<std::uint32_t>::max()};
    }

    ListId rootParentId()
    {
      return ListId{0};
    }
  }

  ListSidebarController::ListSidebarController(Gtk::Window& parent, rt::AppSession& session, Callbacks callbacks)
    : _parent{parent}, _callbacks{std::move(callbacks)}, _session{session}
  {
    auto panelCallbacks = ListSidebarPanel::Callbacks{
      .onSelectionChanged = [this](ListId listId) { onSelectionChanged(listId); },
      .onContextMenuRequested = [this](ListId listId, Gdk::Rectangle const& rect)
      { onContextMenuRequested(listId, rect); },
    };

    _panel = std::make_unique<ListSidebarPanel>(std::move(panelCallbacks));
    setupActions();

    _focusSub = _session.workspace().onFocusedViewChanged(
      [this](rt::ViewId viewId)
      {
        if (viewId != rt::ViewId{})
        {
          auto const state = _session.views().trackListState(viewId);
          if (state.listId != ListId{})
          {
            select(state.listId);
          }
        }
      });
  }

  ListSidebarController::~ListSidebarController() = default;

  Gtk::Widget& ListSidebarController::widget()
  {
    return _panel->widget();
  }

  void ListSidebarController::setupActions()
  {
    _newListAction = Gio::SimpleAction::create("new");
    _newListAction->signal_activate().connect([this](Glib::VariantBase const& /*variant*/)
                                              { openNewSmartListDialog(); });
    _newListAction->set_enabled(false);

    _deleteListAction = Gio::SimpleAction::create("delete");
    _deleteListAction->signal_activate().connect([this](Glib::VariantBase const& /*variant*/) { onDeleteList(); });
    _deleteListAction->set_enabled(false);

    _editListAction = Gio::SimpleAction::create("edit");
    _editListAction->signal_activate().connect([this](Glib::VariantBase const& /*variant*/) { onEditList(); });
    _editListAction->set_enabled(false);
  }

  void ListSidebarController::addActionsTo(Gio::ActionMap& actionMap)
  {
    actionMap.add_action(_newListAction);
    actionMap.add_action(_deleteListAction);
    actionMap.add_action(_editListAction);
  }

  void ListSidebarController::rebuildTree(TrackRowCache& dataProvider, lmdb::ReadTransaction const& txn)
  {
    _dataProvider = &dataProvider;

    _panel->rebuildTree(_session, txn);

    if (_pendingSelectId != ListId{0})
    {
      _panel->selectList(_pendingSelectId);
      _session.workspace().navigateTo(_pendingSelectId);
      _pendingSelectId = ListId{0};
    }
  }

  void ListSidebarController::select(ListId listId)
  {
    _panel->selectList(listId);
  }

  void ListSidebarController::onSelectionChanged(ListId listId)
  {
    _newListAction->set_enabled(true);
    _deleteListAction->set_enabled(listId != allTracksListId());
    _editListAction->set_enabled(listId != allTracksListId());

    if (_callbacks.onListSelected)
    {
      _callbacks.onListSelected(listId);
    }
  }

  void ListSidebarController::onContextMenuRequested(ListId listId, Gdk::Rectangle const& rect)
  {
    bool canDelete = false;
    bool canEdit = false;

    if (listId != ListId{0} && listId != allTracksListId())
    {
      canDelete = !_panel->listHasChildren(listId);
      canEdit = true;
    }

    if (_newListAction)
    {
      _newListAction->set_enabled(true);
    }

    if (_deleteListAction)
    {
      _deleteListAction->set_enabled(canDelete);
    }

    if (_editListAction)
    {
      _editListAction->set_enabled(canEdit);
    }

    _panel->showContextMenu(rect);
  }

  void ListSidebarController::openNewSmartListDialog()
  {
    auto parentListId = rootParentId();
    auto const selectedId = _panel->selectedListId();

    if (selectedId != ListId{0} && selectedId != allTracksListId())
    {
      parentListId = selectedId;
    }

    openNewListDialog(parentListId);
  }

  void ListSidebarController::openNewListDialog(ListId parentListId, std::string initialExpression)
  {
    if (_dataProvider == nullptr)
    {
      return;
    }

    auto* dialog = Gtk::make_managed<SmartListDialog>(_parent, _session, parentListId, *_dataProvider);

    if (!initialExpression.empty())
    {
      dialog->setLocalExpression(std::move(initialExpression));
    }

    dialog->signal_response().connect(
      [this, dialog](int responseId)
      {
        if (responseId == Gtk::ResponseType::OK)
        {
          if (auto const draft = dialog->draft(); draft.listId != ListId{0})
          {
            updateList(draft);
          }
          else
          {
            createList(draft);
          }
        }

        dialog->close();
      });

    dialog->present();
  }

  void ListSidebarController::createSmartListFromExpression(ListId parentListId, std::string expression)
  {
    openNewListDialog(parentListId, std::move(expression));
  }

  void ListSidebarController::openEditListDialog(ListId listId)
  {
    if (_dataProvider == nullptr)
    {
      return;
    }

    auto readTxn = _session.musicLibrary().readTransaction();
    auto reader = _session.musicLibrary().lists().reader(readTxn);
    if (auto view = reader.get(listId); view)
    {
      auto* dialog = Gtk::make_managed<SmartListDialog>(_parent, _session, view->parentId(), *_dataProvider);
      dialog->populate(listId, *view);
      dialog->signal_response().connect(
        [this, dialog](int responseId)
        {
          if (responseId == Gtk::ResponseType::OK)
          {
            auto const draft = dialog->draft();

            if (draft.listId != ListId{0})
            {
              updateList(draft);
            }
          }

          dialog->close();
        });

      dialog->present();
    }
  }

  void ListSidebarController::createList(model::ListDraft const& draft)
  {
    auto listId = _session.mutation().createList(draft);
    _pendingSelectId = listId;
  }

  void ListSidebarController::updateList(model::ListDraft const& draft)
  {
    _session.mutation().updateList(draft);
    _pendingSelectId = draft.listId;
  }

  void ListSidebarController::onEditList()
  {
    if (_dataProvider == nullptr)
    {
      return;
    }

    auto const listId = _panel->selectedListId();

    if (listId == ListId{0} || listId == allTracksListId())
    {
      return;
    }

    openEditListDialog(listId);
  }

  void ListSidebarController::onDeleteList()
  {
    if (_dataProvider == nullptr)
    {
      return;
    }

    auto const listId = _panel->selectedListId();

    if (listId == ListId{0} || listId == allTracksListId())
    {
      return;
    }

    if (_panel->listHasChildren(listId))
    {
      APP_LOG_ERROR("Cannot delete a list that still has child lists");
      return;
    }

    _session.mutation().deleteList(listId);

    _pendingSelectId = allTracksListId();
  }
} // namespace ao::gtk
