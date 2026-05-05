// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#pragma once

#include "CorePrimitives.h"
#include "ProjectionTypes.h"
#include "StateTypes.h"

#include <memory>
#include <vector>

namespace ao::library
{
  class MusicLibrary;
}
namespace ao::model
{
  class SmartListEngine;
  class TrackIdList;
}

namespace ao::app
{
  class CommandBus;
  class EventBus;

  class ViewRegistry final
  {
  public:
    ViewRegistry(ao::library::MusicLibrary& library,
                 ao::model::SmartListEngine& engine,
                 ao::model::TrackIdList& allTracksSource,
                 EventBus& events);
    ~ViewRegistry();

    ViewRegistry(ViewRegistry const&) = delete;
    ViewRegistry& operator=(ViewRegistry const&) = delete;
    ViewRegistry(ViewRegistry&&) = delete;
    ViewRegistry& operator=(ViewRegistry&&) = delete;

    void registerCommandHandlers(CommandBus& commands);

    std::vector<ViewRecord> listViews() const;

    IReadOnlyStore<TrackListViewState>& trackListState(ViewId viewId);
    std::shared_ptr<ITrackListProjection> trackListProjection(ViewId viewId);

  private:
    struct Impl;
    std::unique_ptr<Impl> _impl;
  };
}
