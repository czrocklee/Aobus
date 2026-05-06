// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#pragma once

#include "CorePrimitives.h"
#include "ProjectionTypes.h"
#include "StateTypes.h"

#include <memory>
#include <vector>

namespace ao::app
{
  class EventBus;
  class ViewService;
}

namespace ao::library
{
  class MusicLibrary;
}

namespace ao::app
{
  class TrackDetailProjection final : public ITrackDetailProjection
  {
  public:
    TrackDetailProjection(DetailTarget target,
                          ViewService& views,
                          EventBus& events,
                          ao::library::MusicLibrary& library);
    ~TrackDetailProjection() override;

    TrackDetailProjection(TrackDetailProjection const&) = delete;
    TrackDetailProjection& operator=(TrackDetailProjection const&) = delete;
    TrackDetailProjection(TrackDetailProjection&&) = delete;
    TrackDetailProjection& operator=(TrackDetailProjection&&) = delete;

    TrackDetailSnapshot snapshot() const override;
    Subscription subscribe(std::move_only_function<void(TrackDetailSnapshot const&)> handler) override;

  private:
    void onSelectionChanged();
    TrackDetailSnapshot buildSnapshot(std::span<ao::TrackId const> ids) const;
    void publishSnapshot();

    struct Impl;
    std::unique_ptr<Impl> _impl;
  };
}
