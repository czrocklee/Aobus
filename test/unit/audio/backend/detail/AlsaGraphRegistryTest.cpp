// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/audio/backend/detail/AlsaGraphRegistry.h>
#include <ao/audio/flow/Graph.h>

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <vector>

using namespace ao::audio::backend::detail;
using namespace ao::audio::flow;

TEST_CASE("AlsaGraphRegistry - initial subscription receives neutral graph", "[audio][unit][alsa]")
{
  auto registry = AlsaGraphRegistry{};
  auto receivedGraph = Graph{};
  std::int32_t callCount = 0;

  auto sub = registry.subscribe("hw:0,0",
                                [&](Graph const& g)
                                {
                                  receivedGraph = g;
                                  callCount++;
                                });

  CHECK(callCount == 1);
  REQUIRE(receivedGraph.nodes.size() == 2);
  CHECK(receivedGraph.nodes[1].id == "alsa-sink");
  CHECK(receivedGraph.nodes[1].name == "hw:0,0");
  CHECK_FALSE(receivedGraph.nodes[1].hardwareVolumeNotUnity);
  CHECK_FALSE(receivedGraph.nodes[1].softwareVolumeNotUnity);
  CHECK_FALSE(receivedGraph.nodes[1].isMuted);
}

TEST_CASE("AlsaGraphRegistry - hardware volume publish updates subscribers", "[audio][unit][alsa]")
{
  auto registry = AlsaGraphRegistry{};
  auto receivedGraph = Graph{};

  auto sub = registry.subscribe("hw:0,0", [&](Graph const& g) { receivedGraph = g; });

  registry.publish(
    {.routeAnchor = "hw:0,0", .volume = 0.5F, .muted = false, .volumeMode = AlsaVolumeControlMode::HardwareMixer});

  REQUIRE(receivedGraph.nodes.size() == 2);
  CHECK(receivedGraph.nodes[1].hardwareVolumeNotUnity);
  CHECK_FALSE(receivedGraph.nodes[1].softwareVolumeNotUnity);
  CHECK_FALSE(receivedGraph.nodes[1].isMuted);
}

TEST_CASE("AlsaGraphRegistry - software volume publish updates subscribers", "[audio][unit][alsa]")
{
  auto registry = AlsaGraphRegistry{};
  auto receivedGraph = Graph{};

  auto sub = registry.subscribe("hw:0,0", [&](Graph const& g) { receivedGraph = g; });

  registry.publish(
    {.routeAnchor = "hw:0,0", .volume = 0.8F, .muted = false, .volumeMode = AlsaVolumeControlMode::SoftwareGain});

  REQUIRE(receivedGraph.nodes.size() == 2);
  CHECK_FALSE(receivedGraph.nodes[1].hardwareVolumeNotUnity);
  CHECK(receivedGraph.nodes[1].softwareVolumeNotUnity);
  CHECK_FALSE(receivedGraph.nodes[1].isMuted);
}

TEST_CASE("AlsaGraphRegistry - mute publish updates subscribers", "[audio][unit][alsa]")
{
  auto registry = AlsaGraphRegistry{};
  auto receivedGraph = Graph{};

  auto sub = registry.subscribe("hw:0,0", [&](Graph const& g) { receivedGraph = g; });

  registry.publish(
    {.routeAnchor = "hw:0,0", .volume = 1.0F, .muted = true, .volumeMode = AlsaVolumeControlMode::HardwareMixer});

  REQUIRE(receivedGraph.nodes.size() == 2);
  CHECK_FALSE(receivedGraph.nodes[1].hardwareVolumeNotUnity);
  CHECK(receivedGraph.nodes[1].isMuted);
}

TEST_CASE("AlsaGraphRegistry - unavailable mode with non-unity volume emits unclassified volume", "[audio][unit][alsa]")
{
  auto registry = AlsaGraphRegistry{};
  auto receivedGraph = Graph{};

  auto sub = registry.subscribe("hw:0,0", [&](Graph const& g) { receivedGraph = g; });

  registry.publish(
    {.routeAnchor = "hw:0,0", .volume = 0.5F, .muted = false, .volumeMode = AlsaVolumeControlMode::Unavailable});

  REQUIRE(receivedGraph.nodes.size() == 2);
  CHECK_FALSE(receivedGraph.nodes[1].hardwareVolumeNotUnity);
  CHECK_FALSE(receivedGraph.nodes[1].softwareVolumeNotUnity);
  CHECK(receivedGraph.nodes[1].unclassifiedVolumeNotUnity);
  CHECK_FALSE(receivedGraph.nodes[1].isMuted);
}

TEST_CASE("AlsaGraphRegistry - clear emits empty graph", "[audio][unit][alsa]")
{
  auto registry = AlsaGraphRegistry{};
  auto receivedGraph = Graph{};

  auto sub = registry.subscribe("hw:0,0", [&](Graph const& g) { receivedGraph = g; });

  registry.publish(
    {.routeAnchor = "hw:0,0", .volume = 0.5F, .muted = false, .volumeMode = AlsaVolumeControlMode::HardwareMixer});

  REQUIRE_FALSE(receivedGraph.nodes.empty());

  registry.clear("hw:0,0");
  CHECK(receivedGraph.nodes.empty());
  CHECK(receivedGraph.connections.empty());
}

TEST_CASE("AlsaGraphRegistry - subscriber only receives updates for its anchor", "[audio][unit][alsa]")
{
  auto registry = AlsaGraphRegistry{};
  std::int32_t callCountA = 0;
  std::int32_t callCountB = 0;

  auto subA = registry.subscribe("hw:0,0", [&](Graph const&) { callCountA++; });
  auto subB = registry.subscribe("hw:1,0", [&](Graph const&) { callCountB++; });

  // Initial snapshots
  CHECK(callCountA == 1);
  CHECK(callCountB == 1);

  registry.publish({.routeAnchor = "hw:0,0", .volume = 0.5F});
  CHECK(callCountA == 2);
  CHECK(callCountB == 1);

  registry.publish({.routeAnchor = "hw:1,0", .volume = 0.5F});
  CHECK(callCountA == 2);
  CHECK(callCountB == 2);
}

TEST_CASE("AlsaGraphRegistry - subscription reset stops updates", "[audio][unit][alsa]")
{
  auto registry = AlsaGraphRegistry{};
  std::int32_t callCount = 0;

  auto sub = registry.subscribe("hw:0,0", [&](Graph const&) { callCount++; });
  CHECK(callCount == 1);

  sub.reset();

  registry.publish({.routeAnchor = "hw:0,0", .volume = 0.5F});
  CHECK(callCount == 1);
}
