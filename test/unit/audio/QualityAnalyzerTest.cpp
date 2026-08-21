// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/audio/QualityAnalyzer.h>

#include <ao/audio/PcmFormat.h>
#include <ao/audio/Quality.h>
#include <ao/audio/SampleEncoding.h>
#include <ao/audio/SignalFormat.h>
#include <ao/audio/flow/Graph.h>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <variant>

namespace ao::audio::test
{
  namespace
  {
    SignalFormat integerSignal(std::uint8_t precisionBits = 16,
                               std::uint32_t sampleRate = 44100,
                               std::uint8_t channels = 2)
    {
      return {.sampleRate = sampleRate,
              .channels = channels,
              .precisionBits = precisionBits,
              .sampleKind = SampleKind::Integer};
    }

    PcmFormat pcm(SampleEncoding encoding = SampleEncoding::Signed16Le,
                  std::uint32_t sampleRate = 44100,
                  std::uint8_t channels = 2)
    {
      return {.sampleRate = sampleRate, .channels = channels, .encoding = encoding};
    }

    flow::Graph buildBaseMergedGraph()
    {
      return flow::Graph{
        .nodes =
          {
            {.id = "ao-source",
             .type = flow::NodeType::Source,
             .name = "Source",
             .optFormat = integerSignal(),
             .isLossySource = false},
            {.id = "ao-decoder", .type = flow::NodeType::Decoder, .name = "Decoder", .optFormat = pcm()},
            {.id = "ao-engine", .type = flow::NodeType::Engine, .name = "Engine", .optFormat = pcm()},
            {.id = "ao-stream", .type = flow::NodeType::Stream, .name = "Stream", .optFormat = pcm()},
            {.id = "ao-sink", .type = flow::NodeType::Sink, .name = "Sink", .optFormat = pcm()},
          },
        .connections =
          {
            {.sourceId = "ao-source", .destinationId = "ao-decoder", .isActive = true},
            {.sourceId = "ao-decoder", .destinationId = "ao-engine", .isActive = true},
            {.sourceId = "ao-engine", .destinationId = "ao-stream", .isActive = true},
            {.sourceId = "ao-stream", .destinationId = "ao-sink", .isActive = true},
          },
      };
    }

    PcmFormat& pcmAt(flow::Graph& graph, std::size_t index)
    {
      return std::get<PcmFormat>(*graph.nodes[index].optFormat);
    }

    SignalFormat& signalAt(flow::Graph& graph, std::size_t index)
    {
      return std::get<SignalFormat>(*graph.nodes[index].optFormat);
    }

    void setSampleRate(flow::Node& node, std::uint32_t sampleRate)
    {
      std::visit([sampleRate](auto& format) { format.sampleRate = sampleRate; }, *node.optFormat);
    }

    NodeQualityAssessment const* findAssessment(QualityResult const& result, std::string_view id)
    {
      auto const it = std::ranges::find(result.assessments, id, &NodeQualityAssessment::nodeId);
      return it != result.assessments.end() ? &(*it) : nullptr;
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

    bool hasFinding(NodeQualityAssessment const* assessment, QualityFindingKind kind)
    {
      return findFinding(assessment, kind) != nullptr;
    }
  } // namespace

  TEST_CASE("QualityAnalyzer - unchanged playback path is bitwise perfect", "[audio][unit][quality]")
  {
    auto const result = analyzeAudioQuality(buildBaseMergedGraph());

    CHECK(result.overall == Quality::BitwisePerfect);
    CHECK(result.sourceQuality == Quality::BitwisePerfect);
    CHECK(result.pipelineQuality == Quality::BitwisePerfect);
    CHECK(result.fullyVerified);
    REQUIRE(result.assessments.size() == 5U);

    for (auto const& assessment : result.assessments)
    {
      CHECK(assessment.optFormat);
      CHECK(hasFinding(&assessment, QualityFindingKind::BitPerfect));
    }
  }

  TEST_CASE("QualityAnalyzer - source and pipeline quality remain separate", "[audio][unit][quality]")
  {
    auto graph = buildBaseMergedGraph();
    graph.nodes[0].isLossySource = true;
    setSampleRate(graph.nodes[2], 48000);

    auto const result = analyzeAudioQuality(graph);

    CHECK(result.sourceQuality == Quality::LossySource);
    CHECK(result.pipelineQuality == Quality::LinearIntervention);
    CHECK(result.overall == Quality::LossySource);
    CHECK(hasFinding(findAssessment(result, "ao-source"), QualityFindingKind::LossySource));
    CHECK(hasFinding(findAssessment(result, "ao-engine"), QualityFindingKind::Resampling));
  }

