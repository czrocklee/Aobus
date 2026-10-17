// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include <ao/audio/ChannelDeliveryEvidence.h>
#include <ao/audio/ChannelLayoutEvidence.h>
#include <ao/audio/NodeFormat.h>
#include <ao/audio/PcmFormat.h>
#include <ao/audio/Quality.h>
#include <ao/audio/QualityAnalyzer.h>
#include <ao/audio/SampleEncoding.h>
#include <ao/audio/SignalFormat.h>
#include <ao/audio/flow/Graph.h>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <variant>

namespace ao::audio::test
{
  namespace
  {
    PcmFormat identifiedPcm(std::uint8_t channels, std::uint32_t speakerMask)
    {
      return {
        .sampleRate = 48000, .channels = channels, .encoding = SampleEncoding::Signed16Le, .speakerMask = speakerMask};
    }

    flow::Graph mappedChannelGraph(std::uint8_t channels = 2, std::uint32_t speakerMask = 0x3)
    {
      // The test authors the canonical sets and each native observation. No
      // production layout/default table supplies either the inputs or oracle.
      return {
        .nodes =
          {
            {.id = "ao-source",
             .type = flow::NodeType::Source,
             .optFormat = SignalFormat{.sampleRate = 48000,
                                       .channels = channels,
                                       .precisionBits = 16,
                                       .sampleKind = SampleKind::Integer,
                                       .speakerMask = speakerMask},
             .sourceChannelLayout = {.origin = ChannelLayoutOrigin::Identified}},
            {.id = "decoder", .type = flow::NodeType::Decoder, .optFormat = identifiedPcm(channels, speakerMask)},
            {.id = "engine", .type = flow::NodeType::Engine, .optFormat = identifiedPcm(channels, speakerMask)},
            {.id = "stream",
             .type = flow::NodeType::Stream,
             .optFormat = identifiedPcm(channels, speakerMask),
             .channelDelivery = {.isMapped = true}},
            {.id = "mixer",
             .type = flow::NodeType::Intermediary,
             .optFormat = identifiedPcm(channels, speakerMask),
             .channelDelivery = {.isMapped = true}},
            {.id = "sink",
             .type = flow::NodeType::Sink,
             .optFormat = identifiedPcm(channels, speakerMask),
             .channelDelivery = {.isMapped = true}},
          },
        .connections =
          {
            {.sourceId = "ao-source", .destinationId = "decoder"},
            {.sourceId = "decoder", .destinationId = "engine"},
            {.sourceId = "engine", .destinationId = "stream"},
            {.sourceId = "stream", .destinationId = "mixer"},
            {.sourceId = "mixer", .destinationId = "sink"},
          },
      };
    }

    void setIdentity(flow::Node& node, std::uint8_t channels, std::uint32_t speakerMask)
    {
      std::visit(
        [channels, speakerMask](auto& format)
        {
          format.channels = channels;
          format.speakerMask = speakerMask;
        },
        *node.optFormat);
    }

    bool hasChannelFinding(NodeQualityAssessment const& assessment, QualityFindingKind kind)
    {
      return std::ranges::contains(assessment.findings, kind, &QualityFinding::kind);
    }
  } // namespace

  TEST_CASE("QualityAnalyzer - source history caps confidence without changing equal modes", "[audio][unit][quality]")
  {
    auto graph = mappedChannelGraph();
    auto expectedConfidence = ChannelConfidence::Verified;

    SECTION("identified FL FR history")
    {
      graph.nodes[0].sourceChannelLayout = {.origin = ChannelLayoutOrigin::Identified};
    }

    SECTION("inferred FL FR history")
    {
      graph.nodes[0].sourceChannelLayout = {.origin = ChannelLayoutOrigin::Inferred};
      expectedConfidence = ChannelConfidence::Inferred;
    }

    auto const result = analyzeAudioQuality(graph);

    CHECK(result.channelConfidence == expectedConfidence);
    CHECK(result.fullyVerified);
    CHECK(result.sourceQuality == Quality::BitwisePerfect);
    CHECK(result.pipelineQuality == Quality::BitwisePerfect);
    CHECK(result.overall == Quality::BitwisePerfect);
    REQUIRE(result.assessments.size() == 6U);
    CHECK(result.assessments[0].optFormat == graph.nodes[0].optFormat);
  }

