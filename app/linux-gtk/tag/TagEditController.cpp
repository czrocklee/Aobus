// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "tag/TagEditController.h"

#include "ao/utility/Log.h"
#include "tag/TagPopover.h"
#include "tag/TrackPropertiesDialog.h"
#include "track/TrackRowCache.h"
#include "track/TrackViewPage.h"
#include <ao/rt/AppRuntime.h>
#include <ao/rt/NotificationService.h>
#include <ao/rt/StateTypes.h>
#include <ao/uimodel/tag/TagEditWorkflow.h>

#include <giomm/actionmap.h>
#include <giomm/simpleaction.h>
#include <glibmm/variant.h>
#include <glibmm/varianttype.h>
#include <gtkmm/box.h>
#include <gtkmm/button.h>
#include <gtkmm/enums.h>
#include <gtkmm/object.h>
#include <gtkmm/popover.h>
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
    constexpr int kContextMenuButtonSpacing = 2;
  }

  TagEditController::TagEditController(Gtk::Window& parent, rt::AppRuntime& runtime, Callbacks callbacks)
    : _callbacks{std::move(callbacks)}, _runtime{runtime}, _parent{parent}
  {
    setupActions();
  }

  TagEditController::~TagEditController()
  {
    if (_contextPopover)
    {
      _contextPopover->unparent();
    }

    if (_tagPopover)
    {
      _tagPopover->unparent();
    }
  }

  void TagEditController::setDataProvider(TrackRowCache* provider)
  {
    _dataProvider = provider;
  }

  void TagEditController::setupActions()
  {
    auto const stringType = Glib::VariantType{"s"};

    _trackTagAddAction = Gio::SimpleAction::create("track-tag-add", stringType);
    _trackTagAddAction->signal_activate().connect(
      [this](Glib::VariantBase const& parameter)
      { addTagToCurrentSelection(Glib::VariantBase::cast_dynamic<Glib::Variant<std::string>>(parameter).get()); });

    _trackTagRemoveAction = Gio::SimpleAction::create("track-tag-remove", stringType);
    _trackTagRemoveAction->signal_activate().connect(
      [this](Glib::VariantBase const& parameter)
      { removeTagFromCurrentSelection(Glib::VariantBase::cast_dynamic<Glib::Variant<std::string>>(parameter).get()); });
  }

  void TagEditController::addActionsTo(Gio::ActionMap& actionMap)
  {
    if (_trackTagAddAction)
    {
      actionMap.add_action(_trackTagAddAction);
    }

    if (_trackTagRemoveAction)
    {
      actionMap.add_action(_trackTagRemoveAction);
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

    _contextPopover = std::make_unique<Gtk::Popover>();
    _contextPopover->add_css_class("ao-context-menu");

    auto* const menuBox = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, kContextMenuButtonSpacing);
    menuBox->set_margin_start(2);
    menuBox->set_margin_end(2);
    menuBox->set_margin_top(2);
    menuBox->set_margin_bottom(2);

    auto* const tagsButton = Gtk::make_managed<Gtk::Button>("Edit Tags");
    tagsButton->set_halign(Gtk::Align::FILL);
    tagsButton->set_hexpand(true);
    tagsButton->signal_clicked().connect(
      [this, &page, posX, posY]
      {
        _contextPopover->popdown();
        showTagsPopover(page, posX, posY);
      });
    menuBox->append(*tagsButton);

    auto* const propertiesButton = Gtk::make_managed<Gtk::Button>("Properties");
    propertiesButton->set_halign(Gtk::Align::FILL);
    propertiesButton->set_hexpand(true);
    propertiesButton->signal_clicked().connect(
      [this]
      {
        _contextPopover->popdown();
        showPropertiesDialog();
      });
    menuBox->append(*propertiesButton);

    _contextPopover->set_child(*menuBox);
    _contextPopover->set_parent(page);

    auto const rect = Gdk::Rectangle{static_cast<std::int32_t>(posX), static_cast<std::int32_t>(posY), 1, 1};
    _contextPopover->set_pointing_to(rect);
    _contextPopover->popup();
  }

  void TagEditController::showTagsPopover(TrackViewPage& page, double posX, double posY)
  {
    if (!_optActiveSelection)
    {
      return;
    }

    _tagPopover = std::make_unique<TagPopover>(_runtime.musicLibrary(), _optActiveSelection->selectedIds);

    _tagPopover->signalTagsChanged().connect(
      [this](std::span<std::string const> tagsToAdd, std::span<std::string const> tagsToRemove)
      { applyTagChangeToCurrentSelection(tagsToAdd, tagsToRemove); });

    page.showTagPopover(*_tagPopover, posX, posY);
  }

  void TagEditController::showPropertiesDialog()
  {
    if (!_optActiveSelection || _dataProvider == nullptr)
    {
      return;
    }

    auto* const dialog = Gtk::make_managed<TrackPropertiesDialog>(
      _parent, _runtime.musicLibrary(), _runtime.mutation(), *_dataProvider, _optActiveSelection->selectedIds);
    dialog->present();
  }

  void TagEditController::showTagEditor(TrackSelectionContext const& selection, Gtk::Widget& relativeTo)
  {
    if (selection.selectedIds.empty())
    {
      return;
    }

    _optActiveSelection = selection;

    _tagPopover = std::make_unique<TagPopover>(_runtime.musicLibrary(), selection.selectedIds);

    _tagPopover->signalTagsChanged().connect(
      [this](std::span<std::string const> tagsToAdd, std::span<std::string const> tagsToRemove)
      { applyTagChangeToCurrentSelection(tagsToAdd, tagsToRemove); });

    _tagPopover->set_parent(relativeTo);
    _tagPopover->popup();
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

    auto& selection = *_optActiveSelection;

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
