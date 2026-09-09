// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "LibraryController.h"

#include "SelectionNavigation.h"
#include "ShellText.h"
#include "TrackListEntry.h"
#include "TrackPresentationNavigation.h"
#include "TrackSection.h"
#include <ao/Contract.h>
#include <ao/CoreIds.h>
#include <ao/Error.h>
#include <ao/i18n/MessageCatalog.h>
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
#include <ao/rt/projection/TrackListProjection.h>
#include <ao/uimodel/library/list/ListTreeProjection.h>
#include <ao/uimodel/library/presentation/ListPresentations.h>
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
#include <unordered_set>
#include <utility>
#include <vector>

namespace ao::tui
{
  LibraryController::LibraryController(rt::Library& library,
                                       rt::ViewService& views,
                                       rt::WorkspaceService& workspace,
                                       i18n::MessageCatalog textCatalog,
                                       uimodel::ListPresentations& listPresentations)
    : _library{library}
    , _views{views}
    , _workspace{workspace}
    , _textCatalog{std::move(textCatalog)}
    , _listPresentations{listPresentations}
    , _presentationEntries{loadPresentationNavigation()}
  {
    auto const attachedRes = attachActiveWorkspaceView();

    if (!attachedRes)
    {
      APP_LOG_WARN("TUI: failed to attach the restored workspace view: {}", attachedRes.error().message);
    }

    if (!attachedRes || !*attachedRes)
    {
      auto const fallbackRes = navigateToList(rt::kAllTracksListId);

      if (!fallbackRes)
      {
        APP_LOG_ERROR("TUI could not open the default All Tracks view: {}", fallbackRes.error().message);
      }
    }

    publishSelection();
    refreshNavigationTree();
    _customPresetsSub = _workspace.onChanged(
      [this](rt::WorkspaceChanged const& changed)
      {
        if (changed.cause == rt::WorkspaceChangeCause::Presets || changed.cause == rt::WorkspaceChangeCause::Restore)
        {
          refreshPresentationNavigation();
        }
      });
    _libraryChangesSub = _library.changes().onChanged(
      [this](rt::LibraryChangeSet const& changeSet)
      {
        std::ignore = reloadActiveList();

        if (changeSet.libraryReset || !changeSet.listsUpserted.empty() || !changeSet.listsDeleted.empty())
        {
          refreshNavigationTree();
        }
      });
  }

  std::string LibraryController::emptyStateText() const
  {
    if (!_viewSyncError.empty())
    {
      return _viewSyncError;
    }

    if (!_tracks.empty())
    {
      return {};
    }

    if (!_filterError.empty())
    {
      return chromeText(_textCatalog, i18n::MessageId::TuiLibraryFilterInvalid);
    }

    if (auto const stateRes = _views.findTrackListState(_activeViewId); stateRes && !stateRes->filterExpression.empty())
    {
      return chromeText(_textCatalog, i18n::MessageId::TuiLibraryFilterEmpty);
    }

    if (_currentListId == rt::kAllTracksListId)
    {
      return i18n::requiredFormat(_textCatalog, i18n::MessageId::TuiLibraryEmptyScan, {{"command", ":scan"}});
    }

    return chromeText(_textCatalog, i18n::MessageId::TuiLibraryNoTracksFound);
  }

  std::string LibraryController::currentListTitle() const
  {
    if (_currentListId == rt::kAllTracksListId)
    {
      return std::string{i18n::requiredText(_textCatalog, i18n::MessageId::LibraryAllTracks)};
    }

    auto const optNode = _library.snapshot().listNode(_currentListId);

    if (!optNode)
    {
      return {};
    }

    return optNode->name.empty() ? std::string{i18n::requiredText(_textCatalog, i18n::MessageId::LibraryUnnamedList)}
                                 : optNode->name;
  }

  std::string LibraryController::activePresentationId() const
  {
    // Reached from the workspace observer via refreshPresentationNavigation().
    auto const* presentation = _views.findTrackListPresentation(_activeViewId);
    return presentation == nullptr ? std::string{} : presentation->id;
  }