  TEST_CASE("QualityAnalyzer - every native role needs its own mapping observation", "[audio][unit][quality]")
  {
    auto graph = mappedChannelGraph();

    for (std::size_t index = 3; index < graph.nodes.size(); ++index)
    {
      setIdentity(graph.nodes[index], 2, 0);
      graph.nodes[index].channelDelivery.nativeSpeakerMask = 0x3;
    }

    std::size_t nativeIndex = 3;

    SECTION("Stream")
    {
      nativeIndex = 3;
    }

    SECTION("Intermediary")
    {
      nativeIndex = 4;
    }

    SECTION("Sink")
    {
      nativeIndex = 5;
    }

    graph.nodes[nativeIndex].channelDelivery.isMapped = false;
    // Source history on a native node cannot substitute for native evidence.
    graph.nodes[nativeIndex].sourceChannelLayout = {.origin = ChannelLayoutOrigin::Identified};

    auto const result = analyzeAudioQuality(graph);

    CHECK(result.channelConfidence == ChannelConfidence::Unverified);
    CHECK(result.fullyVerified);
    CHECK(result.pipelineQuality == Quality::BitwisePerfect);
    REQUIRE(result.assessments.size() == 6U);
  }

  TEST_CASE("QualityAnalyzer - absent source identity or delivery path has unknown confidence",
            "[audio][unit][quality]")
  {
    auto graph = mappedChannelGraph();

    SECTION("unknown source history")
    {
      graph.nodes[0].sourceChannelLayout = {};
    }

    SECTION("zero is not a count-derived stereo identity")
    {
      setIdentity(graph.nodes[0], 2, 0);
      // Native evidence cannot fill a missing application identity either.
      graph.nodes[0].channelDelivery.nativeSpeakerMask = 0x3;
    }

    SECTION("no active sink path")
    {
      graph.connections.back().isActive = false;
    }

    SECTION("no application source")
    {
      graph.nodes[0].id = "unrelated-source";
    }

    SECTION("source id alone does not supply its role")
    {
      graph.nodes[0].type = flow::NodeType::ExternalSource;
    }

    auto const result = analyzeAudioQuality(graph);
    CHECK(result.channelConfidence == ChannelConfidence::Unknown);
  }

  TEST_CASE("QualityAnalyzer - unmapped delivery does not invent numerical loss", "[audio][unit][quality]")
  {
    auto graph = mappedChannelGraph();
    auto expectedConfidence = ChannelConfidence::Unverified;

    SECTION("unmapped delivery with observed identities")
    {
      graph.nodes[3].channelDelivery.isMapped = false;
    }

    SECTION("mapped flag with missing native positions")
    {
      setIdentity(graph.nodes[3], 2, 0);
    }

    SECTION("mapped flag with every native position set unknown")
    {
      for (std::size_t index = 3; index < graph.nodes.size(); ++index)
      {
        setIdentity(graph.nodes[index], 2, 0);
      }
    }

    SECTION("unidentified application decoder output")
    {
      setIdentity(graph.nodes[1], 2, 0);
      graph.nodes[1].channelDelivery.nativeSpeakerMask = 0x3;
    }

    SECTION("inferred source with unmapped native delivery")
    {
      graph.nodes[0].sourceChannelLayout.origin = ChannelLayoutOrigin::Inferred;
      graph.nodes[3].channelDelivery.isMapped = false;
    }

    SECTION("partial source followed by compatible complete application and native sets")
    {
      expectedConfidence = ChannelConfidence::Unknown;
      setIdentity(graph.nodes[0], 2, 0x1);
    }

    auto const result = analyzeAudioQuality(graph);

    CHECK(result.channelConfidence == expectedConfidence);
    CHECK(result.fullyVerified);
    CHECK(result.pipelineQuality == Quality::BitwisePerfect);
    CHECK(result.sourceQuality == Quality::BitwisePerfect);

    for (auto const& assessment : result.assessments)
    {
      CHECK_FALSE(hasChannelFinding(assessment, QualityFindingKind::ChannelMapping));
    }
  }

