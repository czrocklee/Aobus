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
#include <ao/query/Expression.h>
#include <ao/query/Serializer.h>
#include <ao/rt/AppRuntime.h>
#include <ao/rt/ListMutation.h>
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
#include <ao/utility/StrongTypeFormatter.h>

#include <gdkmm/rectangle.h>
#include <giomm/actionmap.h>
#include <giomm/simpleaction.h>
#include <glibmm/variant.h>
#include <gtkmm/box.h>
#include <gtkmm/checkbutton.h>
#include <gtkmm/dialog.h>
#include <gtkmm/enums.h>
#include <gtkmm/label.h>
#include <gtkmm/object.h>
#include <gtkmm/widget.h>
#include <gtkmm/window.h>

#include <cstdint>
#include <expected>
#include <format>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace ao::gtk
{
  namespace
  {
    constexpr int kDeleteDialogWidth = 420;
    constexpr int kDeleteMessageWidthChars = 65;

    std::string displayedTag(std::string_view const tag)
    {
      return query::serialize(query::VariableExpression{.type = query::VariableType::Tag, .name = std::string{tag}});
    }
  } // namespace

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
      [this](rt::WorkspaceChanged const& changed) noexcept
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
        // The workspace is authoritative even when it now has no active view
        // or the referenced view has already retired. Replaying an earlier
        // failed selection would navigate back out of that state.
        _pendingSelectId = kInvalidListId;
        syncSelectionFromWorkspace(viewId);
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

    _newPlaylistActionPtr = Gio::SimpleAction::create("list-new-playlist");
    _newPlaylistActionPtr->signal_activate().connect([this](Glib::VariantBase const&) { openNewPlaylistDialog(); });
    _newPlaylistActionPtr->set_enabled(false);

    _deleteListActionPtr = Gio::SimpleAction::create("list-delete");
    _deleteListActionPtr->signal_activate().connect([this](Glib::VariantBase const& /*variant*/)
                                                    { handleDeleteListActivated(); });
    _deleteListActionPtr->set_enabled(false);

    _deleteListSubtreeActionPtr = Gio::SimpleAction::create("list-delete-subtree");
    _deleteListSubtreeActionPtr->signal_activate().connect([this](Glib::VariantBase const&)
                                                           { handleDeleteListSubtreeActivated(); });
    _deleteListSubtreeActionPtr->set_enabled(false);

    _editListActionPtr = Gio::SimpleAction::create("list-edit");
    _editListActionPtr->signal_activate().connect([this](Glib::VariantBase const& /*variant*/)
                                                  { handleEditListActivated(); });
    _editListActionPtr->set_enabled(false);
  }

  void ListNavigationController::addActionsTo(Gio::ActionMap& actionMap)
  {
    actionMap.add_action(_newListActionPtr);
    actionMap.add_action(_newPlaylistActionPtr);
    actionMap.add_action(_deleteListActionPtr);
    actionMap.add_action(_deleteListSubtreeActionPtr);
    actionMap.add_action(_editListActionPtr);
  }

  void ListNavigationController::rebuildTree(TrackRowCache& dataProvider)
  {
    _dataProvider = &dataProvider;

    _panelPtr->rebuildTree(_runtime.library());

    if (_pendingSelectId != kInvalidListId)
    {
      auto const pendingSelectId = _pendingSelectId;
      _syncingWorkspaceSelection = true;
      _panelPtr->selectList(pendingSelectId);
      _syncingWorkspaceSelection = false;

      if (_panelPtr->selectedListId() == pendingSelectId)
      {
        updateListActions(pendingSelectId);
      }

      if (notifyListSelected(pendingSelectId))
      {
        _pendingSelectId = kInvalidListId;
      }

      return;
    }

    syncSelectionFromWorkspace(_runtime.workspace().snapshot().activeViewId);

    if (auto const selectedListId = _panelPtr->selectedListId(); selectedListId != kInvalidListId)
    {
      updateListActions(selectedListId);
    }
  }

  void ListNavigationController::select(ListId listId)
  {
    _panelPtr->selectList(listId);
  }

  void ListNavigationController::handleSelectionChanged(ListId listId)
  {
    updateListActions(listId);

    if (!_syncingWorkspaceSelection && _callbacks.onListSelected)
    {
      // A newer successful selection supersedes any earlier failed one, which
      // would otherwise be replayed by the next rebuildTree().
      _pendingSelectId = notifyListSelected(listId) ? kInvalidListId : listId;
    }
  }

  void ListNavigationController::syncSelectionFromWorkspace(rt::ViewId const viewId)
  {
    if (viewId == rt::kInvalidViewId)
    {
      return;
    }

    auto const foundRes = _runtime.views().findTrackListState(viewId);

    if (!foundRes || foundRes->listId == kInvalidListId)
    {
      return;
    }

    _syncingWorkspaceSelection = true;
    _panelPtr->selectList(foundRes->listId);
    _syncingWorkspaceSelection = false;

    if (_panelPtr->selectedListId() == foundRes->listId)
    {
      updateListActions(foundRes->listId);
    }
  }

  void ListNavigationController::updateListActions(ListId const listId)
  {
    auto const state = ao::uimodel::describeListActions(listId, _panelPtr->hasListChildren(listId));

    _newListActionPtr->set_enabled(state.canCreate);
    _newPlaylistActionPtr->set_enabled(state.canCreate);
    _deleteListActionPtr->set_enabled(state.canDelete);
    _deleteListSubtreeActionPtr->set_enabled(state.canDeleteSubtree);
    _editListActionPtr->set_enabled(state.canEdit);
  }

  bool ListNavigationController::notifyListSelected(ListId const listId) const
  {
    return !_callbacks.onListSelected || _callbacks.onListSelected(listId);
  }

  void ListNavigationController::handleContextMenuRequested(ListId listId, Gdk::Rectangle const& rect)
  {
    auto const state = ao::uimodel::describeListActions(listId, _panelPtr->hasListChildren(listId));

    if (_newListActionPtr)
    {
      _newListActionPtr->set_enabled(state.canCreate);
    }

    if (_newPlaylistActionPtr)
    {
      _newPlaylistActionPtr->set_enabled(state.canCreate);
    }

    if (_deleteListActionPtr)
    {
      _deleteListActionPtr->set_enabled(state.canDelete);
    }

    if (_deleteListSubtreeActionPtr)
    {
      _deleteListSubtreeActionPtr->set_enabled(state.canDeleteSubtree);
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

  void ListNavigationController::openNewPlaylistDialog()
  {
    if (_dataProvider == nullptr)
    {
      return;
    }

    auto const parentListId = ao::uimodel::parentForNewSmartList(_panelPtr->selectedListId());
    auto* dialog = Gtk::make_managed<SmartListDialog>(_parent, _runtime, parentListId, *_dataProvider);
    auto tokenPtr = std::make_shared<ThemeRegistrationToken>(_themeCoordinator.registerToplevel(*dialog));
    dialog->configurePlaylistTemplate();
    dialog->signal_response().connect(
      [this, dialog, tokenPtr](std::int32_t const responseId)
      {
        if (responseId == Gtk::ResponseType::OK)
        {
          if (auto const submittedRes = submitListDraft(dialog->draft(), dialog->presentationId()); !submittedRes)
          {
            dialog->showError(submittedRes.error().message);
            return;
          }
        }

        dialog->close();
      });
    dialog->signal_hide().connect([tokenPtr] { (*tokenPtr).reset(); });
    dialog->present();
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

          if (auto const submittedRes = submitListDraft(dialog->draft(), presId); !submittedRes)
          {
            dialog->showError(submittedRes.error().message);
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
              if (auto const submittedRes = submitListDraft(draft, dialog->presentationId()); !submittedRes)
              {
                dialog->showError(submittedRes.error().message);
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
      if (auto const updateRes = _runtime.library().updateList(draft); !updateRes)
      {
        APP_LOG_ERROR("Failed to update list: {}", updateRes.error().message);
        return std::unexpected{updateRes.error()};
      }

      _pendingSelectId = draft.listId;

      if (_callbacks.onListPresentationSaved)
      {
        _callbacks.onListPresentationSaved(draft.listId, std::move(presentationId));
      }

      return draft.listId;
    }

    auto const listRes = _runtime.library().createList(draft);

    if (!listRes)
    {
      APP_LOG_ERROR("Failed to create list: {}", listRes.error().message);
      return std::unexpected{listRes.error()};
    }

    auto const newListId = *listRes;
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

    if (rt::isVirtualListId(listId))
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

    if (rt::isVirtualListId(listId))
    {
      return;
    }

    auto const previewRes = _runtime.library().previewDeleteList(listId);

    if (!previewRes)
    {
      showDeleteError(listId, previewRes.error().message);
      return;
    }

    presentDeleteConfirmation(
      listId,
      false,
      "Delete List?",
      std::format("Delete \"{}\"?\n\nThe List will be removed. Music files will be kept.", previewRes->name),
      previewRes->optTagImpact);
  }

  void ListNavigationController::handleDeleteListSubtreeActivated()
  {
    auto const listId = _panelPtr->selectedListId();

    if (rt::isVirtualListId(listId))
    {
      return;
    }

    auto const previewRes = _runtime.library().previewDeleteListAndDescendants(listId);

    if (!previewRes)
    {
      showDeleteError(listId, previewRes.error().message);
      return;
    }

    auto message = std::format("Delete {} Lists in this derived subtree?\n\n", previewRes->deletedLists.size());

    for (auto const& list : previewRes->deletedLists)
    {
      message.append(std::format("• {} ({})\n", list.name, list.listId));
    }

    message.append("\nTags used by nested Playlists are kept.\nMusic files will be kept.");
    auto optTagImpact = previewRes->deletedLists.empty() ? std::optional<rt::DeleteListReply::TagImpact>{}
                                                         : previewRes->deletedLists.front().optTagImpact;
    presentDeleteConfirmation(
      listId, true, "Delete List and Descendants?", std::move(message), std::move(optTagImpact));
  }

  void ListNavigationController::presentDeleteConfirmation(ListId const listId,
                                                           bool const deleteDescendants,
                                                           std::string title,
                                                           std::string message,
                                                           std::optional<rt::DeleteListReply::TagImpact> optTagImpact)
  {
    auto* const dialog = Gtk::make_managed<AppDialog>();
    dialog->set_title(std::move(title));
    dialog->configureForParent(_parent);
    dialog->setCloseResponse(Gtk::ResponseType::CANCEL);
    dialog->addCancelAction("Cancel", Gtk::ResponseType::CANCEL);
    dialog->addPrimaryAction(deleteDescendants ? "Delete All" : "Delete", Gtk::ResponseType::YES);
    dialog->setDefaultResponse(Gtk::ResponseType::CANCEL);

    auto* const content = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 12);
    content->set_size_request(kDeleteDialogWidth, -1);
    auto* const messageLabel = Gtk::make_managed<Gtk::Label>(std::move(message));
    messageLabel->set_halign(Gtk::Align::START);
    messageLabel->set_xalign(0.0F);
    messageLabel->set_wrap(true);
    messageLabel->set_max_width_chars(kDeleteMessageWidthChars);
    content->append(*messageLabel);

    auto* cleanupCheck = static_cast<Gtk::CheckButton*>(nullptr);

    if (optTagImpact)
    {
      cleanupCheck = Gtk::make_managed<Gtk::CheckButton>(std::format("Also remove {} from {} track{}",
                                                                     displayedTag(optTagImpact->tag),
                                                                     optTagImpact->taggedTrackCount,
                                                                     optTagImpact->taggedTrackCount == 1 ? "" : "s"));
      cleanupCheck->set_active(false);
      content->append(*cleanupCheck);

      if (!optTagImpact->otherListReferences.empty())
      {
        auto references = std::string{};

        for (auto const& reference : optTagImpact->otherListReferences)
        {
          if (!references.empty())
          {
            references.append(", ");
          }

          references.append(reference.name);
        }

        auto* const warningLabel =
          Gtk::make_managed<Gtk::Label>(std::format("{} is also referenced by: {}. Removing it may change those Lists.",
                                                    displayedTag(optTagImpact->tag),
                                                    references));
        warningLabel->set_halign(Gtk::Align::START);
        warningLabel->set_xalign(0.0F);
        warningLabel->set_wrap(true);
        warningLabel->add_css_class("warning");
        content->append(*warningLabel);
      }
    }

    dialog->setContentWidget(*content);
    dialog->signal_response().connect(
      [this, dialog, listId, deleteDescendants, cleanupCheck](std::int32_t const responseId)
      {
        if (responseId == Gtk::ResponseType::YES)
        {
          commitDeleteList(listId, deleteDescendants, cleanupCheck != nullptr && cleanupCheck->get_active());
        }

        dialog->close();
      });
    auto tokenPtr = std::make_shared<ThemeRegistrationToken>(_themeCoordinator.registerToplevel(*dialog));
    dialog->signal_hide().connect([tokenPtr] { (*tokenPtr).reset(); });
    dialog->present();
  }

  void ListNavigationController::commitDeleteList(ListId const listId,
                                                  bool const deleteDescendants,
                                                  bool const removeWritableTag)
  {
    auto const options = rt::DeleteListOptions{.removeWritableTagFromTracks = removeWritableTag};
    auto result = deleteDescendants
                    ? _runtime.library()
                        .deleteListAndDescendants(listId, options)
                        .transform([](rt::DeleteListSubtreeReply const&) {})
                    : _runtime.library().deleteList(listId, options).transform([](rt::DeleteListReply const&) {});

    if (!result)
    {
      showDeleteError(listId, result.error().message);
      return;
    }

    _pendingSelectId = rt::kAllTracksListId;
  }

  void ListNavigationController::showDeleteError(ListId const listId, std::string_view const message)
  {
    APP_LOG_ERROR("Failed to delete list {}: {}", listId, message);
    auto* const dialog = AppDialog::presentMessage(
      _parent,
      "Unable to Delete List",
      std::string{message},
      {AppDialogAction{.label = "Close", .responseId = Gtk::ResponseType::CLOSE, .role = AppDialogActionRole::Cancel}},
      Gtk::ResponseType::CLOSE);
    auto tokenPtr = std::make_shared<ThemeRegistrationToken>(_themeCoordinator.registerToplevel(*dialog));
    dialog->signal_hide().connect([tokenPtr] { (*tokenPtr).reset(); });
  }
} // namespace ao::gtk
