// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include <ao/CoreIds.h>
#include <ao/Error.h>
#include <ao/library/ListStore.h>
#include <ao/library/ListView.h>
#include <ao/library/MusicLibrary.h>
#include <ao/library/TrackStore.h>
#include <ao/library/TrackView.h>
#include <ao/rt/PlaybackLaunchSpec.h>
#include <ao/rt/ScopedTimer.h>
#include <ao/rt/Signal.h>
#include <ao/rt/StorageResult.h>
#include <ao/rt/Subscription.h>
#include <ao/rt/TrackField.h>
#include <ao/rt/TrackMutation.h>
#include <ao/rt/TrackPresentation.h>
#include <ao/rt/ViewIds.h>
#include <ao/rt/ViewService.h>
#include <ao/rt/ViewState.h>
#include <ao/rt/VirtualListIds.h>
#include <ao/rt/WorkspaceService.h>
#include <ao/rt/library/LibraryChanges.h>
#include <ao/rt/projection/LiveTrackDetailProjection.h>
#include <ao/rt/projection/LiveTrackListProjection.h>
#include <ao/rt/projection/TrackDetailProjection.h>
#include <ao/rt/projection/TrackListProjection.h>
#include <ao/rt/source/SmartListSource.h>
#include <ao/rt/source/TrackSourceCache.h>
#include <ao/rt/source/TrackSourceLease.h>

