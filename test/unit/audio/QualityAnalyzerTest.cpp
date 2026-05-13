// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include <ao/audio/QualityAnalyzer.h>
#include <catch2/catch_test_macros.hpp>

namespace ao::audio::test
{
  namespace
  {
    flow::Graph buildBaseMergedGraph()
    {
      auto graph = flow::Graph{};

      // Base nodes added by Player
      graph.nodes.push_back(
        flow::Node{.id = "ao-decoder",
                   .type = flow::NodeType::Decoder,
                   .name = "Decoder",
                   .optFormat = Format{.sampleRate = 44100, .channels = 2, .bitDepth = 16, .isFloat = false},
                   .isLossySource = false});

      graph.nodes.push_back(
        flow::Node{.id = "ao-engine",
                   .type = flow::NodeType::Engine,
                   .name = "Engine",
                   .optFormat = Format{.sampleRate = 44100, .channels = 2, .bitDepth = 16, .isFloat = false}});

      graph.connections.push_back(flow::Connection{.sourceId = "ao-decoder", .destId = "ao-engine", .isActive = true});

      // System nodes (simulating a simple output)
      graph.nodes.push_back(
        flow::Node{.id = "ao-stream",
                   .type = flow::NodeType::Stream,
                   .name = "Stream",
                   .optFormat = Format{.sampleRate = 44100, .channels = 2, .bitDepth = 16, .isFloat = false}});

      graph.nodes.push_back(
        flow::Node{.id = "ao-sink",
                   .type = flow::NodeType::Sink,
                   .name = "Sink",
                   .optFormat = Format{.sampleRate = 44100, .channels = 2, .bitDepth = 16, .isFloat = false}});

      graph.connections.push_back(flow::Connection{.sourceId = "ao-engine", .destId = "ao-stream", .isActive = true});
      graph.connections.push_back(flow::Connection{.sourceId = "ao-stream", .destId = "ao-sink", .isActive = true});

      return graph;
    }
  } // namespace

  TEST_CASE("QualityAnalyzer - Bitwise Perfect", "[audio][quality]")
  {
    auto const graph = buildBaseMergedGraph();
    auto const result = analyzeAudioQuality(graph);

    REQUIRE(result.quality == Quality::BitwisePerfect);
    REQUIRE(result.tooltip.find("Byte-perfect") != std::string::npos);
  }

  TEST_CASE("QualityAnalyzer - Lossy Source", "[audio][quality]")
  {
    auto graph = buildBaseMergedGraph();
    graph.nodes[0].isLossySource = true;

    auto const result = analyzeAudioQuality(graph);
    REQUIRE(result.quality == Quality::LossySource);
    REQUIRE(result.tooltip.find("Lossy format") != std::string::npos);
  }

  TEST_CASE("QualityAnalyzer - Resampling", "[audio][quality]")
  {
    auto graph = buildBaseMergedGraph();
    // Modify Engine output format to trigger resampling at Decoder -> Engine
    graph.nodes[1].optFormat->sampleRate = 48000;

    auto const result = analyzeAudioQuality(graph);
    REQUIRE(result.quality == Quality::LinearIntervention);
    REQUIRE(result.tooltip.find("Resampling") != std::string::npos);
  }

  TEST_CASE("QualityAnalyzer - Volume Modification", "[audio][quality]")
  {
    auto graph = buildBaseMergedGraph();
    graph.nodes[1].volumeNotUnity = true;

    auto const result = analyzeAudioQuality(graph);
    REQUIRE(result.quality == Quality::LinearIntervention);
    REQUIRE(result.tooltip.find("Volume") != std::string::npos);
  }

  TEST_CASE("QualityAnalyzer - Mute Detected", "[audio][quality]")
  {
    auto graph = buildBaseMergedGraph();
    graph.nodes[2].isMuted = true;

    auto const result = analyzeAudioQuality(graph);
    REQUIRE(result.quality == Quality::LinearIntervention);
    REQUIRE(result.tooltip.find("MUTED") != std::string::npos);
  }

  TEST_CASE("QualityAnalyzer - External Mixing", "[audio][quality]")
  {
    auto graph = buildBaseMergedGraph();

    // Add an external source mixing into the sink
    graph.nodes.push_back(flow::Node{.id = "external-app", .type = flow::NodeType::ExternalSource, .name = "Firefox"});

    graph.connections.push_back(flow::Connection{.sourceId = "external-app", .destId = "ao-sink", .isActive = true});

    auto const result = analyzeAudioQuality(graph);
    REQUIRE(result.quality == Quality::LinearIntervention);
    REQUIRE(result.tooltip.find("shared with Firefox") != std::string::npos);
  }

  TEST_CASE("QualityAnalyzer - Lossless Bit-Depth Extension", "[audio][quality]")
  {
    auto graph = buildBaseMergedGraph();
    // decoder 16b -> engine 24b -> stream 24b -> sink 24b
    graph.nodes[1].optFormat->bitDepth = 24;
    graph.nodes[2].optFormat->bitDepth = 24;
    graph.nodes[3].optFormat->bitDepth = 24;

    auto const result = analyzeAudioQuality(graph);
    REQUIRE(result.quality == Quality::LosslessPadded);
    REQUIRE(result.tooltip.find("Integer padding") != std::string::npos);
  }

  TEST_CASE("QualityAnalyzer - Empty Graph", "[audio][quality]")
  {
    auto const graph = flow::Graph{};
    auto const result = analyzeAudioQuality(graph);
    REQUIRE(result.quality == Quality::Unknown);
  }
} // namespace ao::audio::test
