// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include "Model.h"
#include <ao/CoreIds.h>
#include <ao/rt/CorePrimitives.h>

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace ao::rt
{
  class AppRuntime;
} // namespace ao::rt

namespace ao::tui
{
  struct ListOpenResult final
  {
    bool opened = false;
    std::string status{};
  };

  struct SelectedTrackView final
  {
    TrackListItem const* track = nullptr;
    ResourceId coverArtId = kInvalidResourceId;
  };

  class LibraryController final
  {
  public:
    explicit LibraryController(rt::AppRuntime& runtime);

    std::vector<LibraryNavItem> const& libraryItems() const noexcept { return _libraryItems; }
    std::vector<std::string> const& libraryLabels() const noexcept { return _libraryLabels; }
    std::vector<TrackListItem> const& tracks() const noexcept { return _tracks; }
    ListId currentListId() const noexcept { return _currentListId; }
    rt::ViewId activeViewId() const noexcept { return _activeViewId; }
    std::int32_t selectedList() const noexcept { return _selectedList; }
    std::int32_t selectedTrack() const noexcept { return _selectedTrack; }
    std::string const& filterDraft() const noexcept { return _filterDraft; }

    std::string currentListTitle() const;
    SelectedTrackView selectedTrackView() const;

    void setFilterDraft(std::string value);
    void clearFilterDraft();
    void publishSelection();
    void moveFocusedSelection(bool listChooserFocused, std::int32_t delta);

    std::string revealTrack(TrackId trackId);
    std::string setPresentation(std::string_view presentationId);
    ListOpenResult openSelectedList();
    std::string reloadActiveList();
    std::string applyFilter();

  private:
    bool setSelectedTrackById(TrackId trackId);
    std::vector<LibraryNavItem> loadLibraryNavigation();
    std::vector<TrackListItem> loadTrackItemsFromView(rt::ViewId activeViewId);
    std::vector<TrackListItem> loadTrackItems(ListId listId);

    rt::AppRuntime& _runtime;
    std::vector<LibraryNavItem> _libraryItems{};
    std::vector<std::string> _libraryLabels{};
    std::vector<TrackListItem> _tracks{};
    ListId _currentListId{rt::kAllTracksListId};
    rt::ViewId _activeViewId{rt::kInvalidViewId};
    std::int32_t _selectedList = 0;
    std::int32_t _selectedTrack = 0;
    std::string _filterDraft{};
  };
} // namespace ao::tui