  TEST_CASE("QualityAnalyzer - compatible native subsets of a complete source are uncertainty only",
            "[audio][unit][quality]")
  {
    auto graph = mappedChannelGraph(2, 0x3); // Complete FL FR source and application path
    setIdentity(graph.nodes[3], 2, 0x1);     // Native FL is known; FR is not observed, not contradicted.

    SECTION("partial native format with unmapped delivery")
    {
      graph.nodes[3].channelDelivery.isMapped = false;
    }

    SECTION("partial native format with mapped delivery")
    {
      graph.nodes[3].channelDelivery.isMapped = true;
    }

    SECTION("partial observed native set with no canonical native format mask")
    {
      setIdentity(graph.nodes[3], 2, 0);
      graph.nodes[3].channelDelivery.nativeSpeakerMask = 0x1;
    }

    SECTION("compatible partial fields do not individually establish a complete native set")
    {
      graph.nodes[3].channelDelivery.nativeSpeakerMask = 0x2;
    }

    SECTION("complete native format and compatible partial observed set")
    {
      setIdentity(graph.nodes[3], 2, 0x3);
      graph.nodes[3].channelDelivery = {.isMapped = false, .nativeSpeakerMask = 0x1};
    }

    SECTION("partial native format and compatible complete observed set")
    {
      graph.nodes[3].channelDelivery = {.isMapped = false, .nativeSpeakerMask = 0x3};
    }

    auto const result = analyzeAudioQuality(graph);

    CHECK(result.channelConfidence == ChannelConfidence::Unverified);
    CHECK(result.pipelineQuality == Quality::BitwisePerfect);
    CHECK(result.sourceQuality == Quality::BitwisePerfect);
    CHECK(result.fullyVerified);
    REQUIRE(result.assessments.size() == 6U);
    REQUIRE(result.assessments[0].optFormat);
    CHECK(std::get<SignalFormat>(*result.assessments[0].optFormat).speakerMask == 0x3U);
    CHECK(result.assessments[3].optFormat == graph.nodes[3].optFormat);

    for (auto const& assessment : result.assessments)
    {
      CHECK_FALSE(hasChannelFinding(assessment, QualityFindingKind::ChannelMapping));
    }
  }

  TEST_CASE("QualityAnalyzer - known speaker substitutions and count changes are genuine interventions",
            "[audio][unit][quality]")
  {
    auto graph = mappedChannelGraph(4, 0x33); // FL FR BL BR
    std::uint8_t channels = 4;
    std::uint32_t destinationMask = 0x107; // FL FR FC BC, not quad

    SECTION("quad versus front center back center")
    {
    }

    SECTION("center mono versus left-only mono")
    {
      graph = mappedChannelGraph(1, 0x4);
      channels = 1;
      destinationMask = 0x1;
    }

    SECTION("two-channel front versus back")
    {
      graph = mappedChannelGraph(2, 0x3);
      channels = 2;
      destinationMask = 0x30;
    }

    SECTION("complete FL FR source versus partial native FC")
    {
      graph = mappedChannelGraph(2, 0x3);
      channels = 2;
      destinationMask = 0x4;
    }

    SECTION("complete FL FR source versus partial native FC observation")
    {
      graph = mappedChannelGraph(2, 0x3);
      channels = 2;
      destinationMask = 0;
      graph.nodes[3].channelDelivery.nativeSpeakerMask = 0x4;
      graph.nodes[4].channelDelivery.nativeSpeakerMask = 0x4;
      graph.nodes[5].channelDelivery.nativeSpeakerMask = 0x4;
    }

    SECTION("partial native format cannot hide incompatible roles in its observed set")
    {
      graph = mappedChannelGraph(2, 0x3);
      channels = 2;
      destinationMask = 0x1;
      graph.nodes[3].channelDelivery.nativeSpeakerMask = 0x4;
      graph.nodes[4].channelDelivery.nativeSpeakerMask = 0x4;
      graph.nodes[5].channelDelivery.nativeSpeakerMask = 0x4;
    }

    SECTION("known count reduction with unknown destination positions")
    {
      channels = 2;
      destinationMask = 0;
    }

    for (std::size_t index = 3; index < graph.nodes.size(); ++index)
    {
      setIdentity(graph.nodes[index], channels, destinationMask);
    }

    auto const result = analyzeAudioQuality(graph);

    CHECK(result.pipelineQuality == Quality::LinearIntervention);
    CHECK(result.sourceQuality == Quality::BitwisePerfect);
    CHECK(result.fullyVerified);
    REQUIRE(result.assessments.size() == 6U);
    auto const& stream = result.assessments[3];
    REQUIRE(stream.findings.size() == 1U);
    CHECK(stream.findings[0].kind == QualityFindingKind::ChannelMapping);
    CHECK(stream.findings[0].optFromFormat == graph.nodes[2].optFormat);
    CHECK(stream.findings[0].optToFormat == graph.nodes[3].optFormat);
  }

