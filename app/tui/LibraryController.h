// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include "ListNavigationModel.h"
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

    std::vector<TrackPresentationNavEntry> const& presentationEntries() const noexcept { return _presentationEntries; }
    std::vector<TrackListEntry> const& tracks() const noexcept { return _tracks; }
    std::vector<TrackSection> const& sections() const noexcept { return _sections; }
    std::uint64_t trackRowsRevision() const noexcept { return _trackRowsRevision; }
    ListId currentListId() const noexcept { return _currentListId; }
    rt::ViewId activeViewId() const noexcept { return _activeViewId; }
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

    std::string emptyStateText() const;
    std::string currentListTitle() const;
    std::string activePresentationId() const;
    // Borrowed from the active view (or the process-stable default); consume
    // before the active view can be replaced or destroyed.
    rt::TrackPresentationSpec const& activePresentation() const;
    SelectedTrackView selectedTrackView() const;

    void setTextCatalog(i18n::MessageCatalog textCatalog);
    void setFilterDraft(std::string value);
    void clearFilterDraft();
    void moveTrackSelection(std::int32_t delta);
    void movePresentationSelection(std::int32_t delta);
    bool trySetSelectedPresentation(std::int32_t index);
    void setSelectedTrackIndex(std::int32_t index);
    void toggleFocusedMark();
    void toggleVisualSelection();
    void cancelVisualSelection();
    /// Stops extending a visual range, keeping every row it already marked.
    void commitVisualSelection();
    void markAllTracks();
    void clearMarks();

    std::string jumpToAdjacentSection(std::int32_t delta);
    std::string selectSection(std::int32_t sectionIndex);
    std::string revealTrack(TrackId trackId,
                            rt::ViewId preferredViewId = rt::kInvalidViewId,
                            ListId preferredListId = kInvalidListId);
    Result<> navigateHistory(bool forward);
    std::string setPresentation(std::string_view presentationId);
    std::string selectSelectedPresentation();
    Result<bool> openList(ListId id);
    ListNavigationModel& navigation() noexcept { return _navigation; }
    ListNavigationModel const& navigation() const noexcept { return _navigation; }
    std::string reloadActiveList();
    Result<std::string> applyFilter();

  private:
    bool trySetSelectedTrackById(TrackId trackId);
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
    std::vector<TrackPresentationNavEntry> loadPresentationNavigation();
    void refreshFilterError();
    void refreshNavigationTree();
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
    ListNavigationModel _navigation{};
    std::string _viewSyncError{};
    std::vector<TrackPresentationNavEntry> _presentationEntries{};
    std::vector<TrackListEntry> _tracks{};
    std::vector<TrackSection> _sections{};
    std::uint64_t _trackRowsRevision = 0;
    ListId _currentListId{rt::kAllTracksListId};
    rt::ViewId _activeViewId{rt::kInvalidViewId};
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
