// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "LibraryController.h"

#include "LibraryNavigation.h"
#include "SelectionNavigation.h"
#include "TrackListEntry.h"
#include "TrackPresentationNavigation.h"
#include "TrackSection.h"
#include <ao/CoreIds.h>
#include <ao/Error.h>
#include <ao/rt/AppRuntime.h>
#include <ao/rt/Log.h>
#include <ao/rt/TrackPresentation.h>
#include <ao/rt/ViewIds.h>
#include <ao/rt/ViewService.h>
#include <ao/rt/ViewState.h>
#include <ao/rt/VirtualListIds.h>
#include <ao/rt/WorkspaceService.h>
#include <ao/rt/WorkspaceSnapshot.h>
#include <ao/rt/library/Library.h>
#include <ao/rt/library/LibraryChanges.h>
#include <ao/rt/library/LibraryReader.h>
#include <ao/uimodel/library/presentation/TrackGroupHeadingPresentation.h>
#include <ao/uimodel/library/track/TrackCountFormatter.h>
#include <ao/uimodel/library/track/TrackFilterResolver.h>
#include <ao/uimodel/presentation/PresentationTextCatalog.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <format>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

namespace ao::tui
{
  LibraryController::LibraryController(rt::AppRuntime& runtime)
    : _runtime{runtime}
    , _libraryEntries{loadLibraryNavigation()}
    , _libraryLabels{libraryNavigationLabels(_libraryEntries)}
    , _presentationEntries{loadPresentationNavigation()}
  {
    auto snapshot = loadTrackItems(_currentListId);
    _tracks = std::move(snapshot.tracks);
    _sections = std::move(snapshot.sections);
    syncSelectedPresentation(activePresentationId());
    _customPresetsSub = _runtime.workspace().onChanged(
      [this](rt::WorkspaceChanged const& changed)
      {
        if (changed.cause == rt::WorkspaceChangeCause::Presets || changed.cause == rt::WorkspaceChangeCause::Restore)
        {
          refreshPresentationNavigation();
        }
      });
    _libraryChangesSub = _runtime.library().changes().onChanged(
      [this](rt::LibraryChangeSet const& changeSet)
      {
        if (changeSet.libraryReset || !changeSet.listsUpserted.empty() || !changeSet.listsDeleted.empty())
        {
          _libraryEntries = loadLibraryNavigation();
          _libraryLabels = libraryNavigationLabels(_libraryEntries);
        }

        std::ignore = reloadActiveList();
      });
    publishSelection();
  }

  std::string LibraryController::currentListTitle() const
  {
    return listTitle(_currentListId, _libraryEntries);
  }

  std::string LibraryController::activePresentationId() const
  {
    if (_activeViewId == rt::kInvalidViewId)
    {
      return {};
    }

    return _runtime.views().trackListState(_activeViewId).presentation.id;
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

    if (auto result = _runtime.views().setSelection(_activeViewId, {_tracks[index].id}); !result)
    {
      APP_LOG_ERROR("Failed to publish TUI selection: {}", result.error().message);
    }

    if (auto const focused = _runtime.workspace().focusView(_activeViewId); !focused)
    {
      APP_LOG_ERROR("Failed to focus TUI track view: {}", focused.error().message);
    }
  }

  void LibraryController::moveFocusedSelection(bool const listChooserFocused, std::int32_t const delta)
  {
    if (listChooserFocused)
    {
      _selectedList = moveSelection(_selectedList, delta, _libraryEntries.size());
      return;
    }

    _selectedTrack = moveSelection(_selectedTrack, delta, _tracks.size());
    publishSelection();
  }

  void LibraryController::movePresentationSelection(std::int32_t const delta)
  {
    _selectedPresentation = moveSelection(_selectedPresentation, delta, _presentationEntries.size());
  }

  bool LibraryController::setSelectedPresentation(std::int32_t const index)
  {
    if (index < 0 || static_cast<std::size_t>(index) >= _presentationEntries.size())
    {
      return false;
    }

    _selectedPresentation = index;
    return true;
  }

  void LibraryController::setSelectedTrackIndex(std::int32_t const index)
  {
    _selectedTrack = moveSelection(index, 0, _tracks.size());
    publishSelection();
  }

