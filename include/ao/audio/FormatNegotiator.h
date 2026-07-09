// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#pragma once

#include <ao/audio/Device.h>
#include <ao/audio/Format.h>

#include <string>

namespace ao::audio
{
  struct RenderPlan final
  {
    Format sourceFormat = {};
    Format deviceFormat = {};
    Format decoderOutputFormat = {};
    bool isResampleRequired = false;
    bool isBitDepthConversionRequired = false;
    bool isChannelRemapRequired = false;
    std::string reason = {};
  };

  class FormatNegotiator final
  {
  public:
    static RenderPlan buildPlan(Format sourceFormat, DeviceCapabilities const& caps);
  };
} // namespace ao::audio
