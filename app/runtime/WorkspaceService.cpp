// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/rt/WorkspaceService.h>

#include "WorkspaceSessionYamlSchema.h"
#include <ao/Contract.h>
#include <ao/CoreIds.h>
#include <ao/Error.h>
#include <ao/async/Executor.h>
#include <ao/async/Signal.h>
#include <ao/async/Subscription.h>
#include <ao/compat/MoveOnlyFunction.h>
#include <ao/library/ListStore.h>
#include <ao/library/MetadataLayout.h>
#include <ao/library/MusicLibrary.h>
#include <ao/library/ReadTransaction.h>
#include <ao/rt/ConfigStore.h>
#include <ao/rt/Log.h>
#include <ao/rt/NavigationHistory.h>
#include <ao/rt/TrackField.h>
#include <ao/rt/TrackPresentation.h>
#include <ao/rt/ViewIds.h>
#include <ao/rt/ViewService.h>
#include <ao/rt/ViewState.h>
#include <ao/rt/VirtualListIds.h>
#include <ao/rt/WorkspaceSessionState.h>
#include <ao/rt/WorkspaceSnapshot.h>
#include <ao/rt/library/LibraryChanges.h>
#include <ao/rt/projection/TrackDetailProjection.h>
#include <ao/rt/projection/TrackListProjection.h>
#include <ao/utility/StrongTypeFormatter.h>
#include <ao/utility/Uuid.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdlib>
#include <exception>
#include <expected>
#include <format>
#include <functional>
#include <memory>
#include <optional>
#include <source_location>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <variant>
#include <vector>

namespace ao::rt
{
  namespace
  {
    struct ResolvedNavigationTarget final
    {
      ListId listId = kInvalidListId;
      std::string filterExpression{};
      bool reusePlainView = false;
    };

    Result<ResolvedNavigationTarget> resolveNavigationTarget(NavigationTarget const& target)
    {
      if (auto const* listId = std::get_if<ListId>(&target); listId != nullptr)
      {
        return ResolvedNavigationTarget{.listId = *listId, .reusePlainView = true};
      }

      if (auto const* filtered = std::get_if<FilteredListTarget>(&target); filtered != nullptr)
      {
        return ResolvedNavigationTarget{.listId = filtered->listId, .filterExpression = filtered->filterExpression};
      }

      if (auto const* global = std::get_if<GlobalViewKind>(&target);
          global != nullptr && *global == GlobalViewKind::AllTracks)
      {
        return ResolvedNavigationTarget{.listId = ListId{kAllTracksListId}, .reusePlainView = true};
      }

      return makeError(Error::Code::InvalidInput, "Unsupported workspace navigation target");
    }
  } // namespace

  struct WorkspaceService::Impl final
  {
    struct PendingCommit final
    {
      WorkspaceSnapshot snapshot{};
      NavigationHistory history{};
      WorkspaceChanged changed{};
    };

    async::Executor& executor;
    ViewService& views;
    library::MusicLibrary const& library;
    utility::UuidBytes libraryId{};
    LibraryChanges const& changes;
    WorkspaceSnapshot currentSnapshot;
    NavigationHistory navigationHistory;
    async::Signal<WorkspaceChanged const&> changedSignal;
    async::Subscription listsMutatedSub;

    Impl(async::Executor& callbackExecutor,
         ViewService& viewService,
         library::MusicLibrary const& musicLibrary,
         LibraryChanges const& changes)
      : executor{callbackExecutor}
      , views{viewService}
      , library{musicLibrary}
      , libraryId{musicLibrary.metadataHeader().libraryId}
      , changes{changes}
    {
      listsMutatedSub =
        changes.onChanged([this](LibraryChangeSet const& changeSet) { handleLibraryChange(changeSet); });
    }

    ~Impl() { changedSignal.disconnectAll(); }

    Impl(Impl const&) = delete;
    Impl& operator=(Impl const&) = delete;
    Impl(Impl&&) = delete;
    Impl& operator=(Impl&&) = delete;