  TEST_CASE("QualityAnalyzer - uncertainty cannot hide a known channel alteration", "[audio][unit][quality]")
  {
    auto graph = mappedChannelGraph(1, 0x4);
    std::size_t findingIndex = 3;

    SECTION("unknown intervening positions do not erase FC versus FL")
    {
      setIdentity(graph.nodes[3], 1, 0);
      setIdentity(graph.nodes[4], 1, 0);
      setIdentity(graph.nodes[5], 1, 0x1);
      findingIndex = 5; // The Sink first reports the contradictory FL role.
    }

    SECTION("inferred source plus unmapped known conversion")
    {
      graph.nodes[0].sourceChannelLayout.origin = ChannelLayoutOrigin::Inferred;
      graph.nodes[3].channelDelivery.isMapped = false;
      graph.nodes[3].channelDelivery.hasSharedConversion = true;
      setIdentity(graph.nodes[3], 1, 0);
    }

    SECTION("missing source history plus known conversion")
    {
      graph.nodes[0].sourceChannelLayout = {};
      graph.nodes[3].channelDelivery.hasSharedConversion = true;
    }

    SECTION("incomplete path plus known conversion")
    {
      graph.connections.back().isActive = false;
      graph.nodes[3].channelDelivery.hasSharedConversion = true;
    }

    SECTION("missing native format plus known conversion")
    {
      graph.nodes[3].optFormat.reset();
      graph.nodes[3].channelDelivery.hasSharedConversion = true;
    }

    auto const result = analyzeAudioQuality(graph);

    CHECK(result.pipelineQuality == Quality::LinearIntervention);
    REQUIRE(result.assessments.size() > findingIndex);

    for (std::size_t index = 0; index < result.assessments.size(); ++index)
    {
      auto const count = std::ranges::count(
        result.assessments[index].findings, QualityFindingKind::ChannelMapping, &QualityFinding::kind);
      CHECK(count == (index == findingIndex ? 1 : 0));
    }
  }

  TEST_CASE("QualityAnalyzer - a mapped same-set permutation has no numerical penalty", "[audio][unit][quality]")
  {
    auto graph = mappedChannelGraph(4, 0x33); // Canonical FL FR BL BR

    // Independently observed [BR, FL, BL, FR] positions establish the same set,
    // not canonical native bytes. The backend owns the reversible permutation.
    for (std::size_t index = 3; index < graph.nodes.size(); ++index)
    {
      setIdentity(graph.nodes[index], 4, 0);
      graph.nodes[index].channelDelivery = {.isMapped = true, .nativeSpeakerMask = 0x33};
    }

    auto expectedConfidence = ChannelConfidence::Verified;

    SECTION("identified source with observed native sets")
    {
    }

    SECTION("inferred source remains inferred with observed native sets")
    {
      graph.nodes[0].sourceChannelLayout.origin = ChannelLayoutOrigin::Inferred;
      expectedConfidence = ChannelConfidence::Inferred;
    }

    auto const result = analyzeAudioQuality(graph);

    CHECK(result.channelConfidence == expectedConfidence);
    CHECK(result.pipelineQuality == Quality::BitwisePerfect);
    CHECK(result.fullyVerified);
    REQUIRE(result.assessments.size() == 6U);

    for (auto const& assessment : result.assessments)
    {
      CHECK_FALSE(hasChannelFinding(assessment, QualityFindingKind::ChannelMapping));
    }

    for (std::size_t index = 3; index < result.assessments.size(); ++index)
    {
      auto const& assessment = result.assessments[index];
      REQUIRE(assessment.optFormat);
      CHECK(std::get<PcmFormat>(*assessment.optFormat).speakerMask == 0U);
    }
  }

