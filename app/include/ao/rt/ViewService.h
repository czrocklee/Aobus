// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#pragma once

#include "PlaybackLaunchSpec.h"
#include "TrackPresentation.h"
#include "ViewIds.h"
#include "ViewState.h"
#include "projection/TrackDetailProjection.h"
#include "projection/TrackListProjection.h"
#include "source/TrackSourceLease.h"
#include <ao/CoreIds.h>
#include <ao/Error.h>
#include <ao/async/Subscription.h>
#include <ao/compat/MoveOnlyFunction.h>

#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
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
  class TextOrderingPolicy;

  class TrackSourceCache;
  class TrackSource;
  enum class TrackSourceState : std::uint8_t;
  class WorkspaceService;
  class LibraryChanges;

  struct TrackListProjectionChanged final
  {
    ViewId viewId{};
    std::shared_ptr<TrackListProjection const> projectionPtr{};
  };

  class ViewService final
  {
  public:
    struct SelectionChanged final
    {
      ViewId viewId{};
      std::vector<TrackId> selection{};
    };

    struct PresentationChanged final
    {
      ViewId viewId{};
      TrackPresentationSpec presentation{};
    };

    struct ViewDestroyed final
    {
      ViewId viewId{};
    };

    struct FilterErrorChanged final
    {
      ViewId viewId{};
      std::optional<Error> optFilterError{};
    };

    ViewService(async::Executor& executor,
                library::MusicLibrary const& library,
                TrackSourceCache& sources,
                LibraryChanges const& changes,
                TextOrderingPolicy const* textOrderingPolicy = nullptr);
    ~ViewService();

    ViewService(ViewService const&) = delete;
    ViewService& operator=(ViewService const&) = delete;
    ViewService(ViewService&&) = delete;
    ViewService& operator=(ViewService&&) = delete;

    Result<> setFilter(ViewId viewId, std::string filterExpression);
    Result<> setPresentation(ViewId viewId, TrackPresentationSpec const& presentation);
    Result<> setSelection(ViewId viewId, std::vector<TrackId> selection);
    Result<PlaybackLaunchSpec> capturePlaybackLaunchSpec(ViewId viewId) const;

    async::Subscription onProjectionChanged(compat::MoveOnlyFunction<void(TrackListProjectionChanged const&)> handler);
    async::Subscription onPresentationChanged(compat::MoveOnlyFunction<void(PresentationChanged const&)> handler);
    async::Subscription onSelectionChanged(compat::MoveOnlyFunction<void(SelectionChanged const&)> handler);
    async::Subscription onViewDestroyed(compat::MoveOnlyFunction<void(ViewDestroyed const&)> handler);
    async::Subscription onFilterErrorChanged(compat::MoveOnlyFunction<void(FilterErrorChanged const&)> handler);

    // View lookups come in two forms. The precondition form assumes the caller
    // already holds a live view id, so an unknown id is a programming error and
    // throws std::out_of_range. The find form reports a missing view through the
    // same NotFound error as every other fallible method here.
    //
    // Observers must use the find form: a queued notification can outlive the
    // view it names. A missing view is expected stale-notification state rather
    // than an exception for the owning Signal boundary to diagnose as fatal.
    TrackListViewState trackListState(ViewId viewId) const;
    Result<TrackListViewState> findTrackListState(ViewId viewId) const;

    // Lightweight per-frame lookup avoiding a full TrackListViewState copy
    // (filter/sort/selection) on the render path. Returns nullptr for an unknown
    // view. The pointer remains valid until the view is destroyed.
    TrackPresentationSpec const* findTrackListPresentation(ViewId viewId) const noexcept;

    // Total playback duration of the view's current selection. Returns 0 for an unknown view,
    // an empty selection, or selected ids missing from the library.
    std::chrono::milliseconds selectionDuration(ViewId viewId) const;

    Result<std::shared_ptr<TrackListProjection const>> findTrackListProjection(ViewId viewId) const;
    std::unique_ptr<TrackListProjection> createTransientTrackListProjection(TrackSourceLease sourceLease,
                                                                            TrackOrderSpec const& order = {}) const;
    Result<TrackSourceState> listSourceState(ViewId viewId) const;
    Result<std::vector<TrackId>> listSourceTrackIds(ViewId viewId) const;

  private:
    friend class WorkspaceService;

    Result<ViewId> createView(TrackListViewConfig const& initial);
    void destroyView(ViewId viewId);
    std::vector<ViewId> listViews() const;
    std::unique_ptr<TrackDetailProjection> detailProjection(DetailTarget const& target,
                                                            WorkspaceService& workspace,
                                                            LibraryChanges const& changes);

    struct Impl;
    std::unique_ptr<Impl> _implPtr;
  };
} // namespace ao::rt
