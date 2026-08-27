// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "list/ListNavigationController.h"

#include "app/AppDialog.h"
#include "app/ThemeCoordinator.h"
#include "common/UiWorkflow.h"
#include "i18n/GtkTextCatalog.h"
#include "list/ListNavigationPanel.h"
#include "list/SmartListDialog.h"
#include "track/TrackRowCache.h"
#include <ao/CoreIds.h>
#include <ao/Error.h>
#include <ao/async/Task.h>
#include <ao/i18n/MessageCatalog.h>
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

    async::Task<Result<ListId>> updateListDraft(rt::Library* library, rt::ListDraft draft)
    {
      auto const listId = draft.listId;
      auto result = co_await library->updateList(std::move(draft));
      co_return std::move(result).transform([listId](rt::UpdateListReply const&) { return listId; });
    }

    async::Task<Result<ListId>> writeListDraft(rt::Library* library, rt::ListDraft draft)
    {
      if (draft.listId == kInvalidListId)
      {
        return library->createList(std::move(draft));
      }

      return updateListDraft(library, std::move(draft));
    }
  } // namespace

  ListNavigationController::ListNavigationController(Gtk::Window& parent,
                                                     rt::AppRuntime& runtime,
                                                     i18n::MessageCatalog textCatalog,
                                                     Callbacks callbacks,
                                                     ThemeCoordinator& themeCoordinator)
    : _parent{parent}
    , _callbacks{std::move(callbacks)}
    , _runtime{runtime}
    , _textCatalog{std::move(textCatalog)}
    , _themeCoordinator{themeCoordinator}
  {
    auto panelCallbacks = ListNavigationPanel::Callbacks{
      .onSelectionChanged = [this](ListId listId) { handleSelectionChanged(listId); },
      .onContextMenuRequested = [this](ListId listId, Gdk::Rectangle const& rect)
      { handleContextMenuRequested(listId, rect); },
    };

    _panelPtr = std::make_unique<ListNavigationPanel>(_textCatalog, std::move(panelCallbacks));
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
      reconcilePendingSelection();
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

  void ListNavigationController::reconcilePendingSelection()
  {
    if (_pendingSelectId == kInvalidListId)
    {
      return;
    }

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
    auto* dialog = Gtk::make_managed<SmartListDialog>(_parent, _runtime, _textCatalog, parentListId, *_dataProvider);
    auto tokenPtr = std::make_shared<ThemeRegistrationToken>(_themeCoordinator.registerToplevel(*dialog));
    dialog->configurePlaylistTemplate();
    dialog->signal_response().connect(
      [this, dialog, tokenPtr](std::int32_t const responseId)
      {
        if (responseId == Gtk::ResponseType::OK)
        {
          submitListDraftFromDialog(*dialog, dialog->draft(), dialog->presentationId());
          return;
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

    auto* dialog = Gtk::make_managed<SmartListDialog>(_parent, _runtime, _textCatalog, parentListId, *_dataProvider);
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
          submitListDraftFromDialog(*dialog, dialog->draft(), dialog->presentationId());
          return;
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
      auto* dialog =
        Gtk::make_managed<SmartListDialog>(_parent, _runtime, _textCatalog, optNode->parentId, *_dataProvider);
      auto tokenPtr = std::make_shared<ThemeRegistrationToken>(_themeCoordinator.registerToplevel(*dialog));
      dialog->populate(listId, *optNode, optPres);
      dialog->signal_response().connect(
        [this, dialog, tokenPtr](std::int32_t responseId)
        {
          if (responseId == Gtk::ResponseType::OK)
          {
            if (auto const draft = dialog->draft(); draft.listId != kInvalidListId)
            {
              submitListDraftFromDialog(*dialog, draft, dialog->presentationId());
              return;
            }
          }

          dialog->close();
        });
      dialog->signal_hide().connect([tokenPtr] { (*tokenPtr).reset(); });

      dialog->present();
    }
  }

  void ListNavigationController::submitListDraftFromDialog(SmartListDialog& dialog,
                                                           rt::ListDraft draft,
                                                           std::string presentationId)
  {
    if (!dialog.beginSubmission())
    {
      return;
    }

    auto presentResult = dialog.guardPresentationCallback(
      [dialogHandle = &dialog](Result<ListId> submittedRes)
      {
        if (!submittedRes)
        {
          APP_LOG_ERROR("Failed to save list: {}", submittedRes.error().message);
          dialogHandle->completeSubmission();
          dialogHandle->showError(submittedRes.error().message);
          return;
        }

        dialogHandle->close();
      });
    spawnUiTask(_runtime.async(),
                _tasks,
                *this,
                "save list",
                writeListDraft(&_runtime.library(), std::move(draft)),
                [presentationId = std::move(presentationId), presentResult = std::move(presentResult)](
                  ListNavigationController* owner, Result<ListId> submittedRes) mutable
                {
                  if (submittedRes)
                  {
                    owner->_pendingSelectId = *submittedRes;

                    if (owner->_callbacks.onListPresentationSaved)
                    {
                      owner->_callbacks.onListPresentationSaved(*submittedRes, std::move(presentationId));
                    }

                    owner->reconcilePendingSelection();
                  }

                  presentResult(std::move(submittedRes));
                });
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

    spawnUiTask(_runtime.async(),
                _tasks,
                *this,
                "preview list deletion",
                _runtime.library().previewDeleteList(listId),
                [listId](ListNavigationController* owner, Result<rt::DeleteListReply> previewRes)
                {
                  if (!previewRes)
                  {
                    owner->showDeleteError(listId, previewRes.error().message);
                    return;
                  }

                  owner->presentDeleteConfirmation(
                    listId,
                    false,
                    gtkText(owner->_textCatalog, i18n::MessageId::GtkListDeleteQuestionTitle),
                    deleteListQuestion(owner->_textCatalog, previewRes->name),
                    previewRes->optTagImpact);
                });
  }

  void ListNavigationController::handleDeleteListSubtreeActivated()
  {
    auto const listId = _panelPtr->selectedListId();

    if (rt::isVirtualListId(listId))
    {
      return;
    }

    spawnUiTask(_runtime.async(),
                _tasks,
                *this,
                "preview list subtree deletion",
                _runtime.library().previewDeleteListAndDescendants(listId),
                [listId](ListNavigationController* owner, Result<rt::DeleteListSubtreeReply> previewRes)
                {
                  if (!previewRes)
                  {
                    owner->showDeleteError(listId, previewRes.error().message);
                    return;
                  }

                  auto entries = std::string{};

                  for (auto const& list : previewRes->deletedLists)
                  {
                    entries.append(std::format("• {} ({})\n", list.name, list.listId));
                  }

                  auto message = deleteSubtreeQuestion(owner->_textCatalog, previewRes->deletedLists.size(), entries);
                  auto optTagImpact = previewRes->deletedLists.empty() ? std::optional<rt::DeleteListReply::TagImpact>{}
                                                                       : previewRes->deletedLists.front().optTagImpact;
                  owner->presentDeleteConfirmation(
                    listId,
                    true,
                    gtkText(owner->_textCatalog, i18n::MessageId::GtkListDeleteSubtreeTitle),
                    std::move(message),
                    std::move(optTagImpact));
                });
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
    dialog->addCancelAction(gtkText(_textCatalog, i18n::MessageId::GtkCommonCancel), Gtk::ResponseType::CANCEL);
    dialog->addPrimaryAction(
      gtkText(_textCatalog,
              deleteDescendants ? i18n::MessageId::GtkListDeleteAllAction : i18n::MessageId::GtkListDeleteAction),
      Gtk::ResponseType::YES);
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
      cleanupCheck = Gtk::make_managed<Gtk::CheckButton>(
        removeMembershipTagQuestion(_textCatalog, displayedTag(optTagImpact->tag), optTagImpact->taggedTrackCount));
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

        auto* const warningLabel = Gtk::make_managed<Gtk::Label>(
          membershipTagReferencesWarning(_textCatalog, displayedTag(optTagImpact->tag), references));
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
    auto complete = [listId](ListNavigationController* owner, auto result)
    {
      if (!result)
      {
        owner->showDeleteError(listId, result.error().message);
        return;
      }

      owner->_pendingSelectId = rt::kAllTracksListId;
      owner->reconcilePendingSelection();
    };

    if (deleteDescendants)
    {
      spawnUiTask(_runtime.async(),
                  _tasks,
                  *this,
                  "delete list subtree",
                  _runtime.library().deleteListAndDescendants(listId, options),
                  complete);
      return;
    }

    spawnUiTask(_runtime.async(),
                _tasks,
                *this,
                "delete list",
                _runtime.library().deleteList(listId, options),
                std::move(complete));
  }

  void ListNavigationController::showDeleteError(ListId const listId, std::string_view const message)
  {
    APP_LOG_ERROR("Failed to delete list {}: {}", listId, message);
    auto* const dialog =
      AppDialog::presentMessage(_parent,
                                gtkText(_textCatalog, i18n::MessageId::GtkListUnableToDeleteTitle),
                                std::string{message},
                                {AppDialogAction{.label = gtkText(_textCatalog, i18n::MessageId::GtkCommonClose),
                                                 .responseId = Gtk::ResponseType::CLOSE,
                                                 .role = AppDialogActionRole::Cancel}},
                                Gtk::ResponseType::CLOSE);
    auto tokenPtr = std::make_shared<ThemeRegistrationToken>(_themeCoordinator.registerToplevel(*dialog));
    dialog->signal_hide().connect([tokenPtr] { (*tokenPtr).reset(); });
  }
} // namespace ao::gtk