    void ensureOnExecutor(std::source_location location = std::source_location::current()) const
    {
      AO_EXPECTS_AT(location, executor.isCurrent(), "WorkspaceService invoked off the executor thread");
    }

    PendingCommit prepareCommit(WorkspaceSnapshot nextSnapshot,
                                NavigationHistory nextHistory,
                                WorkspaceChangeCause const cause) const
    {
      nextSnapshot.revision = currentSnapshot.revision + 1;
      auto changed = WorkspaceChanged{.snapshot = nextSnapshot, .cause = cause};

      return PendingCommit{
        .snapshot = std::move(nextSnapshot),
        .history = std::move(nextHistory),
        .changed = std::move(changed),
      };
    }

    void publish(WorkspaceChanged changed) noexcept
    {
      try
      {
        // Signal::post owns the weak-state dance and the non-throwing delivery
        // boundary after admission.
        changedSignal.post(executor, std::move(changed));
      }
      catch (...)
      {
        AO_FATAL_EXCEPTION(std::current_exception(), "workspace observation admission");
      }
    }

    void installCommit(PendingCommit pending) noexcept
    {
      currentSnapshot = std::move(pending.snapshot);
      navigationHistory = std::move(pending.history);
      publish(std::move(pending.changed));
    }

    void commitCandidate(WorkspaceSnapshot nextSnapshot,
                         NavigationHistory nextHistory,
                         WorkspaceChangeCause const cause)
    {
      installCommit(prepareCommit(std::move(nextSnapshot), std::move(nextHistory), cause));
    }

    Result<TrackListViewState> liveViewState(ViewId const viewId) const
    {
      if (viewId == kInvalidViewId)
      {
        return makeError(Error::Code::InvalidInput, "The invalid view id cannot identify a live workspace view");
      }

      return views.findTrackListState(viewId);
    }

    Result<TrackListViewState> openViewState(ViewId const viewId) const
    {
      if (!std::ranges::contains(currentSnapshot.openViews, viewId))
      {
        return makeError(Error::Code::InvalidInput, std::format("View {} is not open in this workspace", viewId));
      }

      return liveViewState(viewId);
    }

    std::optional<TrackListViewState> findOpenView(ListId const listId, std::string_view const filterExpression) const
    {
      // Views being prepared or destroyed can still be registered with ViewService.
      for (auto const viewId : currentSnapshot.openViews)
      {
        if (auto stateRes = views.findTrackListState(viewId);
            stateRes && stateRes->listId == listId && stateRes->filterExpression == filterExpression)
        {
          return std::move(*stateRes);
        }
      }

      return std::nullopt;
    }

    static NavigationPoint navigationPoint(TrackListViewState const& state, TrackPresentationSpec const& presentation)
    {
      return NavigationPoint{
        .listId = state.listId,
        .filterExpression = state.filterExpression,
        .presentation = presentation,
      };
    }

    std::optional<TrackPresentationSpec> presentationForId(TrackPresetId const& id) const
    {
      if (auto const* builtin = std::get_if<BuiltinPreset>(&id); builtin != nullptr)
      {
        return builtinTrackPresentationPreset(*builtin).spec;
      }

      auto const customId = std::get<CustomPresetId>(id);

      for (auto const& custom : currentSnapshot.customPresets)
      {
        if (custom.presetId() == customId)
        {
          return custom.spec;
        }
      }

      return std::nullopt;
    }

