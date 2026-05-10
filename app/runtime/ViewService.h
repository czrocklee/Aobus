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
namespace ao::app
{
  class EventBus;
  class ListSourceStore;
  class TrackSource;
}

namespace ao::app
{
  class EventBus;

  class ViewService final
  {
  public:
    ViewService(ao::library::MusicLibrary& library, ListSourceStore& sources, EventBus& events);
    ~ViewService();

    ViewService(ViewService const&) = delete;
    ViewService& operator=(ViewService const&) = delete;
    ViewService(ViewService&&) = delete;
    ViewService& operator=(ViewService&&) = delete;

    CreateTrackListViewReply createView(TrackListViewConfig const& initial, bool attached = true);
    void destroyView(ViewId viewId);
    void setFilter(ViewId viewId, std::string const& filterExpression);
    void setSort(ViewId viewId, std::vector<TrackSortTerm> const& sortBy);
    void setGrouping(ViewId viewId, TrackGroupKey groupBy);
    void setSelection(ViewId viewId, std::vector<ao::TrackId> const& selection);
    void openListInView(ViewId viewId, ao::ListId listId);

    std::vector<ViewRecord> listViews() const;

    TrackListViewState trackListState(ViewId viewId) const;
    std::shared_ptr<ITrackListProjection> trackListProjection(ViewId viewId);
    std::shared_ptr<ITrackDetailProjection> detailProjection(DetailTarget const& target);

  private:
    struct Impl;
    std::unique_ptr<Impl> _impl;
  };
}
