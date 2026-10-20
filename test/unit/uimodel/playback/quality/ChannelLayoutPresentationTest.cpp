// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "test/unit/MessageCatalogTestSupport.h"
#include <ao/audio/ChannelDeliveryEvidence.h>
#include <ao/audio/ChannelLayoutEvidence.h>
#include <ao/audio/Quality.h>
#include <ao/audio/QualityAnalyzer.h>
#include <ao/audio/SignalFormat.h>
#include <ao/audio/flow/Graph.h>
#include <ao/rt/PlaybackState.h>
#include <ao/uimodel/playback/quality/AudioQualityFormatter.h>

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>
#include <string_view>

namespace ao::uimodel::test
{
  TEST_CASE("AudioQualityFormatter - empty pipeline omits channel uncertainty", "[uimodel][unit][playback][quality]")
  {
    auto const formatter = AudioQualityFormatter{ao::test::englishMessageCatalog()};

    for (auto const confidence : {audio::ChannelConfidence::Unknown,
                                  audio::ChannelConfidence::Unverified,
                                  audio::ChannelConfidence::Inferred,
                                  audio::ChannelConfidence::Verified})
    {
      CAPTURE(confidence);
      auto const state = rt::QualityState{.channelConfidence = confidence};
      auto const presentation = formatter.presentation(state);
      CHECK(presentation.headline == "Unknown pipeline");
      CHECK(presentation.category == AudioQualityCategory::Unknown);
      CHECK(presentation.channelNote.empty());
      CHECK(state.channelConfidence == confidence);
    }
  }

  TEST_CASE("AudioQualityFormatter - missing output path does not blame identified source layout",
            "[uimodel][integration][playback][quality]")
  {
    auto const format =
      audio::SignalFormat{.sampleRate = 48000, .channels = 2, .precisionBits = 16, .speakerMask = 0x3};
    auto const graph = audio::flow::Graph{
      .nodes =
        {
          {.id = "ao-source",
           .type = audio::flow::NodeType::Source,
           .optFormat = format,
           .sourceChannelLayout = {.origin = audio::ChannelLayoutOrigin::Identified}},
          {.id = "stream",
           .type = audio::flow::NodeType::Stream,
           .optFormat = format,
           .channelDelivery = {.isMapped = true, .nativeSpeakerMask = 0x3}},
        },
      .connections = {{.sourceId = "ao-source", .destinationId = "stream"}},
    };
    auto const result = audio::analyzeAudioQuality(graph);
    REQUIRE(result.assessments.size() == 2U);
    CHECK(result.channelConfidence == audio::ChannelConfidence::Unknown);
    CHECK_FALSE(result.fullyVerified);
    CHECK(result.pipelineQuality == audio::Quality::BitwisePerfect);
    CHECK(result.assessments.front().optFormat == graph.nodes.front().optFormat);

    auto const state = rt::QualityState{.sourceQuality = result.sourceQuality,
                                        .pipelineQuality = result.pipelineQuality,
                                        .overall = result.overall,
                                        .fullyVerified = result.fullyVerified,
                                        .channelConfidence = result.channelConfidence,
                                        .assessments = result.assessments};
    auto const formatter = AudioQualityFormatter{ao::test::englishMessageCatalog()};
    auto const presentation = formatter.presentation(state);
    CHECK(presentation.headline == "Partially verified path");
    CHECK(presentation.category == AudioQualityCategory::Informational);
    CHECK(presentation.channelNote == "Channel layout not identified");
    CHECK(formatter.formatLabel(*state.assessments.front().optFormat) == "48.0 kHz · 16-bit · Stereo");
  }

  TEST_CASE("AudioQualityFormatter - channel conversion with equal endpoint descriptions stays generic",
            "[uimodel][unit][playback][quality]")
  {
    auto const formatter = AudioQualityFormatter{ao::test::englishMessageCatalog()};

    for (std::uint32_t const mask : {0U, 0x3U})
    {
      CAPTURE(mask);
      auto const format =
        audio::SignalFormat{.sampleRate = 48000, .channels = 2, .precisionBits = 16, .speakerMask = mask};
      auto const finding = audio::QualityFinding{.kind = audio::QualityFindingKind::ChannelMapping,
                                                 .quality = audio::Quality::LinearIntervention,
                                                 .optFromFormat = format,
                                                 .optToFormat = format};
      CHECK(formatter.findingLabel(finding) == "Channel mapping");
    }
  }

