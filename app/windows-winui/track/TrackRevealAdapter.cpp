// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include <ao/winui/track/TrackRevealAdapter.h>

#include <ao/CoreIds.h>
#include <ao/rt/ViewIds.h>
#include <ao/uimodel/library/track/TrackDisplayIndex.h>

#include <cstddef>
#include <optional>

namespace ao::winui
{
  void recordTrackRevealIntent(TrackRevealIntent& intent, rt::ViewId const viewId, TrackId const trackId) noexcept
  {
    intent.viewId = viewId;
    intent.trackId = trackId;
    ++intent.serial;
  }

  std::optional<TrackRevealTarget> resolveTrackRevealTarget(TrackRevealIntent const& intent,
                                                            rt::ViewId const activeViewId,
                                                            std::optional<std::size_t> const optSourceIndex,
                                                            uimodel::TrackDisplayIndex const& displayIndex) noexcept
  {
    if (activeViewId != intent.viewId || intent.trackId == kInvalidTrackId || !optSourceIndex)
    {
      return std::nullopt;
    }

    auto const optDisplayIndex = displayIndex.displayIndexOfSourceRow(*optSourceIndex);
    return optDisplayIndex ? std::optional{TrackRevealTarget{
                               .serial = intent.serial,
                               .displayIndex = *optDisplayIndex,
                             }}
                           : std::nullopt;
  }
} // namespace ao::winui