  std::string LibraryController::jumpToAdjacentSection(std::int32_t const delta)
  {
    if (_sections.empty())
    {
      return "No sections in this view";
    }

    auto optContainingSection = std::optional<std::int32_t>{};
    auto optPreviousSection = std::optional<std::int32_t>{};
    auto optNextSection = std::optional<std::int32_t>{};
    auto const selected = static_cast<std::size_t>(std::max(0, _selectedTrack));

    for (std::size_t index = 0; index < _sections.size(); ++index)
    {
      auto const& section = _sections[index];

      if (selected >= section.rowBegin && selected < section.rowBegin + section.rowCount)
      {
        optContainingSection = static_cast<std::int32_t>(index);
        break;
      }

      if (selected >= section.rowBegin)
      {
        optPreviousSection = static_cast<std::int32_t>(index);
        continue;
      }

      optNextSection = static_cast<std::int32_t>(index);
      break;
    }

    auto const nextSection = [&]
    {
      if (optContainingSection)
      {
        return moveSelection(*optContainingSection, delta, _sections.size());
      }

      if (delta > 0)
      {
        return optNextSection.value_or(optPreviousSection.value_or(0));
      }

      if (delta < 0)
      {
        return optPreviousSection.value_or(optNextSection.value_or(0));
      }

      return optPreviousSection.value_or(optNextSection.value_or(0));
    }();

    return selectSection(nextSection);
  }

  std::string LibraryController::selectSection(std::int32_t const sectionIndex)
  {
    if (sectionIndex < 0 || static_cast<std::size_t>(sectionIndex) >= _sections.size())
    {
      return "No section selected";
    }

    auto const& section = _sections[static_cast<std::size_t>(sectionIndex)];
    std::size_t const lastTrackIndex = _tracks.empty() ? 0 : _tracks.size() - 1;
    _selectedTrack = static_cast<std::int32_t>(std::min(section.rowBegin, lastTrackIndex));
    publishSelection();
    return std::format("Section: {}", trackSectionDisplayName(section));
  }

