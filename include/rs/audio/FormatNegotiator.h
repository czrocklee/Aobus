// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#pragma once

#include <rs/audio/Backend.h>
#include <rs/audio/Types.h>

#include <string>
#include <vector>

namespace rs::audio
{
  struct RenderPlan final
  {
    Format sourceFormat = {};
    Format deviceFormat = {};
    Format decoderOutputFormat = {};
    bool requiresResample = false;
    bool requiresBitDepthConversion = false;
    bool requiresChannelRemap = false;
    std::string reason = {};
  };

  class FormatNegotiator final
  {
  public:
    static RenderPlan buildPlan(Format sourceFormat, DeviceCapabilities const& caps);
  };
} // namespace rs::audio