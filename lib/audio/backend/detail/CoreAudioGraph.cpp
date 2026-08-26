// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "CoreAudioGraph.h"

#include "AudioBackendVolumeMath.h"
#include <ao/audio/flow/Graph.h>

#include <cmath>
#include <string>
#include <utility>

namespace ao::audio::backend::detail
{
  flow::Graph coreAudioGraph(CoreAudioRouteState const& state)
  {
    if (state.routeAnchor.empty() || !state.optClientFormat)
    {
      return {};
    }

    auto stream = flow::Node{.id = state.routeAnchor + ":client",
                             .type = flow::NodeType::Stream,
                             .name = "Aobus Core Audio stream",
                             .optFormat = *state.optClientFormat};
    auto sink = flow::Node{.id = state.routeAnchor,
                           .type = flow::NodeType::Sink,
                           .name = state.deviceName,
                           .optFormat = state.optDeviceFormat,
                           .softwareVolumeNotUnity = std::abs(state.volume - 1.0F) >= kVolumeEpsilon,
                           .maxSoftwareGain = state.volume,
                           .minSoftwareGain = state.volume,
                           .isMuted = state.muted,
                           .objectPath = state.routeAnchor};
    return {.nodes = {std::move(stream), std::move(sink)},
            .connections = {{.sourceId = state.routeAnchor + ":client", .destinationId = state.routeAnchor}}};
  }
} // namespace ao::audio::backend::detail
