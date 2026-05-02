// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 RockStudio Contributors

#include "platform/linux/ui/TagEditController.h"
#include "platform/linux/ui/TagPopover.h"
#include <format>
#include <rs/library/TrackBuilder.h>

namespace app::ui
{
  namespace
  {
    bool hasTagName(std::vector<std::string_view> const& tagNames, std::string_view tag)
    {
      return std::ranges::contains(tagNames, tag);
    }

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

  TagEditController::TagEditController(Gtk::Window& /*parent*/, Callbacks callbacks)
    : _callbacks(std::move(callbacks))
  {
    setupActions();
  }

  TagEditController::~TagEditController() = default;

  void TagEditController::setLibrarySession(LibrarySession* session)
  {
    _currentSession = session;
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
    if (_currentSession == nullptr || selection.selectedIds.empty())
    {
      return;
    }

    _activeSelection = selection;

    // Create TagPopover with the selected track IDs
    auto* tagPopover = new TagPopover(*_currentSession->musicLibrary, selection.selectedIds);

    // Connect signal to apply tag changes
    tagPopover->signalTagsChanged().connect(
      [this](std::vector<std::string> const& tagsToAdd, std::vector<std::string> const& tagsToRemove)
      { applyTagChangeToCurrentSelection(tagsToAdd, tagsToRemove); });

    // Show the popover anchored to the right-click position
    page.showTagPopover(*tagPopover, posX, posY);
  }

  void TagEditController::showTagEditor(TrackViewPage& page,
                                        TrackSelectionContext const& selection,
                                        double posX,
                                        double posY)
  {
    if (_currentSession == nullptr || selection.selectedIds.empty())
    {
      return;
    }

    _activeSelection = selection;

    // Create TagPopover with the selected track IDs
    auto* tagPopover = new TagPopover(*_currentSession->musicLibrary, selection.selectedIds);

    // Connect signal to apply tag changes
    tagPopover->signalTagsChanged().connect(
      [this](std::vector<std::string> const& tagsToAdd, std::vector<std::string> const& tagsToRemove)
      { applyTagChangeToCurrentSelection(tagsToAdd, tagsToRemove); });

    // Show popover at mouse position
    tagPopover->set_parent(page.getColumnView());
    auto rect = Gdk::Rectangle{static_cast<int>(posX), static_cast<int>(posY), 1, 1};
    tagPopover->set_pointing_to(rect);
    tagPopover->popup();
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
    if (_currentSession == nullptr || !_activeSelection)
    {
      return;
    }

    auto& selection = *_activeSelection;

    if (selection.selectedIds.empty() || (tagsToAdd.empty() && tagsToRemove.empty()))
    {
      return;
    }

    auto txn = _currentSession->musicLibrary->writeTransaction();
    auto writer = _currentSession->musicLibrary->tracks().writer(txn);
    auto& dict = _currentSession->musicLibrary->dictionary();

    for (auto const trackId : selection.selectedIds)
    {
      auto const optView = writer.get(trackId, rs::library::TrackStore::Reader::LoadMode::Hot);

      if (!optView)
      {
        continue;
      }

      auto builder = rs::library::TrackBuilder::fromView(*optView, dict);

      for (auto const& tag : tagsToRemove)
      {
        builder.tags().remove(tag);
      }

      for (auto const& tag : tagsToAdd)
      {
        if (!hasTagName(builder.tags().names(), tag))
        {
          builder.tags().add(tag);
        }
      }

      auto hotData = builder.serializeHot(txn, dict);
      writer.updateHot(trackId, hotData);
    }

    txn.commit();

    for (auto const trackId : selection.selectedIds)
    {
      _currentSession->rowDataProvider->invalidate(trackId);
    }

    // Notify the master list. This will propagate to all derived lists (Smart Lists, Playlists)
    // via the SmartListEngine and ManualTrackIdList observer chains.
    _currentSession->allTrackIds->notifyUpdated(selection.selectedIds);

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
} // namespace app::ui
