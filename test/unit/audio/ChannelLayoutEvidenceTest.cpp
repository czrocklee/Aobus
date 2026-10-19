// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include <ao/audio/ChannelLayoutEvidence.h>

#include "lib/audio/detail/RouteTracker.h"
#include "test/unit/runtime/ExecutorTestSupport.h"
#include <ao/AudioCodec.h>
#include <ao/audio/AudioRouteFormatState.h>
#include <ao/audio/BackendIds.h>
#include <ao/audio/ChannelDeliveryEvidence.h>
#include <ao/audio/DecodedStreamInfo.h>
#include <ao/audio/Engine.h>
#include <ao/audio/NodeFormat.h>
#include <ao/audio/PcmFormat.h>
#include <ao/audio/Player.h>
#include <ao/audio/Quality.h>
#include <ao/audio/QualityAnalyzer.h>
#include <ao/audio/SampleEncoding.h>
#include <ao/audio/SignalFormat.h>
#include <ao/audio/flow/Graph.h>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>
#include <variant>

namespace ao::audio::test
{
  namespace
  {
    using rt::test::ManualExecutor;

    // WAVE-style four-speaker identity set: front pair plus back pair.
    constexpr auto kQuadMask = std::uint32_t{0x33};

    constexpr auto kQuadSourceFormat = SignalFormat{
      .sampleRate = 48000,
      .channels = 4,
      .precisionBits = 24,
      .sampleKind = SampleKind::Integer,
      .speakerMask = kQuadMask,
    };

    constexpr auto kQuadDecoderFormat = PcmFormat{
      .sampleRate = 48000,
      .channels = 4,
      .encoding = SampleEncoding::Signed24In32Le,
      .speakerMask = kQuadMask,
    };

    constexpr auto kQuadEngineFormat = PcmFormat{
      .sampleRate = 48000,
      .channels = 4,
      .encoding = SampleEncoding::Signed32Le,
      .speakerMask = kQuadMask,
    };

    ChannelLayoutEvidence identifiedQuadEvidence()
    {
      return ChannelLayoutEvidence{.origin = ChannelLayoutOrigin::Identified};
    }

    ChannelLayoutEvidence inferredQuadEvidence()
    {
      // Inferred layout: the same effective origin distinction, without raw history.
      return ChannelLayoutEvidence{.origin = ChannelLayoutOrigin::Inferred};
    }

    DecodedStreamInfo quadStreamInfo(ChannelLayoutEvidence evidence)
    {
      return DecodedStreamInfo{
        .sourceFormat = kQuadSourceFormat,
        .outputFormat = kQuadDecoderFormat,
        .isLossy = false,
        .codec = AudioCodec::Flac,
        .sourceChannelLayout = evidence,
      };
    }

    // Synthetic owner-thread route status: Player::handleRouteChanged owns the
    // playback generation separately, so the status generation field stays unset.
    Engine::RouteStatus quadRouteStatus(ChannelLayoutEvidence evidence)
    {
      return Engine::RouteStatus{
        .state =
          AudioRouteFormatState{
            .sourceFormat = kQuadSourceFormat,
            .decoderOutputFormat = kQuadDecoderFormat,
            .engineOutputFormat = kQuadEngineFormat,
            .isLossySource = false,
            .codec = AudioCodec::Flac,
            .sourceChannelLayout = evidence,
          },
      };
    }
  } // namespace

  TEST_CASE("RouteTracker - decoder and engine updates retain effective speaker masks", "[audio][unit][route-tracker]")
  {
    auto tracker = detail::RouteTracker{};

    SECTION("identified masks are copied into the route snapshot")
    {
      tracker.setDecoder(quadStreamInfo(identifiedQuadEvidence()));
      tracker.setEngineFormat(kQuadEngineFormat);

      auto const state = tracker.state();
      CHECK(state.sourceFormat == kQuadSourceFormat);
      CHECK(state.sourceFormat.speakerMask == kQuadMask);
      CHECK(state.decoderOutputFormat == kQuadDecoderFormat);
      CHECK(state.decoderOutputFormat.speakerMask == kQuadMask);
      CHECK(state.engineOutputFormat == kQuadEngineFormat);
      CHECK(state.engineOutputFormat.speakerMask == kQuadMask);
    }

    SECTION("a channel count alone keeps every mask at zero")
    {
      auto const countOnlySource = SignalFormat{
        .sampleRate = 48000,
        .channels = 4,
        .precisionBits = 24,
        .sampleKind = SampleKind::Integer,
      };
      auto const countOnlyOutput = PcmFormat{
        .sampleRate = 48000,
        .channels = 4,
        .encoding = SampleEncoding::Signed24In32Le,
      };

      tracker.setDecoder(DecodedStreamInfo{
        .sourceFormat = countOnlySource,
        .outputFormat = countOnlyOutput,
        .isLossy = false,
        .codec = AudioCodec::Flac,
        .sourceChannelLayout = ChannelLayoutEvidence{},
      });

      auto const state = tracker.state();
      CHECK(state.sourceFormat.speakerMask == 0);
      CHECK(state.decoderOutputFormat.speakerMask == 0);
      // The engine output stays wholly unassigned until it is set.
      CHECK(state.engineOutputFormat == PcmFormat{});
      CHECK(state.engineOutputFormat.speakerMask == 0);
    }
  }

