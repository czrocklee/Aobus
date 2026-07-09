// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#pragma once

#include "../Subscription.h"
#include "../TrackPresentation.h"
#include "../ViewIds.h"
#include "../source/TrackSource.h"
#include "TrackListProjection.h"
#include <ao/CoreIds.h>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>

namespace ao::library
{
  class MusicLibrary;
}

namespace ao::rt
{
  class SmartListSource;

  class LiveTrackListProjection final
    : public TrackListProjection
    , private TrackSourceObserver
  {
  public:
    LiveTrackListProjection(ViewId viewId, TrackSource& source, library::MusicLibrary& library);
    ~LiveTrackListProjection() override;

    LiveTrackListProjection(LiveTrackListProjection const&) = delete;
    LiveTrackListProjection& operator=(LiveTrackListProjection const&) = delete;
    LiveTrackListProjection(LiveTrackListProjection&&) = delete;
    LiveTrackListProjection& operator=(LiveTrackListProjection&&) = delete;

    ViewId viewId() const noexcept override;
    std::uint64_t revision() const noexcept override;

    TrackPresentationSpec presentation() const override;
    std::size_t groupCount() const noexcept override;
    TrackGroupSectionSnapshot groupAt(std::size_t groupIndex) const override;
    std::optional<std::size_t> groupIndexAt(std::size_t rowIndex) const override;

    std::size_t size() const noexcept override;
    TrackId trackIdAt(std::size_t index) const override;
    std::optional<std::size_t> indexOf(TrackId trackId) const noexcept override;

    void setPresentation(TrackPresentationSpec const& presentation);

    Subscription subscribe(std::move_only_function<void(TrackListProjectionDeltaBatch const&)> handler) override;

  private:
    void handleReset() override;
    void handleInserted(TrackId id, std::size_t index) override;
    void handleUpdated(TrackId id, std::size_t index) override;
    void handleRemoved(TrackId id, std::size_t index) override;

    void handleBulkInserted(std::span<TrackId const> ids) override;
    void handleBulkUpdated(std::span<TrackId const> ids) override;
    void handleBulkRemoved(std::span<TrackId const> ids) override;

    void handleSourceDestroyed() override;

    void publishDelta(TrackListProjectionDeltaBatch const& batch);

    struct Impl;
    std::unique_ptr<Impl> _implPtr;
  };
} // namespace ao::rt