  rt::TrackPresentationSpec const& LibraryController::activePresentation() const
  {
    if (auto const* presentation = _views.findTrackListPresentation(_activeViewId); presentation != nullptr)
    {
      return *presentation;
    }

    auto const* fallback = rt::builtinTrackPresentationPreset(rt::kDefaultTrackPresentationId);
    AO_INVARIANT(fallback != nullptr, "The default track presentation must be registered");
    return fallback->spec;
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

  std::vector<TrackId> LibraryController::selectedTrackIds() const
  {
    auto ids = std::vector<TrackId>{};

    if (_tracks.empty())
    {
      return ids;
    }

    if (_markedIds.empty())
    {
      ids.push_back(_tracks[clampSelection(static_cast<std::size_t>(std::max(0, _selectedTrack)), _tracks.size())].id);
      return ids;
    }

    ids.reserve(_markedIds.size());

    for (auto const& entry : _tracks)
    {
      if (_markedIds.contains(entry.id))
      {
        ids.push_back(entry.id);
      }
    }

    return ids;
  }

  void LibraryController::setTextCatalog(i18n::MessageCatalog textCatalog)
  {
    auto const focused = focusedTrackId();
    _textCatalog = std::move(textCatalog);
    refreshPresentationNavigation();
    refreshNavigationTree();

    if (auto const res = refreshActiveView(); !res)
    {
      _viewSyncError =
        i18n::requiredFormat(_textCatalog, i18n::MessageId::TuiNavigationSyncFailed, {{"detail", res.error().message}});
      return;
    }

    std::ignore = trySetSelectedTrackById(focused);
    reconcileMarks();
    publishSelection();
  }

  void LibraryController::setFilterDraft(std::string value)
  {
    _filterDraft = std::move(value);
  }

  void LibraryController::clearFilterDraft()
  {
    _filterDraft.clear();
  }

  void LibraryController::moveTrackSelection(std::int32_t const delta)
  {
    _selectedTrack = moveSelection(_selectedTrack, delta, _tracks.size());
    afterFocusMove();
  }

  void LibraryController::movePresentationSelection(std::int32_t const delta)
  {
    _selectedPresentation = moveSelection(_selectedPresentation, delta, _presentationEntries.size());
  }

  bool LibraryController::trySetSelectedPresentation(std::int32_t const index)
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
    afterFocusMove();
  }

  void LibraryController::toggleFocusedMark()
  {
    auto const trackId = focusedTrackId();

    if (trackId == kInvalidTrackId)
    {
      return;
    }

    // A running visual range would recompute the mark set on the next motion and
    // discard this toggle, so committing the range first keeps both edits.
    endVisualSelection();

    if (!_markedIds.insert(trackId).second)
    {
      _markedIds.erase(trackId);
    }

    publishSelection();
  }

  void LibraryController::toggleVisualSelection()
  {
    auto const focusId = focusedTrackId();

    if (focusId == kInvalidTrackId)
    {
      return;
    }

    if (_optVisualAnchor)
    {
      commitVisualSelection();
      return;
    }

    _visualBaseIds = _markedIds;
    _optVisualAnchor = focusId;
    applyVisualRange();
    publishSelection();
  }

  void LibraryController::commitVisualSelection()
  {
    if (!_optVisualAnchor)
    {
      return;
    }

    // The range stays in the mark set, where any later edit treats it exactly
    // like marks made one at a time.
    endVisualSelection();
    publishSelection();
  }

  void LibraryController::cancelVisualSelection()
  {
    if (!_optVisualAnchor)
    {
      return;
    }

    _markedIds = std::move(_visualBaseIds);
    endVisualSelection();
    publishSelection();
  }

  void LibraryController::applyVisualRange()
  {
    if (!_optVisualAnchor || _tracks.empty())
    {
      return;
    }

    auto const anchorIt = std::ranges::find(_tracks, *_optVisualAnchor, &TrackListEntry::id);

    if (anchorIt == _tracks.end())
    {
      return;
    }

    // Only the anchor has to be located: the focus is already a row index, and
    // this runs on every step of a held motion key.
    auto const anchor = static_cast<std::size_t>(std::distance(_tracks.begin(), anchorIt));
    auto const focus = clampSelection(static_cast<std::size_t>(std::max(0, _selectedTrack)), _tracks.size());
    auto const begin = std::min(anchor, focus);
    auto const end = std::max(anchor, focus);

    _markedIds = _visualBaseIds;
    _markedIds.reserve(_visualBaseIds.size() + (end - begin) + 1);

    for (auto index = begin; index <= end; ++index)
    {
      _markedIds.insert(_tracks[index].id);
    }
  }