  TEST_CASE("RouteTracker - an evidence-only decoder refresh changes the snapshot without changing formats",
            "[audio][unit][route-tracker]")
  {
    auto tracker = detail::RouteTracker{};
    tracker.setDecoder(quadStreamInfo(identifiedQuadEvidence()));
    auto const before = tracker.state();

    tracker.setDecoder(quadStreamInfo(inferredQuadEvidence()));
    auto const after = tracker.state();

    // The effective format chain is unchanged...
    CHECK(before.sourceFormat == after.sourceFormat);
    CHECK(before.decoderOutputFormat == after.decoderOutputFormat);
    CHECK(before.engineOutputFormat == after.engineOutputFormat);
    CHECK(before.codec == after.codec);
    CHECK(before.isLossySource == after.isLossySource);
    // ...but each setDecoder copies the current source evidence, so the
    // snapshot itself differs.
    CHECK(before != after);
    CHECK(before.sourceChannelLayout == identifiedQuadEvidence());
    CHECK(after.sourceChannelLayout == inferredQuadEvidence());
  }

  TEST_CASE("RouteTracker - clear resets formats, evidence, and anchor", "[audio][unit][route-tracker]")
  {
    auto tracker = detail::RouteTracker{};
    tracker.setDecoder(quadStreamInfo(identifiedQuadEvidence()));
    tracker.setEngineFormat(kQuadEngineFormat);
    tracker.setAnchor(kBackendNone, "hw:0,0");
    REQUIRE(tracker.anchor());
    CHECK(tracker.anchor()->id == "hw:0,0");
    REQUIRE(tracker.state() != AudioRouteFormatState{});

    tracker.clear();

    CHECK(tracker.state() == AudioRouteFormatState{});
    CHECK(tracker.state().sourceChannelLayout == ChannelLayoutEvidence{});
    CHECK_FALSE(tracker.anchor().has_value());
  }

  TEST_CASE("Player - a route snapshot carries source evidence and keeps format masks on every node",
            "[audio][unit][player]")
  {
    auto executor = ManualExecutor{};
    auto player = Player{executor};
    executor.runUntilIdle();

    player.handleRouteChanged(quadRouteStatus(identifiedQuadEvidence()), player.playbackGeneration());
    executor.runUntilIdle();

    auto const status = player.status();
    REQUIRE(status.flow.nodes.size() == 3);

    auto const sourceIt = std::ranges::find(status.flow.nodes, std::string_view{"ao-source"}, &flow::Node::id);
    REQUIRE(sourceIt != status.flow.nodes.end());
    CHECK(sourceIt->type == flow::NodeType::Source);
    // Only the source node carries the decoder's layout evidence.
    CHECK(sourceIt->sourceChannelLayout == identifiedQuadEvidence());
    REQUIRE(sourceIt->optFormat);
    CHECK(*sourceIt->optFormat == NodeFormat{kQuadSourceFormat});
    CHECK(std::get<SignalFormat>(*sourceIt->optFormat).speakerMask == kQuadMask);

    auto const decoderIt = std::ranges::find(status.flow.nodes, std::string_view{"ao-decoder"}, &flow::Node::id);
    REQUIRE(decoderIt != status.flow.nodes.end());
    REQUIRE(decoderIt->optFormat);
    CHECK(*decoderIt->optFormat == NodeFormat{kQuadDecoderFormat});
    CHECK(std::get<PcmFormat>(*decoderIt->optFormat).speakerMask == kQuadMask);
    CHECK(decoderIt->sourceChannelLayout == ChannelLayoutEvidence{});

    auto const engineIt = std::ranges::find(status.flow.nodes, std::string_view{"ao-engine"}, &flow::Node::id);
    REQUIRE(engineIt != status.flow.nodes.end());
    REQUIRE(engineIt->optFormat);
    CHECK(*engineIt->optFormat == NodeFormat{kQuadEngineFormat});
    CHECK(std::get<PcmFormat>(*engineIt->optFormat).speakerMask == kQuadMask);
    CHECK(engineIt->sourceChannelLayout == ChannelLayoutEvidence{});
  }

