// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "lib/audio/DecoderFactory.h"
#include "test/MessageCatalogTestSupport.h"
#include "test/audio/AudioFixtureSupport.h"
#include <ao/audio/ChannelDeliveryEvidence.h>
#include <ao/audio/ChannelLayoutEvidence.h>
#include <ao/audio/DecodedStreamInfo.h>
#include <ao/audio/Quality.h>
#include <ao/audio/QualityAnalyzer.h>
#include <ao/audio/SampleEncoding.h>
#include <ao/audio/Transport.h>
#include <ao/audio/flow/Graph.h>
#include <ao/rt/PlaybackState.h>
#include <ao/uimodel/playback/quality/AudioQualityFormatter.h>
#include <ao/uimodel/playback/soul/AobusSoulViewModel.h>

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <string_view>

namespace ao::uimodel::test
{
  namespace
  {
    audio::flow::Graph decodedGraph(audio::DecodedStreamInfo const& info)
    {
      // The controlled downstream path preserves PCM but supplies no native
      // speaker-mapping proof. Decoder facts must retain their real provenance.
      return {
        .nodes =
          {
            {.id = "ao-source",
             .type = audio::flow::NodeType::Source,
             .optFormat = info.sourceFormat,
             .isLossySource = info.isLossy,
             .sourceChannelLayout = info.sourceChannelLayout},
            {.id = "ao-decoder", .type = audio::flow::NodeType::Decoder, .optFormat = info.outputFormat},
            {.id = "ao-engine", .type = audio::flow::NodeType::Engine, .optFormat = info.outputFormat},
            {.id = "stream", .type = audio::flow::NodeType::Stream, .optFormat = info.outputFormat},
            {.id = "sink", .type = audio::flow::NodeType::Sink, .optFormat = info.outputFormat},
          },
        .connections =
          {
            {.sourceId = "ao-source", .destinationId = "ao-decoder"},
            {.sourceId = "ao-decoder", .destinationId = "ao-engine"},
            {.sourceId = "ao-engine", .destinationId = "stream"},
            {.sourceId = "stream", .destinationId = "sink"},
          },
      };
    }

    rt::QualityState qualityState(audio::QualityResult const& result)
    {
      return {.sourceQuality = result.sourceQuality,
              .pipelineQuality = result.pipelineQuality,
              .overall = result.overall,
              .fullyVerified = result.fullyVerified,
              .channelConfidence = result.channelConfidence,
              .assessments = result.assessments};
    }
  } // namespace

  TEST_CASE("AudioQualityFormatter - decoded compressed streams retain numerical delivery headlines")
  {
    struct Case final
    {
      std::string_view fileName;
      audio::SampleEncoding encoding;
      audio::Quality pipeline;
      std::string_view headline;
      AudioQualityCategory category;
      SoulAura aura;
    };

    constexpr auto kCases = std::array{
      Case{"basic_metadata.flac",
           audio::SampleEncoding::Signed16Le,
           audio::Quality::BitwisePerfect,
           "Bit-perfect PCM (observed software path)",
           AudioQualityCategory::Medal,
           SoulAura::Radiant},
      Case{"alac16.m4a",
           audio::SampleEncoding::Signed16Le,
           audio::Quality::BitwisePerfect,
           "Bit-perfect PCM (observed software path)",
           AudioQualityCategory::Medal,
           SoulAura::Radiant},
      Case{"basic_metadata.flac",
           audio::SampleEncoding::Signed24PackedLe,
           audio::Quality::LosslessPadded,
           "Signal preserved",
           AudioQualityCategory::Positive,
           SoulAura::Flowing},
      Case{"basic_metadata.flac",
           audio::SampleEncoding::Float32Le,
           audio::Quality::LosslessFloat,
           "Signal preserved",
           AudioQualityCategory::Positive,
           SoulAura::Flowing},
      Case{"basic_metadata.mp3",
           audio::SampleEncoding::Signed16Le,
           audio::Quality::BitwisePerfect,
           "Clean lossy delivery",
           AudioQualityCategory::Informational,
           SoulAura::Veiled},
    };
    auto const formatter = AudioQualityFormatter{ao::test::englishMessageCatalog()};

    for (auto const& testCase : kCases)
    {
      CAPTURE(testCase.fileName, testCase.encoding);
      auto decoderRes =
        audio::openDecoderSession(audio::test::requireAudioFixture(testCase.fileName), testCase.encoding);
      REQUIRE(decoderRes);
      auto const info = (*decoderRes)->streamInfo();
      auto const graph = decodedGraph(info);
      auto const result = audio::analyzeAudioQuality(graph);
      auto const state = qualityState(result);
      auto const presentation = formatter.presentation(state);

      CHECK(result.fullyVerified);
      CHECK(result.sourceQuality == (info.isLossy ? audio::Quality::LossySource : audio::Quality::BitwisePerfect));
      CHECK(result.pipelineQuality == testCase.pipeline);
      CHECK(result.channelConfidence != audio::ChannelConfidence::Verified);
      REQUIRE_FALSE(result.assessments.empty());
      CHECK(result.assessments.front().optFormat == graph.nodes.front().optFormat);
      CHECK(presentation.headline == testCase.headline);
      CHECK(presentation.category == testCase.category);
      CHECK_FALSE(presentation.channelNote.empty());
      CHECK(resolveSoulAura(audio::Transport::Playing, true, state) == testCase.aura);
    }
  }