  TEST_CASE("QualityAnalyzer - conflicting native position fields cannot verify mapping", "[audio][unit][quality]")
  {
    auto graph = mappedChannelGraph(4, 0x33);
    std::size_t nativeIndex = 3;

    SECTION("Stream has a different observed set")
    {
      nativeIndex = 3;
    }

    SECTION("Intermediary has a different observed set")
    {
      nativeIndex = 4;
    }

    SECTION("Sink has a different observed set")
    {
      nativeIndex = 5;
    }

    graph.nodes[nativeIndex].channelDelivery.nativeSpeakerMask = 0x107; // FL FR FC BC, not FL FR BL BR

    auto const result = analyzeAudioQuality(graph);

    // Complete contradictory sets are still proved positions: the finding owns
    // the conversion, and identity confidence stays independent of it.
    CHECK(result.channelConfidence == ChannelConfidence::Verified);
    CHECK(result.pipelineQuality == Quality::LinearIntervention);
    CHECK(result.fullyVerified);
    REQUIRE(result.assessments.size() == 6U);
    auto const& assessment = result.assessments[nativeIndex];
    CHECK(hasChannelFinding(assessment, QualityFindingKind::ChannelMapping));
    REQUIRE(assessment.optFormat);
    CHECK(std::get<PcmFormat>(*assessment.optFormat).speakerMask == 0x33U);
    REQUIRE(assessment.findings.size() == 1U);
    auto const& finding = assessment.findings.front();
    REQUIRE(finding.optFromFormat);
    REQUIRE(finding.optToFormat);
    CHECK(signalFormat(*finding.optFromFormat).speakerMask == 0x33U);
    CHECK(signalFormat(*finding.optToFormat).speakerMask == 0x33U);
    CHECK_FALSE(finding.optFromSpeakerMask);
    CHECK(finding.optToSpeakerMask == 0x107U);
  }

  TEST_CASE("QualityAnalyzer - count changes retain actual native sets despite conflicting local format roles",
            "[audio][unit][quality]")
  {
    std::uint8_t sourceChannels = 0;
    std::uint8_t sinkChannels = 0;
    std::uint32_t sourceMask = 0;
    std::uint32_t sinkFormatMask = 0;
    std::uint32_t sinkNativeMask = 0;

    SECTION("two to four channels")
    {
      sourceChannels = 2;
      sinkChannels = 4;
      sourceMask = 0x3;
      sinkFormatMask = 0x33;
      sinkNativeMask = 0x107;
    }

    SECTION("four to two channels")
    {
      sourceChannels = 4;
      sinkChannels = 2;
      sourceMask = 0x33;
      sinkFormatMask = 0x3;
      sinkNativeMask = 0xC;
    }

    auto graph = mappedChannelGraph(sourceChannels, sourceMask);
    setIdentity(graph.nodes[4], sinkChannels, sinkFormatMask);
    graph.nodes[4].channelDelivery.nativeSpeakerMask = sinkNativeMask;
    setIdentity(graph.nodes[5], sinkChannels, 0);
    graph.nodes[5].channelDelivery.nativeSpeakerMask = sinkNativeMask;
    auto const result = analyzeAudioQuality(graph);
    CHECK(result.pipelineQuality == Quality::LinearIntervention);
    REQUIRE(result.assessments.size() == 6U);
    auto const& assessment = result.assessments[4];
    REQUIRE(assessment.findings.size() == 1U);
    auto const& finding = assessment.findings.front();
    CHECK(finding.kind == QualityFindingKind::ChannelMapping);
    CHECK(finding.optFromFormat == graph.nodes[3].optFormat);
    CHECK(finding.optToFormat == graph.nodes[4].optFormat);
    REQUIRE(finding.optFromFormat);
    REQUIRE(finding.optToFormat);
    CHECK(signalFormat(*finding.optFromFormat).channels == sourceChannels);
    CHECK(signalFormat(*finding.optToFormat).channels == sinkChannels);
    CHECK(signalFormat(*finding.optToFormat).speakerMask == sinkFormatMask);
    // Count still describes the transition. The native endpoint observation is
    // retained separately, never its impossible union with the storage mask.
    CHECK(finding.optToSpeakerMask == sinkNativeMask);
    // Conflicting local roles cannot make the already established count change
    // happen again at an unchanged downstream node.
    CHECK_FALSE(hasChannelFinding(result.assessments[5], QualityFindingKind::ChannelMapping));
  }

