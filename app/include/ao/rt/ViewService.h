// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#pragma once

#include "PlaybackLaunchContext.h"
#include "Subscription.h"
#include "TrackMutation.h"
#include "TrackPresentation.h"
#include "ViewIds.h"
#include "ViewState.h"
#include "projection/TrackDetailProjection.h"
#include "projection/TrackListProjection.h"
#include <ao/CoreIds.h>
#include <ao/Error.h>

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace ao::library
{
  class MusicLibrary;
}

namespace ao::async
{
  class Executor;
}

namespace ao::rt
{
  class TrackSourceCache;
  class TrackSource;
  class WorkspaceService;
  class LibraryChanges;

  struct FilterStatusChanged final
  {
    ViewId viewId{};
    std::string expression{};
    bool pending = false;
    std::optional<Error> optError = std::nullopt;
    std::uint64_t revision = 0;
  };

  struct TrackListProjectionChanged final
  {
    ViewId viewId{};
    std::shared_ptr<TrackListProjection> projectionPtr{};
    std::uint64_t revision = 0;
  };

  class ViewService final
  {
  public:
    struct FilterChanged final
    {
      ViewId viewId{};
      std::string filterExpression{};
    };

    struct SelectionChanged final
    {
      ViewId viewId{};
      std::vector<TrackId> selection{};
    };

    struct ListChanged final
    {
      ViewId viewId{};
      ListId listId{};
    };

    struct PresentationChanged final
    {
      ViewId viewId{};
      TrackPresentationSpec presentation{};
    };

    ViewService(async::Executor& executor, library::MusicLibrary& library, TrackSourceCache& sources);
    ~ViewService();

    ViewService(ViewService const&) = delete;
    ViewService& operator=(ViewService const&) = delete;
    ViewService(ViewService&&) = delete;
    ViewService& operator=(ViewService&&) = delete;

    Result<CreateTrackListViewReply> createView(TrackListViewConfig const& initial, bool attached = true);
    void destroyView(ViewId viewId);
    Result<> setFilter(ViewId viewId, std::string filterExpression);
    void setPresentation(ViewId viewId, TrackPresentationSpec const& presentation);
    TrackPresentationSpec setPresentation(ViewId viewId, std::string_view presentationId);
    void setSelection(ViewId viewId, std::vector<TrackId> selection);
    Result<> openListInView(ViewId viewId, ListId listId);
    Result<PlaybackLaunchContext> capturePlaybackLaunchContext(ViewId viewId) const;

    Subscription onDestroyed(std::move_only_function<void(ViewId)> handler);
    Subscription onProjectionChanged(std::move_only_function<void(TrackListProjectionChanged const&)> handler);
    Subscription onFilterChanged(std::move_only_function<void(FilterChanged const&)> handler);
    Subscription onFilterStatusChanged(std::move_only_function<void(FilterStatusChanged const&)> handler);
    Subscription onPresentationChanged(std::move_only_function<void(PresentationChanged const&)> handler);
    Subscription onSelectionChanged(std::move_only_function<void(SelectionChanged const&)> handler);
    Subscription onListChanged(std::move_only_function<void(ListChanged const&)> handler);

    std::vector<ViewRecord> listViews() const;

    TrackListViewState trackListState(ViewId viewId) const;

    // Total playback duration of the view's current selection. Returns 0 for an unknown view,
    // an empty selection, or selected ids missing from the library.
    std::chrono::milliseconds selectionDuration(ViewId viewId) const;

    std::shared_ptr<TrackListProjection> trackListProjection(ViewId viewId);
    std::unique_ptr<TrackDetailProjection> detailProjection(DetailTarget const& target,
                                                            WorkspaceService& workspace,
                                                            LibraryChanges const& changes);

  private:
    struct Impl;
    std::unique_ptr<Impl> _implPtr;
  };
} // namespace ao::rt