  TEST_CASE("AudioQualityFormatter - shared conversion without role change keeps its generic finding",
            "[uimodel][integration][playback][quality]")
  {
    auto const format =
      audio::SignalFormat{.sampleRate = 48000, .channels = 2, .precisionBits = 16, .speakerMask = 0x3};
    auto const graph = audio::flow::Graph{
      .nodes =
        {
          {.id = "ao-source",
           .type = audio::flow::NodeType::Source,
           .optFormat = format,
           .sourceChannelLayout = {.origin = audio::ChannelLayoutOrigin::Identified}},
          {.id = "sink",
           .type = audio::flow::NodeType::Sink,
           .optFormat = format,
           .channelDelivery = {.isMapped = true, .nativeSpeakerMask = 0x3, .hasSharedConversion = true}},
        },
      .connections = {{.sourceId = "ao-source", .destinationId = "sink"}},
    };
    auto const result = audio::analyzeAudioQuality(graph);
    REQUIRE(result.assessments.size() == 2U);
    REQUIRE(result.assessments.back().findings.size() == 1U);
    auto const& finding = result.assessments.back().findings.front();
    CHECK(finding.kind == audio::QualityFindingKind::ChannelMapping);
    CHECK_FALSE(finding.optFromFormat);
    CHECK(finding.optToFormat == graph.nodes.back().optFormat);
    CHECK(result.channelConfidence == audio::ChannelConfidence::Verified);
    CHECK(result.pipelineQuality == audio::Quality::LinearIntervention);

    auto const formatter = AudioQualityFormatter{ao::test::englishMessageCatalog()};
    CHECK(formatter.findingLabel(finding) == "Channel mapping");
    auto const state = rt::QualityState{.sourceQuality = result.sourceQuality,
                                        .pipelineQuality = result.pipelineQuality,
                                        .overall = result.overall,
                                        .fullyVerified = result.fullyVerified,
                                        .channelConfidence = result.channelConfidence,
                                        .assessments = result.assessments};
    auto const presentation = formatter.presentation(state);
    CHECK(presentation.headline == "Pipeline intervention");
    CHECK(presentation.category == AudioQualityCategory::Diagnostic);
    CHECK(presentation.channelNote.empty());
  }

  TEST_CASE("AudioQualityFormatter - channel count stays authoritative with partial or overfull role evidence",
            "[uimodel][unit][playback][quality]")
  {
    struct Case final
    {
      std::uint8_t channels;
      std::uint32_t mask;
      std::string_view label;
    };

    constexpr auto kCases = std::array{
      Case{1, 0x3, "48.0 kHz · 16-bit · 1 ch (FL FR)"},
      Case{2, 0x1, "48.0 kHz · 16-bit · 2 ch (FL)"},
      Case{2, 0x4, "48.0 kHz · 16-bit · 2 ch (FC)"},
    };
    auto const formatter = AudioQualityFormatter{ao::test::englishMessageCatalog()};

    for (auto const& testCase : kCases)
    {
      CAPTURE(testCase.channels, testCase.mask);
      CHECK(formatter.formatLabel(audio::SignalFormat{
              .sampleRate = 48000, .channels = testCase.channels, .precisionBits = 16, .speakerMask = testCase.mask}) ==
            testCase.label);
    }
  }

  TEST_CASE("AudioQualityFormatter - assigned height roles retain their names and order",
            "[uimodel][unit][playback][quality]")
  {
    struct Case final
    {
      std::uint32_t mask;
      std::string_view label;
    };

    constexpr auto kCases = std::array{
      Case{0x800, "48.0 kHz · 16-bit · 1 ch (TC)"},
      Case{0x1000, "48.0 kHz · 16-bit · 1 ch (TFL)"},
      Case{0x2000, "48.0 kHz · 16-bit · 1 ch (TFC)"},
      Case{0x4000, "48.0 kHz · 16-bit · 1 ch (TFR)"},
      Case{0x8000, "48.0 kHz · 16-bit · 1 ch (TBL)"},
      Case{0x10000, "48.0 kHz · 16-bit · 1 ch (TBC)"},
      Case{0x20000, "48.0 kHz · 16-bit · 1 ch (TBR)"},
    };
    auto const formatter = AudioQualityFormatter{ao::test::englishMessageCatalog()};

    for (auto const& testCase : kCases)
    {
      CAPTURE(testCase.mask);
      CHECK(formatter.formatLabel(audio::SignalFormat{
              .sampleRate = 48000, .channels = 1, .precisionBits = 16, .speakerMask = testCase.mask}) ==
            testCase.label);
    }

    CHECK(formatter.formatLabel(
            audio::SignalFormat{.sampleRate = 48000, .channels = 18, .precisionBits = 16, .speakerMask = 0x3FFFF}) ==
          "48.0 kHz · 16-bit · 18 ch (FL FR FC LFE BL BR FLC FRC BC SL SR TC TFL TFC TFR TBL TBC TBR)");
  }
} // namespace ao::uimodel::test