  bool LibraryController::setSelectedTrackById(TrackId const trackId)
  {
    if (trackId == kInvalidTrackId)
    {
      return false;
    }

    // The projection maintains an indexed track-to-row lookup. _tracks can drift
    // from projection indices when a row's LMDB lookup was skipped, so trust the
    // index only when the materialized row matches and otherwise scan below.
    if (auto const projectionPtr = _runtime.views().trackListProjection(_activeViewId); projectionPtr != nullptr)
    {
      if (auto const optIndex = projectionPtr->indexOf(trackId);
          optIndex && *optIndex < _tracks.size() && _tracks[*optIndex].id == trackId)
      {
        _selectedTrack = static_cast<std::int32_t>(*optIndex);
        return true;
      }
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
    auto const result = _runtime.workspace().setActivePresentation(presentationId);

    if (!result)
    {
      return std::format("Unknown view {}", presentationId);
    }

    auto const& spec = result->presentation;

    auto snapshot = loadTrackItemsFromView(_activeViewId);
    _tracks = std::move(snapshot.tracks);
    _sections = std::move(snapshot.sections);
    syncSelectedPresentation(spec.id);

    if (!setSelectedTrackById(previousTrackId))
    {
      _selectedTrack = moveSelection(_selectedTrack, 0, _tracks.size());
    }

    publishSelection();
    return std::format("View: {}", spec.id);
  }

  std::string LibraryController::selectSelectedPresentation()
  {
    if (_presentationEntries.empty())
    {
      return "No views available";
    }

    auto const selectedIndex =
      clampSelection(static_cast<std::size_t>(std::max(0, _selectedPresentation)), _presentationEntries.size());
    return setPresentation(_presentationEntries[selectedIndex].id);
  }

  ListOpenResult LibraryController::openSelectedList()
  {
    if (_libraryEntries.empty())
    {
      _tracks.clear();
      _sections.clear();
      _selectedTrack = 0;
      _currentListId = rt::kAllTracksListId;
      _activeViewId = rt::kInvalidViewId;
      return {.opened = false, .status = "No lists available"};
    }

    auto const selectedIndex =
      clampSelection(static_cast<std::size_t>(std::max(0, _selectedList)), _libraryEntries.size());
    _currentListId = _libraryEntries[selectedIndex].id;
    auto snapshot = loadTrackItems(_currentListId);
    _tracks = std::move(snapshot.tracks);
    _sections = std::move(snapshot.sections);
    _selectedTrack = 0;
    _filterDraft.clear();
    publishSelection();

    return {.opened = true, .status = std::format("Opened {}", currentListTitle())};
  }

  std::string LibraryController::reloadActiveList()
  {
    auto snapshot = loadTrackItems(_currentListId);
    _tracks = std::move(snapshot.tracks);
    _sections = std::move(snapshot.sections);
    _selectedTrack = moveSelection(_selectedTrack, 0, _tracks.size());
    _filterDraft.clear();
    publishSelection();
    return std::format("Reloaded {}", uimodel::formatTrackCount(_tracks.size()));
  }

  std::string LibraryController::applyFilter()
  {
    if (_activeViewId == rt::kInvalidViewId)
    {
      return "No active track view";
    }

    auto const resolved = uimodel::resolveTrackFilterExpression(_filterDraft);
    std::ignore = _runtime.views().setFilter(_activeViewId, resolved.expression);
    auto snapshot = loadTrackItemsFromView(_activeViewId);
    _tracks = std::move(snapshot.tracks);
    _sections = std::move(snapshot.sections);
    _selectedTrack = 0;
    publishSelection();

    switch (resolved.mode)
    {
      case uimodel::TrackFilterMode::None: return "Filter cleared";
      case uimodel::TrackFilterMode::Quick:
        return std::format("Quick filter matched {}", uimodel::formatTrackCount(_tracks.size()));
      case uimodel::TrackFilterMode::Expression:
        return std::format("Expression filter matched {}", uimodel::formatTrackCount(_tracks.size()));
    }

    return "Filter applied";
  }

  std::vector<LibraryNavEntry> LibraryController::loadLibraryNavigation()
  {
    auto const reader = _runtime.library().reader();
    return makeLibraryNavigation(reader.lists());
  }

  void LibraryController::syncSelectedPresentation(std::string_view const presentationId)
  {
    auto const it = std::ranges::find(_presentationEntries, presentationId, &TrackPresentationNavEntry::id);

    if (it == _presentationEntries.end())
    {
      _selectedPresentation = moveSelection(_selectedPresentation, 0, _presentationEntries.size());
      return;
    }

    _selectedPresentation = static_cast<std::int32_t>(std::distance(_presentationEntries.begin(), it));
  }

  void LibraryController::refreshPresentationNavigation()
  {
    _presentationEntries = loadPresentationNavigation();
    syncSelectedPresentation(activePresentationId());
  }

  std::vector<TrackPresentationNavEntry> LibraryController::loadPresentationNavigation()
  {
    return makeTrackPresentationNavigation(rt::builtinTrackPresentationPresets(), _runtime.workspace().customPresets());
  }

  LibraryController::TrackItemsSnapshot LibraryController::loadTrackItemsFromView(rt::ViewId const activeViewId)
  {
    auto const projectionPtr = _runtime.views().trackListProjection(activeViewId);

    if (projectionPtr == nullptr)
    {
      return {};
    }

    auto const reader = _runtime.library().reader();
    auto snapshot = TrackItemsSnapshot{};
    snapshot.tracks.reserve(projectionPtr->size());
    snapshot.sections.reserve(projectionPtr->groupCount());

    auto optActiveGroupIndex = std::optional<std::size_t>{};

    for (std::size_t index = 0; index < projectionPtr->size(); ++index)
    {
      auto const trackId = projectionPtr->trackIdAt(index);

      if (auto optRow = reader.trackRow(trackId); optRow)
      {
        auto const optGroupIndex = projectionPtr->groupIndexAt(index);

        if (optGroupIndex && *optGroupIndex < projectionPtr->groupCount())
        {
          if (!optActiveGroupIndex || *optActiveGroupIndex != *optGroupIndex)
          {
            auto group = projectionPtr->groupAt(*optGroupIndex);
            auto heading = uimodel::formatTrackGroupHeading(uimodel::PresentationTextCatalog{}, group.heading);
            snapshot.sections.push_back(TrackSection{
              .rowBegin = snapshot.tracks.size(),
              .rowCount = 0,
              .primaryText = std::move(heading.primaryText),
              .secondaryText = std::move(heading.secondaryText),
              .tertiaryText = std::move(heading.tertiaryText),
              .imageId = group.imageId,
            });
            optActiveGroupIndex = optGroupIndex;
          }

          ++snapshot.sections.back().rowCount;
        }
        else
        {
          optActiveGroupIndex.reset();
        }

        snapshot.tracks.push_back(makeTrackListEntry(*optRow));
      }
    }

    return snapshot;
  }

  LibraryController::TrackItemsSnapshot LibraryController::loadTrackItems(ListId const listId)
  {
    _runtime.reloadAllTracks();
    auto navigationResult = Result<rt::WorkspaceCommitReceipt>{};

    if (listId == rt::kAllTracksListId)
    {
      navigationResult = _runtime.workspace().navigateTo(rt::GlobalViewKind::AllTracks);
    }
    else
    {
      navigationResult = _runtime.workspace().navigateTo(listId);
    }

    if (!navigationResult)
    {
      return {};
    }

    _activeViewId = navigationResult->activeViewId;
    return loadTrackItemsFromView(_activeViewId);
  }
} // namespace ao::tui
