// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#include "core/playback/FormatNegotiator.h"

#include <algorithm>
#include <ranges>

namespace app::core::playback
{

  namespace
  {
    template<typename T, typename Collection, typename Selector>
    void negotiate(T& target,
                   Collection const& supported,
                   bool& flag,
                   std::string& reason,
                   char const* msg,
                   Selector&& selector)
    {
      if (supported.empty() || std::ranges::find(supported, target) != supported.end()) return;

      target = selector();
      flag = true;

      if (!reason.empty()) reason += "; ";

      reason += msg;
    }
  }

  RenderPlan FormatNegotiator::buildPlan(AudioFormat sourceFormat, DeviceCapabilities const& caps)
  {
    RenderPlan plan = {
      .sourceFormat = sourceFormat,
      .deviceFormat = sourceFormat,
      .decoderOutputFormat = {},
      .reason = {},
    };

    negotiate(plan.deviceFormat.sampleRate,
              caps.sampleRates,
              plan.requiresResample,
              plan.reason,
              "sample rate resampling required",
              [&]
              {
                return *std::ranges::max_element(caps.sampleRates,
                                                 [sr = sourceFormat.sampleRate](auto a, auto b)
                                                 {
                                                   return (a % sr) > (b % sr); // Prefer multiples or closest
                                                 });
              });

    negotiate(plan.deviceFormat.bitDepth,
              caps.bitDepths,
              plan.requiresBitDepthConversion,
              plan.reason,
              "bit depth conversion required",
              [&] { return *std::ranges::max_element(caps.bitDepths); });

    negotiate(plan.deviceFormat.channels,
              caps.channelCounts,
              plan.requiresChannelRemap,
              plan.reason,
              "channel remapping required",
              [&] { return caps.channelCounts.front(); });

    plan.decoderOutputFormat = plan.deviceFormat;
    plan.decoderOutputFormat.bitDepth = 16;
    plan.decoderOutputFormat.isInterleaved = true;
    plan.decoderOutputFormat.isFloat = false;

    if (plan.reason.empty()) plan.reason = "Direct passthrough";

    return plan;
  }

} // namespace app::core::playback