  TEST_CASE("Player - an evidence-only route refresh changes the merged graph without changing formats",
            "[audio][unit][player]")
  {
    auto executor = ManualExecutor{};
    auto player = Player{executor};
    executor.runUntilIdle();

    player.handleRouteChanged(quadRouteStatus(identifiedQuadEvidence()), player.playbackGeneration());
    executor.runUntilIdle();
    auto const before = player.status();

    player.handleRouteChanged(quadRouteStatus(inferredQuadEvidence()), player.playbackGeneration());
    executor.runUntilIdle();
    auto const after = player.status();

    // Graph equality sees evidence-only changes...
    CHECK(before.flow != after.flow);
    CHECK(before.flow.connections == after.flow.connections);

    // ...while the effective format chain on every node is preserved.
    REQUIRE(before.flow.nodes.size() == after.flow.nodes.size());

    for (std::size_t index = 0; index < before.flow.nodes.size(); ++index)
    {
      CHECK(before.flow.nodes[index].id == after.flow.nodes[index].id);
      CHECK(before.flow.nodes[index].type == after.flow.nodes[index].type);
      CHECK(before.flow.nodes[index].name == after.flow.nodes[index].name);
      CHECK(before.flow.nodes[index].optFormat == after.flow.nodes[index].optFormat);
    }

    auto const sourceBefore = std::ranges::find(before.flow.nodes, std::string_view{"ao-source"}, &flow::Node::id);
    auto const sourceAfter = std::ranges::find(after.flow.nodes, std::string_view{"ao-source"}, &flow::Node::id);
    REQUIRE(sourceBefore != before.flow.nodes.end());
    REQUIRE(sourceAfter != after.flow.nodes.end());
    CHECK(sourceBefore->sourceChannelLayout == identifiedQuadEvidence());
    CHECK(sourceAfter->sourceChannelLayout == inferredQuadEvidence());
  }

  TEST_CASE("QualityAnalyzer - identified and inferred source histories retain equal numerical quality",
            "[audio][unit][quality]")
  {
    // A pure synthetic playback path. The native sink keeps an explicit raw
    // mask of zero and no delivery evidence: it never inherits the source
    // history, and no channel verification is asserted.
    auto const makeGraph = [](ChannelLayoutEvidence evidence)
    {
      auto const sourceFormat = SignalFormat{
        .sampleRate = 48000,
        .channels = 4,
        .precisionBits = 32,
        .sampleKind = SampleKind::Integer,
        .speakerMask = kQuadMask,
      };
      auto const concreteFormat = PcmFormat{
        .sampleRate = 48000,
        .channels = 4,
        .encoding = SampleEncoding::Signed32Le,
        .speakerMask = kQuadMask,
      };
      auto const nativeSinkFormat = PcmFormat{
        .sampleRate = 48000,
        .channels = 4,
        .encoding = SampleEncoding::Signed32Le,
        .speakerMask = 0,
      };

      return flow::Graph{
        .nodes =
          {
            flow::Node{.id = "ao-source",
                       .type = flow::NodeType::Source,
                       .name = "FLAC",
                       .optFormat = sourceFormat,
                       .sourceChannelLayout = evidence},
            flow::Node{.id = "ao-decoder", .type = flow::NodeType::Decoder, .optFormat = concreteFormat},
            flow::Node{.id = "ao-engine", .type = flow::NodeType::Engine, .optFormat = concreteFormat},
            flow::Node{.id = "native-sink", .type = flow::NodeType::Sink, .optFormat = nativeSinkFormat},
          },
        .connections =
          {
            flow::Connection{.sourceId = "ao-source", .destinationId = "ao-decoder", .isActive = true},
            flow::Connection{.sourceId = "ao-decoder", .destinationId = "ao-engine", .isActive = true},
            flow::Connection{.sourceId = "ao-engine", .destinationId = "native-sink", .isActive = true},
          },
      };
    };

    auto const identifiedResult = analyzeAudioQuality(makeGraph(identifiedQuadEvidence()));
    auto const inferredResult = analyzeAudioQuality(makeGraph(inferredQuadEvidence()));

    // Origin history never creates a numerical intervention or a hidden PCM
    // mode key. Native delivery stays unproved for both histories.
    CHECK(identifiedResult.sourceQuality == inferredResult.sourceQuality);
    CHECK(identifiedResult.pipelineQuality == inferredResult.pipelineQuality);
    CHECK(identifiedResult.overall == inferredResult.overall);
    CHECK(identifiedResult.channelConfidence == inferredResult.channelConfidence);
    CHECK(identifiedResult.channelConfidence == ChannelConfidence::Unverified);
    // fullyVerified keeps its format-presence contract: every node reported a
    // format and the path reaches a Sink. This is not channel verification.
    CHECK(identifiedResult.fullyVerified);
    CHECK(identifiedResult.overall == Quality::BitwisePerfect);
    REQUIRE(identifiedResult.assessments.size() == 4);
    REQUIRE(inferredResult.assessments.size() == 4);
    CHECK(identifiedResult.assessments[0].nodeId == "ao-source");
    CHECK(identifiedResult.assessments[3].nodeId == "native-sink");
  }
} // namespace ao::audio::test
