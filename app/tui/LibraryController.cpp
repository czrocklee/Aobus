// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "LibraryController.h"

#include "LibraryNavigation.h"
#include "SelectionNavigation.h"
#include "TrackListEntry.h"
#include "TrackPresentationNavigation.h"
#include "TrackSection.h"
#include "TuiTextCatalog.h"
#include <ao/CoreIds.h>
#include <ao/Error.h>
#include <ao/i18n/MessageCatalog.h>
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
#include <ao/rt/library/LibrarySnapshot.h>
#include <ao/uimodel/library/presentation/TrackGroupHeadingPresentation.h>
#include <ao/uimodel/library/track/TrackFilter.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

namespace ao::tui
{
  LibraryController::LibraryController(rt::AppRuntime& runtime, i18n::MessageCatalog textCatalog)
    : _runtime{runtime}
    , _textCatalog{std::move(textCatalog)}
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

    // Reached from the workspace observer via refreshPresentationNavigation().
    auto const foundRes = _runtime.views().findTrackListState(_activeViewId);
    return foundRes ? foundRes->presentation.id : std::string{};
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

    if (auto const focusedRes = _runtime.workspace().focusView(_activeViewId); !focusedRes)
    {
      APP_LOG_ERROR("Failed to focus TUI track view: {}", focusedRes.error().message);
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
      return tuiChromeText(_textCatalog, i18n::MessageId::TuiLibraryNoSections);
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
      return tuiChromeText(_textCatalog, i18n::MessageId::TuiLibraryNoSectionSelected);
    }

    auto const& section = _sections[static_cast<std::size_t>(sectionIndex)];
    std::size_t const lastTrackIndex = _tracks.empty() ? 0 : _tracks.size() - 1;
    _selectedTrack = static_cast<std::int32_t>(std::min(section.rowBegin, lastTrackIndex));
    publishSelection();
    return librarySection(_textCatalog, trackSectionDisplayName(_textCatalog, section));
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
    if (auto const foundRes = _runtime.views().findTrackListProjection(_activeViewId); foundRes && *foundRes != nullptr)
    {
      if (auto const optIndex = (*foundRes)->indexOf(trackId);
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
      return tuiChromeText(_textCatalog, i18n::MessageId::TuiLibraryNoCurrentTrack);
    }

    if (setSelectedTrackById(trackId))
    {
      publishSelection();
      return libraryRevealedTrack(_textCatalog, trackDisplayTitle(_textCatalog, _tracks[_selectedTrack].row));
    }

    return tuiChromeText(_textCatalog, i18n::MessageId::TuiLibraryCurrentTrackNotInView);
  }

  std::string LibraryController::setPresentation(std::string_view const presentationId)
  {
    if (_activeViewId == rt::kInvalidViewId)
    {
      return tuiChromeText(_textCatalog, i18n::MessageId::TuiLibraryNoActiveTrackView);
    }

    auto const selectedBefore = selectedTrackView();
    auto const previousTrackId = selectedBefore.track == nullptr ? kInvalidTrackId : selectedBefore.track->id;
    auto const result = _runtime.workspace().setActivePresentation(presentationId);

    if (!result)
    {
      return libraryUnknownView(_textCatalog, presentationId);
    }

    auto const& spec = *result;

    auto snapshot = loadTrackItemsFromView(_activeViewId);
    _tracks = std::move(snapshot.tracks);
    _sections = std::move(snapshot.sections);
    syncSelectedPresentation(spec.id);

    if (!setSelectedTrackById(previousTrackId))
    {
      _selectedTrack = moveSelection(_selectedTrack, 0, _tracks.size());
    }

    publishSelection();
    return libraryView(_textCatalog, spec.id);
  }

