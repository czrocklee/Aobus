// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#pragma once

#include <ao/audio/ChannelDeliveryEvidence.h>
#include <ao/audio/PcmFormat.h>
#include <ao/audio/SignalFormat.h>
#include <ao/audio/flow/Graph.h>

#include <optional>
#include <string>

namespace ao::audio::backend::detail
{
  struct CoreAudioRouteState final
  {
    std::string routeAnchor{};
    std::string deviceName{};
    // Native client storage after the private permutation, not render input.
    std::optional<PcmFormat> optClientFormat{};
    std::optional<SignalFormat> optDeviceFormat{};
    ChannelDeliveryEvidence channelDelivery{};
    ChannelDeliveryEvidence outputChannelDelivery{};
    float volume = 1.0F;
    bool muted = false;
  };

  flow::Graph coreAudioGraph(CoreAudioRouteState const& state);
} // namespace ao::audio::backend::detail
