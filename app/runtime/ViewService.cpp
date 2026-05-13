// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include "ViewService.h"
#include "TrackDetailProjection.h"
#include "TrackListPresentation.h"
#include "TrackListProjection.h"

#include "LibraryMutationService.h"
#include "ListSourceStore.h"

#include "SmartListSource.h"
#include "TrackSource.h"
#include "WorkspaceService.h"
#include <ao/utility/ScopedTimer.h>

#include <ao/library/ListStore.h>
#include <ao/library/MusicLibrary.h>

#include <limits>
#include <memory>
#include <ranges>
#include <unordered_map>

namespace ao::rt
{
  namespace
  {
    struct ViewEntry final
    {
      TrackListViewState state;
      std::unique_ptr<SmartListSource> adHocSource;
      TrackSource* activeSource = nullptr;
      std::shared_ptr<ITrackListProjection> projection;
    };

    void applyPresentation(ViewEntry& entry)
    {
      auto policy = presentationForGroup(entry.state.groupBy);
      entry.state.sortBy = policy.effectiveSortBy;

      if (entry.projection)
      {
        if (auto* const trackListProj = dynamic_cast<TrackListProjection*>(entry.projection.get()))
        {
          trackListProj->setPresentation(policy.groupBy, policy.effectiveSortBy);
        }
      }
    }
  }

  struct ViewService::Impl final
  {
    std::uint64_t nextViewId = 1;
    std::unordered_map<ViewId, ViewEntry> views;

    IControlExecutor& executor;
    ao::library::MusicLibrary& library;
    ListSourceStore& sources;

    Impl(IControlExecutor& exec, ao::library::MusicLibrary& lib, ListSourceStore& src)
      : executor{exec}, library{lib}, sources{src}
    {
    }

    Signal<ViewId> destroyedSignal;
    Signal<TrackListProjectionChanged const&> projectionChangedSignal;
    Signal<ViewService::FilterChanged const&> filterChangedSignal;
    Signal<FilterStatusChanged const&> filterStatusChangedSignal;
    Signal<ViewService::SortChanged const&> sortChangedSignal;
    Signal<ViewService::GroupingChanged const&> groupingChangedSignal;
    Signal<ViewService::SelectionChanged const&> selectionChangedSignal;
    Signal<ViewService::ListChanged const&> listChangedSignal;
  };

  ViewService::ViewService(IControlExecutor& executor, ao::library::MusicLibrary& library, ListSourceStore& sources)
    : _impl{std::make_unique<Impl>(executor, library, sources)}
  {
  }

  ViewService::~ViewService() = default;

  Subscription ViewService::onDestroyed(std::move_only_function<void(ViewId)> handler)
  {
    return _impl->destroyedSignal.connect(std::move(handler));
  }

  Subscription ViewService::onProjectionChanged(
    std::move_only_function<void(TrackListProjectionChanged const&)> handler)
  {
    return _impl->projectionChangedSignal.connect(std::move(handler));
  }

  Subscription ViewService::onFilterChanged(std::move_only_function<void(FilterChanged const&)> handler)
  {
    return _impl->filterChangedSignal.connect(std::move(handler));
  }

  Subscription ViewService::onFilterStatusChanged(std::move_only_function<void(FilterStatusChanged const&)> handler)
  {
    return _impl->filterStatusChangedSignal.connect(std::move(handler));
  }

  Subscription ViewService::onSortChanged(std::move_only_function<void(SortChanged const&)> handler)
  {
    return _impl->sortChangedSignal.connect(std::move(handler));
  }

  Subscription ViewService::onGroupingChanged(std::move_only_function<void(GroupingChanged const&)> handler)
  {
    return _impl->groupingChangedSignal.connect(std::move(handler));
  }

  Subscription ViewService::onSelectionChanged(std::move_only_function<void(SelectionChanged const&)> handler)
  {
    return _impl->selectionChangedSignal.connect(std::move(handler));
  }

  Subscription ViewService::onListChanged(std::move_only_function<void(ListChanged const&)> handler)
  {
    return _impl->listChangedSignal.connect(std::move(handler));
  }