    Result<ViewId> navigateToReusableView(TrackListViewState const& state,
                                          NavigationHistory nextHistory,
                                          std::optional<NavigationPresentation> const& optPresentation = {},
                                          bool const recordHistory = true)
    {
      auto const viewId = state.id;
      auto presentation = state.presentation;

      if (optPresentation && optPresentation->mode == NavigationPresentationMode::Override)
      {
        presentation = normalizeTrackPresentationSpec(optPresentation->spec);
      }

      auto const focusChanged = currentSnapshot.activeViewId != viewId;
      auto const presentationChanged = state.presentation != presentation;
      auto changed = focusChanged || presentationChanged;

      if (recordHistory)
      {
        if (!focusChanged)
        {
          // Compare repeats against a checkpoint of the live column sort, regardless of presentation intent.
          checkpointDeparture(nextHistory, presentationChanged);
        }

        changed = nextHistory.tryCommit(navigationPoint(state, presentation)) || changed;
      }

      if (!changed)
      {
        return viewId;
      }

      auto nextSnapshot = currentSnapshot;
      nextSnapshot.activeViewId = viewId;
      auto pending = prepareCommit(std::move(nextSnapshot), std::move(nextHistory), WorkspaceChangeCause::Navigation);

      if (presentationChanged)
      {
        if (auto res = views.setPresentation(viewId, presentation); !res)
        {
          return std::unexpected{res.error()};
        }
      }

      installCommit(std::move(pending));
      return viewId;
    }

    Result<ViewId> navigateToNewView(ResolvedNavigationTarget const& target,
                                     NavigationRequest const& request,
                                     NavigationHistory nextHistory)
    {
      auto nextSnapshot = currentSnapshot;
      nextSnapshot.openViews.reserve(nextSnapshot.openViews.size() + 1);
      auto config = TrackListViewConfig{
        .listId = target.listId,
        .filterExpression = target.filterExpression,
      };

      if (request.optPresentation)
      {
        config.optPresentation = request.optPresentation->spec;
      }

      auto viewRes = views.createView(config);

      if (!viewRes)
      {
        return std::unexpected{viewRes.error()};
      }

      auto const viewId = *viewRes;

      try
      {
        auto const state = views.trackListState(viewId);
        nextSnapshot.openViews.push_back(viewId);
        nextSnapshot.activeViewId = viewId;

        if (request.recordHistory)
        {
          std::ignore = nextHistory.tryCommit(navigationPoint(state, state.presentation));
        }

        commitCandidate(std::move(nextSnapshot), std::move(nextHistory), WorkspaceChangeCause::Navigation);
      }
      catch (...)
      {
        views.destroyView(viewId);
        throw;
      }

      return viewId;
    }

    bool isMissingSavedList(ListId const listId) const
    {
      if (isVirtualListId(listId))
      {
        return false;
      }

      auto const transaction = library.readTransaction();
      return !library.lists().reader(transaction).get(listId);
    }

    void checkpointDeparture(NavigationHistory& candidate, bool const allowNewStop) const
    {
      if (currentSnapshot.activeViewId == kInvalidViewId)
      {
        return;
      }

      auto const state = views.trackListState(currentSnapshot.activeViewId);

      if (auto point = navigationPoint(state, state.presentation);
          !candidate.tryCheckpointColumnSort(point) && allowNewStop)
      {
        if (auto const optCurrent = candidate.current(); optCurrent)
        {
          // Record a changed filter or the fallback from a deleted List. A different live List can
          // also be focused intentionally, which does not add a departure stop.
          auto const recordDeparture = optCurrent->listId == point.listId
                                         ? optCurrent->filterExpression != point.filterExpression
                                         : isMissingSavedList(optCurrent->listId);

          if (recordDeparture)
          {
            std::ignore = candidate.tryCommit(std::move(point));
          }
        }
      }
    }

    Result<ViewId> navigate(NavigationRequest const& request)
    {
      auto targetRes = resolveNavigationTarget(request.target);

      if (!targetRes)
      {
        return std::unexpected{targetRes.error()};
      }

      auto nextHistory = navigationHistory;
      auto const optState = targetRes->reusePlainView ? findOpenView(targetRes->listId, {}) : std::nullopt;

      if (request.recordHistory && (!optState || optState->id != currentSnapshot.activeViewId))
      {
        checkpointDeparture(nextHistory, true);
      }

      if (optState)
      {
        return navigateToReusableView(
          *optState, std::move(nextHistory), request.optPresentation, request.recordHistory);
      }

      return navigateToNewView(*targetRes, request, std::move(nextHistory));
    }

