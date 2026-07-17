// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "PlaybackActions.h"

#include "SelectionNavigation.h"
#include "TrackListEntry.h"
#include <ao/rt/PlaybackSequenceService.h>
#include <ao/rt/ViewIds.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace ao::tui
{
  bool playSelected(rt::PlaybackSequenceService& sequence,
                    std::vector<TrackListEntry> const& tracks,
                    std::int32_t const selected,
                    rt::ViewId const sourceViewId)
  {
    if (tracks.empty())
    {
      return false;
    }

    auto const index = clampSelection(static_cast<std::size_t>(std::max(0, selected)), tracks.size());
    return static_cast<bool>(sequence.playFromView(sourceViewId, tracks[index].id));
  }
} // namespace ao::tui