  CreateTrackListViewReply ViewService::createView(TrackListViewConfig const& initial, bool attached)
  {
    auto id = ViewId{_impl->nextViewId++};

    auto state = TrackListViewState{
      .id = id,
      .lifecycle = attached ? ViewLifecycleState::Attached : ViewLifecycleState::Detached,
      .listId = initial.listId,
      .filterExpression = initial.filterExpression,
      .groupBy = initial.groupBy,
      .sortBy = initial.sortBy,
      .selection = initial.selection,
    };

    auto* baseSource = &_impl->sources.sourceFor(initial.listId);
    std::unique_ptr<SmartListSource> adHocSource;

    if (!initial.filterExpression.empty())
    {
      adHocSource = std::make_unique<SmartListSource>(*baseSource, _impl->library, _impl->sources.smartEvaluator());
      adHocSource->setExpression(initial.filterExpression);
      adHocSource->reload();
      baseSource = adHocSource.get();
    }

    auto projection = std::make_shared<TrackListProjection>(id, *baseSource, _impl->library);

    auto& entry = _impl->views[id];
    entry.state = state;
    entry.adHocSource = std::move(adHocSource);
    entry.activeSource = baseSource;
    entry.projection = projection;

    applyPresentation(entry);

    return CreateTrackListViewReply{.viewId = id};
  }

  void ViewService::destroyView(ViewId viewId)
  {
    if (auto it = _impl->views.find(viewId); it != _impl->views.end())
    {
      it->second.state.lifecycle = ViewLifecycleState::Destroyed;
      _impl->destroyedSignal.post(_impl->executor, viewId);
    }
  }

  void ViewService::setFilter(ViewId viewId, std::string const& filterExpression)
  {
    auto const timer = ao::utility::ScopedTimer{"ViewService::setFilter"};
    auto it = _impl->views.find(viewId);
    if (it == _impl->views.end())
    {
      return;
    }

    it->second.state.filterExpression = filterExpression;
    it->second.state.revision++;
    _impl->filterChangedSignal.post(_impl->executor,
                                     ViewService::FilterChanged{.viewId = viewId, .filterExpression = filterExpression});

    if (it->second.adHocSource)
    {
      it->second.adHocSource->setExpression(filterExpression);
      it->second.adHocSource->reload();
    }
    else if (!filterExpression.empty())
    {
      // If we didn't have an adHocSource but now we need one
      auto* baseSource = &_impl->sources.sourceFor(it->second.state.listId);
      it->second.adHocSource =
        std::make_unique<SmartListSource>(*baseSource, _impl->library, _impl->sources.smartEvaluator());
      it->second.adHocSource->setExpression(filterExpression);
      it->second.adHocSource->reload();
      it->second.activeSource = it->second.adHocSource.get();

      // Need to attach projection to new source
      it->second.projection = std::make_shared<TrackListProjection>(viewId, *it->second.activeSource, _impl->library);
      applyPresentation(it->second);
      auto ev = TrackListProjectionChanged{
      .viewId = viewId, .projection = it->second.projection, .revision = it->second.state.revision};
    _impl->projectionChangedSignal.post(_impl->executor, std::move(ev));
    }
    else
    {
      // Switching from adHocSource to no filter
      auto* baseSource = &_impl->sources.sourceFor(it->second.state.listId);
      it->second.adHocSource.reset();
      it->second.activeSource = baseSource;
      it->second.projection = std::make_shared<TrackListProjection>(viewId, *it->second.activeSource, _impl->library);
      applyPresentation(it->second);
      auto ev = TrackListProjectionChanged{
      .viewId = viewId, .projection = it->second.projection, .revision = it->second.state.revision};
    _impl->projectionChangedSignal.post(_impl->executor, std::move(ev));
    }

    auto status = FilterStatusChanged{};
    status.viewId = viewId;
    status.expression = filterExpression;
    status.revision = it->second.state.revision;

    if (it->second.adHocSource != nullptr)
    {
      status.hasError = it->second.adHocSource->hasError();
      status.errorMessage = it->second.adHocSource->errorMessage();
    }

    _impl->filterStatusChangedSignal.emit(status);
  }