  TEST_CASE("QualityAnalyzer - observed native sets preserve cross-boundary identity contradictions",
            "[audio][unit][quality]")
  {
    auto graph = mappedChannelGraph(4, 0x33);
    std::size_t changedIndex = 3;

    for (std::size_t index = 3; index < graph.nodes.size(); ++index)
    {
      setIdentity(graph.nodes[index], 4, 0);
    }

    SECTION("canonical application set differs from observed native sets")
    {
      graph.nodes[3].channelDelivery.nativeSpeakerMask = 0x107;
      graph.nodes[4].channelDelivery.nativeSpeakerMask = 0x107;
      graph.nodes[5].channelDelivery.nativeSpeakerMask = 0x107;
    }

    SECTION("unknown native positions cannot hide a later observed different set")
    {
      graph.nodes[5].channelDelivery.nativeSpeakerMask = 0x107;
      changedIndex = 5;
    }

    SECTION("observed native set differs from later canonical native set")
    {
      graph.nodes[3].channelDelivery.nativeSpeakerMask = 0x33;
      setIdentity(graph.nodes[5], 4, 0x107);
      changedIndex = 5;
    }

    SECTION("compatible partial native formats cannot replace the complete quad anchor")
    {
      setIdentity(graph.nodes[3], 4, 0x1);
      setIdentity(graph.nodes[4], 4, 0x2);
      setIdentity(graph.nodes[5], 4, 0x107);
      changedIndex = 5;
    }

    SECTION("compatible partial observed sets cannot replace the complete quad anchor")
    {
      graph.nodes[3].channelDelivery.nativeSpeakerMask = 0x1;
      graph.nodes[4].channelDelivery.nativeSpeakerMask = 0x2;
      graph.nodes[5].channelDelivery.nativeSpeakerMask = 0x107;
      changedIndex = 5;
    }

    auto const result = analyzeAudioQuality(graph);

    CHECK(result.pipelineQuality == Quality::LinearIntervention);
    CHECK(result.fullyVerified);
    REQUIRE(result.assessments.size() == 6U);
    CHECK(hasChannelFinding(result.assessments[changedIndex], QualityFindingKind::ChannelMapping));
    CHECK(result.assessments[changedIndex].optFormat == graph.nodes[changedIndex].optFormat);
    auto const& findings = result.assessments[changedIndex].findings;
    REQUIRE(findings.size() == 1U);
    auto const& finding = findings.front();
    REQUIRE(finding.optFromFormat);
    REQUIRE(finding.optToFormat);
    CHECK(finding.optFromSpeakerMask.value_or(signalFormat(*finding.optFromFormat).speakerMask) == 0x33U);
    CHECK(finding.optToSpeakerMask.value_or(signalFormat(*finding.optToFormat).speakerMask) == 0x107U);
  }

  TEST_CASE("QualityAnalyzer - partial observations retain count-bounded role contradictions", "[audio][unit][quality]")
  {
    auto graph = mappedChannelGraph(2, 0x1); // Only FL is established at the source.
    setIdentity(graph.nodes[1], 2, 0);
    setIdentity(graph.nodes[2], 2, 0);
    setIdentity(graph.nodes[3], 2, 0x2); // FR is compatible with the earlier partial FL.
    setIdentity(graph.nodes[4], 2, 0x4); // FC cannot coexist with both FL and FR in two channels.
    setIdentity(graph.nodes[5], 2, 0);

    auto const result = analyzeAudioQuality(graph);

    // The partial source identity keeps confidence Unknown; the finding still
    // reports the intervention it observed.
    CHECK(result.channelConfidence == ChannelConfidence::Unknown);
    CHECK(result.pipelineQuality == Quality::LinearIntervention);
    CHECK(result.fullyVerified);
    REQUIRE(result.assessments.size() == 6U);
    CHECK_FALSE(hasChannelFinding(result.assessments[3], QualityFindingKind::ChannelMapping));
    CHECK(hasChannelFinding(result.assessments[4], QualityFindingKind::ChannelMapping));
  }

