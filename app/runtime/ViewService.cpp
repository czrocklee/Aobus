// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include "ViewService.h"
#include "EventBus.h"
#include "EventTypes.h"
#include "TrackDetailProjection.h"
#include "TrackListProjection.h"

#include <ao/library/ListStore.h>
#include <ao/library/MusicLibrary.h>
#include <ao/model/FilteredTrackIdList.h>
#include <ao/model/ManualTrackIdList.h>
#include <ao/model/SmartListEngine.h>
#include <ao/model/TrackIdList.h>

#include <limits>
#include <memory>
#include <ranges>
#include <unordered_map>

namespace ao::app
{
  namespace
  {
    struct ViewEntry final
    {
      TrackListViewState state;
      std::shared_ptr<ao::model::TrackIdList> filteredList;
      std::shared_ptr<ITrackListProjection> projection;
    };
  }

  struct ViewService::Impl final
  {
    std::uint64_t nextViewId = 1;
    std::unordered_map<ViewId, ViewEntry> views;

    ao::library::MusicLibrary& library;
    ao::model::SmartListEngine& engine;
    ao::model::TrackIdList& allTracksSource;
    EventBus& events;

    Impl(ao::library::MusicLibrary& lib, ao::model::SmartListEngine& eng, ao::model::TrackIdList& source, EventBus& ev)
      : library{lib}, engine{eng}, allTracksSource{source}, events{ev}
    {
    }
  };

  ViewService::ViewService(ao::library::MusicLibrary& library,
                           ao::model::SmartListEngine& engine,
                           ao::model::TrackIdList& allTracksSource,
                           EventBus& events)
    : _impl{std::make_unique<Impl>(library, engine, allTracksSource, events)}
  {
  }

  ViewService::~ViewService() = default;

  CreateTrackListViewReply ViewService::createView(TrackListViewConfig const& initial,
                                                   bool attached,
                                                   std::shared_ptr<ao::model::TrackIdList> source)
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

    if (!source)
    {
      bool handled = false;
      if (initial.listId != ao::ListId{} && initial.listId != ao::ListId{std::numeric_limits<std::uint32_t>::max()})
      {
        auto txn = _impl->library.readTransaction();
        auto lists = _impl->library.lists().reader(txn);
        auto optView = lists.get(initial.listId);
        if (optView)
        {
          if (optView->isSmart())
          {
            auto filtered =
              std::make_shared<ao::model::FilteredTrackIdList>(_impl->allTracksSource, _impl->library, _impl->engine);
            filtered->setExpression(std::string(optView->filter()));
            filtered->reload();
            source = std::move(filtered);
          }
          else
          {
            source = std::make_shared<ao::model::ManualTrackIdList>(*optView, &_impl->allTracksSource);
          }
          handled = true;
        }
      }

      if (!handled)
      {
        auto filtered =
          std::make_shared<ao::model::FilteredTrackIdList>(_impl->allTracksSource, _impl->library, _impl->engine);
        if (!initial.filterExpression.empty())
        {
          filtered->setExpression(initial.filterExpression);
        }
        _impl->engine.registerList(_impl->allTracksSource, *filtered);
        source = std::move(filtered);
      }
    }

    auto projection = std::make_shared<TrackListProjection>(id, *source, _impl->library);

    auto& entry = _impl->views[id];
    entry.state = state;
    entry.filteredList = source;
    entry.projection = projection;

    return CreateTrackListViewReply{.viewId = id};
  }

  void ViewService::destroyView(ViewId viewId)
  {
    auto it = _impl->views.find(viewId);
    if (it == _impl->views.end())
    {
      return;
    }

    it->second.state.lifecycle = ViewLifecycleState::Destroyed;
    _impl->events.publish(ViewDestroyed{.viewId = viewId});
  }

  void ViewService::setFilter(ViewId viewId, std::string const& filterExpression)
  {
    auto it = _impl->views.find(viewId);
    if (it == _impl->views.end())
    {
      return;
    }

    it->second.state.filterExpression = filterExpression;
    it->second.state.revision++;
    _impl->events.publish(ViewFilterChanged{.viewId = viewId, .filterExpression = filterExpression});

    if (it->second.filteredList)
    {
      if (auto* fl = dynamic_cast<ao::model::FilteredTrackIdList*>(it->second.filteredList.get()))
      {
        fl->setExpression(filterExpression);
      }
    }
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
    _impl->events.publish(ViewSortChanged{.viewId = viewId, .sortBy = sortBy});

    if (it->second.projection)
    {
      if (auto* const trackListProj = dynamic_cast<TrackListProjection*>(it->second.projection.get()))
      {
        trackListProj->setSortBy(sortBy);
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

    it->second.state.groupBy = groupBy;
    it->second.state.revision++;
    _impl->events.publish(ViewGroupingChanged{.viewId = viewId, .groupBy = groupBy});
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

    _impl->events.publish(ViewSelectionChanged{.viewId = viewId, .selection = selection});
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
    _impl->events.publish(ViewListChanged{.viewId = viewId, .listId = listId});
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

  std::shared_ptr<ITrackDetailProjection> ViewService::detailProjection(DetailTarget const& target)
  {
    return std::make_shared<TrackDetailProjection>(target, *this, _impl->events, _impl->library);
  }
}
