// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#pragma once

#include "core/playback/PlaybackTypes.h"

#include <string>
#include <vector>

namespace app::core::playback
{

  struct DeviceCapabilities final
  {
    std::vector<std::uint32_t> sampleRates;
    std::vector<std::uint8_t> bitDepths;
    std::vector<std::uint8_t> channelCounts;
  };

  struct RenderPlan final
  {
    StreamFormat sourceFormat = {};
    StreamFormat deviceFormat = {};
    StreamFormat decoderOutputFormat = {};
    bool requiresResample = false;
    bool requiresBitDepthConversion = false;
    bool requiresChannelRemap = false;
    std::string reason = {};
  };

  class FormatNegotiator final
  {
  public:
    static RenderPlan buildPlan(StreamFormat sourceFormat, DeviceCapabilities const& caps);
  };

} // namespace app::core::playback