  TEST_CASE("QualityAnalyzer - current native uncertainty cannot borrow application mapping facts",
            "[audio][unit][quality]")
  {
    auto graph = mappedChannelGraph();
    // An initial open may have established mapping. Only current graph-native
    // observations count; an application-side retained snapshot is not readback.
    graph.nodes[2].channelDelivery.isMapped = true;
    graph.nodes[2].channelDelivery.nativeSpeakerMask = 0x3;

    for (std::size_t index = 3; index < graph.nodes.size(); ++index)
    {
      graph.nodes[index].channelDelivery.isMapped = false;
      setIdentity(graph.nodes[index], 2, 0);
    }

    auto const result = analyzeAudioQuality(graph);

    CHECK(result.channelConfidence == ChannelConfidence::Unverified);
    CHECK(result.pipelineQuality == Quality::BitwisePerfect);
    CHECK(result.fullyVerified);
    REQUIRE(result.assessments.size() == 6U);
    REQUIRE(result.assessments[3].optFormat);
    CHECK(std::get<PcmFormat>(*result.assessments[3].optFormat).speakerMask == 0U);
  }

  TEST_CASE("QualityAnalyzer - confidence follows only the selected active sink-reaching path",
            "[audio][unit][quality]")
  {
    auto graph = mappedChannelGraph();
    graph.nodes.push_back({.id = "cycle-a",
                           .type = flow::NodeType::Intermediary,
                           .optFormat = identifiedPcm(1, 0x1),
                           .channelDelivery = {.hasSharedConversion = true}});
    graph.nodes.push_back({.id = "cycle-b", .type = flow::NodeType::Intermediary});
    graph.nodes.push_back({.id = "disabled-sink",
                           .type = flow::NodeType::Sink,
                           .optFormat = identifiedPcm(4, 0x107),
                           .channelDelivery = {.hasSharedConversion = true}});
    graph.nodes.push_back(
      {.id = "unrelated", .type = flow::NodeType::Source, .channelDelivery = {.hasSharedConversion = true}});
    graph.connections.insert(graph.connections.begin(),
                             {{.sourceId = "engine", .destinationId = "cycle-a"},
                              {.sourceId = "cycle-a", .destinationId = "cycle-b"},
                              {.sourceId = "cycle-b", .destinationId = "cycle-a"},
                              {.sourceId = "engine", .destinationId = "disabled-sink", .isActive = false}});

    auto const result = analyzeAudioQuality(graph);

    CHECK(result.channelConfidence == ChannelConfidence::Verified);
    CHECK(result.pipelineQuality == Quality::BitwisePerfect);
    REQUIRE(result.assessments.size() == 6U);
    CHECK(result.assessments[3].nodeId == "stream");
    CHECK(result.assessments.back().nodeId == "sink");
  }

  TEST_CASE("QualityAnalyzer - channel confidence uses the first active sink-reaching branch", "[audio][unit][quality]")
  {
    auto graph = mappedChannelGraph();
    graph.nodes.push_back({.id = "alternate",
                           .type = flow::NodeType::Sink,
                           .optFormat = identifiedPcm(2, 0x30),
                           .channelDelivery = {.isMapped = true}});
    auto expectedConfidence = ChannelConfidence::Verified;
    auto expectedQuality = Quality::BitwisePerfect;
    auto const* expectedSink = "sink";
    auto const alternateConnection = flow::Connection{.sourceId = "mixer", .destinationId = "alternate"};

    SECTION("matching sink branch first")
    {
      graph.connections.push_back(alternateConnection);
    }

    SECTION("different known set branch first")
    {
      graph.connections.insert(graph.connections.begin(), alternateConnection);
      // Positions stay proved on the alternate branch: the ChannelMapping
      // finding owns the intervention, not the confidence.
      expectedConfidence = ChannelConfidence::Verified;
      expectedQuality = Quality::LinearIntervention;
      expectedSink = "alternate";
    }

    auto const result = analyzeAudioQuality(graph);

    CHECK(result.channelConfidence == expectedConfidence);
    CHECK(result.pipelineQuality == expectedQuality);
    REQUIRE(result.assessments.size() == 6U);
    CHECK(result.assessments.back().nodeId == expectedSink);
  }

