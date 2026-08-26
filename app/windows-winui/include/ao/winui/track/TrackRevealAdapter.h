// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#pragma once

#include <ao/CoreIds.h>
#include <ao/rt/ViewIds.h>
#include <ao/uimodel/library/track/TrackDisplayIndex.h>

#include <cstddef>
#include <cstdint>
#include <optional>

namespace ao::winui
{
  struct TrackRevealIntent final
  {
    rt::ViewId viewId = rt::kInvalidViewId;
    TrackId trackId = kInvalidTrackId;
    std::uint64_t serial = 0;
  };

  struct TrackRevealTarget final
  {
    std::uint64_t serial = 0;
    std::size_t displayIndex = 0;

    bool operator==(TrackRevealTarget const&) const = default;
  };

  void recordTrackRevealIntent(TrackRevealIntent& intent, rt::ViewId viewId, TrackId trackId) noexcept;

  std::optional<TrackRevealTarget> resolveTrackRevealTarget(TrackRevealIntent const& intent,
                                                            rt::ViewId activeViewId,
                                                            std::optional<std::size_t> optSourceIndex,
                                                            uimodel::TrackDisplayIndex const& displayIndex) noexcept;
} // namespace ao::winui
