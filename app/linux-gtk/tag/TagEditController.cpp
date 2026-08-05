// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "tag/TagEditController.h"

#include "app/ThemeCoordinator.h"
#include "tag/TagPopover.h"
#include "tag/TrackPropertiesDialog.h"
#include "track/TrackRowCache.h"
#include "track/TrackViewPage.h"
#include <ao/CoreIds.h>
#include <ao/query/Expression.h>
#include <ao/query/Serializer.h>
#include <ao/rt/AppRuntime.h>
#include <ao/rt/ListNode.h>
#include <ao/rt/NotificationService.h>
#include <ao/rt/NotificationState.h>
#include <ao/rt/VirtualListIds.h>
#include <ao/rt/library/Library.h>
#include <ao/rt/library/LibraryAuthoring.h>
#include <ao/rt/library/LibraryReader.h>
#include <ao/uimodel/library/list/ListMembershipAuthoringSession.h>
#include <ao/uimodel/library/list/ListOrderPolicy.h>
#include <ao/uimodel/library/property/TagEdit.h>
#include <ao/uimodel/library/property/TrackAuthoringSession.h>

#include <giomm/actionmap.h>
#include <giomm/menu.h>
#include <giomm/simpleaction.h>
#include <giomm/simpleactiongroup.h>
#include <glibmm/main.h>
#include <glibmm/variant.h>
#include <glibmm/varianttype.h>
#include <gtkmm/object.h>
#include <gtkmm/popovermenu.h>
#include <gtkmm/widget.h>
#include <gtkmm/window.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <format>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ao::gtk
{
  namespace
  {
    std::string tagExpression(std::string_view const tag)
    {
      return query::serialize(query::VariableExpression{.type = query::VariableType::Tag, .name = std::string{tag}});
    }
  }

  TagEditController::TagEditController(Gtk::Window& parent,
                                       rt::AppRuntime& runtime,
                                       Callbacks callbacks,
                                       ThemeCoordinator& themeCoordinator)
    : _callbacks{std::move(callbacks)}, _runtime{runtime}, _parent{parent}, _themeCoordinator{themeCoordinator}
  {
    createActions();
  }

  TagEditController::~TagEditController()
  {
    retireContextPopover();
    retireTagPopover();
  }

  void TagEditController::setDataProvider(TrackRowCache* provider)
  {
    _dataProvider = provider;
  }

  void TagEditController::addActionsTo(Gio::ActionMap& actionMap)
  {
    if (_trackTagAddActionPtr)
    {
      actionMap.add_action(_trackTagAddActionPtr);
    }

    if (_trackTagRemoveActionPtr)
    {
      actionMap.add_action(_trackTagRemoveActionPtr);
    }
  }

  void TagEditController::openTrackContextMenu(TrackViewPage& page,
                                               TrackSelection const& selection,
                                               double xPosition,
                                               double yPosition)
  {
    if (selection.selectedIds.empty())
    {
      return;
    }

    _optActiveSelection = selection;
    _tagEditSessionPtr.reset();
    retireContextPopover();
    _contextPage = &page;
    _contextXPosition = xPosition;
    _contextYPosition = yPosition;

    _contextPopoverPtr = std::make_unique<Gtk::PopoverMenu>();
    buildContextActionsAndMenu(page);
    _contextPopoverPtr->insert_action_group("ctx", _contextActionGroupPtr);

    _contextPopoverPtr->set_parent(page);
    _contextPopoverClosedConnection = _contextPopoverPtr->signal_closed().connect(
      [this]
      {
        unparentClosedContextPopover();
        scheduleContextPopoverRetirement();
      });
    _contextAnchorUnmapConnection = page.signal_unmap().connect([this] { scheduleContextPopoverRetirement(); });
    _contextPopoverPtr->set_has_arrow(false);

    auto const rect = Gdk::Rectangle{static_cast<std::int32_t>(xPosition), static_cast<std::int32_t>(yPosition), 1, 1};
    _contextPopoverPtr->set_pointing_to(rect);
    _contextPopoverPtr->popup();
  }

  void TagEditController::presentProperties(TrackSelection const& selection)
  {
    if (selection.selectedIds.empty() || _dataProvider == nullptr)
    {
      return;
    }

    auto* const dialog = Gtk::make_managed<TrackPropertiesDialog>(
      _parent, _runtime.library(), _runtime.completion(), *_dataProvider, selection.selectedIds);
    auto tokenPtr = std::make_shared<ThemeRegistrationToken>(_themeCoordinator.registerToplevel(*dialog));
    dialog->signal_hide().connect([tokenPtr] { (*tokenPtr).reset(); });
    dialog->present();
  }

  void TagEditController::openTagEditor(TrackSelection const& selection, Gtk::Widget& relativeTo)
  {
    if (selection.selectedIds.empty())
    {
      return;
    }

    _optActiveSelection = selection;

    if (!beginTagEditSession(selection.selectedIds))
    {
      return;
    }

    retireTagPopover();
    _tagPopoverPtr = std::make_unique<TagPopover>(_runtime.library(), selection.selectedIds);

    _tagsChangedConnection = _tagPopoverPtr->signalTagsChanged().connect(
      [this](std::span<std::string const> tagsToAdd, std::span<std::string const> tagsToRemove)
      { applyTagChangeToCurrentSelection(tagsToAdd, tagsToRemove); });

    _tagPopoverPtr->set_parent(relativeTo);
    _tagPopoverPtr->popup();
    observeTagPopoverAnchor();
  }

  void TagEditController::submitTagChanges(TrackSelection const& selection,
                                           std::span<std::string const> tagsToAdd,
                                           std::span<std::string const> tagsToRemove)
  {
    if (_tagEditSessionPtr == nullptr || !std::ranges::equal(_tagEditSessionPtr->targetIds(), selection.selectedIds))
    {
      if (!beginTagEditSession(selection.selectedIds))
      {
        return;
      }
    }

    auto const result = ao::uimodel::applyTagEdit(*_tagEditSessionPtr, tagsToAdd, tagsToRemove);

    if (!result)
    {
      _runtime.notifications().post(
        rt::NotificationSeverity::Error, result.error().message, rt::NotificationLifetime::history());
      return;
    }

    if (result->status == rt::TrackAuthoringStatus::Stale || result->status == rt::TrackAuthoringStatus::Unavailable)
    {
      _runtime.notifications().post(
        rt::NotificationSeverity::Error, result->notificationText, rt::NotificationLifetime::history());
      _tagEditSessionPtr.reset();
      return;
    }

    if (result->status != rt::TrackAuthoringStatus::Applied)
    {
      return;
    }

    if (_callbacks.onTagsMutated)
    {
      _callbacks.onTagsMutated();
    }

    _runtime.notifications().post(
      rt::NotificationSeverity::Info, result->notificationText, rt::NotificationLifetime::transient());
  }

  void TagEditController::createActions()
  {
    auto const stringType = Glib::VariantType{"s"};

    _trackTagAddActionPtr = Gio::SimpleAction::create("track-tag-add", stringType);
    _trackTagAddActionPtr->signal_activate().connect(
      [this](Glib::VariantBase const& parameter)
      { addTagToCurrentSelection(Glib::VariantBase::cast_dynamic<Glib::Variant<std::string>>(parameter).get()); });

    _trackTagRemoveActionPtr = Gio::SimpleAction::create("track-tag-remove", stringType);
    _trackTagRemoveActionPtr->signal_activate().connect(
      [this](Glib::VariantBase const& parameter)
      { removeTagFromCurrentSelection(Glib::VariantBase::cast_dynamic<Glib::Variant<std::string>>(parameter).get()); });
  }

  void TagEditController::buildContextActionsAndMenu(TrackViewPage& page)
  {
    _contextActionGroupPtr = Gio::SimpleActionGroup::create();
    auto menuModelPtr = Gio::Menu::create();
    auto addAction = [this](Glib::RefPtr<Gio::Menu> const& menuPtr,
                            std::string const& label,
                            std::string const& name,
                            std::function<void()> callback,
                            bool const enabled = true)
    {
      auto actionPtr = Gio::SimpleAction::create(name);
      actionPtr->set_enabled(enabled);
      actionPtr->signal_activate().connect([callback = std::move(callback)](Glib::VariantBase const&) { callback(); });
      _contextActionGroupPtr->add_action(actionPtr);
      menuPtr->append(label, std::format("ctx.{}", name));
    };

    addAction(menuModelPtr,
              "Edit Tags",
              "edit-tags",
              [this]
              {
                auto* const activePage = _contextPage;
                auto const xPosition = _contextXPosition;
                auto const yPosition = _contextYPosition;
                scheduleContextPopoverRetirement();

                if (activePage != nullptr)
                {
                  openTagsPopover(*activePage, xPosition, yPosition);
                }
              });
    addAction(menuModelPtr,
              "Properties",
              "properties",
              [this]
              {
                scheduleContextPopoverRetirement();
                presentPropertiesDialog();
              });

    auto const lists = _runtime.library().reader().lists();
    auto const targets = uimodel::writableTagListTargets(lists);
    auto addToListMenuPtr = Gio::Menu::create();
    std::size_t addTargetCount = 0;
    auto const activeListId = _optActiveSelection ? _optActiveSelection->listId : rt::kAllTracksListId;

    for (auto const& target : targets)
    {
      if (_optActiveSelection && target.listId == _optActiveSelection->listId)
      {
        continue;
      }

      auto const actionName = std::format("add-to-list-{}", target.listId.raw());
      addAction(addToListMenuPtr,
                std::format("{} ({})", target.name, tagExpression(target.tag)),
                actionName,
                [this, listId = target.listId]
                {
                  scheduleContextPopoverRetirement();
                  applyListMembershipToCurrentSelection(listId, true);
                });
      ++addTargetCount;
    }

    auto const hasOmittedComputedTargets = std::ranges::any_of(
      lists,
      [&targets, activeListId](rt::ListNode const& list)
      {
        return list.id != activeListId &&
               std::ranges::find(targets, list.id, &uimodel::WritableTagListTarget::listId) == targets.end();
      });
    auto addManageListsAction = [this, &addAction, &addToListMenuPtr](std::string const& label)
    {
      addAction(addToListMenuPtr,
                label,
                "manage-lists",
                [this]
                {
                  scheduleContextPopoverRetirement();

                  if (_callbacks.onManageListsRequested)
                  {
                    _callbacks.onManageListsRequested();
                  }
                });
    };

    if (hasOmittedComputedTargets)
    {
      addAction(
        addToListMenuPtr,
        "Other Lists have computed membership; edit their expression or track tags instead.",
        "computed-lists-omitted",
        [] {},
        false);
    }

    if (addTargetCount == 0)
    {
      addAction(addToListMenuPtr, "No directly editable Playlists", "add-to-list-unavailable", [] {}, false);
      addManageListsAction("Create a Playlist...");
    }
    else if (hasOmittedComputedTargets)
    {
      addManageListsAction("Manage Lists...");
    }

    menuModelPtr->append_submenu("Add to Playlist", addToListMenuPtr);

    if (_optActiveSelection)
    {
      auto const current =
        std::ranges::find(targets, _optActiveSelection->listId, &uimodel::WritableTagListTarget::listId);

      if (current != targets.end())
      {
        addAction(menuModelPtr,
                  std::format("Remove from {} ({})", current->name, tagExpression(current->tag)),
                  "remove-from-current-list",
                  [this, listId = current->listId]
                  {
                    scheduleContextPopoverRetirement();
                    applyListMembershipToCurrentSelection(listId, false);
                  });
      }
      else if (!rt::isVirtualListId(_optActiveSelection->listId) &&
               std::ranges::find(lists, _optActiveSelection->listId, &rt::ListNode::id) != lists.end())
      {
        addAction(
          menuModelPtr,
          "Remove from this List is unavailable because its membership is computed.",
          "remove-from-current-list-unavailable",
          [] {},
          false);
      }
    }

    auto orderingMenuPtr = Gio::Menu::create();
    auto const capabilities = page.orderCapabilities();

    auto addOrderAction = [&](std::string const& label, std::string const& name, TrackOrderCommand const action)
    {
      addAction(orderingMenuPtr,
                label,
                name,
                [this, action]
                {
                  scheduleContextPopoverRetirement();
                  applyListOrderToCurrentSelection(action);
                });
    };

    if (!capabilities.canAuthorOrder)
    {
      addAction(orderingMenuPtr, capabilities.disabledReason, "ordering-unavailable", [] {}, false);
    }
    else
    {
      if (capabilities.canRelativeMove)
      {
        addOrderAction("Move Up", "order-up", TrackOrderCommand::MoveUp);
        addOrderAction("Move Down", "order-down", TrackOrderCommand::MoveDown);
      }
      else
      {
        addAction(orderingMenuPtr, "Move Up", "order-up", [] {}, false);
        addAction(orderingMenuPtr, "Move Down", "order-down", [] {}, false);
        addAction(orderingMenuPtr, capabilities.disabledReason, "relative-ordering-unavailable", [] {}, false);
      }

      if (capabilities.canAbsoluteMove)
      {
        addOrderAction("Move to Top", "order-top", TrackOrderCommand::MoveToTop);
        addOrderAction("Move to Bottom", "order-bottom", TrackOrderCommand::MoveToBottom);
      }

      if (capabilities.canResetOrder)
      {
        addOrderAction("Reset Order", "order-reset", TrackOrderCommand::Reset);
      }

      if (capabilities.canForgetHiddenPositions)
      {
        addOrderAction("Forget Hidden Positions", "order-forget-hidden", TrackOrderCommand::ForgetHidden);
      }
    }

    menuModelPtr->append_submenu("Manual Order", orderingMenuPtr);
    _contextPopoverPtr->set_menu_model(menuModelPtr);
  }

  void TagEditController::applyListMembershipToCurrentSelection(ListId const listId, bool const add)
  {
    if (!_optActiveSelection || _optActiveSelection->selectedIds.empty())
    {
      return;
    }

    auto sessionRes =
      uimodel::ListMembershipAuthoringSession::begin(_runtime.library(), _optActiveSelection->selectedIds);

    if (!sessionRes)
    {
      _runtime.notifications().post(
        rt::NotificationSeverity::Error, sessionRes.error().message, rt::NotificationLifetime::history());
      return;
    }

    auto result = add ? (*sessionRes)->addToList(listId) : (*sessionRes)->removeFromList(listId);

    if (!result)
    {
      _runtime.notifications().post(
        rt::NotificationSeverity::Error, result.error().message, rt::NotificationLifetime::history());
      return;
    }

    auto const failed =
      result->status == rt::TrackAuthoringStatus::Stale || result->status == rt::TrackAuthoringStatus::Unavailable;
    _runtime.notifications().post(failed ? rt::NotificationSeverity::Error : rt::NotificationSeverity::Info,
                                  result->notificationText,
                                  failed ? rt::NotificationLifetime::history() : rt::NotificationLifetime::transient());

    if (result->status == rt::TrackAuthoringStatus::Applied && _callbacks.onTagsMutated)
    {
      _callbacks.onTagsMutated();
    }
  }

  void TagEditController::applyListOrderToCurrentSelection(TrackOrderCommand const action)
  {
    auto* const page = _contextPage;

    if (page == nullptr)
    {
      return;
    }

    page->applyListOrderCommand(action);
  }

  void TagEditController::openTagsPopover(TrackViewPage& page, double xPosition, double yPosition)
  {
    if (!_optActiveSelection)
    {
      return;
    }

    auto const selectedIds = _optActiveSelection->selectedIds;

    if (!beginTagEditSession(selectedIds))
    {
      return;
    }

    retireTagPopover();
    _tagPopoverPtr = std::make_unique<TagPopover>(_runtime.library(), selectedIds);

    _tagsChangedConnection = _tagPopoverPtr->signalTagsChanged().connect(
      [this](std::span<std::string const> tagsToAdd, std::span<std::string const> tagsToRemove)
      { applyTagChangeToCurrentSelection(tagsToAdd, tagsToRemove); });

    page.openTagPopover(*_tagPopoverPtr, xPosition, yPosition);
    observeTagPopoverAnchor();
  }

  void TagEditController::presentPropertiesDialog()
  {
    if (!_optActiveSelection || _dataProvider == nullptr)
    {
      return;
    }

    auto* const dialog = Gtk::make_managed<TrackPropertiesDialog>(
      _parent, _runtime.library(), _runtime.completion(), *_dataProvider, _optActiveSelection->selectedIds);
    auto tokenPtr = std::make_shared<ThemeRegistrationToken>(_themeCoordinator.registerToplevel(*dialog));
    dialog->signal_hide().connect([tokenPtr] { (*tokenPtr).reset(); });
    dialog->present();
  }

  void TagEditController::unparentClosedContextPopover()
  {
    _contextPopoverClosedConnection.disconnect();
    _contextAnchorUnmapConnection.disconnect();

    if (_contextPopoverPtr && _contextPopoverPtr->get_parent() != nullptr)
    {
      _contextPopoverPtr->unparent();
    }
  }

  void TagEditController::scheduleContextPopoverRetirement()
  {
    unparentClosedContextPopover();
    _contextPopoverRetirementConnection.disconnect();
    _contextPopoverRetirementConnection = Glib::signal_idle().connect(
      [this]
      {
        finishContextPopoverRetirement();
        return false;
      });
  }

  void TagEditController::finishContextPopoverRetirement()
  {
    unparentClosedContextPopover();
    _contextPopoverPtr.reset();
    _contextPage = nullptr;
  }

  void TagEditController::retireContextPopover()
  {
    _contextPopoverRetirementConnection.disconnect();
    finishContextPopoverRetirement();
  }

  void TagEditController::unparentClosedTagPopover()
  {
    _tagPopoverClosedConnection.disconnect();
    _tagAnchorUnmapConnection.disconnect();
    _tagsChangedConnection.disconnect();

    if (_tagPopoverPtr && _tagPopoverPtr->get_parent() != nullptr)
    {
      _tagPopoverPtr->unparent();
    }
  }

  void TagEditController::retireTagPopover()
  {
    unparentClosedTagPopover();
    _tagPopoverPtr.reset();
  }

  void TagEditController::observeTagPopoverAnchor()
  {
    if (_tagPopoverPtr == nullptr)
    {
      return;
    }

    _tagPopoverClosedConnection = _tagPopoverPtr->signal_closed().connect([this] { unparentClosedTagPopover(); });

    if (auto* const anchor = _tagPopoverPtr->get_parent(); anchor != nullptr)
    {
      _tagAnchorUnmapConnection = anchor->signal_unmap().connect([this] { retireTagPopover(); });
    }
  }

  void TagEditController::addTagToCurrentSelection(std::string tag)
  {
    auto const toAdd = std::array{std::move(tag)};
    applyTagChangeToCurrentSelection(toAdd, {});
  }

  void TagEditController::removeTagFromCurrentSelection(std::string tag)
  {
    auto const toRemove = std::array{std::move(tag)};
    applyTagChangeToCurrentSelection({}, toRemove);
  }

  void TagEditController::applyTagChangeToCurrentSelection(std::span<std::string const> tagsToAdd,
                                                           std::span<std::string const> tagsToRemove)
  {
    if (!_optActiveSelection)
    {
      return;
    }

    submitTagChanges(*_optActiveSelection, tagsToAdd, tagsToRemove);
  }

  bool TagEditController::beginTagEditSession(std::span<TrackId const> trackIds)
  {
    auto sessionRes = uimodel::TrackAuthoringSession::begin(_runtime.library(), trackIds);

    if (!sessionRes)
    {
      _tagEditSessionPtr.reset();
      _runtime.notifications().post(
        rt::NotificationSeverity::Error, sessionRes.error().message, rt::NotificationLifetime::history());
      return false;
    }

    _tagEditSessionPtr = std::move(*sessionRes);
    return true;
  }
} // namespace ao::gtk
