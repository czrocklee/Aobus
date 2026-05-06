// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "TagEditController.h"
#include "TagPopover.h"
#include "TrackRowDataProvider.h"
#include <ao/library/TrackBuilder.h>
#include <runtime/AppSession.h>
#include <runtime/EventBus.h>
#include <runtime/EventTypes.h>
#include <runtime/LibraryMutationService.h>

namespace ao::gtk
{
  namespace
  {
    std::string tagChangeStatusMessage(std::size_t trackCount, std::size_t addedCount, std::size_t removedCount)
    {
      auto parts = std::vector<std::string>{};

      if (addedCount > 0)
      {
        parts.push_back(std::format("added {}", addedCount));
      }

      if (removedCount > 0)
      {
        parts.push_back(std::format("removed {}", removedCount));
      }

      if (parts.empty())
      {
        return std::format("Tags unchanged for {} track{}", trackCount, trackCount == 1 ? "" : "s");
      }

      auto message = std::format("Tags {}", parts[0]);

      if (parts.size() > 1)
      {
        message += " and " + parts[1];
      }

      return std::format("{} for {} track{}", message, trackCount, trackCount == 1 ? "" : "s");
    }
  }

  TagEditController::TagEditController(Gtk::Window& /*parent*/, ao::app::AppSession& session, Callbacks callbacks)
    : _callbacks{std::move(callbacks)}, _session{session}
  {
    setupActions();
  }

  TagEditController::~TagEditController() = default;

  void TagEditController::setDataProvider(TrackRowDataProvider* provider)
  {
    _dataProvider = provider;
  }

  void TagEditController::setupActions()
  {
    auto const stringType = Glib::VariantType("s");

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

    _tagPopover = std::make_unique<TagPopover>(_session.musicLibrary(), selection.selectedIds);

    _tagPopover->signalTagsChanged().connect(
      [this](std::vector<std::string> const& tagsToAdd, std::vector<std::string> const& tagsToRemove)
      { applyTagChangeToCurrentSelection(tagsToAdd, tagsToRemove); });

    page.showTagPopover(*_tagPopover, posX, posY);
  }

  void TagEditController::showTagEditor(TrackSelectionContext const& selection, Gtk::Widget& relativeTo)
  {
    if (selection.selectedIds.empty())
    {
      return;
    }

    _optActiveSelection = selection;

    _tagPopover = std::make_unique<TagPopover>(_session.musicLibrary(), selection.selectedIds);

    _tagPopover->signalTagsChanged().connect(
      [this](std::vector<std::string> const& tagsToAdd, std::vector<std::string> const& tagsToRemove)
      { applyTagChangeToCurrentSelection(tagsToAdd, tagsToRemove); });

    _tagPopover->set_parent(relativeTo);
    _tagPopover->popup();
  }

  void TagEditController::addTagToCurrentSelection(std::string const& tag)
  {
    applyTagChangeToCurrentSelection({tag}, {});
  }

  void TagEditController::removeTagFromCurrentSelection(std::string const& tag)
  {
    applyTagChangeToCurrentSelection({}, {tag});
  }

  void TagEditController::applyTagChangeToCurrentSelection(std::vector<std::string> const& tagsToAdd,
                                                           std::vector<std::string> const& tagsToRemove)
  {
    if (!_optActiveSelection)
    {
      return;
    }

    auto& selection = *_optActiveSelection;

    if (selection.selectedIds.empty() || (tagsToAdd.empty() && tagsToRemove.empty()))
    {
      return;
    }

    auto const result = _session.mutation().editTags(selection.selectedIds, tagsToAdd, tagsToRemove);

    if (!result)
    {
      APP_LOG_ERROR("Failed to edit tags: {}", result.error().message);

      if (_callbacks.onStatusMessage)
      {
        _callbacks.onStatusMessage(std::format("Failed to edit tags: {}", result.error().message));
      }

      return;
    }

    if (_callbacks.onTagsMutated)
    {
      _callbacks.onTagsMutated();
    }

    if (_callbacks.onStatusMessage)
    {
      _callbacks.onStatusMessage(
        tagChangeStatusMessage(selection.selectedIds.size(), tagsToAdd.size(), tagsToRemove.size()));
    }
  }
} // namespace ao::gtk