  void LibraryController::endVisualSelection()
  {
    _optVisualAnchor.reset();
    _visualBaseIds.clear();
  }

  void LibraryController::markAllTracks()
  {
    endVisualSelection();
    _markedIds.clear();
    _markedIds.reserve(_tracks.size());

    for (auto const& entry : _tracks)
    {
      _markedIds.insert(entry.id);
    }

    publishSelection();
  }

  void LibraryController::clearMarks()
  {
    clearMarkState();
    publishSelection();
  }

  std::string LibraryController::jumpToAdjacentSection(std::int32_t const delta)
  {
    if (_sections.empty())
    {
      return chromeText(_textCatalog, i18n::MessageId::TuiLibraryNoSections);
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
      return chromeText(_textCatalog, i18n::MessageId::TuiLibraryNoSectionSelected);
    }

    auto const& section = _sections[static_cast<std::size_t>(sectionIndex)];
    std::size_t const lastTrackIndex = _tracks.empty() ? 0 : _tracks.size() - 1;
    _selectedTrack = static_cast<std::int32_t>(std::min(section.rowBegin, lastTrackIndex));
    afterFocusMove();
    return librarySection(_textCatalog, trackSectionDisplayName(_textCatalog, section));
  }

  bool LibraryController::trySetSelectedTrackById(TrackId const trackId)
  {
    if (trackId == kInvalidTrackId)
    {
      return false;
    }

    // The projection maintains an indexed track-to-row lookup. _tracks can drift
    // from projection indices when a row's LMDB lookup was skipped, so trust the
    // index only when the materialized row matches and otherwise scan below.
    if (auto const foundRes = _views.findTrackListProjection(_activeViewId); foundRes && *foundRes != nullptr)
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

  TrackId LibraryController::focusedTrackId() const noexcept
  {
    if (_tracks.empty())
    {
      return kInvalidTrackId;
    }

    return _tracks[clampSelection(static_cast<std::size_t>(std::max(0, _selectedTrack)), _tracks.size())].id;
  }

  bool LibraryController::containsTrackId(TrackId const trackId) const noexcept
  {
    return std::ranges::any_of(_tracks, [trackId](TrackListEntry const& entry) { return entry.id == trackId; });
  }

  void LibraryController::clearMarkState()
  {
    endVisualSelection();
    _markedIds.clear();
  }

  std::unordered_set<TrackId> LibraryController::liveSubset(std::unordered_set<TrackId> const& ids) const
  {
    auto live = std::unordered_set<TrackId>{};

    if (ids.empty())
    {
      return live;
    }

    live.reserve(ids.size());

    for (auto const& entry : _tracks)
    {
      if (ids.contains(entry.id))
      {
        live.insert(entry.id);
      }
    }

    return live;
  }

  void LibraryController::reconcileMarks()
  {
    _markedIds = liveSubset(_markedIds);

    if (!_optVisualAnchor)
    {
      return;
    }

    if (!containsTrackId(*_optVisualAnchor))
    {
      // A range without its anchor cannot be extended or cancelled coherently,
      // so the selection ends and the rows it had already reached stay marked.
      endVisualSelection();
      return;
    }

    _visualBaseIds = liveSubset(_visualBaseIds);

    // The range is defined by row order, so a rematerialized view has to derive
    // it again; keeping the previous ids would show a range that is no longer
    // contiguous until the next motion snapped it back.
    applyVisualRange();
  }

  void LibraryController::publishSelection()
  {
    if (_activeViewId == rt::kInvalidViewId)
    {
      return;
    }

    if (auto res = _views.setSelection(_activeViewId, selectedTrackIds()); !res)
    {
      APP_LOG_ERROR("Failed to publish TUI selection: {}", res.error().message);
    }

    focusActiveView();
  }

  void LibraryController::afterFocusMove()
  {
    if (_optVisualAnchor)
    {
      applyVisualRange();
      publishSelection();
      return;
    }

    publishFocusMove();
  }

  void LibraryController::publishFocusMove()
  {
    // Marked ids are focus-independent, so moving the cursor can only change the
    // effective selection while nothing is marked. Republishing a select-all set
    // on every cursor step would rebuild and re-emit the whole list.
    if (_markedIds.empty())
    {
      publishSelection();
      return;
    }

    if (_activeViewId == rt::kInvalidViewId)
    {
      return;
    }

    focusActiveView();
  }

  void LibraryController::focusActiveView()
  {
    if (auto const focusedRes = _workspace.focusView(_activeViewId); !focusedRes)
    {
      APP_LOG_ERROR("Failed to focus TUI track view: {}", focusedRes.error().message);
    }
  }

  std::string LibraryController::revealTrack(TrackId const trackId,
                                             rt::ViewId const preferredViewId,
                                             ListId const preferredListId)
  {
    if (trackId == kInvalidTrackId)
    {
      return chromeText(_textCatalog, i18n::MessageId::TuiLibraryNoCurrentTrack);
    }

    if (!_library.snapshot().containsTrack(trackId))
    {
      return chromeText(_textCatalog, i18n::MessageId::TuiLibraryCurrentTrackNotInView);
    }

    if (!containsTrackId(trackId))
    {
      auto target = rt::kInvalidViewId;
      auto const workspace = _workspace.snapshot();

      for (auto const viewId : workspace.openViews)
      {
        auto const stateRes = _views.findTrackListState(viewId);
        auto const projectionRes = _views.findTrackListProjection(viewId);

        if (!stateRes || !projectionRes || !(*projectionRes)->indexOf(trackId) ||
            (viewId != preferredViewId && stateRes->listId != preferredListId))
        {
          continue;
        }

        target = viewId;

        if (viewId == preferredViewId)
        {
          break;
        }
      }

      // Quick Filter changes a live view directly. Checkpoint its current
      // presentation and filter in the existing workspace history before leaving.
      auto const stateRes = _views.findTrackListState(_activeViewId);

      if (!stateRes || !_workspace.setActivePresentation(stateRes->presentation))
      {
        return chromeText(_textCatalog, i18n::MessageId::TuiLibraryCurrentTrackNotInView);
      }

      commitVisualSelection();
      auto navigationRes = Result<>{};

      if (target != rt::kInvalidViewId)
      {
        navigationRes = _workspace.focusView(target);

        if (navigationRes)
        {
          navigationRes = attachView(target);
        }

        if (navigationRes)
        {
          navigationRes = _workspace.setActivePresentation(_views.trackListState(target).presentation);
        }
      }
      else
      {
        // Plain navigation reuses an unfiltered view or opens a new one; it
        // never clears the old view's filter or edits a saved List predicate.
        navigationRes = navigateToList(rt::kAllTracksListId);
      }

      if (!navigationRes)
      {
        return chromeText(_textCatalog, i18n::MessageId::TuiLibraryCurrentTrackNotInView);
      }
    }

    commitVisualSelection();

    if (trySetSelectedTrackById(trackId))
    {
      afterFocusMove();
      return libraryRevealedTrack(_textCatalog, trackDisplayTitle(_textCatalog, _tracks[_selectedTrack].row));
    }

    return chromeText(_textCatalog, i18n::MessageId::TuiLibraryCurrentTrackNotInView);
  }

  Result<> LibraryController::navigateHistory(bool const forward)
  {
    auto res = forward ? _workspace.goForward() : _workspace.goBack();

    if (!res)
    {
      return res;
    }

    commitVisualSelection();
    auto const attachedRes = attachView(_workspace.snapshot().activeViewId);

    if (attachedRes)
    {
      publishSelection();
    }

    return attachedRes;
  }

  std::string LibraryController::setPresentation(std::string_view const presentationId)
  {
    if (_activeViewId == rt::kInvalidViewId)
    {
      return chromeText(_textCatalog, i18n::MessageId::TuiLibraryNoActiveTrackView);
    }

    auto const selectedBefore = selectedTrackView();
    auto const previousTrackId = selectedBefore.track == nullptr ? kInvalidTrackId : selectedBefore.track->id;
    auto const res = _workspace.setActivePresentation(presentationId);

    if (!res)
    {
      return libraryUnknownView(_textCatalog, presentationId);
    }

    auto const& spec = *res;
    auto snapshotRes = materializeView(_activeViewId);

    if (!snapshotRes)
    {
      // A successful presentation mutation guarantees that this view still
      // owns a projection; losing it here violates the ViewService contract.
      AO_FATAL("TUI could not materialize a view after changing its presentation: {}", snapshotRes.error().message);
    }

    _tracks = std::move(snapshotRes->tracks);
    _sections = std::move(snapshotRes->sections);
    ++_trackRowsRevision;
    syncSelectedPresentation(spec.id);

    _listPresentations.setPresentationIdForList(_currentListId, spec.id);

    if (!trySetSelectedTrackById(previousTrackId))
    {
      _selectedTrack = moveSelection(_selectedTrack, 0, _tracks.size());
    }

    // Marks are reconciled against the final focus, because a running visual
    // range is derived from the anchor and the focused row.
    reconcileMarks();
    publishSelection();
    return libraryView(_textCatalog, spec.id);
  }

  std::string LibraryController::selectSelectedPresentation()
  {
    if (_presentationEntries.empty())
    {
      return chromeText(_textCatalog, i18n::MessageId::TuiLibraryNoViewsAvailable);
    }

    auto const selectedIndex =
      clampSelection(static_cast<std::size_t>(std::max(0, _selectedPresentation)), _presentationEntries.size());
    return setPresentation(_presentationEntries[selectedIndex].id);
  }

  void LibraryController::refreshNavigationTree()
  {
    auto const snapshot = _library.snapshot();
    auto tree = uimodel::buildListTreeProjection(_textCatalog, snapshot.lists());

    for (auto& [id, row] : tree.rowsById)
    {
      std::ignore = id;

      if (row.name.empty())
      {
        row.name = i18n::requiredText(_textCatalog, i18n::MessageId::LibraryUnnamedList);
      }
    }

    _navigation.setTree(std::move(tree), _currentListId);
  }

  Result<bool> LibraryController::openList(ListId const id)
  {
    if (id == _currentListId && _activeViewId != rt::kInvalidViewId)
    {
      return false;
    }

    if (id != rt::kAllTracksListId && !_library.snapshot().listNode(id))
    {
      return makeError(Error::Code::NotFound, "List is no longer available");
    }

    if (auto const res = navigateToList(id); !res)
    {
      if (_workspace.snapshot().activeViewId != _activeViewId)
      {
        if (auto const attachedRes = attachActiveWorkspaceView();
            (!attachedRes || !*attachedRes) && !navigateToList(rt::kAllTracksListId))
        {
          _tracks.clear();
          _sections.clear();
          ++_trackRowsRevision;
          clearMarkState();
          _activeViewId = rt::kInvalidViewId;
          _currentListId = kInvalidListId;
          _viewSyncError = i18n::requiredFormat(
            _textCatalog, i18n::MessageId::TuiNavigationSyncFailed, {{"detail", res.error().message}});
        }

        publishSelection();
      }

      return std::unexpected{res.error()};
    }

    publishSelection();
    return true;
  }

  std::string LibraryController::reloadActiveList()
  {
    auto const selectedBefore = selectedTrackView();
    auto const previousTrackId = selectedBefore.track == nullptr ? kInvalidTrackId : selectedBefore.track->id;
    auto const previousSelectedTrack = _selectedTrack;

    if (auto reloadRes = refreshActiveView(); !reloadRes)
    {
      APP_LOG_WARN(
        "TUI active view is no longer available; opening a workspace fallback: {}", reloadRes.error().message);
      auto const workspaceActiveViewId = _workspace.snapshot().activeViewId;

      if (workspaceActiveViewId != rt::kInvalidViewId && workspaceActiveViewId != _activeViewId)
      {
        reloadRes = attachView(workspaceActiveViewId);
      }

      if (!reloadRes)
      {
        reloadRes = navigateToList(rt::kAllTracksListId);
      }

      if (!reloadRes)
      {
        APP_LOG_ERROR("TUI could not recover an active workspace view: {}", reloadRes.error().message);
        return libraryReloadedTracks(_textCatalog, _tracks.size());
      }
    }
    else
    {
      if (!trySetSelectedTrackById(previousTrackId))
      {
        _selectedTrack = moveSelection(previousSelectedTrack, 0, _tracks.size());
      }

      // The recovery paths above attach or navigate, which reconcile marks
      // themselves; only the plain reload still owes that to the restored focus.
      reconcileMarks();
    }

    publishSelection();
    return libraryReloadedTracks(_textCatalog, _tracks.size());
  }

  Result<std::string> LibraryController::applyFilter()
  {
    if (_activeViewId == rt::kInvalidViewId)
    {
      return chromeText(_textCatalog, i18n::MessageId::TuiLibraryNoActiveTrackView);
    }

    auto previousExpression = std::string{};

    if (auto const stateRes = _views.findTrackListState(_activeViewId); stateRes)
    {
      previousExpression = stateRes->filterExpression;
    }

    auto const selectedBefore = selectedTrackView();
    auto const previousTrackId = selectedBefore.track == nullptr ? kInvalidTrackId : selectedBefore.track->id;
    auto const previousSelectedTrack = _selectedTrack;
    auto const resolved = uimodel::resolveTrackFilter(_filterDraft);
    auto filterRes = _views.setFilter(_activeViewId, resolved.expression);

    if (!filterRes)
    {
      _filterError = i18n::requiredFormat(
        _textCatalog, i18n::MessageId::TrackFilterError, {{"diagnostic", filterRes.error().message}});
      return std::unexpected{filterRes.error()};
    }

    refreshFilterError();
    auto snapshotRes = materializeView(_activeViewId);

    if (!snapshotRes)
    {
      // A successful filter mutation guarantees that this view still owns a
      // projection; losing it here violates the ViewService contract.
      AO_FATAL("TUI could not materialize a view after applying its filter: {}", snapshotRes.error().message);
    }

    _tracks = std::move(snapshotRes->tracks);
    _sections = std::move(snapshotRes->sections);
    ++_trackRowsRevision;

    if (resolved.expression == previousExpression)
    {
      // Re-materializing a no-op filter can still drop rows, so the kept focus
      // follows its track the way every other rematerialize path does, and marks
      // are reconciled only once that focus is settled.
      if (!trySetSelectedTrackById(previousTrackId))
      {
        _selectedTrack = moveSelection(previousSelectedTrack, 0, _tracks.size());
      }

      reconcileMarks();
    }
    else
    {
      clearMarkState();
      _selectedTrack = 0;
    }

    publishSelection();

    switch (resolved.mode)
    {
      case uimodel::TrackFilterMode::None: return chromeText(_textCatalog, i18n::MessageId::TuiLibraryFilterCleared);
      case uimodel::TrackFilterMode::Quick: return libraryQuickFilterMatched(_textCatalog, _tracks.size());
      case uimodel::TrackFilterMode::Expression: return libraryExpressionFilterMatched(_textCatalog, _tracks.size());
    }

    return chromeText(_textCatalog, i18n::MessageId::TuiLibraryFilterApplied);
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
      _textCatalog, rt::builtinTrackPresentationPresets(), _workspace.customPresets());
  }