  std::string LibraryController::selectSelectedPresentation()
  {
    if (_presentationEntries.empty())
    {
      return tuiChromeText(_textCatalog, i18n::MessageId::TuiLibraryNoViewsAvailable);
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
      _filterError.clear();
      _selectedTrack = 0;
      _currentListId = rt::kAllTracksListId;
      _activeViewId = rt::kInvalidViewId;
      return {.opened = false, .status = tuiChromeText(_textCatalog, i18n::MessageId::TuiLibraryNoListsAvailable)};
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

    return {.opened = true, .status = libraryOpenedList(_textCatalog, currentListTitle())};
  }

  std::string LibraryController::reloadActiveList()
  {
    auto snapshot = loadTrackItems(_currentListId);
    _tracks = std::move(snapshot.tracks);
    _sections = std::move(snapshot.sections);
    _selectedTrack = moveSelection(_selectedTrack, 0, _tracks.size());
    _filterDraft.clear();
    publishSelection();
    return libraryReloadedTracks(_textCatalog, _tracks.size());
  }

  Result<std::string> LibraryController::applyFilter()
  {
    if (_activeViewId == rt::kInvalidViewId)
    {
      return tuiChromeText(_textCatalog, i18n::MessageId::TuiLibraryNoActiveTrackView);
    }

    auto const resolved = uimodel::resolveTrackFilter(_filterDraft);
    auto filterRes = _runtime.views().setFilter(_activeViewId, resolved.expression);

    if (!filterRes)
    {
      _filterError = i18n::requiredFormat(
        _textCatalog, i18n::MessageId::TrackFilterError, {{"diagnostic", filterRes.error().message}});
      return std::unexpected{filterRes.error()};
    }

    refreshFilterError();

    auto snapshot = loadTrackItemsFromView(_activeViewId);
    _tracks = std::move(snapshot.tracks);
    _sections = std::move(snapshot.sections);
    _selectedTrack = 0;
    publishSelection();

    switch (resolved.mode)
    {
      case uimodel::TrackFilterMode::None: return tuiChromeText(_textCatalog, i18n::MessageId::TuiLibraryFilterCleared);
      case uimodel::TrackFilterMode::Quick: return libraryQuickFilterMatched(_textCatalog, _tracks.size());
      case uimodel::TrackFilterMode::Expression: return libraryExpressionFilterMatched(_textCatalog, _tracks.size());
    }

    return tuiChromeText(_textCatalog, i18n::MessageId::TuiLibraryFilterApplied);
  }

  std::vector<LibraryNavEntry> LibraryController::loadLibraryNavigation()
  {
    auto const reader = _runtime.library().snapshot();
    return makeLibraryNavigation(_textCatalog, reader.lists());
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
    return makeTrackPresentationNavigation(
      _textCatalog, rt::builtinTrackPresentationPresets(), _runtime.workspace().customPresets());
  }

  void LibraryController::refreshFilterError()
  {
    _filterError.clear();

    if (_activeViewId == rt::kInvalidViewId)
    {
      return;
    }

    auto const stateRes = _runtime.views().findTrackListState(_activeViewId);

    if (stateRes && stateRes->optFilterError)
    {
      _filterError = i18n::requiredFormat(
        _textCatalog, i18n::MessageId::TrackFilterError, {{"diagnostic", stateRes->optFilterError->message}});
    }
  }

  LibraryController::TrackItemsSnapshot LibraryController::loadTrackItemsFromView(rt::ViewId const activeViewId)
  {
    // Reached from the library-changes observer via reloadActiveList().
    auto const foundProjectionRes = _runtime.views().findTrackListProjection(activeViewId);

    if (!foundProjectionRes || *foundProjectionRes == nullptr)
    {
      return {};
    }

    auto const& projectionPtr = *foundProjectionRes;

    auto const reader = _runtime.library().snapshot();
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
            auto heading = uimodel::formatTrackGroupHeading(_textCatalog, group.heading);
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

        snapshot.tracks.push_back(makeTrackListEntry(_textCatalog, *optRow));
      }
    }

    return snapshot;
  }

  LibraryController::TrackItemsSnapshot LibraryController::loadTrackItems(ListId const listId)
  {
    auto navigationRes = Result<rt::ViewId>{};

    if (listId == rt::kAllTracksListId)
    {
      navigationRes = _runtime.workspace().navigate({.target = rt::GlobalViewKind::AllTracks});
    }
    else
    {
      navigationRes = _runtime.workspace().navigate({.target = listId});
    }

    if (!navigationRes)
    {
      _filterError.clear();
      return {};
    }

    _activeViewId = *navigationRes;
    auto snapshot = loadTrackItemsFromView(_activeViewId);
    refreshFilterError();
    return snapshot;
  }
} // namespace ao::tui
