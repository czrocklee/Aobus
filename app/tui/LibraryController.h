// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include "LibraryNavigation.h"
#include "TrackListEntry.h"
#include "TrackPresentationNavigation.h"
#include "TrackSection.h"
#include <ao/CoreIds.h>
#include <ao/Error.h>
#include <ao/async/Subscription.h>
#include <ao/i18n/MessageCatalog.h>
#include <ao/rt/TrackPresentation.h>
#include <ao/rt/ViewIds.h>
#include <ao/rt/VirtualListIds.h>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace ao::rt
{
  class Library;
  class ViewService;
  class WorkspaceService;
} // namespace ao::rt

namespace ao::uimodel
{
  class ListPresentations;
} // namespace ao::uimodel

namespace ao::tui
{
  struct ListOpenResult final
  {
    bool opened = false;
    std::string status{};
  };

  struct SelectedTrackView final
  {
    TrackListEntry const* track = nullptr;
    ResourceId coverArtId = kInvalidResourceId;
  };

  class LibraryController final
  {
  public:
    LibraryController(rt::Library& library,
                      rt::ViewService& views,
                      rt::WorkspaceService& workspace,
                      i18n::MessageCatalog textCatalog,
                      uimodel::ListPresentations& listPresentations);

    std::vector<LibraryNavEntry> const& libraryEntries() const noexcept { return _libraryEntries; }
    std::vector<std::string> const& libraryLabels() const noexcept { return _libraryLabels; }
    std::vector<TrackPresentationNavEntry> const& presentationEntries() const noexcept { return _presentationEntries; }
    std::vector<TrackListEntry> const& tracks() const noexcept { return _tracks; }
    std::vector<TrackSection> const& sections() const noexcept { return _sections; }
    ListId currentListId() const noexcept { return _currentListId; }
    rt::ViewId activeViewId() const noexcept { return _activeViewId; }
    std::int32_t selectedList() const noexcept { return _selectedList; }
    std::int32_t selectedPresentation() const noexcept { return _selectedPresentation; }
    std::int32_t selectedTrack() const noexcept { return _selectedTrack; }
    std::unordered_set<TrackId> const& markedIds() const noexcept { return _markedIds; }
    bool isVisualSelectionActive() const noexcept { return _optVisualAnchor.has_value(); }
    // Marked ids in current materialized track order, or the focused track when
    // none are marked.
    std::vector<TrackId> selectedTrackIds() const;
    std::string const& filterDraft() const noexcept { return _filterDraft; }
    std::string const& filterError() const noexcept { return _filterError; }
    i18n::MessageCatalog const& textCatalog() const noexcept { return _textCatalog; }

    std::string currentListTitle() const;
    std::string activePresentationId() const;
    // Borrowed from the active view (or the process-stable default); consume
    // before the active view can be replaced or destroyed.
    rt::TrackPresentationSpec const& activePresentation() const;
    SelectedTrackView selectedTrackView() const;

    void setFilterDraft(std::string value);
    void clearFilterDraft();
    void moveFocusedSelection(bool listChooserFocused, std::int32_t delta);
    void movePresentationSelection(std::int32_t delta);
    bool setSelectedPresentation(std::int32_t index);
    void setSelectedTrackIndex(std::int32_t index);
    void toggleFocusedMark();
    void toggleVisualSelection();
    void cancelVisualSelection();
    void markAllTracks();
    void clearMarks();

    std::string jumpToAdjacentSection(std::int32_t delta);
    std::string selectSection(std::int32_t sectionIndex);
    std::string revealTrack(TrackId trackId);
    std::string setPresentation(std::string_view presentationId);
    std::string selectSelectedPresentation();
    ListOpenResult openSelectedList();
    std::string reloadActiveList();
    Result<std::string> applyFilter();

  private:
    bool setSelectedTrackById(TrackId trackId);
    TrackId focusedTrackId() const noexcept;
    bool containsTrackId(TrackId trackId) const noexcept;
    void clearMarkState();
    void reconcileMarks();
    std::unordered_set<TrackId> liveSubset(std::unordered_set<TrackId> const& ids) const;
    void applyVisualRange();
    void endVisualSelection();
    void publishSelection();
    void publishFocusMove();
    void afterFocusMove();
    void focusActiveView();
    void syncSelectedPresentation(std::string_view presentationId);
    void refreshPresentationNavigation();
    std::vector<LibraryNavEntry> loadLibraryNavigation();
    std::vector<TrackPresentationNavEntry> loadPresentationNavigation();
    void refreshFilterError();
    struct TrackItemsSnapshot final
    {
      std::vector<TrackListEntry> tracks{};
      std::vector<TrackSection> sections{};
    };

    Result<TrackItemsSnapshot> materializeView(rt::ViewId viewId);
    Result<> refreshActiveView();
    Result<> attachView(rt::ViewId viewId);
    Result<bool> attachActiveWorkspaceView();
    rt::TrackPresentationSpec presentationForList(ListId listId) const;
    Result<> navigateToList(ListId listId);

    rt::Library& _library;
    rt::ViewService& _views;
    rt::WorkspaceService& _workspace;
    i18n::MessageCatalog _textCatalog;
    uimodel::ListPresentations& _listPresentations;
    std::vector<LibraryNavEntry> _libraryEntries{};
    std::vector<std::string> _libraryLabels{};
    std::vector<TrackPresentationNavEntry> _presentationEntries{};
    std::vector<TrackListEntry> _tracks{};
    std::vector<TrackSection> _sections{};
    ListId _currentListId{rt::kAllTracksListId};
    rt::ViewId _activeViewId{rt::kInvalidViewId};
    std::int32_t _selectedList = 0;
    std::int32_t _selectedPresentation = 0;
    std::int32_t _selectedTrack = 0;
    std::unordered_set<TrackId> _markedIds{};
    // Set while a visual selection is running; the base holds the mark set the
    // selection started from so cancelling can restore it exactly.
    std::optional<TrackId> _optVisualAnchor{};
    std::unordered_set<TrackId> _visualBaseIds{};
    std::string _filterDraft{};
    std::string _filterError{};
    async::Subscription _customPresetsSub;
    async::Subscription _libraryChangesSub;
  };
} // namespace ao::tui