    static void recordLiveDeparture(NavigationHistory& history, TrackListViewState const& state)
    {
      if (auto point = navigationPoint(state, state.presentation); !history.tryCheckpointColumnSort(point))
      {
        std::ignore = history.tryCommit(std::move(point));
      }
    }

    Result<ViewId> navigateToOpenView(ViewId const viewId)
    {
      auto const targetRes = openViewState(viewId);

      if (!targetRes)
      {
        return std::unexpected{targetRes.error()};
      }

      // The view can still be open between a List deletion and its workspace notification.
      if (isMissingSavedList(targetRes->listId))
      {
        return makeError(Error::Code::NotFound, "The destination view's List no longer exists");
      }

      auto const active = currentSnapshot.activeViewId;

      if (active == viewId)
      {
        return viewId;
      }

      auto nextHistory = navigationHistory;

      if (active != kInvalidViewId)
      {
        auto const departureRes = liveViewState(active);

        if (!departureRes)
        {
          return std::unexpected{departureRes.error()};
        }

        recordLiveDeparture(nextHistory, *departureRes);
      }

      return navigateToReusableView(*targetRes, std::move(nextHistory));
    }

    Result<ListId> trackSourceList(TrackId const trackId, ListId const sourceListId) const
    {
      if (sourceListId != kInvalidListId && sourceListId != kAllTracksListId)
      {
        auto const memberRes = views.listContainsTrack(sourceListId, trackId);

        if (memberRes && *memberRes)
        {
          return sourceListId;
        }

        if (!memberRes && memberRes.error().code != Error::Code::NotFound)
        {
          return std::unexpected{memberRes.error()};
        }
      }

      auto const memberRes = views.listContainsTrack(kAllTracksListId, trackId);

      if (!memberRes)
      {
        return std::unexpected{memberRes.error()};
      }

      if (!*memberRes)
      {
        return makeError(Error::Code::NotFound, "Track is no longer in the library");
      }

      return kAllTracksListId;
    }

    Result<ViewId> navigateToTrackSource(TrackId const trackId,
                                         ListId const sourceListId,
                                         std::function<TrackPresentationSpec(ListId)> const& newViewPresentation)
    {
      auto const listRes = trackSourceList(trackId, sourceListId);

      if (!listRes)
      {
        return std::unexpected{listRes.error()};
      }

      auto nextHistory = navigationHistory;

      if (auto const active = currentSnapshot.activeViewId; active != kInvalidViewId)
      {
        auto const stateRes = liveViewState(active);

        if (!stateRes)
        {
          return std::unexpected{stateRes.error()};
        }

        if (stateRes->listId == *listRes)
        {
          auto const projectionRes = views.findTrackListProjection(active);

          if (!projectionRes)
          {
            return std::unexpected{projectionRes.error()};
          }

          if ((*projectionRes)->indexOf(trackId))
          {
            return active;
          }
        }

        recordLiveDeparture(nextHistory, *stateRes);
      }

      auto const target = ResolvedNavigationTarget{.listId = *listRes, .reusePlainView = true};

      if (auto const optState = findOpenView(target.listId, {}); optState)
      {
        return navigateToReusableView(*optState, std::move(nextHistory));
      }

      auto request = NavigationRequest{.target = *listRes};

      if (newViewPresentation)
      {
        request.optPresentation = NavigationPresentation{
          .mode = NavigationPresentationMode::NewViewDefault, .spec = newViewPresentation(*listRes)};
      }

      return navigateToNewView(target, request, std::move(nextHistory));
    }

    Result<> focus(ViewId const viewId)
    {
      if (auto stateRes = openViewState(viewId); !stateRes)
      {
        return std::unexpected{stateRes.error()};
      }

      if (currentSnapshot.activeViewId == viewId)
      {
        return {};
      }

      auto nextSnapshot = currentSnapshot;
      nextSnapshot.activeViewId = viewId;
      commitCandidate(std::move(nextSnapshot), navigationHistory, WorkspaceChangeCause::Focus);
      return {};
    }

