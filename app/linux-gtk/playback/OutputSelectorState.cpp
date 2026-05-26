// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "playback/OutputSelectorState.h"

#include <ao/audio/Backend.h>
#include <ao/rt/StateTypes.h>

#include <vector>

namespace ao::gtk
{
  std::vector<OutputSelectorRow> buildOutputSelectorRows(rt::PlaybackState const& state)
  {
    auto rows = std::vector<OutputSelectorRow>{};

    for (auto const& backend : state.availableOutputs)
    {
      rows.push_back(OutputSelectorRow{
        .kind = OutputSelectorRow::Kind::BackendHeader,
        .title = backend.name,
        .backendId = backend.id,
      });

      for (auto const& device : backend.devices)
      {
        for (auto const& profile : backend.supportedProfiles)
        {
          auto const isActive = (state.selectedOutput.backendId == backend.id
                                 && state.selectedOutput.deviceId == device.id
                                 && state.selectedOutput.profileId == profile.id);

          auto title = device.displayName;

          if (profile.id == audio::kProfileExclusive)
          {
            title += " [E]";
          }

          rows.push_back(OutputSelectorRow{
            .kind = OutputSelectorRow::Kind::DeviceProfile,
            .title = title,
            .backendId = backend.id,
            .deviceId = device.id,
            .profileId = profile.id,
            .active = isActive,
          });
        }
      }
    }

    return rows;
  }
} // namespace ao::gtk
