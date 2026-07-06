// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/audio/Backend.h>
#include <ao/audio/Format.h>
#include <ao/audio/FormatNegotiator.h>

#include <algorithm>
#include <cstdint>
#include <string>

namespace ao::audio
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

      target = std::forward<Selector>(selector)();
      flag = true;

      if (!reason.empty())
      {
        reason += "; ";
      }

      reason += msg;
    }

    void requireBitDepthConversion(RenderPlan& plan)
    {
      if (plan.requiresBitDepthConversion)
      {
        return;
      }

      plan.requiresBitDepthConversion = true;

      if (!plan.reason.empty())
      {
        plan.reason += "; ";
      }

      plan.reason += "bit depth conversion required";
    }

    void setSampleFormat(Format& format, std::uint8_t bitDepth, std::uint8_t validBits, bool isFloat = false)
    {
      format.bitDepth = bitDepth;
      format.validBits = validBits;
      format.isFloat = isFloat;
      format.isInterleaved = true;
    }

    bool supportsSampleFormat(DeviceCapabilities const& caps,
                              std::uint8_t bitDepth,
                              std::uint8_t validBits,
                              bool isFloat = false)
    {
      return std::ranges::contains(caps.sampleFormats,
                                   SampleFormatCapability{
                                     .bitDepth = bitDepth,
                                     .validBits = validBits,
                                     .isFloat = isFloat,
                                   });
    }

    void applyLegacyDecoderNegotiation(RenderPlan& plan, DeviceCapabilities const& caps)
    {
      std::uint8_t bitDepth = 16;
      std::uint8_t validBits = 16;

      if (plan.sourceFormat.isFloat)
      {
        bitDepth = 32;
        validBits = 32;
        plan.decoderOutputFormat.isFloat = true;
      }
      else if (plan.sourceFormat.bitDepth < 16)
      {
        if (!caps.bitDepths.empty())
        {
          bitDepth = *std::ranges::max_element(caps.bitDepths);
        }

        validBits = plan.sourceFormat.validBits != 0 ? plan.sourceFormat.validBits : plan.sourceFormat.bitDepth;
      }
      else if (plan.sourceFormat.bitDepth == 24)
      {
        if (std::ranges::contains(caps.bitDepths, 24))
        {
          bitDepth = 24;
          validBits = 24;
        }
        else if (std::ranges::contains(caps.bitDepths, 32))
        {
          bitDepth = 32;
          validBits = 24;
        }
      }
      else if (plan.sourceFormat.bitDepth == 16)
      {
        if (!std::ranges::contains(caps.bitDepths, 16) && std::ranges::contains(caps.bitDepths, 32))
        {
          bitDepth = 32;
          validBits = 16;
        }
      }
      else if (plan.sourceFormat.bitDepth == 32)
      {
        if (std::ranges::contains(caps.bitDepths, 32))
        {
          bitDepth = 32;
          validBits = 32;
        }
      }

      plan.decoderOutputFormat.bitDepth = bitDepth;
      plan.decoderOutputFormat.validBits = validBits;
    }

    bool supportsNativeSampleFormat(Format const& sourceFormat, DeviceCapabilities const& caps)
    {
      auto const validBits = sourceFormat.validBits != 0 ? sourceFormat.validBits : sourceFormat.bitDepth;
      return !caps.sampleFormats.empty() &&
             supportsSampleFormat(caps, sourceFormat.bitDepth, validBits, sourceFormat.isFloat);
    }

    void applyFloatSampleFormatNegotiation(RenderPlan& plan, DeviceCapabilities const& caps)
    {
      if (caps.sampleFormats.empty() || supportsSampleFormat(caps, 32, 32, true))
      {
        setSampleFormat(plan.decoderOutputFormat, 32, 32, true);
        setSampleFormat(plan.deviceFormat, 32, 32, true);
        return;
      }

      requireBitDepthConversion(plan);

      if (supportsSampleFormat(caps, 32, 32))
      {
        setSampleFormat(plan.decoderOutputFormat, 32, 32);
        setSampleFormat(plan.deviceFormat, 32, 32);
        return;
      }

      setSampleFormat(plan.decoderOutputFormat, 16, 16);
      setSampleFormat(plan.deviceFormat, 16, 16);
    }

    void apply24BitSampleFormatNegotiation(RenderPlan& plan, DeviceCapabilities const& caps)
    {
      if (supportsSampleFormat(caps, 24, 24))
      {
        setSampleFormat(plan.decoderOutputFormat, 24, 24);
        setSampleFormat(plan.deviceFormat, 24, 24);
        return;
      }

      requireBitDepthConversion(plan);

      if (supportsSampleFormat(caps, 32, 24))
      {
        setSampleFormat(plan.decoderOutputFormat, 32, 24);
        setSampleFormat(plan.deviceFormat, 32, 24);
      }
      else if (supportsSampleFormat(caps, 32, 32))
      {
        setSampleFormat(plan.decoderOutputFormat, 32, 24);
        setSampleFormat(plan.deviceFormat, 32, 32);
      }
      else
      {
        setSampleFormat(plan.decoderOutputFormat, 16, 16);
        setSampleFormat(plan.deviceFormat, 16, 16);
      }
    }

    void apply32BitSampleFormatNegotiation(RenderPlan& plan, DeviceCapabilities const& caps)
    {
      auto const sourceValidBits =
        plan.sourceFormat.validBits != 0 ? plan.sourceFormat.validBits : plan.sourceFormat.bitDepth;

      if (sourceValidBits == 24 && supportsSampleFormat(caps, 32, 24))
      {
        setSampleFormat(plan.decoderOutputFormat, 32, 24);
        setSampleFormat(plan.deviceFormat, 32, 24);
        return;
      }

      if (supportsSampleFormat(caps, 32, 32))
      {
        setSampleFormat(plan.decoderOutputFormat, 32, 32);
        setSampleFormat(plan.deviceFormat, 32, 32);
        return;
      }

      requireBitDepthConversion(plan);

      if (supportsSampleFormat(caps, 24, 24))
      {
        setSampleFormat(plan.decoderOutputFormat, 24, 24);
        setSampleFormat(plan.deviceFormat, 24, 24);
      }
      else
      {
        // Default to 16-bit if 24-bit is also not supported
        setSampleFormat(plan.decoderOutputFormat, 16, 16);
        setSampleFormat(plan.deviceFormat, 16, 16);
      }
    }
  } // namespace

  RenderPlan FormatNegotiator::buildPlan(Format sourceFormat, DeviceCapabilities const& caps)
  {
    auto plan = RenderPlan{
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

    if (!supportsNativeSampleFormat(plan.sourceFormat, caps))
    {
      negotiate(plan.deviceFormat.bitDepth,
                caps.bitDepths,
                plan.requiresBitDepthConversion,
                plan.reason,
                "bit depth conversion required",
                [&] { return *std::ranges::max_element(caps.bitDepths); });
    }

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
    plan.decoderOutputFormat.validBits =
      (plan.sourceFormat.validBits != 0) ? plan.sourceFormat.validBits : plan.sourceFormat.bitDepth;

    // Prefer exact backend sample formats for exclusive backends so 32/24 padded
    // PCM is not confused with true 32/32 support.
    if (plan.sourceFormat.isFloat)
    {
      applyFloatSampleFormatNegotiation(plan, caps);
    }
    else if (caps.sampleFormats.empty() || (plan.sourceFormat.bitDepth != 24 && plan.sourceFormat.bitDepth != 32))
    {
      applyLegacyDecoderNegotiation(plan, caps);
    }
    else if (plan.sourceFormat.bitDepth == 24)
    {
      apply24BitSampleFormatNegotiation(plan, caps);
    }
    else if (plan.sourceFormat.bitDepth == 32)
    {
      apply32BitSampleFormatNegotiation(plan, caps);
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
} // namespace ao::audio
