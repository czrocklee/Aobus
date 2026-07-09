// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/audio/Device.h>
#include <ao/audio/Format.h>

#include <algorithm>

namespace ao::audio::backend::detail
{
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

  inline Format preserveRequestedSignalPrecision(Format const& requested, Format current) noexcept
  {
    if (requested.isFloat || current.isFloat || requested.sampleRate != current.sampleRate ||
        requested.channels != current.channels)
    {
      return current;
    }

    if (auto const requestedBits = effectiveBits(requested);
        requestedBits <= effectiveBits(current) && requestedBits <= current.bitDepth)
    {
      current.validBits = requestedBits;
    }

    return current;
  }
} // namespace ao::audio::backend::detail
