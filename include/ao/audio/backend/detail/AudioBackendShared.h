// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#pragma once

#include <ao/audio/Backend.h>

#include <algorithm>

namespace ao::audio::backend::detail
{
  constexpr float kVolumeEpsilon = 1e-4F;

  /**
   * @brief Shared helper to add a sample format capability to DeviceCapabilities.
   */
  inline void addSampleFormatCapability(DeviceCapabilities& caps, SampleFormatCapability const& capability)
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
