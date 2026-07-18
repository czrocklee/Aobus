// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "tag/TagEditController.h"

#include "app/ThemeCoordinator.h"
#include "tag/TagPopover.h"
#include "tag/TrackPropertiesDialog.h"
#include "track/TrackRowCache.h"
#include "track/TrackViewPage.h"
#include <ao/CoreIds.h>
#include <ao/rt/AppRuntime.h>
#include <ao/rt/NotificationService.h>
#include <ao/rt/NotificationState.h>
#include <ao/uimodel/library/property/TagEditWorkflow.h>
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
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace ao::gtk
{
  namespace
  {
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

    auto menuModelPtr = Gio::Menu::create();
    menuModelPtr->append("Edit Tags", "ctx.edit-tags");
    menuModelPtr->append("Properties", "ctx.properties");

    _contextPopoverPtr->set_menu_model(menuModelPtr);
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

    auto request = ao::uimodel::TagEditRequest{};
    request.selectedIds = selection.selectedIds;
    request.tagsToAdd.assign(tagsToAdd.begin(), tagsToAdd.end());
    request.tagsToRemove.assign(tagsToRemove.begin(), tagsToRemove.end());

    auto workflow = ao::uimodel::TagEditWorkflow{*_tagEditSessionPtr};
    auto const result = workflow.apply(request);

    if (result.rejected || result.stale)
    {
      _runtime.notifications().post(
        rt::NotificationSeverity::Error, result.notificationText, rt::NotificationLifetime::sessionHistory());

      if (result.stale)
      {
        _tagEditSessionPtr.reset();
      }

      return;
    }

    if (!result.applied)
    {
      return;
    }

    if (_callbacks.onTagsMutated)
    {
      _callbacks.onTagsMutated();
    }

    _runtime.notifications().post(
      rt::NotificationSeverity::Info, result.notificationText, rt::NotificationLifetime::transient());
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

    _contextActionGroupPtr = Gio::SimpleActionGroup::create();

    auto editTagsActionPtr = Gio::SimpleAction::create("edit-tags");
    editTagsActionPtr->signal_activate().connect(
      [this](Glib::VariantBase const&)
      {
        auto* const page = _contextPage;
        auto const xPosition = _contextXPosition;
        auto const yPosition = _contextYPosition;
        scheduleContextPopoverRetirement();

        if (page != nullptr)
        {
          openTagsPopover(*page, xPosition, yPosition);
        }
      });
    _contextActionGroupPtr->add_action(editTagsActionPtr);

    auto propertiesActionPtr = Gio::SimpleAction::create("properties");
    propertiesActionPtr->signal_activate().connect(
      [this](Glib::VariantBase const&)
      {
        scheduleContextPopoverRetirement();
        presentPropertiesDialog();
      });
    _contextActionGroupPtr->add_action(propertiesActionPtr);
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
    auto sessionResult = uimodel::TrackAuthoringSession::begin(_runtime.library(), trackIds);

    if (!sessionResult)
    {
      _tagEditSessionPtr.reset();
      _runtime.notifications().post(
        rt::NotificationSeverity::Error, sessionResult.error().message, rt::NotificationLifetime::sessionHistory());
      return false;
    }

    _tagEditSessionPtr = std::move(*sessionResult);
    return true;
  }
} // namespace ao::gtk
