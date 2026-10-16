// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "lib/audio/backend/detail/CoreAudioGraph.h"

#include "lib/audio/backend/detail/CoreAudioChannelLayout.h"
#include "lib/audio/backend/detail/CoreAudioFormat.h"
#include <ao/audio/ChannelDeliveryEvidence.h>
#include <ao/audio/ChannelLayoutEvidence.h>
#include <ao/audio/NodeFormat.h>
#include <ao/audio/PcmFormat.h>
#include <ao/audio/Quality.h>
#include <ao/audio/QualityAnalyzer.h>
#include <ao/audio/SampleEncoding.h>
#include <ao/audio/SignalFormat.h>
#include <ao/audio/flow/Graph.h>

#include <CoreAudioTypes/CoreAudioBaseTypes.h>
#include <MacTypes.h>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace ao::audio::backend::detail::test
{
  TEST_CASE("CoreAudioGraph - distinguishes the client stream, endpoint signal, and software gain",
            "[audio][unit][coreaudio]")
  {
    auto const graph = coreAudioGraph(
      {.routeAnchor = "device-uid",
       .deviceName = "Built-in Output",
       .optClientFormat =
         PcmFormat{.sampleRate = 44100, .channels = 2, .encoding = SampleEncoding::Signed24In32Le, .speakerMask = 0x3},
       .optDeviceFormat =
         SignalFormat{.sampleRate = 48000, .channels = 2, .precisionBits = 32, .sampleKind = SampleKind::FloatingPoint},
       .channelDelivery = {.mapping = ChannelDeliveryEvidence::Mapping::LegacyUnverified, .nativeSpeakerMask = 0x3},
       .outputChannelDelivery = {.mapping = ChannelDeliveryEvidence::Mapping::LegacyUnverified},
       .volume = 0.5F,
       .muted = true});

    REQUIRE(graph.nodes.size() == 2U);
    CHECK(graph.nodes[0].id == "device-uid:client");
    CHECK(graph.nodes[0].type == flow::NodeType::Stream);
    CHECK(graph.nodes[0].optFormat ==
          NodeFormat{PcmFormat{
            .sampleRate = 44100, .channels = 2, .encoding = SampleEncoding::Signed24In32Le, .speakerMask = 0x3}});
    CHECK(graph.nodes[0].channelDelivery.mapping == ChannelDeliveryEvidence::Mapping::LegacyUnverified);
    CHECK(graph.nodes[0].channelDelivery.nativeSpeakerMask == 0x3U);
    CHECK_FALSE(graph.nodes[0].channelDelivery.hasSharedConversion);
    CHECK(graph.nodes[1].id == "device-uid");
    CHECK(graph.nodes[1].type == flow::NodeType::Sink);
    CHECK(graph.nodes[1].optFormat ==
          NodeFormat{SignalFormat{
            .sampleRate = 48000, .channels = 2, .precisionBits = 32, .sampleKind = SampleKind::FloatingPoint}});
    CHECK(graph.nodes[1].channelDelivery.mapping == ChannelDeliveryEvidence::Mapping::LegacyUnverified);
    CHECK(graph.nodes[1].channelDelivery.nativeSpeakerMask == 0U);
    CHECK_FALSE(graph.nodes[1].channelDelivery.hasSharedConversion);
    CHECK(graph.nodes[1].sourceChannelLayout == ChannelLayoutEvidence{});
    CHECK(graph.nodes[1].softwareVolumeNotUnity);
    CHECK_FALSE(graph.nodes[1].hardwareVolumeNotUnity);
    CHECK(graph.nodes[1].maxSoftwareGain == 0.5F);
    CHECK(graph.nodes[1].isMuted);
    REQUIRE(graph.connections.size() == 1U);
    CHECK(graph.connections.front().sourceId == "device-uid:client");
    CHECK(graph.connections.front().destinationId == "device-uid");
  }

  TEST_CASE("CoreAudioGraph - legacy positioning and independently known mono to stereo conversion coexist",
            "[audio][unit][coreaudio]")
  {
    auto const graph = coreAudioGraph(
      {.routeAnchor = "device-uid",
       .optClientFormat =
         PcmFormat{
           .sampleRate = 48000, .channels = 1, .encoding = SampleEncoding::Signed24PackedLe, .speakerMask = 0x4},
       .optDeviceFormat =
         SignalFormat{.sampleRate = 48000, .channels = 2, .precisionBits = 32, .sampleKind = SampleKind::FloatingPoint},
       .channelDelivery = {.mapping = ChannelDeliveryEvidence::Mapping::LegacyUnverified,
                           .nativeSpeakerMask = 0x4,
                           .hasSharedConversion = true},
       .outputChannelDelivery = {.mapping = ChannelDeliveryEvidence::Mapping::LegacyUnverified,
                                 .nativeSpeakerMask = 0x3,
                                 .hasSharedConversion = true}});
    REQUIRE(graph.nodes.size() == 2U);
    CHECK(graph.nodes[0].channelDelivery.mapping == ChannelDeliveryEvidence::Mapping::LegacyUnverified);
    CHECK_FALSE(graph.nodes[0].channelDelivery.hasSharedConversion);
    CHECK(graph.nodes[0].channelDelivery.nativeSpeakerMask == 0x4U);
    CHECK(graph.nodes[1].channelDelivery.nativeSpeakerMask == 0x3U);
    CHECK(graph.nodes[1].channelDelivery.hasSharedConversion);
  }

  TEST_CASE("CoreAudioGraph - planned mono to stereo conversion produces one sink-owned analyzer finding",
            "[audio][integration][coreaudio]")
  {
    auto const client =
      PcmFormat{.sampleRate = 48000, .channels = 1, .encoding = SampleEncoding::Signed24PackedLe, .speakerMask = 0x4};
    auto const outputRes =
      coreAudioFormat({.sampleRate = 48000, .channels = 2, .encoding = SampleEncoding::Signed24PackedLe});
    REQUIRE(outputRes);
    auto const planRes = planCoreAudioChannels(
      client,
      *outputRes,
      {.optClientLabels = std::vector<::AudioChannelLabel>{::kAudioChannelLabel_Center},
       .optOutputLabels = std::vector<::AudioChannelLabel>{::kAudioChannelLabel_Left, ::kAudioChannelLabel_Right},
       .optOutputMap = std::vector<::SInt32>{0, 0}});
    REQUIRE(planRes);
    CHECK(planRes->sourceSlots == std::vector<std::uint32_t>{0U});
    CHECK(planRes->hasIdentityOrder);
    CHECK(planRes->delivery.hasSharedConversion);
    CHECK(planRes->outputDelivery.hasSharedConversion);
    auto const opened = coreAudioOpenedPcmMode(client, *planRes);
    CHECK(opened.clientFormat == client);
    CHECK(opened.channelDelivery == planRes->delivery);

    auto graph = coreAudioGraph(
      {.routeAnchor = "device-uid",
       .optClientFormat = planRes->nativeClientFormat,
       .optDeviceFormat =
         SignalFormat{.sampleRate = 48000, .channels = 2, .precisionBits = 24, .sampleKind = SampleKind::Integer},
       .channelDelivery = planRes->delivery,
       .outputChannelDelivery = planRes->outputDelivery});
    REQUIRE(graph.nodes.size() == 2U);
    CHECK(graph.nodes[0].channelDelivery.mapping == ChannelDeliveryEvidence::Mapping::LegacyUnverified);
    CHECK(graph.nodes[0].channelDelivery.nativeSpeakerMask == 0x4U);
    CHECK_FALSE(graph.nodes[0].channelDelivery.hasSharedConversion);
    CHECK(graph.nodes[1].channelDelivery.mapping == ChannelDeliveryEvidence::Mapping::LegacyUnverified);
    CHECK(graph.nodes[1].channelDelivery.nativeSpeakerMask == 0x3U);
    CHECK(graph.nodes[1].channelDelivery.hasSharedConversion);

    auto source = client;
    bool hasUpstreamCountChange = false;

    SECTION("matching upstream mono leaves only the sink-owned conversion")
    {
    }

    SECTION("an independent upstream count change retains its own stream-owned conversion")
    {
      // Controlled graph composition, not a claim that Engine admits a 4-to-1 mode.
      source.channels = 4;
      source.speakerMask = 0x33;
      hasUpstreamCountChange = true;
    }

    graph.nodes.insert(
      graph.nodes.begin(),
      {.id = "ao-source",
       .type = flow::NodeType::Source,
       .optFormat = source,
       .sourceChannelLayout = {.origin = ChannelLayoutOrigin::Declared, .optDeclaredMask = source.speakerMask}});
    graph.connections.push_back({.sourceId = "ao-source", .destinationId = "device-uid:client"});
    auto const quality = analyzeAudioQuality(graph);
    CHECK(quality.channelConfidence == ChannelConfidence::Converted);
    CHECK(quality.pipelineQuality == Quality::LinearIntervention);
    REQUIRE(quality.assessments.size() == 3U);
    CHECK(quality.assessments[0].nodeId == "ao-source");
    CHECK(quality.assessments[1].nodeId == "device-uid:client");
    CHECK(quality.assessments[2].nodeId == "device-uid");
    CHECK(quality.assessments[1].channelDelivery.mapping == ChannelDeliveryEvidence::Mapping::LegacyUnverified);
    CHECK_FALSE(quality.assessments[1].channelDelivery.hasSharedConversion);
    CHECK(quality.assessments[2].channelDelivery.mapping == ChannelDeliveryEvidence::Mapping::LegacyUnverified);
    CHECK(quality.assessments[2].channelDelivery.hasSharedConversion);
    std::size_t conversionCount = 0U;
    std::size_t streamConversionCount = 0U;
    std::size_t sinkConversionCount = 0U;

    for (auto const& assessment : quality.assessments)
    {
      for (auto const& finding : assessment.findings)
      {
        if (finding.kind == QualityFindingKind::ChannelMapping)
        {
          ++conversionCount;
          CHECK(finding.quality == Quality::LinearIntervention);

          if (assessment.nodeId == "device-uid:client")
          {
            ++streamConversionCount;
            CHECK(assessment.nodeType == flow::NodeType::Stream);
            CHECK(finding.optFromFormat == NodeFormat{source});
            CHECK(finding.optToFormat == NodeFormat{client});
          }
          else
          {
            ++sinkConversionCount;
            CHECK(assessment.nodeId == "device-uid");
            CHECK(assessment.nodeType == flow::NodeType::Sink);
            CHECK(finding.optFromFormat == NodeFormat{client});
            CHECK(finding.optToFormat == graph.nodes[2].optFormat);
          }
        }
      }
    }

    CHECK(sinkConversionCount == 1U);

    if (hasUpstreamCountChange)
    {
      CHECK(conversionCount == 2U);
      CHECK(streamConversionCount == 1U);
    }
    else
    {
      CHECK(conversionCount == 1U);
      CHECK(streamConversionCount == 0U);
      CHECK_FALSE(std::ranges::contains(
        quality.assessments[1].findings, QualityFindingKind::ChannelMapping, &QualityFinding::kind));
    }
  }

  TEST_CASE("CoreAudioGraph - stays empty until a client stream is configured", "[audio][unit][coreaudio]")
  {
    CHECK(coreAudioGraph({.routeAnchor = "device-uid"}) == flow::Graph{});
  }
} // namespace ao::audio::backend::detail::test
