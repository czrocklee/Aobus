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

      graph.connections.push_back(
        flow::Connection{.sourceId = "ao-source", .destinationId = "ao-decoder", .isActive = true});
      graph.connections.push_back(
        flow::Connection{.sourceId = "ao-decoder", .destinationId = "ao-engine", .isActive = true});

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

      graph.connections.push_back(
        flow::Connection{.sourceId = "ao-engine", .destinationId = "ao-stream", .isActive = true});
      graph.connections.push_back(
        flow::Connection{.sourceId = "ao-stream", .destinationId = "ao-sink", .isActive = true});

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

    QualityFinding const* findFinding(NodeQualityAssessment const* assessment, QualityFindingKind kind)
    {
      if (assessment == nullptr)
      {
        return nullptr;
      }

      auto const it = std::ranges::find(assessment->findings, kind, &QualityFinding::kind);
      return it != assessment->findings.end() ? &(*it) : nullptr;
    }
  } // namespace

  TEST_CASE("QualityAnalyzer - unchanged playback path is bitwise perfect", "[audio][unit][quality]")
  {
    auto const graph = buildBaseMergedGraph();
    auto const result = analyzeAudioQuality(graph);

    CHECK(result.overall == Quality::BitwisePerfect);
    CHECK(result.sourceQuality == Quality::BitwisePerfect);
    CHECK(result.pipelineQuality == Quality::BitwisePerfect);
    CHECK(result.fullyVerified == true);
    CHECK(result.assessments.size() == 5);

    for (auto const& assessment : result.assessments)
    {
      CHECK(assessment.optFormat);
      CHECK(assessment.worstQuality == Quality::BitwisePerfect);
      CHECK(hasFinding(&assessment, QualityFindingKind::BitPerfect));
    }
  }

  TEST_CASE("QualityAnalyzer - lossy source marks source as worst quality", "[audio][unit][quality]")
  {
    auto graph = buildBaseMergedGraph();
    graph.nodes[0].isLossySource = true; // ao-source

    auto const result = analyzeAudioQuality(graph);
    CHECK(result.overall == Quality::LossySource);
    CHECK(result.sourceQuality == Quality::LossySource);
    CHECK(result.pipelineQuality == Quality::BitwisePerfect);

    auto const* sourceAssessment = findAssessment(result, "ao-source");
    auto const* finding = findFinding(sourceAssessment, QualityFindingKind::LossySource);
    REQUIRE(finding != nullptr);
    CHECK(finding->quality == Quality::LossySource);
    CHECK(sourceAssessment->worstQuality == Quality::LossySource);
  }

  TEST_CASE("QualityAnalyzer - resampling marks engine as linear intervention", "[audio][unit][quality]")
  {
    auto graph = buildBaseMergedGraph();
    // Modify Engine output format to trigger resampling at Decoder -> Engine
    graph.nodes[2].optFormat->sampleRate = 48000;

    auto const result = analyzeAudioQuality(graph);
    CHECK(result.overall == Quality::LinearIntervention);
    CHECK(result.sourceQuality == Quality::BitwisePerfect);
    CHECK(result.pipelineQuality == Quality::LinearIntervention);

    auto const* eng = findAssessment(result, "ao-engine");
    auto const* finding = findFinding(eng, QualityFindingKind::Resampling);
    REQUIRE(finding != nullptr);
    CHECK(finding->quality == Quality::LinearIntervention);
    CHECK(eng->worstQuality == Quality::LinearIntervention);
  }

  TEST_CASE("QualityAnalyzer - lossy source and resampling are reported on separate axes", "[audio][unit][quality]")
  {
    auto graph = buildBaseMergedGraph();
    graph.nodes[0].isLossySource = true;          // ao-source
    graph.nodes[2].optFormat->sampleRate = 48000; // ao-engine

    auto const result = analyzeAudioQuality(graph);
    CHECK(result.sourceQuality == Quality::LossySource);
    CHECK(result.pipelineQuality == Quality::LinearIntervention);
    CHECK(result.overall == Quality::LossySource);

    auto const* sourceAssessment = findAssessment(result, "ao-source");
    CHECK(hasFinding(sourceAssessment, QualityFindingKind::LossySource));

    auto const* eng = findAssessment(result, "ao-engine");
    CHECK(hasFinding(eng, QualityFindingKind::Resampling));
  }

  TEST_CASE("QualityAnalyzer - software volume change marks linear intervention", "[audio][unit][quality]")
  {
    auto graph = buildBaseMergedGraph();
    graph.nodes[2].softwareVolumeNotUnity = true;
    graph.nodes[2].maxSoftwareGain = 0.5F;

    auto const result = analyzeAudioQuality(graph);
    CHECK(result.overall == Quality::LinearIntervention);

    auto const* eng = findAssessment(result, "ao-engine");
    auto const* finding = findFinding(eng, QualityFindingKind::SoftwareVolumeModification);
    REQUIRE(finding != nullptr);
    CHECK(finding->gain == 0.5F);
    CHECK(eng->worstQuality == Quality::LinearIntervention);
  }

  TEST_CASE("QualityAnalyzer - software amplification marks clipping-risk intervention", "[audio][unit][quality]")
  {
    auto graph = buildBaseMergedGraph();
    graph.nodes[2].softwareVolumeNotUnity = true;
    graph.nodes[2].maxSoftwareGain = 1.5F;

    auto const result = analyzeAudioQuality(graph);
    CHECK(result.overall == Quality::LinearIntervention);
    CHECK(result.pipelineQuality == Quality::LinearIntervention);

    auto const* eng = findAssessment(result, "ao-engine");
    auto const* finding = findFinding(eng, QualityFindingKind::SoftwareAmplification);
    REQUIRE(finding != nullptr);
    CHECK(finding->quality == Quality::LinearIntervention);
    CHECK(finding->gain == 1.5F);
    CHECK_FALSE(hasFinding(eng, QualityFindingKind::SoftwareVolumeModification));
  }

  TEST_CASE("QualityAnalyzer - hardware volume change keeps bitwise quality", "[audio][unit][quality]")
  {
    auto graph = buildBaseMergedGraph();
    graph.nodes[2].hardwareVolumeNotUnity = true;

    auto const result = analyzeAudioQuality(graph);
    CHECK(result.overall == Quality::BitwisePerfect);

    auto const* eng = findAssessment(result, "ao-engine");
    CHECK(hasFinding(eng, QualityFindingKind::HardwareVolumeModification));
    CHECK(eng->worstQuality == Quality::BitwisePerfect);
  }

  TEST_CASE("QualityAnalyzer - unclassified volume change marks linear intervention", "[audio][unit][quality]")
  {
    auto graph = buildBaseMergedGraph();
    graph.nodes[2].unclassifiedVolumeNotUnity = true;

    auto const result = analyzeAudioQuality(graph);
    CHECK(result.overall == Quality::LinearIntervention);

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
    CHECK(result.overall == Quality::LinearIntervention);

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
    CHECK(result.overall == Quality::LinearIntervention);

    auto const* strm = findAssessment(result, "ao-stream");
    CHECK(hasFinding(strm, QualityFindingKind::Muted));
    CHECK(strm->worstQuality == Quality::LinearIntervention);
  }

  TEST_CASE("QualityAnalyzer - external mixing reports shared applications", "[audio][unit][quality]")
  {
    auto graph = buildBaseMergedGraph();

    // Add an external source mixing into the sink
    graph.nodes.push_back(flow::Node{.id = "external-app", .type = flow::NodeType::ExternalSource, .name = "Firefox"});

    graph.connections.push_back(
      flow::Connection{.sourceId = "external-app", .destinationId = "ao-sink", .isActive = true});

    auto const result = analyzeAudioQuality(graph);
    CHECK(result.overall == Quality::LinearIntervention);

    auto const* sink = findAssessment(result, "ao-sink");
    CHECK(hasFinding(sink, QualityFindingKind::MixedSources));
    CHECK(sink->worstQuality == Quality::LinearIntervention);

    auto const findingIt =
      std::ranges::find_if(sink->findings, [](auto const& f) { return f.kind == QualityFindingKind::MixedSources; });
    CHECK(findingIt != sink->findings.end());
    CHECK(std::ranges::contains(findingIt->sharedApps, std::string{"Firefox"}));
  }

  TEST_CASE("QualityAnalyzer - external mixing without app name remains explicit", "[audio][unit][quality]")
  {
    auto graph = buildBaseMergedGraph();

    graph.nodes.push_back(flow::Node{.id = "external-app", .type = flow::NodeType::ExternalSource});
    graph.connections.push_back(
      flow::Connection{.sourceId = "external-app", .destinationId = "ao-sink", .isActive = true});

    auto const result = analyzeAudioQuality(graph);
    CHECK(result.overall == Quality::LinearIntervention);

    auto const* sink = findAssessment(result, "ao-sink");
    REQUIRE(sink != nullptr);
    auto const* finding = findFinding(sink, QualityFindingKind::MixedSources);
    REQUIRE(finding != nullptr);
    CHECK(finding->sharedApps.empty());
  }

  TEST_CASE("QualityAnalyzer - lossless bit-depth extension reports padded quality", "[audio][unit][quality]")
  {
    auto graph = buildBaseMergedGraph();
    // decoder 16b -> engine 24b -> stream 24b -> sink 24b
    graph.nodes[2].optFormat->bitDepth = 24;
    graph.nodes[3].optFormat->bitDepth = 24;
    graph.nodes[4].optFormat->bitDepth = 24;

    auto const result = analyzeAudioQuality(graph);
    CHECK(result.overall == Quality::LosslessPadded);

    auto const* eng = findAssessment(result, "ao-engine");
    CHECK(hasFinding(eng, QualityFindingKind::LosslessPadding));
    CHECK(eng->worstQuality == Quality::LosslessPadded);
  }

  TEST_CASE("QualityAnalyzer - source padded into wider container", "[audio][unit][quality]")
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
    CHECK(result.overall == Quality::LosslessPadded);

    // The padding is attributed to the decoder (destination of source -> decoder),
    // not the source itself.
    auto const* dec = findAssessment(result, "ao-decoder");
    CHECK(hasFinding(dec, QualityFindingKind::LosslessPadding));

    auto const* src = findAssessment(result, "ao-source");
    CHECK(hasFinding(src, QualityFindingKind::BitPerfect));
  }

  TEST_CASE("QualityAnalyzer - valid-bit-only precision loss reports truncation", "[audio][unit][quality]")
  {
    auto graph = buildBaseMergedGraph();

    for (auto& node : graph.nodes)
    {
      if (node.optFormat)
      {
        node.optFormat->bitDepth = 32;
        node.optFormat->validBits = 32;
      }
    }

    graph.nodes[2].optFormat->validBits = 24; // engine
    graph.nodes[3].optFormat->validBits = 24; // stream
    graph.nodes[4].optFormat->validBits = 24; // sink

    auto const result = analyzeAudioQuality(graph);
    CHECK(result.overall == Quality::LinearIntervention);

    auto const* eng = findAssessment(result, "ao-engine");
    REQUIRE(eng != nullptr);
    CHECK(hasFinding(eng, QualityFindingKind::Truncation));
    CHECK(eng->worstQuality == Quality::LinearIntervention);
  }

  TEST_CASE("QualityAnalyzer - valid-bit-only precision widening reports padding", "[audio][unit][quality]")
  {
    auto graph = buildBaseMergedGraph();

    for (auto& node : graph.nodes)
    {
      if (node.optFormat)
      {
        node.optFormat->bitDepth = 32;
        node.optFormat->validBits = 24;
      }
    }

    graph.nodes[2].optFormat->validBits = 32; // engine
    graph.nodes[3].optFormat->validBits = 32; // stream
    graph.nodes[4].optFormat->validBits = 32; // sink

    auto const result = analyzeAudioQuality(graph);
    CHECK(result.overall == Quality::LosslessPadded);

    auto const* eng = findAssessment(result, "ao-engine");
    REQUIRE(eng != nullptr);
    CHECK(hasFinding(eng, QualityFindingKind::LosslessPadding));
    CHECK(eng->worstQuality == Quality::LosslessPadded);
  }

  TEST_CASE("QualityAnalyzer - unknown valid bits do not create precision findings", "[audio][unit][quality]")
  {
    auto graph = buildBaseMergedGraph();

    for (auto& node : graph.nodes)
    {
      if (node.optFormat)
      {
        node.optFormat->bitDepth = 32;
        node.optFormat->validBits = 0;
      }
    }

    graph.nodes[2].optFormat->validBits = 24; // engine
    graph.nodes[3].optFormat->validBits = 24; // stream
    graph.nodes[4].optFormat->validBits = 24; // sink

    auto const result = analyzeAudioQuality(graph);
    CHECK(result.overall == Quality::BitwisePerfect);

    auto const* eng = findAssessment(result, "ao-engine");
    REQUIRE(eng != nullptr);
    CHECK(hasFinding(eng, QualityFindingKind::BitPerfect));
    CHECK_FALSE(hasFinding(eng, QualityFindingKind::Truncation));
    CHECK_FALSE(hasFinding(eng, QualityFindingKind::LosslessPadding));
  }

  TEST_CASE("QualityAnalyzer - missing downstream format marks path unverified without fabricated transitions",
            "[audio][unit][quality]")
  {
    auto graph = buildBaseMergedGraph();
    graph.nodes[3].optFormat.reset(); // stream
    graph.nodes[4].optFormat->sampleRate = 48000;
    graph.nodes[4].optFormat->bitDepth = 24;

    auto const result = analyzeAudioQuality(graph);
    CHECK(result.fullyVerified == false);
    CHECK(result.sourceQuality == Quality::BitwisePerfect);
    CHECK(result.pipelineQuality == Quality::BitwisePerfect);
    CHECK(result.overall == Quality::BitwisePerfect);

    auto const* stream = findAssessment(result, "ao-stream");
    REQUIRE(stream != nullptr);
    CHECK_FALSE(stream->optFormat);
    CHECK(hasFinding(stream, QualityFindingKind::BitPerfect));

    auto const* sink = findAssessment(result, "ao-sink");
    REQUIRE(sink != nullptr);
    CHECK(hasFinding(sink, QualityFindingKind::BitPerfect));
    CHECK_FALSE(hasFinding(sink, QualityFindingKind::Resampling));
    CHECK_FALSE(hasFinding(sink, QualityFindingKind::Truncation));
    CHECK_FALSE(hasFinding(sink, QualityFindingKind::LosslessPadding));
  }

  TEST_CASE("QualityAnalyzer - path that does not reach a sink is not fully verified", "[audio][unit][quality]")
  {
    auto graph = buildBaseMergedGraph();
    graph.nodes.resize(3);
    graph.connections.resize(2);

    auto const result = analyzeAudioQuality(graph);

    CHECK(result.overall == Quality::BitwisePerfect);
    CHECK(result.fullyVerified == false);
    REQUIRE(result.assessments.size() == 3);
    CHECK(result.assessments.back().nodeId == "ao-engine");
  }

  TEST_CASE("QualityAnalyzer - known ALSA padded endpoint remains verified", "[audio][unit][quality]")
  {
    auto graph = buildBaseMergedGraph();
    auto const alsaFormat =
      Format{.sampleRate = 44100, .channels = 2, .bitDepth = 32, .validBits = 16, .isFloat = false};

    graph.nodes[3].id = "alsa-stream";
    graph.nodes[3].type = flow::NodeType::Stream;
    graph.nodes[3].name = "ALSA Stream";
    graph.nodes[3].optFormat = alsaFormat;
    graph.nodes[4].id = "alsa-sink";
    graph.nodes[4].type = flow::NodeType::Sink;
    graph.nodes[4].name = "hw:1,0";
    graph.nodes[4].optFormat = alsaFormat;
    graph.connections[2].destinationId = "alsa-stream";
    graph.connections[3].sourceId = "alsa-stream";
    graph.connections[3].destinationId = "alsa-sink";

    auto const result = analyzeAudioQuality(graph);
    CHECK(result.fullyVerified == true);
    CHECK(result.overall == Quality::LosslessPadded);

    auto const* stream = findAssessment(result, "alsa-stream");
    REQUIRE(stream != nullptr);
    CHECK(hasFinding(stream, QualityFindingKind::LosslessPadding));

    auto const* sink = findAssessment(result, "alsa-sink");
    REQUIRE(sink != nullptr);
    CHECK(hasFinding(sink, QualityFindingKind::BitPerfect));
  }

  TEST_CASE("QualityAnalyzer - combined channel and depth changes keep both findings", "[audio][unit][quality]")
  {
    auto graph = buildBaseMergedGraph();

    graph.nodes[2].optFormat->channels = 1; // engine
    graph.nodes[2].optFormat->bitDepth = 8;
    graph.nodes[2].optFormat->validBits = 8;
    graph.nodes[3].optFormat->channels = 1; // stream
    graph.nodes[3].optFormat->bitDepth = 8;
    graph.nodes[3].optFormat->validBits = 8;
    graph.nodes[4].optFormat->channels = 1; // sink
    graph.nodes[4].optFormat->bitDepth = 8;
    graph.nodes[4].optFormat->validBits = 8;

    auto const result = analyzeAudioQuality(graph);
    CHECK(result.overall == Quality::LinearIntervention);

    auto const* eng = findAssessment(result, "ao-engine");
    REQUIRE(eng != nullptr);
    CHECK(hasFinding(eng, QualityFindingKind::ChannelMapping));
    CHECK(hasFinding(eng, QualityFindingKind::Truncation));
    CHECK(eng->worstQuality == Quality::LinearIntervention);
  }

  TEST_CASE("QualityAnalyzer - empty graph has unknown quality", "[audio][unit][quality]")
  {
    auto const graph = flow::Graph{};
    auto const result = analyzeAudioQuality(graph);
    CHECK(result.overall == Quality::Unknown);
    CHECK(result.assessments.empty());
  }

  TEST_CASE("QualityAnalyzer - missing playback path has unknown quality", "[audio][unit][quality]")
  {
    auto graph = flow::Graph{};
    graph.nodes.push_back(flow::Node{.id = "external-app", .type = flow::NodeType::ExternalSource, .name = "Firefox"});

    auto const result = analyzeAudioQuality(graph);
    CHECK(result.overall == Quality::Unknown);
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
      CHECK(result.overall == Quality::LosslessFloat);

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
      CHECK(result.overall == Quality::LinearIntervention);

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
      CHECK(result.overall == Quality::LosslessFloat);
    }
  }

  TEST_CASE("QualityAnalyzer - clean float engine round trip preserves the signal", "[audio][unit][quality]")
  {
    auto graph = buildBaseMergedGraph();
    graph.nodes[2].optFormat =
      Format{.sampleRate = 44100, .channels = 2, .bitDepth = 32, .validBits = 32, .isFloat = true};
    graph.nodes[3].optFormat =
      Format{.sampleRate = 44100, .channels = 2, .bitDepth = 32, .validBits = 32, .isFloat = true};
    graph.nodes[4].optFormat =
      Format{.sampleRate = 44100, .channels = 2, .bitDepth = 16, .validBits = 16, .isFloat = false};

    auto const result = analyzeAudioQuality(graph);
    CHECK(result.sourceQuality == Quality::BitwisePerfect);
    CHECK(result.pipelineQuality == Quality::LosslessFloat);
    CHECK(result.overall == Quality::LosslessFloat);

    auto const* sink = findAssessment(result, "ao-sink");
    auto const* roundTrip = findFinding(sink, QualityFindingKind::LosslessRoundTrip);
    REQUIRE(roundTrip != nullptr);
    CHECK(roundTrip->quality == Quality::LosslessFloat);
    CHECK_FALSE(hasFinding(sink, QualityFindingKind::Truncation));
  }

  TEST_CASE("QualityAnalyzer - float source to integer output is quantization", "[audio][unit][quality]")
  {
    auto graph = buildBaseMergedGraph();
    graph.nodes[0].optFormat =
      Format{.sampleRate = 44100, .channels = 2, .bitDepth = 32, .validBits = 32, .isFloat = true};
    graph.nodes[1].optFormat =
      Format{.sampleRate = 44100, .channels = 2, .bitDepth = 32, .validBits = 32, .isFloat = true};
    graph.nodes[2].optFormat =
      Format{.sampleRate = 44100, .channels = 2, .bitDepth = 32, .validBits = 32, .isFloat = true};
    graph.nodes[3].optFormat =
      Format{.sampleRate = 44100, .channels = 2, .bitDepth = 32, .validBits = 32, .isFloat = true};
    graph.nodes[4].optFormat =
      Format{.sampleRate = 44100, .channels = 2, .bitDepth = 32, .validBits = 32, .isFloat = false};

    auto const result = analyzeAudioQuality(graph);

    CHECK(result.pipelineQuality == Quality::LinearIntervention);
    auto const* sink = findAssessment(result, "ao-sink");
    CHECK(hasFinding(sink, QualityFindingKind::Truncation));
    CHECK_FALSE(hasFinding(sink, QualityFindingKind::LosslessRoundTrip));
  }

  TEST_CASE("QualityAnalyzer - asymmetric software attenuation reports the attenuated channel",
            "[audio][unit][quality]")
  {
    auto graph = buildBaseMergedGraph();
    graph.nodes[3].softwareVolumeNotUnity = true;
    graph.nodes[3].maxSoftwareGain = 1.0F;
    graph.nodes[3].minSoftwareGain = 0.5F;

    auto const result = analyzeAudioQuality(graph);

    auto const* stream = findAssessment(result, "ao-stream");
    auto const* finding = findFinding(stream, QualityFindingKind::SoftwareVolumeModification);
    REQUIRE(finding != nullptr);
    CHECK(finding->gain == 0.5F);
  }

  TEST_CASE("QualityAnalyzer - interventions invalidate float round trip proof", "[audio][unit][quality]")
  {
    auto graph = buildBaseMergedGraph();
    graph.nodes[2].optFormat =
      Format{.sampleRate = 44100, .channels = 2, .bitDepth = 32, .validBits = 32, .isFloat = true};
    graph.nodes[3].optFormat =
      Format{.sampleRate = 44100, .channels = 2, .bitDepth = 32, .validBits = 32, .isFloat = true};
    graph.nodes[4].optFormat =
      Format{.sampleRate = 44100, .channels = 2, .bitDepth = 16, .validBits = 16, .isFloat = false};

    SECTION("software volume invalidates the proof")
    {
      graph.nodes[3].softwareVolumeNotUnity = true;

      auto const result = analyzeAudioQuality(graph);
      CHECK(result.pipelineQuality == Quality::LinearIntervention);

      auto const* sink = findAssessment(result, "ao-sink");
      CHECK(hasFinding(sink, QualityFindingKind::Truncation));
      CHECK_FALSE(hasFinding(sink, QualityFindingKind::LosslessRoundTrip));
    }

    SECTION("software amplification invalidates the proof")
    {
      graph.nodes[3].softwareVolumeNotUnity = true;
      graph.nodes[3].maxSoftwareGain = 1.5F;

      auto const result = analyzeAudioQuality(graph);
      CHECK(result.pipelineQuality == Quality::LinearIntervention);

      auto const* stream = findAssessment(result, "ao-stream");
      CHECK(hasFinding(stream, QualityFindingKind::SoftwareAmplification));

      auto const* sink = findAssessment(result, "ao-sink");
      CHECK(hasFinding(sink, QualityFindingKind::Truncation));
      CHECK_FALSE(hasFinding(sink, QualityFindingKind::LosslessRoundTrip));
    }

    SECTION("resampling invalidates the proof")
    {
      graph.nodes[3].optFormat->sampleRate = 48000;
      graph.nodes[4].optFormat->sampleRate = 48000;

      auto const result = analyzeAudioQuality(graph);
      CHECK(result.pipelineQuality == Quality::LinearIntervention);

      auto const* sink = findAssessment(result, "ao-sink");
      CHECK(hasFinding(sink, QualityFindingKind::Truncation));
      CHECK_FALSE(hasFinding(sink, QualityFindingKind::LosslessRoundTrip));
    }
  }

  TEST_CASE("QualityAnalyzer - unknown intermediate format invalidates float round trip proof",
            "[audio][unit][quality]")
  {
    auto graph = buildBaseMergedGraph();
    graph.nodes[1].optFormat.reset(); // decoder
    graph.nodes[2].optFormat =
      Format{.sampleRate = 44100, .channels = 2, .bitDepth = 32, .validBits = 32, .isFloat = true};
    graph.nodes[3].optFormat =
      Format{.sampleRate = 44100, .channels = 2, .bitDepth = 32, .validBits = 32, .isFloat = true};
    graph.nodes[4].optFormat =
      Format{.sampleRate = 44100, .channels = 2, .bitDepth = 16, .validBits = 16, .isFloat = false};

    auto const result = analyzeAudioQuality(graph);
    CHECK(result.pipelineQuality == Quality::LinearIntervention);

    auto const* sink = findAssessment(result, "ao-sink");
    CHECK(hasFinding(sink, QualityFindingKind::Truncation));
    CHECK_FALSE(hasFinding(sink, QualityFindingKind::LosslessRoundTrip));
  }
} // namespace ao::audio::test
