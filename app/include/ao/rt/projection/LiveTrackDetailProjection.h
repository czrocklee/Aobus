// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#pragma once

#include "../Subscription.h"
#include "TrackDetailProjection.h"
#include <ao/CoreIds.h>

#include <functional>
#include <memory>
#include <span>

namespace ao::rt
{
  class ViewService;
  class WorkspaceService;
  class LibraryChanges;
}

namespace ao::library
{
  class MusicLibrary;
}

namespace ao::rt
{
  class LiveTrackDetailProjection final : public TrackDetailProjection
  {
  public:
    LiveTrackDetailProjection(DetailTarget target,
                              ViewService& views,
                              library::MusicLibrary& library,
                              WorkspaceService& workspace,
                              LibraryChanges const& changes);
    ~LiveTrackDetailProjection() override;

    LiveTrackDetailProjection(LiveTrackDetailProjection const&) = delete;
    LiveTrackDetailProjection& operator=(LiveTrackDetailProjection const&) = delete;
    LiveTrackDetailProjection(LiveTrackDetailProjection&&) = delete;
    LiveTrackDetailProjection& operator=(LiveTrackDetailProjection&&) = delete;

    TrackDetailSnapshot snapshot() const override;
    Subscription subscribe(std::move_only_function<void(TrackDetailSnapshot const&)> handler) override;

  private:
    void onSelectionChanged();
    TrackDetailSnapshot buildSnapshot(std::span<TrackId const> ids) const;
    void publishSnapshot();

    struct Impl;
    std::unique_ptr<Impl> _implPtr;
  };
} // namespace ao::rt
