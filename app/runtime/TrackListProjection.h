// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#pragma once

#include "ProjectionTypes.h"
#include "StateTypes.h"

#include "TrackSource.h"
#include <ao/library/MusicLibrary.h>

#include <memory>
#include <span>
#include <vector>

namespace ao::library
{
  class MusicLibrary;
}
namespace ao::app
{
  class SmartListSource;
}

namespace ao::app
{
  class TrackListProjection final
    : public ITrackListProjection
    , private TrackSourceObserver
  {
  public:
    TrackListProjection(ViewId viewId, TrackSource& source, ao::library::MusicLibrary& library);
    ~TrackListProjection() override;

    TrackListProjection(TrackListProjection const&) = delete;
    TrackListProjection& operator=(TrackListProjection const&) = delete;
    TrackListProjection(TrackListProjection&&) = delete;
    TrackListProjection& operator=(TrackListProjection&&) = delete;

    ViewId viewId() const noexcept override;
    std::uint64_t revision() const noexcept override;

    void setSortBy(std::vector<TrackSortTerm> sortBy);

    std::size_t size() const noexcept override;
    ao::TrackId trackIdAt(std::size_t index) const override;

    Subscription subscribe(std::move_only_function<void(TrackListProjectionDeltaBatch const&)> handler) override;

  private:
    void onReset() override;
    void onInserted(ao::TrackId id, std::size_t index) override;
    void onUpdated(ao::TrackId id, std::size_t index) override;
    void onRemoved(ao::TrackId id, std::size_t index) override;

    void onInserted(std::span<ao::TrackId const> ids) override;
    void onUpdated(std::span<ao::TrackId const> ids) override;
    void onRemoved(std::span<ao::TrackId const> ids) override;

    void publishDelta(TrackListProjectionDeltaBatch const& batch);

    struct Impl;
    std::unique_ptr<Impl> _impl;
  };
}
