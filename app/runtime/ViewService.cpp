// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include <ao/CoreIds.h>
#include <ao/library/MusicLibrary.h>
#include <ao/library/TrackStore.h>
#include <ao/library/TrackView.h>
#include <ao/rt/ScopedTimer.h>
#include <ao/rt/Signal.h>
#include <ao/rt/StorageResult.h>
#include <ao/rt/Subscription.h>
#include <ao/rt/TrackMutation.h>
#include <ao/rt/TrackPresentation.h>
#include <ao/rt/ViewIds.h>
#include <ao/rt/ViewService.h>
#include <ao/rt/ViewState.h>
#include <ao/rt/WorkspaceService.h>
#include <ao/rt/library/LibraryChanges.h>
#include <ao/rt/projection/LiveTrackDetailProjection.h>
#include <ao/rt/projection/LiveTrackListProjection.h>
#include <ao/rt/projection/TrackDetailProjection.h>
#include <ao/rt/projection/TrackListProjection.h>
#include <ao/rt/source/SmartListSource.h>
#include <ao/rt/source/TrackSource.h>
#include <ao/rt/source/TrackSourceCache.h>

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
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
      std::unique_ptr<SmartListSource> adHocSourcePtr;
      TrackSource* activeSource = nullptr;
      std::shared_ptr<LiveTrackListProjection> projectionPtr;
    };

    void applyPresentation(ViewEntry& entry)
    {
      // Derive sort and redundant fields from the built-in preset whose group-by
      // matches the entry.
      auto const& presets = builtinTrackPresentationPresets();
      auto const* preset = builtinTrackPresentationPreset(kDefaultTrackPresentationId);

      for (auto const& candidate : presets)
      {
        if (candidate.spec.groupBy == entry.state.groupBy)
        {
          preset = &candidate;
          break;
        }
      }

      entry.state.presentation = preset->spec;
      entry.state.sortBy = entry.state.presentation.sortBy;

      if (entry.projectionPtr)
      {
        entry.projectionPtr->setPresentation(preset->spec);
      }
    }

    void applyPresentation(ViewEntry& entry, TrackPresentationSpec const& spec)
    {
      entry.state.presentation = spec;
      entry.state.groupBy = spec.groupBy;
      entry.state.sortBy = spec.sortBy;

      if (entry.projectionPtr)
      {
        entry.projectionPtr->setPresentation(spec);
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

  CreateTrackListViewReply ViewService::createView(TrackListViewConfig const& initial, bool attached)
  {
    auto id = ViewId{_implPtr->nextViewId++};

    auto state = TrackListViewState{
      .id = id,
      .lifecycle = attached ? ViewLifecycleState::Attached : ViewLifecycleState::Detached,
      .listId = initial.listId,
      .filterExpression = initial.filterExpression,
      .groupBy = initial.groupBy,
      .sortBy = initial.sortBy,
    };

    auto* baseSource = &_implPtr->sources.sourceFor(initial.listId);
    auto adHocSourcePtr = std::unique_ptr<SmartListSource>{};

    if (!initial.filterExpression.empty())
    {
      adHocSourcePtr =
        std::make_unique<SmartListSource>(*baseSource, _implPtr->library, _implPtr->sources.smartEvaluator());
      adHocSourcePtr->setExpression(initial.filterExpression);
      adHocSourcePtr->reload();
      baseSource = adHocSourcePtr.get();
    }

    auto projectionPtr = std::make_shared<LiveTrackListProjection>(id, *baseSource, _implPtr->library);

    auto& entry = _implPtr->views[id];
    entry.state = state;
    entry.adHocSourcePtr = std::move(adHocSourcePtr);
    entry.activeSource = baseSource;
    entry.projectionPtr = projectionPtr;

    if (initial.optPresentation)
    {
      applyPresentation(entry, *initial.optPresentation);
    }
    else
    {
      applyPresentation(entry);
    }

    return CreateTrackListViewReply{.viewId = id};
  }

  void ViewService::destroyView(ViewId viewId)
  {
    if (auto it = _implPtr->views.find(viewId); it != _implPtr->views.end())
    {
      it->second.state.lifecycle = ViewLifecycleState::Destroyed;
      _implPtr->destroyedSignal.post(_implPtr->executor, viewId);

      // Reset projection and ad-hoc source to detach from TrackSource,
      // preventing dangling references if the underlying source is deleted.
      it->second.projectionPtr.reset();
      it->second.adHocSourcePtr.reset();
      it->second.activeSource = nullptr;
    }
  }

  void ViewService::setFilter(ViewId viewId, std::string filterExpression)
  {
    auto const timer = rt::ScopedTimer{"ViewService::setFilter"};
    auto it = _implPtr->views.find(viewId);

    if (it == _implPtr->views.end())
    {
      return;
    }

    it->second.state.filterExpression = std::move(filterExpression);
    it->second.state.revision++;
    _implPtr->filterChangedSignal.post(
      _implPtr->executor,
      ViewService::FilterChanged{.viewId = viewId, .filterExpression = it->second.state.filterExpression});

    if (it->second.adHocSourcePtr && !it->second.state.filterExpression.empty())
    {
      it->second.adHocSourcePtr->setExpression(it->second.state.filterExpression);
      it->second.adHocSourcePtr->reload();
    }
    else if (!it->second.state.filterExpression.empty())
    {
      // If we didn't have an adHocSourcePtr but now we need one
      auto* baseSource = &_implPtr->sources.sourceFor(it->second.state.listId);
      it->second.adHocSourcePtr =
        std::make_unique<SmartListSource>(*baseSource, _implPtr->library, _implPtr->sources.smartEvaluator());
      it->second.adHocSourcePtr->setExpression(it->second.state.filterExpression);
      it->second.adHocSourcePtr->reload();
      it->second.activeSource = it->second.adHocSourcePtr.get();

      // Need to attach projection to new source
      it->second.projectionPtr =
        std::make_shared<LiveTrackListProjection>(viewId, *it->second.activeSource, _implPtr->library);
      applyPresentation(it->second, it->second.state.presentation);
      auto ev = TrackListProjectionChanged{
        .viewId = viewId, .projectionPtr = it->second.projectionPtr, .revision = it->second.state.revision};
      _implPtr->projectionChangedSignal.post(_implPtr->executor, std::move(ev));
    }
    else
    {
      // Switching from adHocSourcePtr to no filter
      auto* baseSource = &_implPtr->sources.sourceFor(it->second.state.listId);
      it->second.projectionPtr.reset();
      it->second.adHocSourcePtr.reset();
      it->second.activeSource = baseSource;
      it->second.projectionPtr =
        std::make_shared<LiveTrackListProjection>(viewId, *it->second.activeSource, _implPtr->library);
      applyPresentation(it->second, it->second.state.presentation);
      auto ev = TrackListProjectionChanged{
        .viewId = viewId, .projectionPtr = it->second.projectionPtr, .revision = it->second.state.revision};
      _implPtr->projectionChangedSignal.post(_implPtr->executor, std::move(ev));
    }

    auto status = FilterStatusChanged{
      .viewId = viewId,
      .expression = it->second.state.filterExpression,
      .revision = it->second.state.revision,
    };

    if (it->second.adHocSourcePtr != nullptr)
    {
      status.optError = it->second.adHocSourcePtr->error();
    }

    _implPtr->filterStatusChangedSignal.emit(status);
  }

  void ViewService::setPresentation(ViewId viewId, TrackPresentationSpec const& presentation)
  {
    auto it = _implPtr->views.find(viewId);

    if (it == _implPtr->views.end())
    {
      return;
    }

    auto spec = normalizeTrackPresentationSpec(presentation);

    if (it->second.state.presentation.id == spec.id && it->second.state.groupBy == spec.groupBy &&
        it->second.state.sortBy == spec.sortBy)
    {
      return;
    }

    applyPresentation(it->second, spec);
    it->second.state.revision++;
    _implPtr->presentationChangedSignal.post(
      _implPtr->executor, ViewService::PresentationChanged{.viewId = viewId, .presentation = spec});
  }

  TrackPresentationSpec ViewService::setPresentation(ViewId viewId, std::string_view presentationId)
  {
    if (auto const it = _implPtr->views.find(viewId); it == _implPtr->views.end())
    {
      return {};
    }

    auto const* const preset = builtinTrackPresentationPreset(presentationId);
    auto const spec = (preset != nullptr) ? preset->spec : defaultTrackPresentationSpec();

    setPresentation(viewId, spec);
    return spec;
  }

  void ViewService::setSelection(ViewId viewId, std::vector<TrackId> selection)
  {
    auto it = _implPtr->views.find(viewId);

    if (it == _implPtr->views.end())
    {
      return;
    }

    it->second.state.selection = std::move(selection);
    it->second.state.revision++;

    _implPtr->selectionChangedSignal.emit(
      ViewService::SelectionChanged{.viewId = viewId, .selection = it->second.state.selection});
  }

  void ViewService::openListInView(ViewId viewId, ListId listId)
  {
    auto it = _implPtr->views.find(viewId);

    if (it == _implPtr->views.end())
    {
      return;
    }

    it->second.state.listId = listId;
    it->second.state.revision++;

    it->second.projectionPtr.reset();

    if (auto* baseSource = &_implPtr->sources.sourceFor(listId); it->second.adHocSourcePtr)
    {
      // adHocSourcePtr holds a reference to the old baseSource. We must recreate it.
      it->second.adHocSourcePtr =
        std::make_unique<SmartListSource>(*baseSource, _implPtr->library, _implPtr->sources.smartEvaluator());
      it->second.adHocSourcePtr->setExpression(it->second.state.filterExpression);
      it->second.adHocSourcePtr->reload();
      it->second.activeSource = it->second.adHocSourcePtr.get();
    }
    else
    {
      it->second.activeSource = baseSource;
    }

    it->second.projectionPtr =
      std::make_shared<LiveTrackListProjection>(viewId, *it->second.activeSource, _implPtr->library);
    applyPresentation(it->second, it->second.state.presentation);
    _implPtr->projectionChangedSignal.post(
      _implPtr->executor,
      TrackListProjectionChanged{
        .viewId = viewId, .projectionPtr = it->second.projectionPtr, .revision = it->second.state.revision});

    _implPtr->listChangedSignal.post(_implPtr->executor, ViewService::ListChanged{.viewId = viewId, .listId = listId});
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
