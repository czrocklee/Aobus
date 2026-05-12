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
namespace ao::rt
{
  class ListSourceStore;
  class TrackSource;
  class WorkspaceService;
  class LibraryMutationService;

  class ViewService final
  {
  public:
    struct FilterChanged final
    {
      ViewId viewId{};
      std::string filterExpression{};
    };

    struct SortChanged final
    {
      ViewId viewId{};
      std::vector<TrackSortTerm> sortBy{};
    };

    struct GroupingChanged final
    {
      ViewId viewId{};
      TrackGroupKey groupBy = TrackGroupKey::None;
    };

    struct SelectionChanged final
    {
      ViewId viewId{};
      std::vector<ao::TrackId> selection{};
    };

    struct ListChanged final
    {
      ViewId viewId{};
      ao::ListId listId{};
    };

    ViewService(ao::library::MusicLibrary& library, ListSourceStore& sources);
    ~ViewService();

    ViewService(ViewService const&) = delete;
    ViewService& operator=(ViewService const&) = delete;
    ViewService(ViewService&&) = delete;
    ViewService& operator=(ViewService&&) = delete;

    void setWorkspaceService(WorkspaceService& workspace);
    void setLibraryMutationService(LibraryMutationService& mutation);

    CreateTrackListViewReply createView(TrackListViewConfig const& initial, bool attached = true);
    void destroyView(ViewId viewId);
    void setFilter(ViewId viewId, std::string const& filterExpression);
    void setSort(ViewId viewId, std::vector<TrackSortTerm> const& sortBy);
    void setGrouping(ViewId viewId, TrackGroupKey groupBy);
    void setSelection(ViewId viewId, std::vector<ao::TrackId> const& selection);
    void openListInView(ViewId viewId, ao::ListId listId);

    Subscription onDestroyed(std::move_only_function<void(ViewId)> handler);
    Subscription onProjectionChanged(std::move_only_function<void(TrackListProjectionChanged const&)> handler);
    Subscription onFilterChanged(std::move_only_function<void(FilterChanged const&)> handler);
    Subscription onFilterStatusChanged(std::move_only_function<void(FilterStatusChanged const&)> handler);
    Subscription onSortChanged(std::move_only_function<void(SortChanged const&)> handler);
    Subscription onGroupingChanged(std::move_only_function<void(GroupingChanged const&)> handler);
    Subscription onSelectionChanged(std::move_only_function<void(SelectionChanged const&)> handler);
    Subscription onListChanged(std::move_only_function<void(ListChanged const&)> handler);

    std::vector<ViewRecord> listViews() const;

    TrackListViewState trackListState(ViewId viewId) const;
    std::shared_ptr<ITrackListProjection> trackListProjection(ViewId viewId);
    std::shared_ptr<ITrackDetailProjection> detailProjection(DetailTarget const& target);

  private:
    struct Impl;
    std::unique_ptr<Impl> _impl;
  };
}