  TEST_CASE("AudioQualityFormatter - unidentified source keeps numerical delivery and count-only text")
  {
    // This controlled source owns Unknown coverage; real decoder producers may
    // independently gain channel identification without changing these rules.
    auto const info = audio::DecodedStreamInfo{
      .sourceFormat = {.sampleRate = 44100, .channels = 2, .precisionBits = 16, .speakerMask = 0},
      .outputFormat = {.sampleRate = 44100,
                       .channels = 2,
                       .encoding = audio::SampleEncoding::Signed16Le,
                       .speakerMask = 0},
      .sourceChannelLayout = {.origin = audio::ChannelLayoutOrigin::Unknown},
    };
    auto const result = audio::analyzeAudioQuality(decodedGraph(info));
    auto const state = qualityState(result);
    auto const formatter = AudioQualityFormatter{ao::test::englishMessageCatalog()};
    auto const presentation = formatter.presentation(state);

    CHECK(result.fullyVerified);
    CHECK(result.pipelineQuality == audio::Quality::BitwisePerfect);
    CHECK(result.channelConfidence == audio::ChannelConfidence::Unknown);
    CHECK(presentation.headline == "Bit-perfect PCM (observed software path)");
    CHECK(presentation.category == AudioQualityCategory::Medal);
    CHECK(presentation.channelNote == "Channel layout not identified");
    CHECK(formatter.formatLabel(info.sourceFormat) == "44.1 kHz · 16-bit · 2 ch");
    CHECK(resolveSoulAura(audio::Transport::Playing, true, state) == SoulAura::Radiant);
  }

  TEST_CASE("AudioQualityFormatter - decoded lossless delivery keeps real interventions ahead of positives")
  {
    auto decoderRes = audio::openDecoderSession(
      audio::test::requireAudioFixture("basic_metadata.flac"), audio::SampleEncoding::Signed16Le);
    REQUIRE(decoderRes);
    auto const info = (*decoderRes)->streamInfo();
    auto graph = decodedGraph(info);
    auto& sink = graph.nodes.back();
    auto expectedCategory = AudioQualityCategory::Diagnostic;
    auto expectedHeadline = std::string_view{"Pipeline intervention"};

    SECTION("known channel conversion")
    {
      sink.channelDelivery.hasSharedConversion = true;
    }

    SECTION("resampling")
    {
      auto output = info.outputFormat;
      output.sampleRate = 48000;
      sink.optFormat = output;
    }

    SECTION("software attenuation")
    {
      sink.softwareVolumeNotUnity = true;
      sink.minSoftwareGain = 0.5F;
      sink.maxSoftwareGain = 0.5F;
    }

    SECTION("software amplification")
    {
      sink.softwareVolumeNotUnity = true;
      sink.maxSoftwareGain = 1.5F;
      expectedCategory = AudioQualityCategory::Warning;
      expectedHeadline = "Clipping risk";
    }

    SECTION("mute")
    {
      sink.isMuted = true;
    }

    auto const result = audio::analyzeAudioQuality(graph);
    auto const state = qualityState(result);
    auto const presentation = AudioQualityFormatter{ao::test::englishMessageCatalog()}.presentation(state);
    CHECK(result.sourceQuality == audio::Quality::BitwisePerfect);
    CHECK(result.pipelineQuality == audio::Quality::LinearIntervention);
    CHECK(presentation.headline == expectedHeadline);
    CHECK(presentation.category == expectedCategory);

    // No native mapping proof is supplied, even if the decoder identifies its
    // source layout. Channel uncertainty does not outrank the intervention.
    CHECK(result.channelConfidence != audio::ChannelConfidence::Verified);
    CHECK_FALSE(presentation.channelNote.empty());
    CHECK(resolveSoulAura(audio::Transport::Playing, true, state) == SoulAura::Turbulent);
  }
} // namespace ao::uimodel::test