  void LibraryController::refreshFilterError()
  {
    _filterError.clear();

    if (_activeViewId == rt::kInvalidViewId)
    {
      return;
    }

    auto const stateRes = _views.findTrackListState(_activeViewId);

    if (stateRes && stateRes->optFilterError)
    {
      _filterError = i18n::requiredFormat(
        _textCatalog, i18n::MessageId::TrackFilterError, {{"diagnostic", stateRes->optFilterError->message}});
    }
  }

  Result<LibraryController::TrackItemsSnapshot> LibraryController::materializeView(rt::ViewId const viewId)
  {
    auto const foundProjectionRes = _views.findTrackListProjection(viewId);

    if (!foundProjectionRes)
    {
      return std::unexpected{foundProjectionRes.error()};
    }

    if (*foundProjectionRes == nullptr)
    {
      return makeError(Error::Code::InvalidState, "TUI track view has no projection");
    }

    auto const& projectionPtr = *foundProjectionRes;
    auto const reader = _library.snapshot();
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

  Result<> LibraryController::refreshActiveView()
  {
    auto const stateRes = _views.findTrackListState(_activeViewId);

    if (!stateRes)
    {
      return std::unexpected{stateRes.error()};
    }

    if (stateRes->listId != rt::kAllTracksListId && !_library.snapshot().listNode(stateRes->listId))
    {
      return makeError(Error::Code::NotFound, "TUI track view refers to an unavailable library list");
    }

    auto snapshotRes = materializeView(_activeViewId);

    if (!snapshotRes)
    {
      return std::unexpected{snapshotRes.error()};
    }

    auto filterError = std::string{};

    if (stateRes->optFilterError)
    {
      filterError = i18n::requiredFormat(
        _textCatalog, i18n::MessageId::TrackFilterError, {{"diagnostic", stateRes->optFilterError->message}});
    }

    _filterError = std::move(filterError);
    _tracks = std::move(snapshotRes->tracks);
    _sections = std::move(snapshotRes->sections);
    ++_trackRowsRevision;

    // Marks stay for the caller to reconcile: a running visual range is derived
    // from the focused row, and the focus this view keeps is only restored once
    // the caller has decided which track it follows.
    return {};
  }

  Result<> LibraryController::attachView(rt::ViewId const viewId)
  {
    auto const stateRes = _views.findTrackListState(viewId);

    if (!stateRes)
    {
      return std::unexpected{stateRes.error()};
    }

    auto snapshotRes = materializeView(viewId);

    if (!snapshotRes)
    {
      return std::unexpected{snapshotRes.error()};
    }

    if (stateRes->listId != rt::kAllTracksListId && !_library.snapshot().listNode(stateRes->listId))
    {
      return makeError(Error::Code::NotFound, "TUI track view refers to an unavailable library list");
    }

    auto const presentationIt =
      std::ranges::find(_presentationEntries, stateRes->presentation.id, &TrackPresentationNavEntry::id);
    auto const selectedPresentation =
      presentationIt == _presentationEntries.end()
        ? moveSelection(_selectedPresentation, 0, _presentationEntries.size())
        : static_cast<std::int32_t>(std::distance(_presentationEntries.begin(), presentationIt));
    auto filterError = std::string{};

    if (stateRes->optFilterError)
    {
      filterError = i18n::requiredFormat(
        _textCatalog, i18n::MessageId::TrackFilterError, {{"diagnostic", stateRes->optFilterError->message}});
    }

    auto const sameView = viewId == _activeViewId;
    auto const previousTrackId = sameView ? focusedTrackId() : kInvalidTrackId;
    auto const previousSelectedTrack = _selectedTrack;
    _viewSyncError.clear();
    _activeViewId = viewId;
    _currentListId = stateRes->listId;
    _selectedPresentation = selectedPresentation;
    _selectedTrack = 0;
    _filterDraft = stateRes->filterExpression;
    _filterError = std::move(filterError);
    _tracks = std::move(snapshotRes->tracks);
    _sections = std::move(snapshotRes->sections);
    ++_trackRowsRevision;

    if (!sameView)
    {
      clearMarkState();
      return {};
    }

    // Reattaching the open view reconciles focus alongside marks; only a
    // different view starts at the top.
    if (!trySetSelectedTrackById(previousTrackId))
    {
      _selectedTrack = moveSelection(previousSelectedTrack, 0, _tracks.size());
    }

    reconcileMarks();
    return {};
  }

  Result<bool> LibraryController::attachActiveWorkspaceView()
  {
    auto const workspace = _workspace.snapshot();

    if (workspace.activeViewId == rt::kInvalidViewId)
    {
      return false;
    }

    if (!std::ranges::contains(workspace.openViews, workspace.activeViewId))
    {
      return makeError(Error::Code::InvalidState, "TUI workspace active view is not open");
    }

    if (auto const attachedRes = attachView(workspace.activeViewId); !attachedRes)
    {
      return std::unexpected{attachedRes.error()};
    }

    return true;
  }

  rt::TrackPresentationSpec LibraryController::presentationForList(ListId const listId) const
  {
    auto context = uimodel::ListPresentationContext{
      .listId = listId,
      .sourceKind = uimodel::ListPresentationSourceKind::AllTracks,
    };

    if (!rt::isVirtualListId(listId))
    {
      if (auto const optNode = _library.snapshot().listNode(listId); optNode)
      {
        context.sourceKind = uimodel::ListPresentationSourceKind::SavedList;
        context.listExpression = optNode->expression;
        return _listPresentations.presentationForList(context);
      }
    }

    return _listPresentations.presentationForList(context);
  }

  Result<> LibraryController::navigateToList(ListId const listId)
  {
    auto navigationRes = Result<rt::ViewId>{};
    auto const presentation = rt::NavigationPresentation{
      .mode = rt::NavigationPresentationMode::NewViewDefault,
      .spec = presentationForList(listId),
    };

    if (listId == rt::kAllTracksListId)
    {
      navigationRes = _workspace.navigate({.target = rt::GlobalViewKind::AllTracks, .optPresentation = presentation});
    }
    else
    {
      navigationRes = _workspace.navigate({.target = listId, .optPresentation = presentation});
    }

    if (!navigationRes)
    {
      return std::unexpected{navigationRes.error()};
    }

    if (auto const attachedRes = attachView(*navigationRes); !attachedRes)
    {
      return std::unexpected{attachedRes.error()};
    }

    return {};
  }
} // namespace ao::tui
