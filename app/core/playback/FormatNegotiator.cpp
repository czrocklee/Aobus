// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#include "core/playback/FormatNegotiator.h"

#include <algorithm>

namespace app::core::playback
{

  RenderPlan FormatNegotiator::buildPlan(StreamFormat sourceFormat, DeviceCapabilities const& caps)
  {
    RenderPlan plan;
    plan.sourceFormat = sourceFormat;

    // Default to source format as device format
    plan.deviceFormat = sourceFormat;

    // Check if device supports the source sample rate
    auto const sampleRateSupported =
      std::find(caps.sampleRates.begin(), caps.sampleRates.end(), sourceFormat.sampleRate) != caps.sampleRates.end();

    if (!sampleRateSupported && !caps.sampleRates.empty())
    {
      // Use the highest supported sample rate close to the source
      auto const bestRate = *std::max_element(caps.sampleRates.begin(),
                                              caps.sampleRates.end(),
                                              [sourceRate = sourceFormat.sampleRate](std::uint32_t a, std::uint32_t b)
                                              {
                                                // Prefer rates that are multiples of source rate or close to it
                                                auto const aDiv = (a > sourceRate) ? a : sourceRate;
                                                auto const bDiv = (b > sourceRate) ? b : sourceRate;
                                                auto const aMod = aDiv % sourceRate;
                                                auto const bMod = bDiv % sourceRate;
                                                return (aMod > bMod);
                                              });
      plan.deviceFormat.sampleRate = bestRate;
      plan.requiresResample = true;
      plan.reason = "Sample rate not supported, resampling required";
    }

    // Check bit depth support
    auto const bitDepthSupported =
      std::find(caps.bitDepths.begin(), caps.bitDepths.end(), sourceFormat.bitDepth) != caps.bitDepths.end();

    if (!bitDepthSupported && !caps.bitDepths.empty())
    {
      // Use highest supported bit depth
      plan.deviceFormat.bitDepth = *std::max_element(caps.bitDepths.begin(), caps.bitDepths.end());
      plan.requiresBitDepthConversion = true;
      plan.reason = plan.reason.empty() ? "Bit depth not supported, conversion required"
                                        : plan.reason + "; bit depth conversion required";
    }

    // Check channel count support
    auto const channelsSupported =
      std::find(caps.channelCounts.begin(), caps.channelCounts.end(), sourceFormat.channels) !=
      caps.channelCounts.end();

    if (!channelsSupported && !caps.channelCounts.empty())
    {
      // Use first supported channel count (usually stereo 2)
      plan.deviceFormat.channels = caps.channelCounts.front();
      plan.requiresChannelRemap = true;
      plan.reason = plan.reason.empty() ? "Channel count not supported, remapping required"
                                        : plan.reason + "; channel remapping required";
    }

    // Decoder output format is always S16 interleaved for now
    plan.decoderOutputFormat = plan.deviceFormat;
    plan.decoderOutputFormat.bitDepth = 16;
    plan.decoderOutputFormat.isInterleaved = true;
    plan.decoderOutputFormat.isFloat = false;

    if (plan.reason.empty())
    {
      plan.reason = "Direct passthrough - no conversion needed";
    }

    return plan;
  }

} // namespace app::core::playback