#include <chrono>
#include <cstdint>
#include <expected>
#include <format>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace ao::rt
{
  namespace
  {
    struct ViewEntry final
    {
      TrackListViewState state;
      std::optional<TrackSourceLease> optBaseSourceLease;
      std::optional<TrackSourceLease> optActiveSourceLease;
      std::optional<Error> optFilterError;
      std::shared_ptr<LiveTrackListProjection> projectionPtr = nullptr;
    };

    struct PreparedViewResources final
    {
      TrackSourceLease baseSourceLease;
      TrackSourceLease activeSourceLease;
      std::optional<Error> optFilterError;
      std::shared_ptr<LiveTrackListProjection> projectionPtr;
    };

    TrackPresentationSpec presentationForGroup(TrackGroupKey const groupBy)
    {
      auto const& presets = builtinTrackPresentationPresets();
      auto const* preset = builtinTrackPresentationPreset(kDefaultTrackPresentationId);

      for (auto const& candidate : presets)
      {
        if (candidate.spec.groupBy == groupBy)
        {
          preset = &candidate;
          break;
        }
      }

      return preset->spec;
    }

    bool isManualList(library::MusicLibrary& library, ListId const listId)
    {
      if (listId == kAllTracksListId)
      {
        return false;
      }

      auto const transaction = library.readTransaction();
      auto const optView = library.lists().reader(transaction).get(listId);
      return optView && !optView->isSmart();
    }

    TrackPresentationSpec initialPresentation(TrackListViewConfig const& initial, library::MusicLibrary& library)
    {
      if (initial.optPresentation)
      {
        return normalizeTrackPresentationSpec(*initial.optPresentation);
      }

      auto const manualList = isManualList(library, initial.listId);
      auto const hasExplicitOrder = initial.groupBy != TrackGroupKey::None || !initial.sortBy.empty();

      if (hasExplicitOrder)
      {
        auto result = presentationForGroup(initial.groupBy);

        if (manualList && initial.groupBy == TrackGroupKey::None)
        {
          if (auto const* preset = builtinTrackPresentationPreset(kListOrderTrackPresentationId); preset != nullptr)
          {
            result = preset->spec;
          }
        }

        result.id.clear();
        result.groupBy = initial.groupBy;

        if (!initial.sortBy.empty())
        {
          result.sortBy = initial.sortBy;
        }

        return result;
      }

      if (manualList)
      {
        auto const* preset = builtinTrackPresentationPreset(kListOrderTrackPresentationId);

        if (preset != nullptr)
        {
          return preset->spec;
        }
      }

      return presentationForGroup(initial.groupBy);
    }

    Result<PreparedViewResources> prepareViewResources(ViewId const viewId,
                                                       ListId const baseListId,
                                                       TrackSourceLease baseSourceLease,
                                                       std::string const& filterExpression,
                                                       TrackPresentationSpec const& presentation,
                                                       library::MusicLibrary& library,
                                                       TrackSourceCache& sources)
    {
      auto activeSourceLease = baseSourceLease;
      auto optFilterError = std::optional<Error>{};

      if (!filterExpression.empty())
      {
        auto sourceResult = sources.acquire(SourceSpec{.baseListId = baseListId, .filterExpression = filterExpression});

        if (!sourceResult)
        {
          return std::unexpected{sourceResult.error()};
        }

        activeSourceLease = std::move(*sourceResult);
        optFilterError = sources.sourceError(activeSourceLease);
      }

      auto projectionPtr = std::make_shared<LiveTrackListProjection>(viewId, activeSourceLease, library);
      projectionPtr->setPresentation(presentation);

      return PreparedViewResources{
        .baseSourceLease = std::move(baseSourceLease),
        .activeSourceLease = std::move(activeSourceLease),
        .optFilterError = std::move(optFilterError),
        .projectionPtr = std::move(projectionPtr),
      };
    }

    void installResources(ViewEntry& entry, PreparedViewResources resources)
    {
      entry.projectionPtr = std::move(resources.projectionPtr);
      entry.optActiveSourceLease = std::move(resources.activeSourceLease);
      entry.optFilterError = std::move(resources.optFilterError);
      entry.optBaseSourceLease = std::move(resources.baseSourceLease);
    }

    std::unexpected<Error> missingViewError(ViewId const viewId)
    {
      return makeError(Error::Code::NotFound, std::format("View {} does not exist", viewId));
    }

    std::unexpected<Error> destroyedViewError(ViewId const viewId)
    {
      return makeError(Error::Code::InvalidState, std::format("View {} is destroyed", viewId));
    }

    void applyPresentation(ViewEntry& entry, TrackPresentationSpec const& spec)
    {
      auto const normalized = normalizeTrackPresentationSpec(spec);
      entry.state.presentation = normalized;
      entry.state.groupBy = normalized.groupBy;
      entry.state.sortBy = normalized.sortBy;

      if (entry.projectionPtr)
      {
        entry.projectionPtr->setPresentation(normalized);
      }
    }
  } // namespace

  struct ViewService::Impl final
  {
    std::uint64_t nextViewId = 1;
    std::unordered_map<ViewId, ViewEntry> views;

    async::Executor& executor;
    library::MusicLibrary& library;
    TrackSourceCache& sources;

    Impl(async::Executor& exec, library::MusicLibrary& lib, TrackSourceCache& sourceCache)
      : executor{exec}, library{lib}, sources{sourceCache}
    {
    }

    Signal<ViewId> destroyedSignal;
    Signal<TrackListProjectionChanged const&> projectionChangedSignal;
    Signal<ViewService::FilterChanged const&> filterChangedSignal;
    Signal<FilterStatusChanged const&> filterStatusChangedSignal;
    Signal<ViewService::PresentationChanged const&> presentationChangedSignal;
    Signal<ViewService::SelectionChanged const&> selectionChangedSignal;
    Signal<ViewService::ListChanged const&> listChangedSignal;
  };

  ViewService::ViewService(async::Executor& executor, library::MusicLibrary& library, TrackSourceCache& sources)
    : _implPtr{std::make_unique<Impl>(executor, library, sources)}
  {
  }

  ViewService::~ViewService() = default;

  Subscription ViewService::onDestroyed(std::move_only_function<void(ViewId)> handler)
  {
    return _implPtr->destroyedSignal.connect(std::move(handler));
  }

  Subscription ViewService::onProjectionChanged(
    std::move_only_function<void(TrackListProjectionChanged const&)> handler)
  {
    return _implPtr->projectionChangedSignal.connect(std::move(handler));
  }

  Subscription ViewService::onFilterChanged(std::move_only_function<void(FilterChanged const&)> handler)
  {
    return _implPtr->filterChangedSignal.connect(std::move(handler));
  }

  Subscription ViewService::onFilterStatusChanged(std::move_only_function<void(FilterStatusChanged const&)> handler)
  {
    return _implPtr->filterStatusChangedSignal.connect(std::move(handler));
  }

  Subscription ViewService::onPresentationChanged(std::move_only_function<void(PresentationChanged const&)> handler)
  {
    return _implPtr->presentationChangedSignal.connect(std::move(handler));
  }

  Subscription ViewService::onSelectionChanged(std::move_only_function<void(SelectionChanged const&)> handler)
  {
    return _implPtr->selectionChangedSignal.connect(std::move(handler));
  }

  Subscription ViewService::onListChanged(std::move_only_function<void(ListChanged const&)> handler)
  {
    return _implPtr->listChangedSignal.connect(std::move(handler));
  }

  Result<CreateTrackListViewReply> ViewService::createView(TrackListViewConfig const& initial, bool const attached)
  {
    auto baseSourceResult = _implPtr->sources.acquire(initial.listId);

    if (!baseSourceResult)
    {
      return std::unexpected{baseSourceResult.error()};
    }

    auto const id = ViewId{_implPtr->nextViewId};
    auto const presentation = initialPresentation(initial, _implPtr->library);
    auto resourcesResult = prepareViewResources(id,
                                                initial.listId,
                                                std::move(*baseSourceResult),
                                                initial.filterExpression,
                                                presentation,
                                                _implPtr->library,
                                                _implPtr->sources);

    if (!resourcesResult)
    {
      return std::unexpected{resourcesResult.error()};
    }

    auto state = TrackListViewState{
      .id = id,
      .lifecycle = attached ? ViewLifecycleState::Attached : ViewLifecycleState::Detached,
      .listId = initial.listId,
      .filterExpression = initial.filterExpression,
      .groupBy = presentation.groupBy,
      .sortBy = presentation.sortBy,
      .presentation = presentation,
    };
    auto entry = ViewEntry{
      .state = std::move(state),
      .optBaseSourceLease = std::nullopt,
      .optActiveSourceLease = std::nullopt,
      .optFilterError = std::nullopt,
    };
    installResources(entry, std::move(*resourcesResult));
    _implPtr->views.emplace(id, std::move(entry));
    ++_implPtr->nextViewId;
    return CreateTrackListViewReply{.viewId = id};
  }

  Result<> ViewService::destroyView(ViewId viewId)
  {
    auto const it = _implPtr->views.find(viewId);

    if (it == _implPtr->views.end())
    {
      return missingViewError(viewId);
    }

    if (it->second.state.lifecycle == ViewLifecycleState::Destroyed)
    {
      return destroyedViewError(viewId);
    }

    it->second.state.lifecycle = ViewLifecycleState::Destroyed;
    _implPtr->destroyedSignal.post(_implPtr->executor, viewId);
    it->second.projectionPtr.reset();
    it->second.optActiveSourceLease.reset();
    it->second.optBaseSourceLease.reset();
    return {};
  }

  Result<> ViewService::setFilter(ViewId const viewId, std::string filterExpression)
  {
    auto const timer = rt::ScopedTimer{"ViewService::setFilter"};
    auto it = _implPtr->views.find(viewId);

    if (it == _implPtr->views.end())
    {
      return missingViewError(viewId);
    }

    auto& entry = it->second;

    if (entry.state.lifecycle == ViewLifecycleState::Destroyed)
    {
      return destroyedViewError(viewId);
    }

    if (entry.state.filterExpression == filterExpression)
    {
      return {};
    }

    if (!entry.optBaseSourceLease)
    {
      return makeError(Error::Code::InvalidState, "View has no live source lease");
    }

    auto resourcesResult = prepareViewResources(viewId,
                                                entry.state.listId,
                                                *entry.optBaseSourceLease,
                                                filterExpression,
                                                entry.state.presentation,
                                                _implPtr->library,
                                                _implPtr->sources);

    if (!resourcesResult)
    {
      return std::unexpected{resourcesResult.error()};
    }

    installResources(entry, std::move(*resourcesResult));
    entry.state.filterExpression = std::move(filterExpression);
    ++entry.state.revision;
    _implPtr->filterChangedSignal.post(
      _implPtr->executor,
      ViewService::FilterChanged{.viewId = viewId, .filterExpression = entry.state.filterExpression});
    _implPtr->projectionChangedSignal.post(
      _implPtr->executor,
      TrackListProjectionChanged{
        .viewId = viewId, .projectionPtr = entry.projectionPtr, .revision = entry.state.revision});

    auto status = FilterStatusChanged{
      .viewId = viewId,
      .expression = entry.state.filterExpression,
      .revision = entry.state.revision,
    };

    status.optError = entry.optFilterError;

    _implPtr->filterStatusChangedSignal.emit(status);
    return {};
  }

  Result<> ViewService::setPresentation(ViewId viewId, TrackPresentationSpec const& presentation)
  {
    auto it = _implPtr->views.find(viewId);

    if (it == _implPtr->views.end())
    {
      return missingViewError(viewId);
    }

    if (it->second.state.lifecycle == ViewLifecycleState::Destroyed)
    {
      return destroyedViewError(viewId);
    }

    auto spec = normalizeTrackPresentationSpec(presentation);

    if (it->second.state.presentation.id == spec.id && it->second.state.groupBy == spec.groupBy &&
        it->second.state.sortBy == spec.sortBy)
    {
      return {};
    }

    applyPresentation(it->second, spec);
    it->second.state.revision++;
    _implPtr->presentationChangedSignal.post(
      _implPtr->executor, ViewService::PresentationChanged{.viewId = viewId, .presentation = spec});
    return {};
  }

  Result<TrackPresentationSpec> ViewService::setPresentation(ViewId viewId, std::string_view presentationId)
  {
    if (auto const it = _implPtr->views.find(viewId); it == _implPtr->views.end())
    {
      return missingViewError(viewId);
    }

    auto const* const preset = builtinTrackPresentationPreset(presentationId);
    auto spec = (preset != nullptr) ? preset->spec : defaultTrackPresentationSpec();

    if (auto result = setPresentation(viewId, spec); !result)
    {
      return std::unexpected{result.error()};
    }

    return spec;
  }

  Result<> ViewService::setSelection(ViewId viewId, std::vector<TrackId> selection)
  {
    auto it = _implPtr->views.find(viewId);

    if (it == _implPtr->views.end())
    {
      return missingViewError(viewId);
    }

    if (it->second.state.lifecycle == ViewLifecycleState::Destroyed)
    {
      return destroyedViewError(viewId);
    }

    it->second.state.selection = std::move(selection);
    it->second.state.revision++;

    _implPtr->selectionChangedSignal.emit(
      ViewService::SelectionChanged{.viewId = viewId, .selection = it->second.state.selection});
    return {};
  }

  Result<> ViewService::openListInView(ViewId const viewId, ListId const listId)
  {
    auto it = _implPtr->views.find(viewId);

    if (it == _implPtr->views.end())
    {
      return missingViewError(viewId);
    }

    auto& entry = it->second;

    if (entry.state.lifecycle == ViewLifecycleState::Destroyed)
    {
      return destroyedViewError(viewId);
    }

    if (entry.state.listId == listId)
    {
      return {};
    }

    auto baseSourceResult = _implPtr->sources.acquire(listId);

    if (!baseSourceResult)
    {
      return std::unexpected{baseSourceResult.error()};
    }

    auto resourcesResult = prepareViewResources(viewId,
                                                listId,
                                                std::move(*baseSourceResult),
                                                entry.state.filterExpression,
                                                entry.state.presentation,
                                                _implPtr->library,
                                                _implPtr->sources);

    if (!resourcesResult)
    {
      return std::unexpected{resourcesResult.error()};
    }

    installResources(entry, std::move(*resourcesResult));
    entry.state.listId = listId;
    ++entry.state.revision;
    _implPtr->projectionChangedSignal.post(
      _implPtr->executor,
      TrackListProjectionChanged{
        .viewId = viewId, .projectionPtr = entry.projectionPtr, .revision = entry.state.revision});

    _implPtr->listChangedSignal.post(_implPtr->executor, ViewService::ListChanged{.viewId = viewId, .listId = listId});
    return {};
  }

  Result<PlaybackLaunchSpec> ViewService::capturePlaybackLaunchSpec(ViewId const viewId) const
  {
    auto const it = _implPtr->views.find(viewId);

    if (it == _implPtr->views.end())
    {
      return missingViewError(viewId);
    }

    if (it->second.state.lifecycle == ViewLifecycleState::Destroyed)
    {
      return destroyedViewError(viewId);
    }

    auto const& state = it->second.state;
    return PlaybackLaunchSpec{
      .sourceListId = state.listId,
      .quickFilterExpression = state.filterExpression,
      .order = TrackOrderSpec{.sortBy = state.sortBy},
    };
  }

  std::vector<ViewRecord> ViewService::listViews() const
  {
    auto records = std::vector<ViewRecord>{};
    records.reserve(_implPtr->views.size());

    for (auto const& [viewId, entry] : _implPtr->views)
    {
      if (entry.state.lifecycle != ViewLifecycleState::Destroyed)
      {
        records.push_back(ViewRecord{.id = viewId, .kind = ViewKind::TrackList, .lifecycle = entry.state.lifecycle});
      }
    }

    return records;
  }

  TrackListViewState ViewService::trackListState(ViewId viewId) const
  {
    return _implPtr->views.at(viewId).state;
  }

  TrackPresentationSpec const& ViewService::trackListPresentation(ViewId viewId) const&
  {
    return _implPtr->views.at(viewId).state.presentation;
  }

  std::chrono::milliseconds ViewService::selectionDuration(ViewId viewId) const
  {
    auto const it = _implPtr->views.find(viewId);

    if (it == _implPtr->views.end() || it->second.state.selection.empty())
    {
      return std::chrono::milliseconds{0};
    }

    auto const transaction = _implPtr->library.readTransaction();
    auto const reader = _implPtr->library.tracks().reader(transaction);

    auto totalDuration = std::chrono::milliseconds{0};

    for (auto const trackId : it->second.state.selection)
    {
      if (auto const optView = storageValueOrNullopt(
            reader.get(trackId, library::TrackStore::Reader::LoadMode::Cold), "Failed to calculate selection duration");
          optView)
      {
        totalDuration += optView->property().duration();
      }
    }

    return totalDuration;
  }

  std::shared_ptr<TrackListProjection> ViewService::trackListProjection(ViewId viewId)
  {
    return _implPtr->views.at(viewId).projectionPtr;
  }

  std::unique_ptr<TrackDetailProjection> ViewService::detailProjection(DetailTarget const& target,
                                                                       WorkspaceService& workspace,
                                                                       LibraryChanges const& changes)
  {
    return std::make_unique<LiveTrackDetailProjection>(target, *this, _implPtr->library, workspace, changes);
  }
} // namespace ao::rt
