// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include "../PlaybackLaunchContext.h"
#include "../Subscription.h"
#include "../TrackPresentation.h"
#include "../ViewIds.h"
#include "../source/TrackSourceLease.h"
#include "TrackListProjection.h"
#include <ao/CoreIds.h>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>

namespace ao::library
{
  class MusicLibrary;
}

namespace ao::rt
{
  struct TrackListProjectionOperationCounts final
  {
    std::uint64_t fullProjectionRebuilds = 0;
    std::uint64_t incrementalProjectionUpdates = 0;
    std::uint64_t arenaRebases = 0;
    std::uint64_t rowIndexRebuilds = 0;

    bool operator==(TrackListProjectionOperationCounts const&) const = default;
  };

  class LiveTrackListProjection final : public TrackListProjection
  {
  public:
    LiveTrackListProjection(ViewId viewId, TrackSourceLease sourceLease, library::MusicLibrary& library);
    LiveTrackListProjection(ViewId viewId,
                            TrackSourceLease sourceLease,
                            library::MusicLibrary& library,
                            TrackOrderSpec const& order);
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

    TrackListProjectionOperationCounts operationCounts() const noexcept;

    void setPresentation(TrackPresentationSpec const& presentation);

    Subscription subscribe(std::move_only_function<void(TrackListProjectionDeltaBatch const&)> handler) override;

  private:
    struct Impl;
    std::unique_ptr<Impl> _implPtr;
  };
} // namespace ao::rt