  TEST_CASE("QualityAnalyzer - volume observations retain their intervention class", "[audio][unit][quality]")
  {
    auto graph = buildBaseMergedGraph();

    SECTION("software attenuation")
    {
      graph.nodes[2].softwareVolumeNotUnity = true;
      graph.nodes[2].minSoftwareGain = 0.5F;
      graph.nodes[2].maxSoftwareGain = 0.5F;

      auto const result = analyzeAudioQuality(graph);
      auto const* finding =
        findFinding(findAssessment(result, "ao-engine"), QualityFindingKind::SoftwareVolumeModification);
      REQUIRE(finding != nullptr);
      CHECK(finding->gain == 0.5F);
      CHECK(result.pipelineQuality == Quality::LinearIntervention);
    }

    SECTION("software amplification")
    {
      graph.nodes[2].softwareVolumeNotUnity = true;
      graph.nodes[2].minSoftwareGain = 0.5F;
      graph.nodes[2].maxSoftwareGain = 1.5F;

      auto const result = analyzeAudioQuality(graph);
      auto const* finding = findFinding(findAssessment(result, "ao-engine"), QualityFindingKind::SoftwareAmplification);
      REQUIRE(finding != nullptr);
      CHECK(finding->gain == 1.5F);
      CHECK(result.pipelineQuality == Quality::LinearIntervention);
    }

    SECTION("hardware volume")
    {
      graph.nodes[2].hardwareVolumeNotUnity = true;

      auto const result = analyzeAudioQuality(graph);
      CHECK(hasFinding(findAssessment(result, "ao-engine"), QualityFindingKind::HardwareVolumeModification));
      CHECK(result.pipelineQuality == Quality::BitwisePerfect);
    }

    SECTION("unclassified attenuation carries the positive minimum gain")
    {
      graph.nodes[2].unclassifiedVolumeNotUnity = true;
      graph.nodes[2].minUnclassifiedGain = 0.5F;
      graph.nodes[2].maxUnclassifiedGain = 1.0F;

      auto const result = analyzeAudioQuality(graph);
      auto const* finding =
        findFinding(findAssessment(result, "ao-engine"), QualityFindingKind::UnclassifiedVolumeModification);
      REQUIRE(finding != nullptr);
      CHECK(finding->gain == 0.5F);
      CHECK(result.pipelineQuality == Quality::LinearIntervention);
    }

    SECTION("unclassified amplification keeps its kind and never becomes software amplification")
    {
      graph.nodes[2].unclassifiedVolumeNotUnity = true;
      graph.nodes[2].minUnclassifiedGain = 0.5F;
      graph.nodes[2].maxUnclassifiedGain = 1.5F;

      auto const result = analyzeAudioQuality(graph);
      auto const& assessment = findAssessment(result, "ao-engine");
      auto const* finding = findFinding(assessment, QualityFindingKind::UnclassifiedVolumeModification);
      REQUIRE(finding != nullptr);
      CHECK(finding->gain == 1.5F);
      CHECK_FALSE(hasFinding(assessment, QualityFindingKind::SoftwareAmplification));
      CHECK(result.pipelineQuality == Quality::LinearIntervention);
    }

    SECTION("software and unclassified observations produce independent findings")
    {
      graph.nodes[2].softwareVolumeNotUnity = true;
      graph.nodes[2].minSoftwareGain = 0.5F;
      graph.nodes[2].maxSoftwareGain = 0.5F;
      graph.nodes[2].unclassifiedVolumeNotUnity = true;
      graph.nodes[2].minUnclassifiedGain = 0.5F;
      graph.nodes[2].maxUnclassifiedGain = 1.5F;

      auto const result = analyzeAudioQuality(graph);
      auto const& assessment = findAssessment(result, "ao-engine");
      auto const* softwareFinding = findFinding(assessment, QualityFindingKind::SoftwareVolumeModification);
      auto const* unclassifiedFinding = findFinding(assessment, QualityFindingKind::UnclassifiedVolumeModification);
      REQUIRE(softwareFinding != nullptr);
      REQUIRE(unclassifiedFinding != nullptr);
      CHECK(softwareFinding->gain == 0.5F);
      CHECK(unclassifiedFinding->gain == 1.5F);
      CHECK(result.pipelineQuality == Quality::LinearIntervention);
    }

    SECTION("unclassified observation without a range carries no gain")
    {
      graph.nodes[2].unclassifiedVolumeNotUnity = true;

      auto const result = analyzeAudioQuality(graph);
      auto const* finding =
        findFinding(findAssessment(result, "ao-engine"), QualityFindingKind::UnclassifiedVolumeModification);
      REQUIRE(finding != nullptr);
      CHECK(finding->gain == 0.0F);
      CHECK(result.pipelineQuality == Quality::LinearIntervention);
    }

    SECTION("mute")
    {
      graph.nodes[3].isMuted = true;

      auto const result = analyzeAudioQuality(graph);
      CHECK(hasFinding(findAssessment(result, "ao-stream"), QualityFindingKind::Muted));
      CHECK(result.pipelineQuality == Quality::LinearIntervention);
    }
  }

