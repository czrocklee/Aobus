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

      // Base nodes added by Player: source -> decoder -> engine
      graph.nodes.push_back(
        flow::Node{.id = "ao-source",
                   .type = flow::NodeType::Source,
                   .name = "Source",
                   .optFormat = Format{.sampleRate = 44100, .channels = 2, .bitDepth = 16, .isFloat = false},
                   .isLossySource = false});

      graph.nodes.push_back(
        flow::Node{.id = "ao-decoder",
                   .type = flow::NodeType::Decoder,
                   .name = "Decoder",
                   .optFormat = Format{.sampleRate = 44100, .channels = 2, .bitDepth = 16, .isFloat = false}});

      graph.nodes.push_back(
        flow::Node{.id = "ao-engine",
                   .type = flow::NodeType::Engine,
                   .name = "Engine",
                   .optFormat = Format{.sampleRate = 44100, .channels = 2, .bitDepth = 16, .isFloat = false}});

      graph.connections.push_back(flow::Connection{.sourceId = "ao-source", .destId = "ao-decoder", .isActive = true});
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

  TEST_CASE("QualityAnalyzer - unchanged playback path is bitwise perfect", "[audio][unit][quality]")
  {
    auto const graph = buildBaseMergedGraph();
    auto const result = analyzeAudioQuality(graph);

    CHECK(result.quality == Quality::BitwisePerfect);
    CHECK(result.assessments.size() == 5);

    for (auto const& assessment : result.assessments)
    {
      CHECK(assessment.worstQuality == Quality::BitwisePerfect);
      CHECK(hasFinding(&assessment, QualityFindingKind::BitPerfect));
    }
  }

  TEST_CASE("QualityAnalyzer - lossy source marks source as worst quality", "[audio][unit][quality]")
  {
    auto graph = buildBaseMergedGraph();
    graph.nodes[0].isLossySource = true; // ao-source

    auto const result = analyzeAudioQuality(graph);
    CHECK(result.quality == Quality::LossySource);

    auto const* src = findAssessment(result, "ao-source");
    CHECK(hasFinding(src, QualityFindingKind::LossySource));
    CHECK(src->worstQuality == Quality::LossySource);
  }

  TEST_CASE("QualityAnalyzer - resampling marks engine as linear intervention", "[audio][unit][quality]")
  {
    auto graph = buildBaseMergedGraph();
    // Modify Engine output format to trigger resampling at Decoder -> Engine
    graph.nodes[2].optFormat->sampleRate = 48000;

    auto const result = analyzeAudioQuality(graph);
    CHECK(result.quality == Quality::LinearIntervention);

    auto const* eng = findAssessment(result, "ao-engine");
    CHECK(hasFinding(eng, QualityFindingKind::Resampling));
    CHECK(eng->worstQuality == Quality::LinearIntervention);
  }

  TEST_CASE("QualityAnalyzer - software volume change marks linear intervention", "[audio][unit][quality]")
  {
    auto graph = buildBaseMergedGraph();
    graph.nodes[2].softwareVolumeNotUnity = true;

    auto const result = analyzeAudioQuality(graph);
    CHECK(result.quality == Quality::LinearIntervention);

    auto const* eng = findAssessment(result, "ao-engine");
    CHECK(hasFinding(eng, QualityFindingKind::SoftwareVolumeModification));
    CHECK(eng->worstQuality == Quality::LinearIntervention);
  }

  TEST_CASE("QualityAnalyzer - hardware volume change keeps bitwise quality", "[audio][unit][quality]")
  {
    auto graph = buildBaseMergedGraph();
    graph.nodes[2].hardwareVolumeNotUnity = true;

    auto const result = analyzeAudioQuality(graph);
    CHECK(result.quality == Quality::BitwisePerfect);

    auto const* eng = findAssessment(result, "ao-engine");
    CHECK(hasFinding(eng, QualityFindingKind::HardwareVolumeModification));
    CHECK(eng->worstQuality == Quality::BitwisePerfect);
  }

  TEST_CASE("QualityAnalyzer - unclassified volume change marks linear intervention", "[audio][unit][quality]")
  {
    auto graph = buildBaseMergedGraph();
    graph.nodes[2].unclassifiedVolumeNotUnity = true;

    auto const result = analyzeAudioQuality(graph);
    CHECK(result.quality == Quality::LinearIntervention);

    auto const* eng = findAssessment(result, "ao-engine");
    CHECK(hasFinding(eng, QualityFindingKind::UnclassifiedVolumeModification));
    CHECK(eng->worstQuality == Quality::LinearIntervention);
  }

  TEST_CASE("QualityAnalyzer - mixed hardware and software volume keeps both findings", "[audio][unit][quality]")
  {
    auto graph = buildBaseMergedGraph();
    graph.nodes[2].hardwareVolumeNotUnity = true;
    graph.nodes[2].softwareVolumeNotUnity = true;

    auto const result = analyzeAudioQuality(graph);
    CHECK(result.quality == Quality::LinearIntervention);

    auto const* eng = findAssessment(result, "ao-engine");
    CHECK(eng != nullptr);
    CHECK(hasFinding(eng, QualityFindingKind::HardwareVolumeModification));
    CHECK(hasFinding(eng, QualityFindingKind::SoftwareVolumeModification));
    CHECK(eng->worstQuality == Quality::LinearIntervention);
  }

  TEST_CASE("QualityAnalyzer - muted stream marks linear intervention", "[audio][unit][quality]")
  {
    auto graph = buildBaseMergedGraph();
    graph.nodes[3].isMuted = true;

    auto const result = analyzeAudioQuality(graph);
    CHECK(result.quality == Quality::LinearIntervention);

    auto const* strm = findAssessment(result, "ao-stream");
    CHECK(hasFinding(strm, QualityFindingKind::Muted));
    CHECK(strm->worstQuality == Quality::LinearIntervention);
  }

  TEST_CASE("QualityAnalyzer - external mixing reports shared applications", "[audio][unit][quality]")
  {
    auto graph = buildBaseMergedGraph();

    // Add an external source mixing into the sink
    graph.nodes.push_back(flow::Node{.id = "external-app", .type = flow::NodeType::ExternalSource, .name = "Firefox"});

    graph.connections.push_back(flow::Connection{.sourceId = "external-app", .destId = "ao-sink", .isActive = true});

    auto const result = analyzeAudioQuality(graph);
    CHECK(result.quality == Quality::LinearIntervention);

    auto const* sink = findAssessment(result, "ao-sink");
    CHECK(hasFinding(sink, QualityFindingKind::MixedSources));
    CHECK(sink->worstQuality == Quality::LinearIntervention);

    auto const findingIt =
      std::ranges::find_if(sink->findings, [](auto const& f) { return f.kind == QualityFindingKind::MixedSources; });
    CHECK(findingIt != sink->findings.end());
    CHECK(std::ranges::contains(findingIt->sharedApps, std::string{"Firefox"}));
  }

  TEST_CASE("QualityAnalyzer - lossless bit-depth extension reports padded quality", "[audio][unit][quality]")
  {
    auto graph = buildBaseMergedGraph();
    // decoder 16b -> engine 24b -> stream 24b -> sink 24b
    graph.nodes[2].optFormat->bitDepth = 24;
    graph.nodes[3].optFormat->bitDepth = 24;
    graph.nodes[4].optFormat->bitDepth = 24;

    auto const result = analyzeAudioQuality(graph);
    CHECK(result.quality == Quality::LosslessPadded);

    auto const* eng = findAssessment(result, "ao-engine");
    CHECK(hasFinding(eng, QualityFindingKind::LosslessPadding));
    CHECK(eng->worstQuality == Quality::LosslessPadded);
  }

  TEST_CASE("QualityAnalyzer - Source padded into wider container", "[audio][unit][quality]")
  {
    auto graph = buildBaseMergedGraph();
    // 16-bit source carried in a 32-bit container from the decoder onward: the
    // padding surfaces as a lossless source -> decoder bit-depth transition.
    graph.nodes[1].optFormat->bitDepth = 32; // decoder
    graph.nodes[1].optFormat->validBits = 16;
    graph.nodes[2].optFormat->bitDepth = 32; // engine
    graph.nodes[2].optFormat->validBits = 16;
    graph.nodes[3].optFormat->bitDepth = 32; // stream
    graph.nodes[3].optFormat->validBits = 16;
    graph.nodes[4].optFormat->bitDepth = 32; // sink
    graph.nodes[4].optFormat->validBits = 16;

    auto const result = analyzeAudioQuality(graph);
    CHECK(result.quality == Quality::LosslessPadded);

    // The padding is attributed to the decoder (destination of source -> decoder),
    // not the source itself.
    auto const* dec = findAssessment(result, "ao-decoder");
    CHECK(hasFinding(dec, QualityFindingKind::LosslessPadding));

    auto const* src = findAssessment(result, "ao-source");
    CHECK(hasFinding(src, QualityFindingKind::BitPerfect));
  }

  TEST_CASE("QualityAnalyzer - empty graph has unknown quality", "[audio][unit][quality]")
  {
    auto const graph = flow::Graph{};
    auto const result = analyzeAudioQuality(graph);
    CHECK(result.quality == Quality::Unknown);
    CHECK(result.assessments.empty());
  }

  TEST_CASE("QualityAnalyzer - missing playback path has unknown quality", "[audio][unit][quality]")
  {
    auto graph = flow::Graph{};
    graph.nodes.push_back(flow::Node{.id = "external-app", .type = flow::NodeType::ExternalSource, .name = "Firefox"});

    auto const result = analyzeAudioQuality(graph);
    CHECK(result.quality == Quality::Unknown);
    CHECK(result.assessments.empty());
  }

  TEST_CASE("QualityAnalyzer - float conversions distinguish lossless and lossy paths", "[audio][unit][quality]")
  {
    auto graph = buildBaseMergedGraph();

    SECTION("16-bit to 32-bit float is lossless")
    {
      graph.nodes[2].optFormat->bitDepth = 32;
      graph.nodes[2].optFormat->isFloat = true;
      graph.nodes[3].optFormat->bitDepth = 32;
      graph.nodes[3].optFormat->isFloat = true;
      graph.nodes[4].optFormat->bitDepth = 32;
      graph.nodes[4].optFormat->isFloat = true;

      auto const result = analyzeAudioQuality(graph);
      CHECK(result.quality == Quality::LosslessFloat);

      auto const* eng = findAssessment(result, "ao-engine");
      CHECK(hasFinding(eng, QualityFindingKind::LosslessFloat));
    }

    SECTION("32-bit integer to 32-bit float is lossy (mantissa truncation)")
    {
      graph.nodes[0].optFormat->bitDepth = 32; // source 32-bit int
      graph.nodes[1].optFormat->bitDepth = 32; // decoder 32-bit int
      graph.nodes[2].optFormat->bitDepth = 32;
      graph.nodes[2].optFormat->isFloat = true;
      graph.nodes[3].optFormat->bitDepth = 32;
      graph.nodes[3].optFormat->isFloat = true;
      graph.nodes[4].optFormat->bitDepth = 32;
      graph.nodes[4].optFormat->isFloat = true;

      auto const result = analyzeAudioQuality(graph);
      CHECK(result.quality == Quality::LinearIntervention);

      auto const* eng = findAssessment(result, "ao-engine");
      CHECK(hasFinding(eng, QualityFindingKind::Truncation));
    }

    SECTION("32-bit integer to 64-bit float is lossless")
    {
      graph.nodes[0].optFormat->bitDepth = 32; // source 32-bit int
      graph.nodes[1].optFormat->bitDepth = 32; // decoder 32-bit int
      graph.nodes[2].optFormat->bitDepth = 64;
      graph.nodes[2].optFormat->isFloat = true;
      graph.nodes[3].optFormat->bitDepth = 64;
      graph.nodes[3].optFormat->isFloat = true;
      graph.nodes[4].optFormat->bitDepth = 64;
      graph.nodes[4].optFormat->isFloat = true;

      auto const result = analyzeAudioQuality(graph);
      CHECK(result.quality == Quality::LosslessFloat);
    }
  }
} // namespace ao::audio::test