    void closeViews(std::span<ViewId const> viewIds, WorkspaceChangeCause const cause)
    {
      if (viewIds.empty())
      {
        return;
      }

      auto nextSnapshot = currentSnapshot;
      std::erase_if(
        nextSnapshot.openViews, [&](ViewId const viewId) { return std::ranges::contains(viewIds, viewId); });

      if (std::ranges::contains(viewIds, nextSnapshot.activeViewId))
      {
        nextSnapshot.activeViewId = nextSnapshot.openViews.empty() ? kInvalidViewId : nextSnapshot.openViews.back();
      }

      commitCandidate(std::move(nextSnapshot), navigationHistory, cause);

      for (auto const viewId : viewIds)
      {
        views.destroyView(viewId);
      }
    }

    Result<> close(ViewId const viewId)
    {
      if (!std::ranges::contains(currentSnapshot.openViews, viewId))
      {
        return {};
      }

      if (auto stateRes = liveViewState(viewId); !stateRes)
      {
        return std::unexpected{stateRes.error()};
      }

      auto const ids = std::array{viewId};
      closeViews(ids, WorkspaceChangeCause::Close);
      return {};
    }

    Result<> applyPresentation(TrackPresentationSpec const& requested, PresentationChangeOptions const& options)
    {
      auto const viewId = currentSnapshot.activeViewId;

      if (viewId == kInvalidViewId)
      {
        return makeError(Error::Code::InvalidState, "No workspace view is focused");
      }

      auto stateRes = liveViewState(viewId);

      if (!stateRes)
      {
        return std::unexpected{stateRes.error()};
      }

      auto const presentation = normalizeTrackPresentationSpec(requested);
      auto nextHistory = navigationHistory;
      auto const presentationChanged = stateRes->presentation != presentation;
      auto changed = presentationChanged;

      if (options.recordHistory)
      {
        if (changed)
        {
          std::ignore = nextHistory.tryCheckpointColumnSort(navigationPoint(*stateRes, stateRes->presentation));
        }

        changed = nextHistory.tryCommit(navigationPoint(*stateRes, presentation)) || changed;
      }

      if (!changed)
      {
        return {};
      }

      auto pending = prepareCommit(currentSnapshot, std::move(nextHistory), WorkspaceChangeCause::Presentation);

      if (presentationChanged)
      {
        if (auto res = views.setPresentation(viewId, presentation); !res)
        {
          return std::unexpected{res.error()};
        }
      }

      installCommit(std::move(pending));
      return {};
    }

    Result<> restoreNavigationPoint(NavigationPoint const& point, NavigationHistory nextHistory)
    {
      auto const optState = findOpenView(point.listId, point.filterExpression);
      auto viewId = optState ? optState->id : kInvalidViewId;
      auto nextSnapshot = currentSnapshot;

      if (!optState)
      {
        nextSnapshot.openViews.reserve(nextSnapshot.openViews.size() + 1);
        auto res = views.createView(TrackListViewConfig{
          .listId = point.listId,
          .filterExpression = point.filterExpression,
          .optPresentation = point.presentation,
        });

        if (!res)
        {
          return std::unexpected{res.error()};
        }

        viewId = *res;
      }

      try
      {
        if (!optState)
        {
          nextSnapshot.openViews.push_back(viewId);
        }

        nextSnapshot.activeViewId = viewId;
        auto pending = prepareCommit(std::move(nextSnapshot), std::move(nextHistory), WorkspaceChangeCause::Navigation);

        if (optState && optState->presentation != point.presentation)
        {
          if (auto res = views.setPresentation(viewId, point.presentation); !res)
          {
            return std::unexpected{res.error()};
          }
        }

        installCommit(std::move(pending));
      }
      catch (...)
      {
        if (!optState)
        {
          views.destroyView(viewId);
        }

        throw;
      }

      return {};
    }

    Result<> goBack()
    {
      auto nextHistory = navigationHistory;
      checkpointDeparture(nextHistory, false);
      auto optPoint = nextHistory.back();

      if (!optPoint)
      {
        return makeError(Error::Code::NotFound, "Workspace navigation history has no previous entry");
      }

      return restoreNavigationPoint(*optPoint, std::move(nextHistory));
    }

