// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include <ao/audio/Backend.h>
#include <ao/audio/Format.h>
#include <ao/audio/QualityAnalyzer.h>
#include <ao/audio/flow/Graph.h>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <string_view>

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

    NodeQualityAssessment const* findAssessment(QualityResult const& result, std::string_view id)
    {
      auto const it = std::ranges::find(result.assessments, id, &NodeQualityAssessment::nodeId);
      return it != result.assessments.end() ? &(*it) : nullptr;
    }

    bool hasFinding(NodeQualityAssessment const* assessment, QualityFindingKind kind)
    {
      if (assessment == nullptr)
      {
        return false;
      }

      return std::ranges::any_of(assessment->findings, [kind](auto const& f) { return f.kind == kind; });
    }
  } // namespace

  TEST_CASE("QualityAnalyzer - Bitwise Perfect", "[audio][unit][quality]")
  {
    auto const graph = buildBaseMergedGraph();
    auto const result = analyzeAudioQuality(graph);

    REQUIRE(result.quality == Quality::BitwisePerfect);
    REQUIRE(result.assessments.size() == 4);

    for (auto const& assessment : result.assessments)
    {
      REQUIRE(assessment.worstQuality == Quality::BitwisePerfect);
      REQUIRE(hasFinding(&assessment, QualityFindingKind::BitPerfect));
    }
  }

  TEST_CASE("QualityAnalyzer - Lossy Source", "[audio][unit][quality]")
  {
    auto graph = buildBaseMergedGraph();
    graph.nodes[0].isLossySource = true;

    auto const result = analyzeAudioQuality(graph);
    REQUIRE(result.quality == Quality::LossySource);

    auto const* dec = findAssessment(result, "ao-decoder");
    REQUIRE(hasFinding(dec, QualityFindingKind::LossySource));
    REQUIRE(dec->worstQuality == Quality::LossySource);
  }

  TEST_CASE("QualityAnalyzer - Resampling", "[audio][unit][quality]")
  {
    auto graph = buildBaseMergedGraph();
    // Modify Engine output format to trigger resampling at Decoder -> Engine
    graph.nodes[1].optFormat->sampleRate = 48000;

    auto const result = analyzeAudioQuality(graph);
    REQUIRE(result.quality == Quality::LinearIntervention);

    auto const* eng = findAssessment(result, "ao-engine");
    REQUIRE(hasFinding(eng, QualityFindingKind::Resampling));
    REQUIRE(eng->worstQuality == Quality::LinearIntervention);
  }

  TEST_CASE("QualityAnalyzer - Volume Modification", "[audio][unit][quality]")
  {
    auto graph = buildBaseMergedGraph();
    graph.nodes[1].volumeNotUnity = true;

    auto const result = analyzeAudioQuality(graph);
    REQUIRE(result.quality == Quality::LinearIntervention);

    auto const* eng = findAssessment(result, "ao-engine");
    REQUIRE(hasFinding(eng, QualityFindingKind::VolumeModification));
    REQUIRE(eng->worstQuality == Quality::LinearIntervention);
  }

  TEST_CASE("QualityAnalyzer - Mute Detected", "[audio][unit][quality]")
  {
    auto graph = buildBaseMergedGraph();
    graph.nodes[2].isMuted = true;

    auto const result = analyzeAudioQuality(graph);
    REQUIRE(result.quality == Quality::LinearIntervention);

    auto const* strm = findAssessment(result, "ao-stream");
    REQUIRE(hasFinding(strm, QualityFindingKind::Muted));
    REQUIRE(strm->worstQuality == Quality::LinearIntervention);
  }

  TEST_CASE("QualityAnalyzer - External Mixing", "[audio][unit][quality]")
  {
    auto graph = buildBaseMergedGraph();

    // Add an external source mixing into the sink
    graph.nodes.push_back(flow::Node{.id = "external-app", .type = flow::NodeType::ExternalSource, .name = "Firefox"});

    graph.connections.push_back(flow::Connection{.sourceId = "external-app", .destId = "ao-sink", .isActive = true});

    auto const result = analyzeAudioQuality(graph);
    REQUIRE(result.quality == Quality::LinearIntervention);

    auto const* sink = findAssessment(result, "ao-sink");
    REQUIRE(hasFinding(sink, QualityFindingKind::MixedSources));
    REQUIRE(sink->worstQuality == Quality::LinearIntervention);

    auto const findingIt =
      std::ranges::find_if(sink->findings, [](auto const& f) { return f.kind == QualityFindingKind::MixedSources; });
    REQUIRE(findingIt != sink->findings.end());
    REQUIRE(std::ranges::contains(findingIt->sharedApps, std::string{"Firefox"}));
  }

  TEST_CASE("QualityAnalyzer - Lossless Bit-Depth Extension", "[audio][unit][quality]")
  {
    auto graph = buildBaseMergedGraph();
    // decoder 16b -> engine 24b -> stream 24b -> sink 24b
    graph.nodes[1].optFormat->bitDepth = 24;
    graph.nodes[2].optFormat->bitDepth = 24;
    graph.nodes[3].optFormat->bitDepth = 24;

    auto const result = analyzeAudioQuality(graph);
    REQUIRE(result.quality == Quality::LosslessPadded);

    auto const* eng = findAssessment(result, "ao-engine");
    REQUIRE(hasFinding(eng, QualityFindingKind::LosslessPadding));
    REQUIRE(eng->worstQuality == Quality::LosslessPadded);
  }

  TEST_CASE("QualityAnalyzer - Empty Graph", "[audio][unit][quality]")
  {
    auto const graph = flow::Graph{};
    auto const result = analyzeAudioQuality(graph);
    REQUIRE(result.quality == Quality::Unknown);
    REQUIRE(result.assessments.empty());
  }

  TEST_CASE("QualityAnalyzer - Missing Playback Path", "[audio][unit][quality]")
  {
    auto graph = flow::Graph{};
    graph.nodes.push_back(flow::Node{.id = "external-app", .type = flow::NodeType::ExternalSource, .name = "Firefox"});

    auto const result = analyzeAudioQuality(graph);
    REQUIRE(result.quality == Quality::Unknown);
    REQUIRE(result.assessments.empty());
  }

  TEST_CASE("QualityAnalyzer - Float Conversions", "[audio][unit][quality]")
  {
    auto graph = buildBaseMergedGraph();

    SECTION("16-bit to 32-bit float is lossless")
    {
      graph.nodes[1].optFormat->bitDepth = 32;
      graph.nodes[1].optFormat->isFloat = true;
      graph.nodes[2].optFormat->bitDepth = 32;
      graph.nodes[2].optFormat->isFloat = true;
      graph.nodes[3].optFormat->bitDepth = 32;
      graph.nodes[3].optFormat->isFloat = true;

      auto const result = analyzeAudioQuality(graph);
      REQUIRE(result.quality == Quality::LosslessFloat);

      auto const* eng = findAssessment(result, "ao-engine");
      REQUIRE(hasFinding(eng, QualityFindingKind::LosslessFloat));
    }

    SECTION("32-bit integer to 32-bit float is lossy (mantissa truncation)")
    {
      graph.nodes[0].optFormat->bitDepth = 32;
      graph.nodes[1].optFormat->bitDepth = 32;
      graph.nodes[1].optFormat->isFloat = true;
      graph.nodes[2].optFormat->bitDepth = 32;
      graph.nodes[2].optFormat->isFloat = true;
      graph.nodes[3].optFormat->bitDepth = 32;
      graph.nodes[3].optFormat->isFloat = true;

      auto const result = analyzeAudioQuality(graph);
      REQUIRE(result.quality == Quality::LinearIntervention);

      auto const* eng = findAssessment(result, "ao-engine");
      REQUIRE(hasFinding(eng, QualityFindingKind::Truncation));
    }

    SECTION("32-bit integer to 64-bit float is lossless")
    {
      graph.nodes[0].optFormat->bitDepth = 32;
      graph.nodes[1].optFormat->bitDepth = 64;
      graph.nodes[1].optFormat->isFloat = true;
      graph.nodes[2].optFormat->bitDepth = 64;
      graph.nodes[2].optFormat->isFloat = true;
      graph.nodes[3].optFormat->bitDepth = 64;
      graph.nodes[3].optFormat->isFloat = true;

      auto const result = analyzeAudioQuality(graph);
      REQUIRE(result.quality == Quality::LosslessFloat);
    }
  }
} // namespace ao::audio::test
