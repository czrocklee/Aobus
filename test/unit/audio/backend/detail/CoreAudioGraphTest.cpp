// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "lib/audio/backend/detail/CoreAudioGraph.h"

#include <ao/audio/PcmFormat.h>
#include <ao/audio/SampleEncoding.h>
#include <ao/audio/SignalFormat.h>
#include <ao/audio/flow/Graph.h>

#include <catch2/catch_test_macros.hpp>

namespace ao::audio::backend::detail::test
{
  TEST_CASE("CoreAudioGraph - distinguishes the client stream, endpoint signal, and software gain",
            "[audio][unit][coreaudio]")
  {
    auto const graph = coreAudioGraph(
      {.routeAnchor = "device-uid",
       .deviceName = "Built-in Output",
       .optClientFormat = PcmFormat{.sampleRate = 44100, .channels = 2, .encoding = SampleEncoding::Signed24In32Le},
       .optDeviceFormat =
         SignalFormat{.sampleRate = 48000, .channels = 2, .precisionBits = 32, .sampleKind = SampleKind::FloatingPoint},
       .volume = 0.5F,
       .muted = true});

    REQUIRE(graph.nodes.size() == 2U);
    CHECK(graph.nodes[0].id == "device-uid:client");
    CHECK(graph.nodes[0].type == flow::NodeType::Stream);
    CHECK(graph.nodes[1].id == "device-uid");
    CHECK(graph.nodes[1].type == flow::NodeType::Sink);
    CHECK(graph.nodes[1].softwareVolumeNotUnity);
    CHECK_FALSE(graph.nodes[1].hardwareVolumeNotUnity);
    CHECK(graph.nodes[1].maxSoftwareGain == 0.5F);
    CHECK(graph.nodes[1].isMuted);
    REQUIRE(graph.connections.size() == 1U);
    CHECK(graph.connections.front().sourceId == "device-uid:client");
    CHECK(graph.connections.front().destinationId == "device-uid");
  }

  TEST_CASE("CoreAudioGraph - stays empty until a client stream is configured", "[audio][unit][coreaudio]")
  {
    CHECK(coreAudioGraph({.routeAnchor = "device-uid"}).nodes.empty());
  }
} // namespace ao::audio::backend::detail::test