  void ViewService::setSort(ViewId viewId, std::vector<TrackSortTerm> const& sortBy)
  {
    auto it = _impl->views.find(viewId);
    if (it == _impl->views.end())
    {
      return;
    }

    it->second.state.sortBy = sortBy;
    it->second.state.revision++;
    _impl->sortChangedSignal.post(_impl->executor, ViewService::SortChanged{.viewId = viewId, .sortBy = sortBy});

    if (it->second.projection)
    {
      if (auto* const trackListProj = dynamic_cast<TrackListProjection*>(it->second.projection.get()))
      {
        trackListProj->setPresentation(it->second.state.groupBy, sortBy);
      }
    }
  }

  void ViewService::setGrouping(ViewId viewId, TrackGroupKey groupBy)
  {
    auto it = _impl->views.find(viewId);
    if (it == _impl->views.end())
    {
      return;
    }

    if (it->second.state.groupBy == groupBy)
    {
      return;
    }

    it->second.state.groupBy = groupBy;
    it->second.state.revision++;
    applyPresentation(it->second);
    _impl->groupingChangedSignal.post(_impl->executor,
                                       ViewService::GroupingChanged{.viewId = viewId, .groupBy = groupBy});
  }

  void ViewService::setSelection(ViewId viewId, std::vector<ao::TrackId> const& selection)
  {
    auto it = _impl->views.find(viewId);
    if (it == _impl->views.end())
    {
      return;
    }

    it->second.state.selection = selection;
    it->second.state.revision++;

    _impl->selectionChangedSignal.emit(ViewService::SelectionChanged{.viewId = viewId, .selection = selection});
  }

  void ViewService::openListInView(ViewId viewId, ao::ListId listId)
  {
    auto it = _impl->views.find(viewId);
    if (it == _impl->views.end())
    {
      return;
    }

    it->second.state.listId = listId;
    it->second.state.revision++;

    auto* baseSource = &_impl->sources.sourceFor(listId);

    if (it->second.adHocSource)
    {
      // adHocSource holds a reference to the old baseSource. We must recreate it.
      it->second.adHocSource =
        std::make_unique<SmartListSource>(*baseSource, _impl->library, _impl->sources.smartEvaluator());
      it->second.adHocSource->setExpression(it->second.state.filterExpression);
      it->second.adHocSource->reload();
      it->second.activeSource = it->second.adHocSource.get();
    }
    else
    {
      it->second.activeSource = baseSource;
    }

    it->second.projection = std::make_shared<TrackListProjection>(viewId, *it->second.activeSource, _impl->library);
    applyPresentation(it->second);
    _impl->projectionChangedSignal.post(_impl->executor,
      TrackListProjectionChanged{
        .viewId = viewId, .projection = it->second.projection, .revision = it->second.state.revision});

    _impl->listChangedSignal.post(_impl->executor, ViewService::ListChanged{.viewId = viewId, .listId = listId});
  }

  std::vector<ViewRecord> ViewService::listViews() const
  {
    return _impl->views |
           std::views::filter([](auto const& kv)
                              { return kv.second.state.lifecycle != ViewLifecycleState::Destroyed; }) |
           std::views::transform(
             [](auto const& kv) -> ViewRecord
             {
               return ViewRecord{
                 .id = kv.first,
                 .kind = ViewKind::TrackList,
                 .lifecycle = kv.second.state.lifecycle,
               };
             }) |
           std::ranges::to<std::vector>();
  }

  TrackListViewState ViewService::trackListState(ViewId viewId) const
  {
    return _impl->views.at(viewId).state;
  }

  std::shared_ptr<ITrackListProjection> ViewService::trackListProjection(ViewId viewId)
  {
    return _impl->views.at(viewId).projection;
  }

  std::shared_ptr<ITrackDetailProjection> ViewService::detailProjection(DetailTarget const& target,
                                                                         WorkspaceService& workspace,
                                                                         LibraryMutationService& mutation)
  {
    return std::make_shared<TrackDetailProjection>(target, *this, _impl->library, workspace, mutation);
  }
}