    Result<> goForward()
    {
      auto nextHistory = navigationHistory;
      checkpointDeparture(nextHistory, false);
      auto optPoint = nextHistory.forward();

      if (!optPoint)
      {
        return makeError(Error::Code::NotFound, "Workspace navigation history has no forward entry");
      }

      return restoreNavigationPoint(*optPoint, std::move(nextHistory));
    }

    Result<> addPreset(CustomTrackPresentationPreset const& preset)
    {
      if (!std::holds_alternative<CustomPresetId>(preset.spec.presetId))
      {
        return makeError(Error::Code::InvalidInput, "A custom preset requires a custom preset id");
      }

      if (preset.spec.optColumnSort)
      {
        return makeError(Error::Code::InvalidInput, "A preset definition cannot have a view-local column sort");
      }

      auto seen = std::array<bool, kTrackSortFieldCount>{};

      for (auto const& term : preset.spec.sortBy)
      {
        auto const index = static_cast<std::size_t>(term.field);

        if (index >= seen.size() || seen[index])
        {
          return makeError(Error::Code::InvalidInput, "A custom preset requires distinct, known sort fields");
        }

        seen[index] = true;
      }

      auto nextSnapshot = currentSnapshot;
      auto const it =
        std::ranges::find(nextSnapshot.customPresets, preset.presetId(), &CustomTrackPresentationPreset::presetId);

      if (it != nextSnapshot.customPresets.end())
      {
        if (*it == preset)
        {
          return {};
        }

        *it = preset;
      }
      else
      {
        nextSnapshot.customPresets.push_back(preset);
      }

      commitCandidate(std::move(nextSnapshot), navigationHistory, WorkspaceChangeCause::Presets);
      return {};
    }

    Result<> removePreset(CustomPresetId const presetId)
    {
      auto nextSnapshot = currentSnapshot;
      auto const removed = std::erase_if(
        nextSnapshot.customPresets, [presetId](auto const& preset) { return preset.presetId() == presetId; });

      if (removed == 0)
      {
        return {};
      }

      commitCandidate(std::move(nextSnapshot), navigationHistory, WorkspaceChangeCause::Presets);
      return {};
    }

    void handleLibraryChange(LibraryChangeSet const& changeSet)
    {
      ensureOnExecutor();
      auto const toClose =
        changeSet.libraryReset ? viewsOfAbsentLists() : viewsOfLists(std::span{changeSet.listsDeleted});

      closeViews(toClose, WorkspaceChangeCause::ListDeletion);
    }

    std::vector<ViewId> viewsOfLists(std::span<ListId const> const listIds) const
    {
      auto result = std::vector<ViewId>{};

      for (auto const viewId : currentSnapshot.openViews)
      {
        if (std::ranges::contains(listIds, views.trackListState(viewId).listId))
        {
          result.push_back(viewId);
        }
      }

      return result;
    }

    // A reset publishes no per-List deletion, so each open base List is looked up instead.
    std::vector<ViewId> viewsOfAbsentLists() const
    {
      auto const transaction = library.readTransaction();
      auto const reader = library.lists().reader(transaction);
      auto result = std::vector<ViewId>{};

      for (auto const viewId : currentSnapshot.openViews)
      {
        if (auto const listId = views.trackListState(viewId).listId; !isVirtualListId(listId) && !reader.get(listId))
        {
          result.push_back(viewId);
        }
      }

      return result;
    }
  };

  WorkspaceService::WorkspaceService(async::Executor& executor,
                                     ViewService& views,
                                     library::MusicLibrary const& library,
                                     LibraryChanges const& changes)
    : _implPtr{std::make_unique<Impl>(executor, views, library, changes)}
  {
  }

  WorkspaceService::~WorkspaceService() = default;

  WorkspaceSnapshot WorkspaceService::snapshot() const
  {
    _implPtr->ensureOnExecutor();
    return _implPtr->currentSnapshot;
  }

