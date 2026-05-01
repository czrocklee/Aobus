// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#pragma once

#include <rs/audio/BackendTypes.h>
#include <rs/audio/PlaybackTypes.h>

#include <string>
#include <vector>

namespace rs::audio
{
  struct RenderPlan final
  {
    AudioFormat sourceFormat = {};
    AudioFormat deviceFormat = {};
    AudioFormat decoderOutputFormat = {};
    bool requiresResample = false;
    bool requiresBitDepthConversion = false;
    bool requiresChannelRemap = false;
    std::string reason = {};
  };

  class FormatNegotiator final
  {
  public:
    static RenderPlan buildPlan(AudioFormat sourceFormat, DeviceCapabilities const& caps);
  };
} // namespace rs::audio