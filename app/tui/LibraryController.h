// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include "LibraryNavigation.h"
#include "ListNavigationModel.h"
#include "TrackListEntry.h"
#include "TrackPresentationNavigation.h"
#include "TrackSection.h"
#include <ao/CoreIds.h>
#include <ao/Error.h>
#include <ao/async/Signal.h>
#include <ao/async/Subscription.h>
#include <ao/i18n/MessageCatalog.h>
#include <ao/rt/NotificationState.h>
#include <ao/rt/TrackField.h>
#include <ao/rt/TrackPresentation.h>
#include <ao/rt/ViewIds.h>
#include <ao/rt/VirtualListIds.h>
#include <ao/uimodel/library/detail/TrackCredits.h>

#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace ao::rt
{
  class Library;
  class TrackListProjection;
  class ViewService;
  class WorkspaceService;
  struct NavigationRequest;
  struct TrackListViewState;
} // namespace ao::rt

namespace ao::uimodel
{
  class ListPresentations;
  class TrackColumnLayouts;
} // namespace ao::uimodel

namespace ao::tui
{
  struct SelectedTrackView final
  {
    TrackListEntry const* track = nullptr;
    ResourceId coverArtId = kInvalidResourceId;
  };

  struct PresentationSelectionError final
  {
    std::string message{};
    rt::NotificationSeverity severity = rt::NotificationSeverity::Info;
  };

  using PresentationSelectionResult = std::expected<std::string, PresentationSelectionError>;

  class LibraryController final
  {
  public:
    LibraryController(rt::Library& library,
                      rt::ViewService& views,
                      rt::WorkspaceService& workspace,
                      i18n::MessageCatalog textCatalog,
                      uimodel::ListPresentations& listPresentations,
                      uimodel::TrackColumnLayouts const& trackColumnLayouts);

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
    // Borrowed from the active view (or the process-stable default); consume
    // before the active view can be replaced or destroyed.
    rt::TrackPresentationSpec const& activePresentation() const;
    SelectedTrackView selectedTrackView() const noexcept;
    async::Signal<bool>& signalTrackCursorAvailable() noexcept { return _trackCursorAvailableSignal; }

    /// Borrowed owning credit sections for the focused track. Valid only for synchronous use
    /// in the current turn; the next focus change, reload, reset, deletion,
    /// committed library change, or cache miss invalidates the reference.
    /// Absent when no loaded track is focused; an engaged empty list is a
    /// stored value, not a missing track.
    uimodel::TrackCreditSections const& focusedCredits() const;

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
    Result<std::string> revealTrack(TrackId trackId,
                                    rt::ViewId preferredViewId = rt::kInvalidViewId,
                                    ListId preferredListId = kInvalidListId);
    Result<std::string> revealPlayingTrack(TrackId trackId, ListId sourceListId);
    bool canNavigateBack() const;
    bool canNavigateForward() const;
    Result<> navigateHistory(bool forward);
    Result<> navigateToArtist(std::string_view artist);
    Result<> revealAlbum(TrackId trackId);
    PresentationSelectionResult setPresentation(rt::TrackPresetId const& id);
    PresentationSelectionResult setPresentationKey(std::string_view key);
    PresentationSelectionResult selectSelectedPresentation();
    std::vector<rt::TrackField> sortableTrackColumns() const;
    Result<> sortByColumn(std::string_view columnId);
    Result<bool> openList(ListId id);
    std::vector<LibraryNavEntry> const& libraryEntries() const noexcept { return _libraryEntries; }
    std::vector<std::string> const& libraryLabels() const noexcept { return _libraryLabels; }
    std::int32_t selectedList() const noexcept { return _selectedList; }
    void selectListRow(std::int32_t index);
    void revealActiveList();
    ListNavigationModel& navigation() noexcept { return _navigation; }
    ListNavigationModel const& navigation() const noexcept { return _navigation; }
    std::string reloadActiveList();
    Result<std::string> applyFilter();

  private:
    Result<std::string> revealLoadedTrack(TrackId trackId);
    bool trySetSelectedTrackById(TrackId trackId);
    TrackId focusedTrackId() const noexcept;
    void invalidateFocusedCredits() noexcept;
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
    void syncSelectedPresentation();
    void refreshPresentationNavigation();
    void refreshFilterError();
    void refreshNavigationTree();
    struct TrackItemsSnapshot final
    {
      std::shared_ptr<rt::TrackListProjection const> projectionPtr{};
      std::vector<TrackListEntry> tracks{};
      std::vector<TrackSection> sections{};
    };

    Result<TrackItemsSnapshot> materializeView(rt::ViewId viewId);
    Result<TrackItemsSnapshot> materializeView(rt::TrackListViewState const& state);
    void installRows(TrackItemsSnapshot snapshot);
    Result<> refreshActiveView();
    Result<> refreshKeepingSelection();
    void restoreSelection(TrackId previousTrackId);
    void recoverWorkspaceView(bool tryKeepCurrentView = true);
    Error formatNavigationFailedError(Error const& error) const;
    Result<> syncRevealView(rt::ViewId viewId);
    Result<> attachView(rt::ViewId viewId);
    Result<bool> attachActiveWorkspaceView();
    rt::TrackPresentationSpec presentationForList(ListId listId) const;
    Result<> navigateToList(ListId listId);
    Result<> navigateFromActiveView(rt::NavigationRequest const& request);

    rt::Library& _library;
    rt::ViewService& _views;
    rt::WorkspaceService& _workspace;
    i18n::MessageCatalog _textCatalog;
    uimodel::ListPresentations& _listPresentations;
    uimodel::TrackColumnLayouts const& _trackColumnLayouts;
    ListNavigationModel _navigation{};
    std::vector<LibraryNavEntry> _libraryEntries{};
    std::vector<std::string> _libraryLabels{};
    std::int32_t _selectedList = 0;
    std::string _viewSyncError{};
    std::vector<TrackPresentationNavEntry> _presentationEntries{};
    std::vector<TrackListEntry> _tracks{};
    std::vector<TrackSection> _sections{};
    std::uint64_t _trackRowsRevision = 0;
    std::weak_ptr<rt::TrackListProjection const> _loadedProjectionPtr;
    bool _loadedRowsDirty = false;
    ListId _currentListId{rt::kAllTracksListId};
    rt::ViewId _activeViewId{rt::kInvalidViewId};
    std::int32_t _selectedPresentation = 0;
    std::int32_t _selectedTrack = 0;
    bool _trackCursorAvailable = false;
    async::Signal<bool> _trackCursorAvailableSignal;
    std::unordered_set<TrackId> _markedIds{};
    // Set while a visual selection is running; the base holds the mark set the
    // selection started from so cancelling can restore it exactly.
    std::optional<TrackId> _optVisualAnchor{};
    std::unordered_set<TrackId> _visualBaseIds{};
    std::string _filterDraft{};
    std::string _filterError{};
    // One focused track. Hits compare that id with the library revision last
    // observed from a committed change or from the snapshot that filled the
    // slot. They do not open a read transaction, and trackRowsRevision is not
    // a library revision.
    mutable TrackId _focusedCreditsTrackId = kInvalidTrackId;
    mutable std::uint64_t _focusedCreditsLibraryRevision = 0;
    mutable std::uint64_t _observedLibraryRevision = 0;
    mutable bool _focusedCreditsPopulated = false;
    mutable uimodel::TrackCreditSections _focusedCredits{};
    async::Subscription _loadedProjectionSub;
    async::Subscription _customPresetsSub;
    async::Subscription _libraryChangesSub;
  };
} // namespace ao::tui