  Result<> WorkspaceService::focusView(ViewId const viewId)
  {
    _implPtr->ensureOnExecutor();
    return _implPtr->focus(viewId);
  }

  Result<ViewId> WorkspaceService::navigate(NavigationRequest const& request)
  {
    _implPtr->ensureOnExecutor();
    return _implPtr->navigate(request);
  }

  Result<ViewId> WorkspaceService::navigateToOpenView(ViewId const viewId)
  {
    _implPtr->ensureOnExecutor();
    return _implPtr->navigateToOpenView(viewId);
  }

  Result<ViewId> WorkspaceService::navigateToTrackSource(
    TrackId const trackId,
    ListId const sourceListId,
    std::function<TrackPresentationSpec(ListId)> const& newViewPresentation)
  {
    _implPtr->ensureOnExecutor();
    return _implPtr->navigateToTrackSource(trackId, sourceListId, newViewPresentation);
  }

  Result<> WorkspaceService::closeView(ViewId const viewId)
  {
    _implPtr->ensureOnExecutor();
    return _implPtr->close(viewId);
  }

  Result<> WorkspaceService::setActivePresentation(TrackPresentationSpec const& presentation,
                                                   PresentationChangeOptions const options)
  {
    _implPtr->ensureOnExecutor();
    return _implPtr->applyPresentation(presentation, options);
  }

  Result<TrackPresentationSpec> WorkspaceService::setActivePresentation(TrackPresetId const& presetId,
                                                                        PresentationChangeOptions const options)
  {
    _implPtr->ensureOnExecutor();
    auto const optPresentation = _implPtr->presentationForId(presetId);

    if (!optPresentation)
    {
      return makeError(Error::Code::NotFound, std::format("Unknown track presentation '{}'", trackPresetKey(presetId)));
    }

    auto res = _implPtr->applyPresentation(*optPresentation, options);

    if (!res)
    {
      return std::unexpected{res.error()};
    }

    return normalizeTrackPresentationSpec(*optPresentation);
  }

  Result<> WorkspaceService::goBack()
  {
    _implPtr->ensureOnExecutor();
    return _implPtr->goBack();
  }

  Result<> WorkspaceService::goForward()
  {
    _implPtr->ensureOnExecutor();
    return _implPtr->goForward();
  }

  bool WorkspaceService::canGoBack() const
  {
    _implPtr->ensureOnExecutor();
    return _implPtr->navigationHistory.canGoBack();
  }

  bool WorkspaceService::canGoForward() const
  {
    _implPtr->ensureOnExecutor();
    return _implPtr->navigationHistory.canGoForward();
  }

  std::unique_ptr<TrackDetailProjection> WorkspaceService::detailProjection(DetailTarget const& target)
  {
    _implPtr->ensureOnExecutor();
    return _implPtr->views.detailProjection(target, *this, _implPtr->changes);
  }

  async::Subscription WorkspaceService::onChanged(compat::MoveOnlyFunction<void(WorkspaceChanged const&)> handler)
  {
    _implPtr->ensureOnExecutor();
    return _implPtr->changedSignal.connect(std::move(handler));
  }

  std::span<CustomTrackPresentationPreset const> WorkspaceService::customPresets() const
  {
    _implPtr->ensureOnExecutor();
    return _implPtr->currentSnapshot.customPresets;
  }

  Result<> WorkspaceService::addCustomPreset(CustomTrackPresentationPreset const& preset)
  {
    _implPtr->ensureOnExecutor();
    return _implPtr->addPreset(preset);
  }

  Result<> WorkspaceService::removeCustomPreset(CustomPresetId const presetId)
  {
    _implPtr->ensureOnExecutor();
    return _implPtr->removePreset(presetId);
  }