  TEST_CASE("QualityAnalyzer - format presence verification stays independent of channel confidence",
            "[audio][unit][quality]")
  {
    auto graph = mappedChannelGraph();
    graph.nodes[3].optFormat.reset();

    auto const result = analyzeAudioQuality(graph);

    CHECK_FALSE(result.fullyVerified);
    CHECK(result.channelConfidence == ChannelConfidence::Unverified);
    CHECK(result.pipelineQuality == Quality::BitwisePerfect);
    REQUIRE(result.assessments.size() == 6U);
    CHECK_FALSE(result.assessments[3].optFormat);
  }

  TEST_CASE("QualityAnalyzer - channel interventions invalidate integer precision round-trip proof",
            "[audio][unit][quality]")
  {
    auto graph = mappedChannelGraph(4, 0x33);

    for (std::size_t index = 2; index < 5; ++index)
    {
      std::get<PcmFormat>(*graph.nodes[index].optFormat).encoding = SampleEncoding::Float32Le;
    }

    SECTION("identity change at the narrowing transition")
    {
      setIdentity(graph.nodes[5], 4, 0x107);
    }

    SECTION("identity change upstream of narrowing")
    {
      for (std::size_t index = 3; index < graph.nodes.size(); ++index)
      {
        setIdentity(graph.nodes[index], 4, 0x107);
      }
    }

    SECTION("known shared conversion without mapped positions")
    {
      graph.nodes[3].channelDelivery.isMapped = false;
      graph.nodes[3].channelDelivery.hasSharedConversion = true;
    }

    SECTION("compatible partial observation with separately known shared conversion")
    {
      setIdentity(graph.nodes[3], 4, 0x1);
      graph.nodes[3].channelDelivery.isMapped = false;
      graph.nodes[3].channelDelivery.hasSharedConversion = true;
    }

    SECTION("observed native set changes at the narrowing transition")
    {
      setIdentity(graph.nodes[5], 4, 0);
      graph.nodes[5].channelDelivery.nativeSpeakerMask = 0x107;
    }

    SECTION("conflicting native fields at the narrowing transition")
    {
      graph.nodes[5].channelDelivery.nativeSpeakerMask = 0x107;
    }

    auto const result = analyzeAudioQuality(graph);

    CHECK(result.pipelineQuality == Quality::LinearIntervention);
    REQUIRE(result.assessments.size() == 6U);
    CHECK(hasChannelFinding(result.assessments[5], QualityFindingKind::Truncation));
    CHECK_FALSE(hasChannelFinding(result.assessments[5], QualityFindingKind::LosslessRoundTrip));
  }

  TEST_CASE("QualityAnalyzer - channel uncertainty alone preserves numerical round-trip proof",
            "[audio][unit][quality]")
  {
    auto graph = mappedChannelGraph();

    for (std::size_t index = 2; index < 5; ++index)
    {
      std::get<PcmFormat>(*graph.nodes[index].optFormat).encoding = SampleEncoding::Float32Le;
    }

    graph.nodes[3].channelDelivery.isMapped = false;

    SECTION("unknown native positions")
    {
      setIdentity(graph.nodes[3], 2, 0);
    }

    SECTION("compatible partial native FL positions after complete FL FR source")
    {
      setIdentity(graph.nodes[3], 2, 0x1);
    }

    SECTION("compatible partial observed native FL set")
    {
      setIdentity(graph.nodes[3], 2, 0);
      graph.nodes[3].channelDelivery.nativeSpeakerMask = 0x1;
    }

    SECTION("complete native format plus compatible partial observed FL set")
    {
      graph.nodes[3].channelDelivery.nativeSpeakerMask = 0x1;
    }

    SECTION("compatible partial FL observation at the narrowing transition")
    {
      setIdentity(graph.nodes[5], 2, 0x1);
    }

    auto const result = analyzeAudioQuality(graph);

    CHECK(result.channelConfidence == ChannelConfidence::Unverified);
    CHECK(result.pipelineQuality == Quality::LosslessFloat);
    REQUIRE(result.assessments.size() == 6U);
    CHECK(hasChannelFinding(result.assessments[5], QualityFindingKind::LosslessRoundTrip));
    CHECK_FALSE(hasChannelFinding(result.assessments[5], QualityFindingKind::Truncation));

    for (auto const& assessment : result.assessments)
    {
      CHECK_FALSE(hasChannelFinding(assessment, QualityFindingKind::ChannelMapping));
    }
  }
} // namespace ao::audio::test