  TEST_CASE("QualityAnalyzer - external mixing reports shared applications", "[audio][unit][quality]")
  {
    auto graph = buildBaseMergedGraph();
    graph.nodes.push_back(flow::Node{.id = "external-app", .type = flow::NodeType::ExternalSource, .name = "Firefox"});
    graph.connections.push_back(
      flow::Connection{.sourceId = "external-app", .destinationId = "ao-sink", .isActive = true});

    auto const result = analyzeAudioQuality(graph);
    auto const* finding = findFinding(findAssessment(result, "ao-sink"), QualityFindingKind::MixedSources);

    REQUIRE(finding != nullptr);
    CHECK(std::ranges::contains(finding->sharedApps, std::string{"Firefox"}));
    CHECK(result.pipelineQuality == Quality::LinearIntervention);
  }

  TEST_CASE("QualityAnalyzer - wider PCM containers report lossless padding", "[audio][unit][quality]")
  {
    auto graph = buildBaseMergedGraph();

    for (std::size_t index = 2; index < graph.nodes.size(); ++index)
    {
      pcmAt(graph, index).encoding = SampleEncoding::Signed24In32Le;
    }

    auto const result = analyzeAudioQuality(graph);

    CHECK(result.overall == Quality::LosslessPadded);
    CHECK(hasFinding(findAssessment(result, "ao-engine"), QualityFindingKind::LosslessPadding));
    CHECK(hasFinding(findAssessment(result, "ao-stream"), QualityFindingKind::BitPerfect));
  }

  TEST_CASE("QualityAnalyzer - source padding is attributed to the decoder", "[audio][unit][quality]")
  {
    auto graph = buildBaseMergedGraph();

    for (std::size_t index = 1; index < graph.nodes.size(); ++index)
    {
      pcmAt(graph, index).encoding = SampleEncoding::Signed32Le;
    }

    auto const result = analyzeAudioQuality(graph);

    CHECK(result.overall == Quality::LosslessPadded);
    CHECK(hasFinding(findAssessment(result, "ao-decoder"), QualityFindingKind::LosslessPadding));
    CHECK(hasFinding(findAssessment(result, "ao-source"), QualityFindingKind::BitPerfect));
  }

  TEST_CASE("QualityAnalyzer - precision loss reports truncation", "[audio][unit][quality]")
  {
    auto graph = buildBaseMergedGraph();
    signalAt(graph, 0).precisionBits = 32;
    pcmAt(graph, 1) = pcm(SampleEncoding::Signed32Le);

    for (std::size_t index = 2; index < graph.nodes.size(); ++index)
    {
      pcmAt(graph, index) = pcm(SampleEncoding::Signed24PackedLe);
    }

    auto const result = analyzeAudioQuality(graph);

    CHECK(result.overall == Quality::LinearIntervention);
    CHECK(hasFinding(findAssessment(result, "ao-engine"), QualityFindingKind::Truncation));
  }

  TEST_CASE("QualityAnalyzer - channel mapping and truncation remain distinct findings", "[audio][unit][quality]")
  {
    auto graph = buildBaseMergedGraph();
    signalAt(graph, 0).precisionBits = 24;
    pcmAt(graph, 1).encoding = SampleEncoding::Signed24PackedLe;

    for (std::size_t index = 2; index < graph.nodes.size(); ++index)
    {
      pcmAt(graph, index).channels = 1;
      pcmAt(graph, index).encoding = SampleEncoding::Signed16Le;
    }

    auto const result = analyzeAudioQuality(graph);
    auto const* engine = findAssessment(result, "ao-engine");

    CHECK(hasFinding(engine, QualityFindingKind::ChannelMapping));
    CHECK(hasFinding(engine, QualityFindingKind::Truncation));
  }

