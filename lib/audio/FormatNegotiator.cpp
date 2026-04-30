// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#include <rs/audio/FormatNegotiator.h>

#include <algorithm>
#include <ranges>

namespace rs::audio
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
      if (supported.empty() || std::ranges::contains(supported, target))
      {
        return;
      }

      target = selector();
      flag = true;

      if (!reason.empty())
      {
        reason += "; ";
      }

      reason += msg;
    }
  }

  RenderPlan FormatNegotiator::buildPlan(AudioFormat sourceFormat, rs::audio::DeviceCapabilities const& caps)
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
                                                 [sr = sourceFormat.sampleRate](auto lhs, auto rhs)
                                                 {
                                                   return (lhs % sr) > (rhs % sr); // Prefer multiples or closest
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

    // --- Decoder Output Format Negotiation ---
    plan.decoderOutputFormat = plan.sourceFormat;
    plan.decoderOutputFormat.isInterleaved = true;
    plan.decoderOutputFormat.isFloat = false;

    // Determine best decoder output format based on device capabilities
    if (plan.sourceFormat.bitDepth == 24)
    {
      if (std::ranges::contains(caps.bitDepths, 32))
      {
        plan.decoderOutputFormat.bitDepth = 32;
        plan.decoderOutputFormat.validBits = 24;
      }
      else if (std::ranges::contains(caps.bitDepths, 24))
      {
        plan.decoderOutputFormat.bitDepth = 24;
        plan.decoderOutputFormat.validBits = 24;
      }
      else
      {
        plan.decoderOutputFormat.bitDepth = 16;
        plan.decoderOutputFormat.validBits = 16;
      }
    }
    else if (plan.sourceFormat.bitDepth == 16)
    {
      plan.decoderOutputFormat.bitDepth = 16;
      plan.decoderOutputFormat.validBits = 16;
    }
    else if (plan.sourceFormat.bitDepth == 32)
    {
      if (std::ranges::contains(caps.bitDepths, 32))
      {
        plan.decoderOutputFormat.bitDepth = 32;
        plan.decoderOutputFormat.validBits = 32;
      }
      else
      {
        plan.decoderOutputFormat.bitDepth = 16;
        plan.decoderOutputFormat.validBits = 16;
      }
    }

    // Ensure deviceFormat matches decoderOutputFormat if no other conversion is needed
    if (!plan.requiresBitDepthConversion)
    {
      plan.deviceFormat.bitDepth = plan.decoderOutputFormat.bitDepth;
      plan.deviceFormat.validBits = plan.decoderOutputFormat.validBits;
    }

    if (plan.reason.empty())
    {
      plan.reason = "Direct passthrough";
    }

    return plan;
  }

} // namespace rs::audio