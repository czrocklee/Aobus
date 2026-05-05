// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include "ViewRegistry.h"
#include "CommandBus.h"
#include "CommandTypes.h"
#include "EventBus.h"
#include "EventTypes.h"
#include "ObservableStore.h"
#include "TrackListProjection.h"

#include <ao/library/MusicLibrary.h>
#include <ao/model/FilteredTrackIdList.h>
#include <ao/model/SmartListEngine.h>
#include <ao/model/TrackIdList.h>

#include <memory>
#include <ranges>
#include <unordered_map>

namespace ao::app
{
  namespace
  {
    struct ViewEntry final
    {
      ObservableStore<TrackListViewState> state;
      std::shared_ptr<ITrackListProjection> projection;
      std::shared_ptr<ao::model::FilteredTrackIdList> filteredList;
    };
  }

  struct ViewRegistry::Impl final
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

  ViewRegistry::ViewRegistry(ao::library::MusicLibrary& library,
                             ao::model::SmartListEngine& engine,
                             ao::model::TrackIdList& allTracksSource,
                             EventBus& events)
    : _impl{std::make_unique<Impl>(library, engine, allTracksSource, events)}
  {
  }

  ViewRegistry::~ViewRegistry() = default;

  void ViewRegistry::registerCommandHandlers(CommandBus& commands)
  {
    commands.registerHandler<CreateTrackListView>(
      [this](CreateTrackListView const& cmd) -> ao::Result<CreateTrackListViewReply>
      {
        auto id = ViewId{_impl->nextViewId++};

        auto initial = TrackListViewState{
          .id = id,
          .lifecycle = cmd.attached ? ViewLifecycleState::Attached : ViewLifecycleState::Detached,
          .listId = cmd.initial.listId,
          .filterExpression = cmd.initial.filterExpression,
          .groupBy = cmd.initial.groupBy,
          .sortBy = cmd.initial.sortBy,
          .selection = cmd.initial.selection,
        };

        auto filtered =
          std::make_shared<ao::model::FilteredTrackIdList>(_impl->allTracksSource, _impl->library, _impl->engine);

        if (!cmd.initial.filterExpression.empty())
        {
          filtered->setExpression(cmd.initial.filterExpression);
        }

        _impl->engine.registerList(_impl->allTracksSource, *filtered);

        auto projection = std::make_shared<TrackListProjection>(id, *filtered, _impl->library);

        auto& entry = _impl->views[id];
        entry.state = ObservableStore<TrackListViewState>{initial};
        entry.projection = projection;
        entry.filteredList = filtered;

        return CreateTrackListViewReply{.viewId = id};
      });

    commands.registerHandler<DestroyView>(
      [this](DestroyView const& cmd) -> ao::Result<void>
      {
        auto it = _impl->views.find(cmd.viewId);
        if (it == _impl->views.end())
        {
          return {};
        }

        auto state = it->second.state.snapshot();
        state.lifecycle = ViewLifecycleState::Destroyed;
        it->second.state.update(std::move(state));

        _impl->events.publish(ViewDestroyed{.viewId = cmd.viewId});

        return {};
      });
  }

  std::vector<ViewRecord> ViewRegistry::listViews() const
  {
    return _impl->views |
           std::views::filter([](auto const& kv)
                              { return kv.second.state.snapshot().lifecycle != ViewLifecycleState::Destroyed; }) |
           std::views::transform(
             [](auto const& kv) -> ViewRecord
             {
               return ViewRecord{
                 .id = kv.first,
                 .kind = ViewKind::TrackList,
                 .lifecycle = kv.second.state.snapshot().lifecycle,
               };
             }) |
           std::ranges::to<std::vector>();
  }

  IReadOnlyStore<TrackListViewState>& ViewRegistry::trackListState(ViewId viewId)
  {
    return _impl->views.at(viewId).state;
  }

  std::shared_ptr<ITrackListProjection> ViewRegistry::trackListProjection(ViewId viewId)
  {
    return _impl->views.at(viewId).projection;
  }
}
