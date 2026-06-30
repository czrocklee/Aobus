// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "LibraryController.h"

#include "Model.h"
#include <ao/CoreIds.h>
#include <ao/rt/AppRuntime.h>
#include <ao/rt/CorePrimitives.h>
#include <ao/rt/ViewService.h>
#include <ao/rt/ViewState.h>
#include <ao/rt/WorkspaceService.h>
#include <ao/rt/library/Library.h>
#include <ao/rt/library/LibraryReader.h>
#include <ao/uimodel/library/track/TrackFilterResolver.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <format>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ao::tui
{
  LibraryController::LibraryController(rt::AppRuntime& runtime)
    : _runtime{runtime}, _libraryItems{loadLibraryNavigation()}, _libraryLabels{libraryNavigationLabels(_libraryItems)}
  {
    _tracks = loadTrackItems(_currentListId);
    publishSelection();
  }

  std::string LibraryController::currentListTitle() const
  {
    return listTitle(_currentListId, _libraryItems);
  }

  SelectedTrackView LibraryController::selectedTrackView() const
  {
    if (_tracks.empty())
    {
      return {};
    }

    auto const selectedIndex = clampSelection(static_cast<std::size_t>(std::max(0, _selectedTrack)), _tracks.size());
    return {.track = &_tracks[selectedIndex], .coverArtId = _tracks[selectedIndex].coverArtId};
  }

  void LibraryController::setFilterDraft(std::string value)
  {
    _filterDraft = std::move(value);
  }

  void LibraryController::clearFilterDraft()
  {
    _filterDraft.clear();
  }

  void LibraryController::publishSelection()
  {
    if (_activeViewId == rt::kInvalidViewId || _tracks.empty())
    {
      return;
    }

    auto const index = clampSelection(static_cast<std::size_t>(std::max(0, _selectedTrack)), _tracks.size());
    _runtime.views().setSelection(_activeViewId, {_tracks[index].id});
    _runtime.workspace().setFocusedView(_activeViewId);
  }

  void LibraryController::moveFocusedSelection(bool const listChooserFocused, std::int32_t const delta)
  {
    if (listChooserFocused)
    {
      _selectedList = moveSelection(_selectedList, delta, _libraryItems.size());
      return;
    }

    _selectedTrack = moveSelection(_selectedTrack, delta, _tracks.size());
    publishSelection();
  }

  bool LibraryController::setSelectedTrackById(TrackId const trackId)
  {
    if (trackId == kInvalidTrackId)
    {
      return false;
    }

    for (std::size_t index = 0; index < _tracks.size(); ++index)
    {
      if (_tracks[index].id == trackId)
      {
        _selectedTrack = static_cast<std::int32_t>(index);
        return true;
      }
    }

    return false;
  }

  std::string LibraryController::revealTrack(TrackId const trackId)
  {
    if (trackId == kInvalidTrackId)
    {
      return "No current track";
    }

    if (setSelectedTrackById(trackId))
    {
      publishSelection();
      return std::format("Revealed {}", trackDisplayTitle(_tracks[_selectedTrack].row));
    }

    return "Current track is not in this view";
  }

  std::string LibraryController::setPresentation(std::string_view const presentationId)
  {
    if (_activeViewId == rt::kInvalidViewId)
    {
      return "No active track view";
    }

    auto const selectedBefore = selectedTrackView();
    auto const previousTrackId = selectedBefore.track == nullptr ? kInvalidTrackId : selectedBefore.track->id;
    auto const spec = _runtime.workspace().setActivePresentation(presentationId);

    if (spec.id.empty())
    {
      return std::format("Unknown view {}", presentationId);
    }

    _tracks = loadTrackItemsFromView(_activeViewId);

    if (!setSelectedTrackById(previousTrackId))
    {
      _selectedTrack = moveSelection(_selectedTrack, 0, _tracks.size());
    }

    publishSelection();
    return std::format("View: {}", spec.id);
  }

  ListOpenResult LibraryController::openSelectedList()
  {
    if (_libraryItems.empty())
    {
      _tracks.clear();
      _selectedTrack = 0;
      _currentListId = rt::kAllTracksListId;
      _activeViewId = rt::kInvalidViewId;
      return {.opened = false, .status = "No lists available"};
    }

    auto const selectedIndex =
      clampSelection(static_cast<std::size_t>(std::max(0, _selectedList)), _libraryItems.size());
    _currentListId = _libraryItems[selectedIndex].id;
    _tracks = loadTrackItems(_currentListId);
    _selectedTrack = 0;
    _filterDraft.clear();
    publishSelection();

    return {.opened = true, .status = std::format("Opened {}", currentListTitle())};
  }

  std::string LibraryController::reloadActiveList()
  {
    _tracks = loadTrackItems(_currentListId);
    _selectedTrack = moveSelection(_selectedTrack, 0, _tracks.size());
    _filterDraft.clear();
    publishSelection();
    return std::format("Reloaded {} tracks", _tracks.size());
  }

  std::string LibraryController::applyFilter()
  {
    if (_activeViewId == rt::kInvalidViewId)
    {
      return "No active track view";
    }

    auto const resolved = uimodel::resolveTrackFilterExpression(_filterDraft);
    _runtime.views().setFilter(_activeViewId, resolved.expression);
    _tracks = loadTrackItemsFromView(_activeViewId);
    _selectedTrack = 0;
    publishSelection();

    switch (resolved.mode)
    {
      case uimodel::TrackFilterMode::None: return "Filter cleared";
      case uimodel::TrackFilterMode::Quick: return std::format("Quick filter matched {} tracks", _tracks.size());
      case uimodel::TrackFilterMode::Expression:
        return std::format("Expression filter matched {} tracks", _tracks.size());
    }

    return "Filter applied";
  }

  std::vector<LibraryNavItem> LibraryController::loadLibraryNavigation()
  {
    auto reader = _runtime.library().reader();
    return makeLibraryNavigation(reader.lists());
  }

  std::vector<TrackListItem> LibraryController::loadTrackItemsFromView(rt::ViewId const activeViewId)
  {
    auto projectionPtr = _runtime.views().trackListProjection(activeViewId);

    if (projectionPtr == nullptr)
    {
      return {};
    }

    auto reader = _runtime.library().reader();
    auto tracks = std::vector<TrackListItem>{};
    tracks.reserve(projectionPtr->size());

    for (std::size_t index = 0; index < projectionPtr->size(); ++index)
    {
      auto const trackId = projectionPtr->trackIdAt(index);

      if (auto optRow = reader.trackRow(trackId); optRow)
      {
        tracks.push_back(makeTrackListItem(*optRow));
      }
    }

    return tracks;
  }

  std::vector<TrackListItem> LibraryController::loadTrackItems(ListId const listId)
  {
    _runtime.reloadAllTracks();

    if (listId == rt::kAllTracksListId)
    {
      _runtime.workspace().navigateTo(rt::GlobalViewKind::AllTracks);
    }
    else
    {
      _runtime.workspace().navigateTo(listId);
    }

    _activeViewId = _runtime.workspace().layoutState().activeViewId;
    return loadTrackItemsFromView(_activeViewId);
  }
} // namespace ao::tui
