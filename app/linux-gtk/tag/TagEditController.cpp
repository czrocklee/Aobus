// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "tag/TagEditController.h"

#include "app/ThemeCoordinator.h"
#include "tag/TagPopover.h"
#include "tag/TrackPropertiesDialog.h"
#include "track/TrackRowCache.h"
#include "track/TrackViewPage.h"
#include <ao/rt/AppRuntime.h>
#include <ao/rt/NotificationService.h>
#include <ao/rt/StateTypes.h>
#include <ao/uimodel/tag/TagEditWorkflow.h>
#include <ao/utility/Log.h>

#include <giomm/actionmap.h>
#include <giomm/menu.h>
#include <giomm/simpleaction.h>
#include <giomm/simpleactiongroup.h>
#include <glibmm/variant.h>
#include <glibmm/varianttype.h>
#include <gtkmm/object.h>
#include <gtkmm/popovermenu.h>
#include <gtkmm/widget.h>
#include <gtkmm/window.h>

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
                                       ThemeCoordinator& themeController)
    : _callbacks{std::move(callbacks)}, _runtime{runtime}, _parent{parent}, _themeController{themeController}
  {
    setupActions();
  }

  TagEditController::~TagEditController()
  {
    if (_contextPopoverPtr)
    {
      _contextPopoverPtr->unparent();
    }

    if (_tagPopoverPtr)
    {
      _tagPopoverPtr->unparent();
    }
  }

  void TagEditController::setDataProvider(TrackRowCache* provider)
  {
    _dataProvider = provider;
  }

  void TagEditController::setupActions()
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
        if (_contextPopoverPtr)
        {
          _contextPopoverPtr->popdown();
        }

        if (_contextPage)
        {
          showTagsPopover(*_contextPage, _contextPosX, _contextPosY);
        }
      });
    _contextActionGroupPtr->add_action(editTagsActionPtr);

    auto propertiesActionPtr = Gio::SimpleAction::create("properties");
    propertiesActionPtr->signal_activate().connect(
      [this](Glib::VariantBase const&)
      {
        if (_contextPopoverPtr)
        {
          _contextPopoverPtr->popdown();
        }

        showPropertiesDialog();
      });
    _contextActionGroupPtr->add_action(propertiesActionPtr);
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

  void TagEditController::showTrackContextMenu(TrackViewPage& page,
                                               TrackSelectionContext const& selection,
                                               double posX,
                                               double posY)
  {
    if (selection.selectedIds.empty())
    {
      return;
    }

    _optActiveSelection = selection;
    _contextPage = &page;
    _contextPosX = posX;
    _contextPosY = posY;

    _contextPopoverPtr = std::make_unique<Gtk::PopoverMenu>();

    auto menuModelPtr = Gio::Menu::create();
    menuModelPtr->append("Edit Tags", "ctx.edit-tags");
    menuModelPtr->append("Properties", "ctx.properties");

    _contextPopoverPtr->set_menu_model(menuModelPtr);
    _contextPopoverPtr->insert_action_group("ctx", _contextActionGroupPtr);

    _contextPopoverPtr->set_parent(page);
    _contextPopoverPtr->set_has_arrow(false);

    auto const rect = Gdk::Rectangle{static_cast<std::int32_t>(posX), static_cast<std::int32_t>(posY), 1, 1};
    _contextPopoverPtr->set_pointing_to(rect);
    _contextPopoverPtr->popup();
  }

  void TagEditController::showTagsPopover(TrackViewPage& page, double posX, double posY)
  {
    if (!_optActiveSelection)
    {
      return;
    }

    _tagPopoverPtr = std::make_unique<TagPopover>(_runtime.musicLibrary(), _optActiveSelection->selectedIds);

    _tagPopoverPtr->signalTagsChanged().connect(
      [this](std::span<std::string const> tagsToAdd, std::span<std::string const> tagsToRemove)
      { applyTagChangeToCurrentSelection(tagsToAdd, tagsToRemove); });

    page.showTagPopover(*_tagPopoverPtr, posX, posY);
  }

  void TagEditController::showPropertiesDialog()
  {
    if (!_optActiveSelection || _dataProvider == nullptr)
    {
      return;
    }

    auto* const dialog = Gtk::make_managed<TrackPropertiesDialog>(
      _parent, _runtime.musicLibrary(), _runtime.mutation(), *_dataProvider, _optActiveSelection->selectedIds);
    _themeController.registerToplevel(*dialog);
    dialog->present();
  }

  void TagEditController::showProperties(TrackSelectionContext const& selection)
  {
    if (selection.selectedIds.empty() || _dataProvider == nullptr)
    {
      return;
    }

    auto* const dialog = Gtk::make_managed<TrackPropertiesDialog>(
      _parent, _runtime.musicLibrary(), _runtime.mutation(), *_dataProvider, selection.selectedIds);
    _themeController.registerToplevel(*dialog);
    dialog->present();
  }

  void TagEditController::showTagEditor(TrackSelectionContext const& selection, Gtk::Widget& relativeTo)
  {
    if (selection.selectedIds.empty())
    {
      return;
    }

    _optActiveSelection = selection;

    _tagPopoverPtr = std::make_unique<TagPopover>(_runtime.musicLibrary(), selection.selectedIds);

    _tagPopoverPtr->signalTagsChanged().connect(
      [this](std::span<std::string const> tagsToAdd, std::span<std::string const> tagsToRemove)
      { applyTagChangeToCurrentSelection(tagsToAdd, tagsToRemove); });

    _tagPopoverPtr->set_parent(relativeTo);
    _tagPopoverPtr->popup();
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

  void TagEditController::submitTagChanges(TrackSelectionContext const& selection,
                                           std::span<std::string const> tagsToAdd,
                                           std::span<std::string const> tagsToRemove)
  {
    auto request = ao::uimodel::tag::TagEditRequest{};
    request.selectedIds = selection.selectedIds;
    request.tagsToAdd.assign(tagsToAdd.begin(), tagsToAdd.end());
    request.tagsToRemove.assign(tagsToRemove.begin(), tagsToRemove.end());

    auto workflow = ao::uimodel::tag::TagEditWorkflow{_runtime.mutation()};
    auto const result = workflow.apply(request);

    if (result.optError)
    {
      APP_LOG_ERROR("{}", result.notificationText);
      _runtime.notifications().post(rt::NotificationSeverity::Error, result.notificationText);
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

    _runtime.notifications().post(rt::NotificationSeverity::Info, result.notificationText);
  }
} // namespace ao::gtk
