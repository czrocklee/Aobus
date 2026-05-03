// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#pragma once

#include <algorithm>
#include <ao/audio/Backend.h>

namespace ao::audio::backend::detail
{
  /**
   * @brief Shared helper to add a sample format capability to DeviceCapabilities.
   */
  inline void addSampleFormatCapability(ao::audio::DeviceCapabilities& caps,
                                        ao::audio::SampleFormatCapability const& capability)
  {
    if (!std::ranges::contains(caps.sampleFormats, capability))
    {
      caps.sampleFormats.push_back(capability);
    }

    if (!capability.isFloat && capability.bitDepth == capability.validBits &&
        !std::ranges::contains(caps.bitDepths, capability.bitDepth))
    {
      caps.bitDepths.push_back(capability.bitDepth);
    }
  }
} // namespace ao::audio::backend::detail