  TEST_CASE("QualityAnalyzer - integer to float conversion uses retained signal precision", "[audio][unit][quality]")
  {
    auto graph = buildBaseMergedGraph();

    SECTION("16-bit integer to float32 is lossless")
    {
      for (std::size_t index = 2; index < graph.nodes.size(); ++index)
      {
        pcmAt(graph, index) = pcm(SampleEncoding::Float32Le);
      }

      auto const result = analyzeAudioQuality(graph);
      CHECK(result.overall == Quality::LosslessFloat);
      CHECK(hasFinding(findAssessment(result, "ao-engine"), QualityFindingKind::LosslessFloat));
    }

    SECTION("32-bit integer to float32 loses precision")
    {
      signalAt(graph, 0).precisionBits = 32;
      pcmAt(graph, 1) = pcm(SampleEncoding::Signed32Le);

      for (std::size_t index = 2; index < graph.nodes.size(); ++index)
      {
        pcmAt(graph, index) = pcm(SampleEncoding::Float32Le);
      }

      auto const result = analyzeAudioQuality(graph);
      CHECK(result.overall == Quality::LinearIntervention);
      CHECK(hasFinding(findAssessment(result, "ao-engine"), QualityFindingKind::Truncation));
    }
  }

  TEST_CASE("QualityAnalyzer - clean float round trip preserves proven integer precision", "[audio][unit][quality]")
  {
    auto graph = buildBaseMergedGraph();
    pcmAt(graph, 2) = pcm(SampleEncoding::Float32Le);
    pcmAt(graph, 3) = pcm(SampleEncoding::Float32Le);

    auto const result = analyzeAudioQuality(graph);
    auto const* sink = findAssessment(result, "ao-sink");

    CHECK(result.pipelineQuality == Quality::LosslessFloat);
    CHECK(hasFinding(sink, QualityFindingKind::LosslessRoundTrip));
    CHECK_FALSE(hasFinding(sink, QualityFindingKind::Truncation));
  }

  TEST_CASE("QualityAnalyzer - interventions invalidate float round-trip proof", "[audio][unit][quality]")
  {
    auto graph = buildBaseMergedGraph();
    pcmAt(graph, 2) = pcm(SampleEncoding::Float32Le);
    pcmAt(graph, 3) = pcm(SampleEncoding::Float32Le);
    graph.nodes[3].softwareVolumeNotUnity = true;

    auto const result = analyzeAudioQuality(graph);
    auto const* sink = findAssessment(result, "ao-sink");

    CHECK(result.pipelineQuality == Quality::LinearIntervention);
    CHECK(hasFinding(sink, QualityFindingKind::Truncation));
    CHECK_FALSE(hasFinding(sink, QualityFindingKind::LosslessRoundTrip));
  }

  TEST_CASE("QualityAnalyzer - native float source quantized to integer is truncation", "[audio][unit][quality]")
  {
    auto graph = buildBaseMergedGraph();
    graph.nodes[0].optFormat =
      SignalFormat{.sampleRate = 44100, .channels = 2, .precisionBits = 32, .sampleKind = SampleKind::FloatingPoint};

    for (std::size_t index = 1; index < 4; ++index)
    {
      pcmAt(graph, index) = pcm(SampleEncoding::Float32Le);
    }

    pcmAt(graph, 4) = pcm(SampleEncoding::Signed32Le);

    auto const result = analyzeAudioQuality(graph);

    CHECK(result.pipelineQuality == Quality::LinearIntervention);
    CHECK(hasFinding(findAssessment(result, "ao-sink"), QualityFindingKind::Truncation));
  }