  void WorkspaceService::saveSession(ConfigStore& store) const
  {
    _implPtr->ensureOnExecutor();
    auto const snapshotValue = _implPtr->currentSnapshot;
    auto state = WorkspaceSessionState{.libraryId = _implPtr->libraryId};
    // A nonempty snapshot starts one past the valid range so the schema rejects a missing active id.
    state.activeViewIndex = snapshotValue.openViews.empty() ? 0 : snapshotValue.openViews.size();
    state.customPresets = snapshotValue.customPresets;

    for (std::size_t index = 0; index < snapshotValue.openViews.size(); ++index)
    {
      auto const viewId = snapshotValue.openViews[index];
      auto const viewState = _implPtr->views.trackListState(viewId);

      if (viewId == snapshotValue.activeViewId)
      {
        state.activeViewIndex = index;
      }

      state.openViews.push_back(TrackListViewConfig{
        .listId = viewState.listId,
        .filterExpression = viewState.filterExpression,
        .optPresentation = viewState.presentation,
      });
    }

    if (auto const res = store.save("workspace", state, detail::WorkspaceSessionYamlSchema{}); !res)
    {
      APP_LOG_ERROR("WorkspaceService: Failed to save session - {}", res.error().message);
    }
  }

  Result<> WorkspaceService::restoreSession(ConfigStore& store)
  {
    _implPtr->ensureOnExecutor();
    auto state = WorkspaceSessionState{};
    auto const loadedRes = store.load("workspace", state, detail::WorkspaceSessionYamlSchema{});

    if (!loadedRes)
    {
      if (loadedRes.error().code == Error::Code::NotFound)
      {
        return {};
      }

      // Hosts can continue with a fallback view and checkpoint it on normal shutdown.
      store.preserveGroup("workspace");
      return std::unexpected{loadedRes.error()};
    }

    if (!*loadedRes)
    {
      return {};
    }

    // Another library's list ids name nothing here, so only its custom presets,
    // which name no library object, are restored.
    if (state.libraryId != _implPtr->libraryId)
    {
      state.openViews.clear();
      state.activeViewIndex = 0;
    }

    auto createdViewIds = std::vector<ViewId>{};
    createdViewIds.reserve(state.openViews.size());

    for (auto const& viewConfig : state.openViews)
    {
      auto res = _implPtr->views.createView(viewConfig);

      if (!res)
      {
        for (auto const viewId : createdViewIds)
        {
          _implPtr->views.destroyView(viewId);
        }

        return std::unexpected{res.error()};
      }

      createdViewIds.push_back(*res);
    }

    try
    {
      auto nextSnapshot = _implPtr->currentSnapshot;
      nextSnapshot.openViews.reserve(nextSnapshot.openViews.size() + createdViewIds.size());
      nextSnapshot.openViews.insert(nextSnapshot.openViews.end(), createdViewIds.begin(), createdViewIds.end());
      nextSnapshot.customPresets = std::move(state.customPresets);
      auto focused = kInvalidViewId;

      if (!createdViewIds.empty())
      {
        focused = createdViewIds[state.activeViewIndex];
      }
      else if (!nextSnapshot.openViews.empty())
      {
        focused = nextSnapshot.openViews.front();
      }

      nextSnapshot.activeViewId = focused;
      auto nextHistory = _implPtr->navigationHistory;
      bool historyChanged = false;

      if (focused != kInvalidViewId)
      {
        auto const viewState = _implPtr->views.trackListState(focused);
        historyChanged = nextHistory.tryCommit(Impl::navigationPoint(viewState, viewState.presentation));
      }

      auto const aggregateChanged = !createdViewIds.empty() ||
                                    nextSnapshot.customPresets != _implPtr->currentSnapshot.customPresets ||
                                    nextSnapshot.activeViewId != _implPtr->currentSnapshot.activeViewId;

      if (!aggregateChanged && !historyChanged)
      {
        return {};
      }

      _implPtr->commitCandidate(std::move(nextSnapshot), std::move(nextHistory), WorkspaceChangeCause::Restore);
    }
    catch (...)
    {
      for (auto const viewId : createdViewIds)
      {
        _implPtr->views.destroyView(viewId);
      }

      throw;
    }

    return {};
  }
} // namespace ao::rt
