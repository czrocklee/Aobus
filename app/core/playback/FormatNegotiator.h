// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#pragma once

#include "core/backend/BackendTypes.h"
#include "core/playback/PlaybackTypes.h"

#include <string>
#include <vector>

namespace app::core::playback
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
    static RenderPlan buildPlan(AudioFormat sourceFormat, backend::DeviceCapabilities const& caps);
  };

} // namespace app::core::playback