  // The ALSA sink reports the precision its converter resolves, which may be
  // narrower than the container feeding it. Narrowing back to bits that were
  // only padded on the way in returns the original samples.
  TEST_CASE("QualityAnalyzer - narrowing back to proven integer precision is lossless", "[audio][regression][quality]")
  {
    auto graph = buildBaseMergedGraph();
    signalAt(graph, 0).precisionBits = 24;
    pcmAt(graph, 1) = pcm(SampleEncoding::Signed24PackedLe);
    pcmAt(graph, 2) = pcm(SampleEncoding::Signed32Le);
    pcmAt(graph, 3) = pcm(SampleEncoding::Signed32Le);
    graph.nodes[4].optFormat = integerSignal(24);

    auto const result = analyzeAudioQuality(graph);

    CHECK(result.overall == Quality::LosslessPadded);
    CHECK(hasFinding(findAssessment(result, "ao-engine"), QualityFindingKind::LosslessPadding));
    CHECK(hasFinding(findAssessment(result, "ao-sink"), QualityFindingKind::LosslessRoundTrip));
    CHECK_FALSE(hasFinding(findAssessment(result, "ao-sink"), QualityFindingKind::Truncation));
  }

  TEST_CASE("QualityAnalyzer - an endpoint narrower than the source reports truncation", "[audio][regression][quality]")
  {
    // Same 32-bit container, but now the source really has 32 bits, so the
    // endpoint discards eight of them.
    auto graph = buildBaseMergedGraph();
    signalAt(graph, 0).precisionBits = 32;
    pcmAt(graph, 1) = pcm(SampleEncoding::Signed32Le);
    pcmAt(graph, 2) = pcm(SampleEncoding::Signed32Le);
    pcmAt(graph, 3) = pcm(SampleEncoding::Signed32Le);
    graph.nodes[4].optFormat = integerSignal(24);

    auto const result = analyzeAudioQuality(graph);

    CHECK(result.overall == Quality::LinearIntervention);
    CHECK(hasFinding(findAssessment(result, "ao-sink"), QualityFindingKind::Truncation));
  }

  TEST_CASE("QualityAnalyzer - a reduced endpoint after full-precision delivery reports truncation",
            "[audio][regression][quality]")
  {
    // The Jabra shape: a 24-bit source delivered as 16-bit bytes to a 16-bit
    // endpoint. The loss happens upstream of the sink and must be reported.
    auto graph = buildBaseMergedGraph();
    signalAt(graph, 0).precisionBits = 24;
    pcmAt(graph, 1) = pcm(SampleEncoding::Signed24PackedLe);

    for (std::size_t index = 2; index < 4; ++index)
    {
      pcmAt(graph, index) = pcm(SampleEncoding::Signed16Le);
    }

    graph.nodes[4].optFormat = integerSignal(16);

    auto const result = analyzeAudioQuality(graph);

    CHECK(result.overall == Quality::LinearIntervention);
    CHECK(hasFinding(findAssessment(result, "ao-engine"), QualityFindingKind::Truncation));
    // Once precision is genuinely lost, the sink transition must not be
    // excused as a round trip back to proven bits.
    CHECK_FALSE(hasFinding(findAssessment(result, "ao-sink"), QualityFindingKind::LosslessRoundTrip));
  }

  TEST_CASE("QualityAnalyzer - missing format leaves downstream transitions unverified", "[audio][unit][quality]")
  {
    auto graph = buildBaseMergedGraph();
    graph.nodes[3].optFormat.reset();
    setSampleRate(graph.nodes[4], 48000);

    auto const result = analyzeAudioQuality(graph);

    CHECK_FALSE(result.fullyVerified);
    CHECK(result.pipelineQuality == Quality::BitwisePerfect);
    CHECK_FALSE(findAssessment(result, "ao-stream")->optFormat);
    CHECK_FALSE(hasFinding(findAssessment(result, "ao-sink"), QualityFindingKind::Resampling));
  }

  TEST_CASE("QualityAnalyzer - incomplete and absent playback paths are not verified", "[audio][unit][quality]")
  {
    SECTION("path without sink")
    {
      auto graph = buildBaseMergedGraph();
      graph.nodes.resize(3);
      graph.connections.resize(2);

      auto const result = analyzeAudioQuality(graph);
      CHECK_FALSE(result.fullyVerified);
      CHECK(result.overall == Quality::BitwisePerfect);
    }

    SECTION("empty graph")
    {
      auto const result = analyzeAudioQuality({});
      CHECK(result.overall == Quality::Unknown);
      CHECK(result.assessments.empty());
    }

    SECTION("no Aobus source")
    {
      auto graph = flow::Graph{};
      graph.nodes.push_back(flow::Node{.id = "external", .type = flow::NodeType::ExternalSource});

      auto const result = analyzeAudioQuality(graph);
      CHECK(result.overall == Quality::Unknown);
      CHECK(result.assessments.empty());
    }
  }
} // namespace ao::audio::test
