// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/uimodel/playback/quality/AudioQualityFormatter.h>

#include "test/MessageCatalogTestSupport.h"
#include <ao/audio/ChannelDeliveryEvidence.h>
#include <ao/audio/ChannelLayoutEvidence.h>
#include <ao/audio/NodeFormat.h>
#include <ao/audio/PcmFormat.h>
#include <ao/audio/Quality.h>
#include <ao/audio/QualityAnalyzer.h>
#include <ao/audio/SampleEncoding.h>
#include <ao/audio/SignalFormat.h>
#include <ao/audio/flow/Graph.h>
#include <ao/rt/PlaybackState.h>

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace ao::uimodel::test
{
  namespace
  {
    audio::flow::Graph identifiedChannelGraph(std::uint8_t const channels,
                                              std::uint32_t const sourceMask,
                                              audio::ChannelDeliveryEvidence const sinkDelivery)
    {
      return {
        .nodes =
          {
            {.id = "ao-source",
             .type = audio::flow::NodeType::Source,
             .optFormat = audio::SignalFormat{.sampleRate = 48000,
                                              .channels = channels,
                                              .precisionBits = 16,
                                              .sampleKind = audio::SampleKind::Integer,
                                              .speakerMask = sourceMask},
             .sourceChannelLayout = {.origin = audio::ChannelLayoutOrigin::Identified}},
            {.id = "stream",
             .type = audio::flow::NodeType::Stream,
             .optFormat =
               audio::PcmFormat{
                 .sampleRate = 48000, .channels = channels, .encoding = audio::SampleEncoding::Signed16Le},
             .channelDelivery = {.isMapped = true, .nativeSpeakerMask = sourceMask}},
            {.id = "sink",
             .type = audio::flow::NodeType::Sink,
             .optFormat =
               audio::PcmFormat{
                 .sampleRate = 48000, .channels = channels, .encoding = audio::SampleEncoding::Signed16Le},
             .channelDelivery = sinkDelivery},
          },
        .connections = {{.sourceId = "ao-source", .destinationId = "stream", .isActive = true},
                        {.sourceId = "stream", .destinationId = "sink", .isActive = true}},
      };
    }

    AudioQualityFormatter const& englishFormatter()
    {
      static auto const kFormatter = AudioQualityFormatter{ao::test::englishMessageCatalog()};
      return kFormatter;
    }

    std::string audioNodeTypeLabel(audio::flow::NodeType const type)
    {
      return englishFormatter().nodeTypeLabel(type);
    }

    std::string audioFormatLabel(audio::NodeFormat const& format)
    {
      return englishFormatter().formatLabel(format);
    }

    std::string audioFindingLabel(audio::QualityFinding const& finding)
    {
      return englishFormatter().findingLabel(finding);
    }

    std::string audioQualityConclusion(audio::Quality const quality)
    {
      return englishFormatter().qualityConclusion(quality);
    }

    AudioQualityPresentation audioQualityPresentation(rt::QualityState const& state)
    {
      return englishFormatter().presentation(state);
    }

    rt::QualityState withChannelConfidence(rt::QualityState state, audio::ChannelConfidence const confidence)
    {
      state.channelConfidence = confidence;
      return state;
    }
  } // namespace

  TEST_CASE("AudioQualityFormatter - audioNodeTypeLabel")
  {
    CHECK(audioNodeTypeLabel(audio::flow::NodeType::Source) == "[Source]");
    CHECK(audioNodeTypeLabel(audio::flow::NodeType::Decoder) == "[Decoder]");
    CHECK(audioNodeTypeLabel(audio::flow::NodeType::Engine) == "[Engine]");
    CHECK(audioNodeTypeLabel(audio::flow::NodeType::Stream) == "[Stream]");
    CHECK(audioNodeTypeLabel(audio::flow::NodeType::Intermediary) == "[Filter]");
    CHECK(audioNodeTypeLabel(audio::flow::NodeType::Sink) == "[Device]");
    CHECK(audioNodeTypeLabel(audio::flow::NodeType::ExternalSource) == "[Other Source]");
  }

  TEST_CASE("AudioQualityFormatter - audioFormatLabel")
  {
    auto format = audio::SignalFormat{.sampleRate = 44100, .channels = 2, .precisionBits = 16, .speakerMask = 0x3};
    CHECK(audioFormatLabel(format) == "44.1 kHz · 16-bit · Stereo");

    format.channels = 1;
    format.speakerMask = 0x4;
    CHECK(audioFormatLabel(format) == "44.1 kHz · 16-bit · Mono");

    format.channels = 0;
    format.speakerMask = 0;
    CHECK(audioFormatLabel(format) == "44.1 kHz · 16-bit · 0 ch");

    format.channels = 6;
    format.sampleRate = 48000;
    format.precisionBits = 24;
    CHECK(audioFormatLabel(format) == "48.0 kHz · 24-bit · 6 ch");

    auto const pcmFormat = audio::PcmFormat{
      .sampleRate = 44100, .channels = 2, .encoding = audio::SampleEncoding::Signed32Le, .speakerMask = 0x3};
    CHECK(audioFormatLabel(pcmFormat) == "44.1 kHz · 32-bit · Stereo");
  }

  TEST_CASE("AudioQualityFormatter - formatLabel names only effective speaker identities")
  {
    auto const signal =
      [](std::uint8_t const channels, std::uint32_t const speakerMask, std::uint8_t const precisionBits = 16)
    {
      return audio::SignalFormat{
        .sampleRate = 44100, .channels = channels, .precisionBits = precisionBits, .speakerMask = speakerMask};
    };

    // Only the FC mono set carries the Mono word; FL-only mono keeps its role.
    CHECK(audioFormatLabel(signal(1, 0x4)) == "44.1 kHz · 16-bit · Mono");
    CHECK(audioFormatLabel(signal(1, 0x1)) == "44.1 kHz · 16-bit · 1 ch (FL)");

    // A zero mask assigns no speakers; the count never guesses roles.
    CHECK(audioFormatLabel(signal(1, 0)) == "44.1 kHz · 16-bit · 1 ch");
    CHECK(audioFormatLabel(signal(2, 0)) == "44.1 kHz · 16-bit · 2 ch");

    // Complete alternate sets retain their roles instead of a count-only relabel.
    CHECK(audioFormatLabel(signal(2, 0x30)) == "44.1 kHz · 16-bit · 2 ch (BL BR)");
    CHECK(audioFormatLabel(signal(2, 0x600)) == "44.1 kHz · 16-bit · 2 ch (SL SR)");
    CHECK(audioFormatLabel(signal(6, 0x3F, 24)) == "44.1 kHz · 24-bit · 6 ch (FL FR FC LFE BL BR)");

    // A partial two-channel format retains its count and only the observed role.
    CHECK(audioFormatLabel(signal(2, 0x1)) == "44.1 kHz · 16-bit · 2 ch (FL)");

    // Assigned height positions are named; reserved and wildcard bits stay raw.
    CHECK(audioFormatLabel(signal(1, 0x1000)) == "44.1 kHz · 16-bit · 1 ch (TFL)");
    CHECK(audioFormatLabel(signal(1, 0x40000)) == "44.1 kHz · 16-bit · 1 ch (0x40000)");
    CHECK(audioFormatLabel(signal(2, 0x80000000U)) == "44.1 kHz · 16-bit · 2 ch (0x80000000)");

    CHECK(audioFormatLabel(signal(2, 0x1001)) == "44.1 kHz · 16-bit · 2 ch (FL TFL)");
    // Mixing named roles with reserved bits must not discard the latter.
    CHECK(audioFormatLabel(signal(2, 0x40003)) == "44.1 kHz · 16-bit · 2 ch (0x40003)");
  }

  TEST_CASE("AudioQualityFormatter - channel mapping findings distinguish same-count relabels")
  {
    auto const frontPair =
      audio::SignalFormat{.sampleRate = 44100, .channels = 2, .precisionBits = 16, .speakerMask = 0x3};
    auto const sidePair =
      audio::SignalFormat{.sampleRate = 44100, .channels = 2, .precisionBits = 16, .speakerMask = 0x600};
    auto const sixChannel =
      audio::SignalFormat{.sampleRate = 44100, .channels = 6, .precisionBits = 16, .speakerMask = 0x3F};
    auto const unassignedPair = audio::SignalFormat{.sampleRate = 44100, .channels = 2, .precisionBits = 16};

    CHECK(audioFindingLabel(audio::QualityFinding{
            .kind = audio::QualityFindingKind::ChannelMapping, .optFromFormat = frontPair, .optToFormat = sidePair}) ==
          "Channel layout: Stereo → 2 ch (SL SR)");

    CHECK(audioFindingLabel(audio::QualityFinding{.kind = audio::QualityFindingKind::ChannelMapping,
                                                  .optFromFormat = frontPair,
                                                  .optToFormat = unassignedPair}) == "Channel layout: Stereo → 2 ch");

    CHECK(audioFindingLabel(audio::QualityFinding{.kind = audio::QualityFindingKind::ChannelMapping,
                                                  .optFromFormat = frontPair,
                                                  .optToFormat = sixChannel}) == "Channels: 2ch → 6ch");

    CHECK(audioFindingLabel(audio::QualityFinding{.kind = audio::QualityFindingKind::ChannelMapping}) ==
          "Channel mapping");
  }

  TEST_CASE("AudioQualityFormatter - same-count independent mask overrides retain whole raw layouts")
  {
    auto const frontPair =
      audio::SignalFormat{.sampleRate = 44100, .channels = 2, .precisionBits = 16, .speakerMask = 0x3};

    // Identical stored layouts must not hide independent role evidence on either side.
    CHECK(audioFindingLabel(audio::QualityFinding{.kind = audio::QualityFindingKind::ChannelMapping,
                                                  .optFromFormat = frontPair,
                                                  .optToFormat = frontPair,
                                                  .optFromSpeakerMask = 0x40001}) ==
          "Channel layout: 2 ch (0x40001) → Stereo");
    CHECK(audioFindingLabel(audio::QualityFinding{.kind = audio::QualityFindingKind::ChannelMapping,
                                                  .optFromFormat = frontPair,
                                                  .optToFormat = frontPair,
                                                  .optToSpeakerMask = 0x40000}) ==
          "Channel layout: Stereo → 2 ch (0x40000)");
    CHECK(audioFindingLabel(audio::QualityFinding{.kind = audio::QualityFindingKind::ChannelMapping,
                                                  .optFromFormat = frontPair,
                                                  .optToFormat = frontPair,
                                                  .optFromSpeakerMask = 0x40001,
                                                  .optToSpeakerMask = 0x80000000U}) ==
          "Channel layout: 2 ch (0x40001) → 2 ch (0x80000000)");
  }

  TEST_CASE("AudioQualityFormatter - analyzer findings retain independently observed native layouts")
  {
    auto graph = identifiedChannelGraph(4, 0x33, {.isMapped = true});
    std::size_t changedIndex = 2;
    std::uint32_t expectedStorageMask = 0;

    SECTION("native sets differ while both storage masks remain unknown")
    {
      graph.nodes[2].channelDelivery.nativeSpeakerMask = 0x107;
    }

    SECTION("a native set contradicts the node's unchanged canonical format")
    {
      changedIndex = 1;
      expectedStorageMask = 0x33;
      graph.nodes[1].optFormat = audio::PcmFormat{
        .sampleRate = 48000, .channels = 4, .encoding = audio::SampleEncoding::Signed16Le, .speakerMask = 0x33};
      graph.nodes[1].channelDelivery.nativeSpeakerMask = 0x107;
      graph.nodes[2].channelDelivery.nativeSpeakerMask = 0x33;
    }

    auto const result = audio::analyzeAudioQuality(graph);
    // Both contradictory sets are proved positions: the finding owns the
    // conversion, and the confidence stays independent of it.
    CHECK(result.channelConfidence == audio::ChannelConfidence::Verified);
    CHECK(result.pipelineQuality == audio::Quality::LinearIntervention);
    REQUIRE(result.assessments.size() == 3U);
    auto const& assessment = result.assessments[changedIndex];
    REQUIRE(assessment.findings.size() == 1U);
    auto const& finding = assessment.findings.front();
    CHECK(finding.kind == audio::QualityFindingKind::ChannelMapping);
    REQUIRE(finding.optFromFormat);
    REQUIRE(finding.optToFormat);
    CHECK(finding.optFromSpeakerMask.value_or(audio::signalFormat(*finding.optFromFormat).speakerMask) == 0x33U);
    CHECK(finding.optToSpeakerMask == 0x107U);
    CHECK(audioFindingLabel(finding) == "Channel layout: 4 ch (FL FR BL BR) → 4 ch (FL FR FC BC)");
    // Position-set evidence must not rewrite the canonical storage contract.
    CHECK(audio::signalFormat(*finding.optToFormat).speakerMask == expectedStorageMask);
    CHECK(audio::signalFormat(*graph.nodes[changedIndex].optFormat).speakerMask == expectedStorageMask);
  }

  TEST_CASE("AudioQualityFormatter - partial native constraints remain visible without guessed roles")
  {
    auto const graph = identifiedChannelGraph(2, 0x3, {.nativeSpeakerMask = 0x4});
    auto const result = audio::analyzeAudioQuality(graph);
    CHECK(result.channelConfidence == audio::ChannelConfidence::Unverified);
    CHECK(result.pipelineQuality == audio::Quality::LinearIntervention);
    REQUIRE(result.assessments.size() == 3U);
    REQUIRE(result.assessments[2].findings.size() == 1U);
    auto const& finding = result.assessments[2].findings.front();
    CHECK(finding.kind == audio::QualityFindingKind::ChannelMapping);
    CHECK(finding.optFromSpeakerMask == 0x3U);
    CHECK(finding.optToSpeakerMask == 0x4U);
    CHECK(audioFindingLabel(finding) == "Channel layout: Stereo → 2 ch (FC)");
    REQUIRE(finding.optToFormat);
    CHECK(audio::signalFormat(*finding.optToFormat).speakerMask == 0U);
    // Without endpoint geometry, role evidence still does not invent a count.
    CHECK(audioFindingLabel(audio::QualityFinding{
            .kind = audio::QualityFindingKind::ChannelMapping, .optFromSpeakerMask = 0x3, .optToSpeakerMask = 0x4}) ==
          "Channel mapping");
  }

  TEST_CASE("AudioQualityFormatter - long channel layout finding labels stay intact across locales")
  {
    // Different four-channel sets keep both argument strings alive through
    // formatting. These longer labels also exceed MSVC's small-string capacity;
    // other standard libraries need not use the same capacity.
    auto const quadSurround =
      audio::SignalFormat{.sampleRate = 44100, .channels = 4, .precisionBits = 16, .speakerMask = 0x33};
    auto const frontCenterBack =
      audio::SignalFormat{.sampleRate = 44100, .channels = 4, .precisionBits = 16, .speakerMask = 0x107};
    auto const layoutFinding = audio::QualityFinding{
      .kind = audio::QualityFindingKind::ChannelMapping, .optFromFormat = quadSurround, .optToFormat = frontCenterBack};

    CHECK(audioFindingLabel(layoutFinding) == "Channel layout: 4 ch (FL FR BL BR) → 4 ch (FL FR FC BC)");

    auto const german = AudioQualityFormatter{ao::test::messageCatalog("de-DE")};
    CHECK(german.findingLabel(layoutFinding) == "Kanallayout: 4 Kan. (FL FR BL BR) → 4 Kan. (FL FR FC BC)");
  }

  TEST_CASE("AudioQualityFormatter - quality categories map raw quality to visual tiers")
  {
    CHECK(audioQualityCategory(audio::Quality::BitwisePerfect) == AudioQualityCategory::Medal);
    CHECK(audioQualityCategory(audio::Quality::LosslessPadded) == AudioQualityCategory::Positive);
    CHECK(audioQualityCategory(audio::Quality::LosslessFloat) == AudioQualityCategory::Positive);
    CHECK(audioQualityCategory(audio::Quality::LinearIntervention) == AudioQualityCategory::Diagnostic);
    CHECK(audioQualityCategory(audio::Quality::LossySource) == AudioQualityCategory::Informational);
    CHECK(audioQualityCategory(audio::Quality::Clipped) == AudioQualityCategory::Clipped);
    CHECK(audioQualityCategory(audio::Quality::Unknown) == AudioQualityCategory::Unknown);

    CHECK(audioFindingCategory(audio::QualityFinding{
            .kind = audio::QualityFindingKind::SoftwareAmplification, .quality = audio::Quality::LinearIntervention}) ==
          AudioQualityCategory::Warning);
    CHECK(audioFindingCategory(audio::QualityFinding{
            .kind = audio::QualityFindingKind::Resampling, .quality = audio::Quality::LinearIntervention}) ==
          AudioQualityCategory::Diagnostic);
  }

  TEST_CASE("AudioQualityFormatter - precision findings describe effective domain changes")
  {
    auto const float32 =
      audio::PcmFormat{.sampleRate = 44100, .channels = 2, .encoding = audio::SampleEncoding::Float32Le};
    auto const int32 =
      audio::PcmFormat{.sampleRate = 44100, .channels = 2, .encoding = audio::SampleEncoding::Signed32Le};
    auto const int24In32 =
      audio::PcmFormat{.sampleRate = 44100, .channels = 2, .encoding = audio::SampleEncoding::Signed24In32Le};

    CHECK(audioFindingLabel(audio::QualityFinding{
            .kind = audio::QualityFindingKind::Truncation, .optFromFormat = float32, .optToFormat = int24In32}) ==
          "Float → integer quantization: 32f → 24b");
    CHECK(audioFindingLabel(audio::QualityFinding{
            .kind = audio::QualityFindingKind::Truncation, .optFromFormat = int32, .optToFormat = float32}) ==
          "Integer → float quantization: 32b → 32f");
    CHECK(audioFindingLabel(audio::QualityFinding{
            .kind = audio::QualityFindingKind::Truncation, .optFromFormat = int32, .optToFormat = int24In32}) ==
          "Precision truncated: 32b → 24b");
  }

  TEST_CASE("AudioQualityFormatter - volume findings describe software gain magnitude")
  {
    CHECK(audioFindingLabel(audio::QualityFinding{.kind = audio::QualityFindingKind::SoftwareVolumeModification,
                                                  .gain = 0.5F}) == "Software volume attenuation: -6.0 dB");
    CHECK(audioFindingLabel(
            audio::QualityFinding{.kind = audio::QualityFindingKind::SoftwareAmplification, .gain = 1.5F}) ==
          "Software amplification: +3.5 dB gain (clipping risk)");
    CHECK(audioFindingLabel(audio::QualityFinding{.kind = audio::QualityFindingKind::SoftwareAmplification}) ==
          "Software amplification (clipping risk)");
  }

  TEST_CASE("AudioQualityFormatter - unclassified volume findings describe device gain magnitude")
  {
    CHECK(audioFindingLabel(audio::QualityFinding{.kind = audio::QualityFindingKind::UnclassifiedVolumeModification,
                                                  .gain = 0.5F}) == "Device volume: -6.0 dB (source unverified)");
    CHECK(audioFindingLabel(audio::QualityFinding{.kind = audio::QualityFindingKind::UnclassifiedVolumeModification,
                                                  .gain = 1.5F}) == "Device volume: +3.5 dB (source unverified)");
    CHECK(audioFindingLabel(audio::QualityFinding{.kind = audio::QualityFindingKind::UnclassifiedVolumeModification}) ==
          "Device volume change (source unverified)");
  }

  TEST_CASE("AudioQualityFormatter - signal-preserved quality conclusions share one verdict")
  {
    CHECK(audioQualityConclusion(audio::Quality::BitwisePerfect) == "Bit-perfect PCM (observed software path)");
    CHECK(audioQualityConclusion(audio::Quality::LosslessPadded) == "Signal preserved");
    CHECK(audioQualityConclusion(audio::Quality::LosslessFloat) == "Signal preserved");
  }

  TEST_CASE("AudioQualityFormatter - presentation headlines prioritize pipeline delivery")
  {
    auto const sourceFormat = audio::SignalFormat{.sampleRate = 44100, .channels = 2, .precisionBits = 16};
    auto const paddedFormat =
      audio::PcmFormat{.sampleRate = 44100, .channels = 2, .encoding = audio::SampleEncoding::Signed24PackedLe};
    auto const floatFormat =
      audio::PcmFormat{.sampleRate = 44100, .channels = 2, .encoding = audio::SampleEncoding::Float32Le};
    auto const resampledFormat =
      audio::PcmFormat{.sampleRate = 48000, .channels = 2, .encoding = audio::SampleEncoding::Signed16Le};

    SECTION("lossless clean path is bit-perfect")
    {
      auto const presentation = audioQualityPresentation(withChannelConfidence(
        rt::QualityState{.sourceQuality = audio::Quality::BitwisePerfect,
                         .pipelineQuality = audio::Quality::BitwisePerfect,
                         .overall = audio::Quality::BitwisePerfect,
                         .assessments =
                           {
                             audio::NodeQualityAssessment{
                               .nodeId = "ao-source",
                               .nodeType = audio::flow::NodeType::Source,
                               .optFormat = sourceFormat,
                               .findings = {audio::QualityFinding{.kind = audio::QualityFindingKind::BitPerfect,
                                                                  .quality = audio::Quality::BitwisePerfect}},
                             },
                           }},
        audio::ChannelConfidence::Verified));

      CHECK(presentation.headline == "Bit-perfect PCM (observed software path)");
      CHECK(presentation.category == AudioQualityCategory::Medal);
    }

    SECTION("padding preserves the signal")
    {
      auto const presentation = audioQualityPresentation(withChannelConfidence(
        rt::QualityState{.sourceQuality = audio::Quality::BitwisePerfect,
                         .pipelineQuality = audio::Quality::LosslessPadded,
                         .overall = audio::Quality::LosslessPadded,
                         .assessments =
                           {
                             audio::NodeQualityAssessment{
                               .nodeId = "ao-engine",
                               .nodeType = audio::flow::NodeType::Engine,
                               .optFormat = paddedFormat,
                               .findings = {audio::QualityFinding{.kind = audio::QualityFindingKind::LosslessPadding,
                                                                  .quality = audio::Quality::LosslessPadded,
                                                                  .optFromFormat = sourceFormat,
                                                                  .optToFormat = paddedFormat}},
                             },
                           }},
        audio::ChannelConfidence::Verified));

      CHECK(presentation.headline == "Signal preserved");
      CHECK(presentation.category == AudioQualityCategory::Positive);
    }

    SECTION("float round trip preserves the signal")
    {
      auto const presentation = audioQualityPresentation(withChannelConfidence(
        rt::QualityState{.sourceQuality = audio::Quality::BitwisePerfect,
                         .pipelineQuality = audio::Quality::LosslessFloat,
                         .overall = audio::Quality::LosslessFloat,
                         .assessments =
                           {
                             audio::NodeQualityAssessment{
                               .nodeId = "ao-sink",
                               .nodeType = audio::flow::NodeType::Sink,
                               .optFormat = sourceFormat,
                               .findings = {audio::QualityFinding{.kind = audio::QualityFindingKind::LosslessRoundTrip,
                                                                  .quality = audio::Quality::LosslessFloat,
                                                                  .optFromFormat = floatFormat,
                                                                  .optToFormat = sourceFormat}},
                             },
                           }},
        audio::ChannelConfidence::Verified));

      CHECK(presentation.headline == "Signal preserved");
      CHECK(presentation.category == AudioQualityCategory::Positive);
    }

    SECTION("lossy clean source is clean delivery")
    {
      auto const presentation = audioQualityPresentation(withChannelConfidence(
        rt::QualityState{.sourceQuality = audio::Quality::LossySource,
                         .pipelineQuality = audio::Quality::BitwisePerfect,
                         .overall = audio::Quality::LossySource,
                         .assessments =
                           {
                             audio::NodeQualityAssessment{
                               .nodeId = "ao-source",
                               .nodeType = audio::flow::NodeType::Source,
                               .optFormat = sourceFormat,
                               .findings = {audio::QualityFinding{.kind = audio::QualityFindingKind::LossySource,
                                                                  .quality = audio::Quality::LossySource}},
                             },
                           }},
        audio::ChannelConfidence::Verified));

      CHECK(presentation.headline == "Clean lossy delivery");
      CHECK(presentation.category == AudioQualityCategory::Informational);
    }

    SECTION("resampling is diagnostic and reports rate change")
    {
      auto const presentation = audioQualityPresentation(
        rt::QualityState{.sourceQuality = audio::Quality::BitwisePerfect,
                         .pipelineQuality = audio::Quality::LinearIntervention,
                         .overall = audio::Quality::LinearIntervention,
                         .assessments = {
                           audio::NodeQualityAssessment{
                             .nodeId = "ao-engine",
                             .nodeType = audio::flow::NodeType::Engine,
                             .optFormat = resampledFormat,
                             .findings = {audio::QualityFinding{.kind = audio::QualityFindingKind::Resampling,
                                                                .quality = audio::Quality::LinearIntervention,
                                                                .optFromFormat = sourceFormat,
                                                                .optToFormat = resampledFormat}},
                           },
                         }});

      CHECK(presentation.headline == "Pipeline intervention");
      CHECK(presentation.category == AudioQualityCategory::Diagnostic);
    }

    SECTION("channel mapping is diagnostic")
    {
      auto const monoFormat =
        audio::PcmFormat{.sampleRate = 44100, .channels = 1, .encoding = audio::SampleEncoding::Signed16Le};
      auto const presentation = audioQualityPresentation(
        rt::QualityState{.sourceQuality = audio::Quality::BitwisePerfect,
                         .pipelineQuality = audio::Quality::LinearIntervention,
                         .overall = audio::Quality::LinearIntervention,
                         .assessments = {
                           audio::NodeQualityAssessment{
                             .nodeId = "ao-engine",
                             .nodeType = audio::flow::NodeType::Engine,
                             .optFormat = monoFormat,
                             .findings = {audio::QualityFinding{.kind = audio::QualityFindingKind::ChannelMapping,
                                                                .quality = audio::Quality::LinearIntervention,
                                                                .optFromFormat = sourceFormat,
                                                                .optToFormat = monoFormat}},
                           },
                         }});

      CHECK(presentation.headline == "Pipeline intervention");
      CHECK(presentation.category == AudioQualityCategory::Diagnostic);
    }

    SECTION("software amplification is a warning")
    {
      auto const presentation = audioQualityPresentation(rt::QualityState{
        .sourceQuality = audio::Quality::BitwisePerfect,
        .pipelineQuality = audio::Quality::LinearIntervention,
        .overall = audio::Quality::LinearIntervention,
        .fullyVerified = false,
        .assessments = {
          audio::NodeQualityAssessment{
            .nodeId = "ao-engine",
            .nodeType = audio::flow::NodeType::Engine,
            .optFormat = sourceFormat,
            .findings = {audio::QualityFinding{.kind = audio::QualityFindingKind::SoftwareAmplification,
                                               .quality = audio::Quality::LinearIntervention,
                                               .gain = 1.5F}},
          },
        }});

      CHECK(presentation.headline == "Clipping risk");
      CHECK(presentation.category == AudioQualityCategory::Warning);
    }

    SECTION("unclassified volume is not presented as software")
    {
      auto const presentation = audioQualityPresentation(rt::QualityState{
        .sourceQuality = audio::Quality::BitwisePerfect,
        .pipelineQuality = audio::Quality::LinearIntervention,
        .overall = audio::Quality::LinearIntervention,
        .assessments = {
          audio::NodeQualityAssessment{
            .nodeId = "ao-sink",
            .nodeType = audio::flow::NodeType::Sink,
            .optFormat = sourceFormat,
            .findings = {audio::QualityFinding{.kind = audio::QualityFindingKind::UnclassifiedVolumeModification,
                                               .quality = audio::Quality::LinearIntervention}},
          },
        }});

      CHECK(presentation.headline == "Pipeline intervention");
      CHECK(presentation.category == AudioQualityCategory::Diagnostic);
    }

    SECTION("hardware volume remains bit-perfect presentation")
    {
      auto const state = withChannelConfidence(
        rt::QualityState{
          .sourceQuality = audio::Quality::BitwisePerfect,
          .pipelineQuality = audio::Quality::BitwisePerfect,
          .overall = audio::Quality::BitwisePerfect,
          .assessments =
            {
              audio::NodeQualityAssessment{
                .nodeId = "ao-sink",
                .nodeType = audio::flow::NodeType::Sink,
                .optFormat = sourceFormat,
                .findings = {audio::QualityFinding{.kind = audio::QualityFindingKind::HardwareVolumeModification,
                                                   .quality = audio::Quality::BitwisePerfect}},
              },
            }},
        audio::ChannelConfidence::Verified);
      auto const presentation = audioQualityPresentation(state);

      CHECK(presentation.headline == "Bit-perfect PCM (observed software path)");
      CHECK(presentation.category == AudioQualityCategory::Medal);
    }

    SECTION("otherwise clean unverified path is informational")
    {
      auto const presentation =
        audioQualityPresentation(rt::QualityState{.sourceQuality = audio::Quality::BitwisePerfect,
                                                  .pipelineQuality = audio::Quality::BitwisePerfect,
                                                  .overall = audio::Quality::BitwisePerfect,
                                                  .fullyVerified = false});

      CHECK(presentation.headline == "Partially verified path");
      CHECK(presentation.category == AudioQualityCategory::Informational);
    }
  }

  TEST_CASE("AudioQualityFormatter - layout notes remain independent of numerical headlines")
  {
    struct Case final
    {
      audio::ChannelConfidence confidence;
      std::string_view note;
      std::string_view simplifiedNote;
    };

    constexpr auto kCases = std::array{
      Case{audio::ChannelConfidence::Unknown, "Channel layout not identified", "声道布局未识别"},
      Case{audio::ChannelConfidence::Unverified, "Channel delivery not proved", "声道传递尚未证实"},
      Case{audio::ChannelConfidence::Inferred, "Source channel layout inferred", "源声道布局来自推断"},
      Case{audio::ChannelConfidence::Verified, "", ""},
    };
    auto const simplified = AudioQualityFormatter{ao::test::messageCatalog("zh-CN")};

    for (auto const& testCase : kCases)
    {
      auto state = withChannelConfidence(rt::QualityState{.sourceQuality = audio::Quality::BitwisePerfect,
                                                          .pipelineQuality = audio::Quality::BitwisePerfect,
                                                          .overall = audio::Quality::BitwisePerfect,
                                                          .assessments = {{.nodeType = audio::flow::NodeType::Source}}},
                                         testCase.confidence);
      auto const presentation = audioQualityPresentation(state);
      CHECK(presentation.headline == "Bit-perfect PCM (observed software path)");
      CHECK(presentation.category == AudioQualityCategory::Medal);
      CHECK(presentation.channelNote == testCase.note);
      CHECK(simplified.presentation(state).channelNote == testCase.simplifiedNote);
      CHECK(state.channelConfidence == testCase.confidence);

      state.sourceQuality = audio::Quality::LossySource;
      state.overall = audio::Quality::LossySource;
      auto const lossy = audioQualityPresentation(state);
      CHECK(lossy.headline == "Clean lossy delivery");
      CHECK(lossy.category == AudioQualityCategory::Informational);
      CHECK(lossy.channelNote == testCase.note);

      state.sourceQuality = audio::Quality::BitwisePerfect;

      for (auto const kind : {audio::QualityFindingKind::LosslessPadding,
                              audio::QualityFindingKind::LosslessFloat,
                              audio::QualityFindingKind::LosslessRoundTrip})
      {
        auto const quality = kind == audio::QualityFindingKind::LosslessPadding ? audio::Quality::LosslessPadded
                                                                                : audio::Quality::LosslessFloat;
        state.pipelineQuality = quality;
        state.overall = quality;
        state.assessments = {{.findings = {{.kind = kind, .quality = quality}}}};
        auto const preserved = audioQualityPresentation(state);
        CHECK(preserved.headline == "Signal preserved");
        CHECK(preserved.category == AudioQualityCategory::Positive);
        CHECK(preserved.channelNote == testCase.note);
      }
    }
  }

  TEST_CASE("AudioQualityFormatter - inferred source layout accompanies lossless numerical positives")
  {
    auto const sourceFormat =
      audio::SignalFormat{.sampleRate = 44100, .channels = 6, .precisionBits = 24, .speakerMask = 0x3F};
    auto const paddedFormat = audio::PcmFormat{
      .sampleRate = 44100, .channels = 6, .encoding = audio::SampleEncoding::Signed24PackedLe, .speakerMask = 0x3F};
    auto const floatFormat = audio::PcmFormat{
      .sampleRate = 44100, .channels = 6, .encoding = audio::SampleEncoding::Float32Le, .speakerMask = 0x3F};

    auto const paddedState = withChannelConfidence(
      rt::QualityState{.sourceQuality = audio::Quality::BitwisePerfect,
                       .pipelineQuality = audio::Quality::LosslessPadded,
                       .overall = audio::Quality::LosslessPadded,
                       .assessments =
                         {
                           audio::NodeQualityAssessment{
                             .nodeId = "ao-engine",
                             .nodeType = audio::flow::NodeType::Engine,
                             .optFormat = paddedFormat,
                             .findings = {audio::QualityFinding{.kind = audio::QualityFindingKind::LosslessPadding,
                                                                .quality = audio::Quality::LosslessPadded,
                                                                .optFromFormat = sourceFormat,
                                                                .optToFormat = paddedFormat}},
                           },
                         }},
      audio::ChannelConfidence::Inferred);

    // Numerical preservation and source-layout inference are separate claims.
    CHECK(paddedState.pipelineQuality == audio::Quality::LosslessPadded);
    CHECK(paddedState.overall == audio::Quality::LosslessPadded);
    auto const padded = audioQualityPresentation(paddedState);
    CHECK(padded.headline == "Signal preserved");
    CHECK(padded.category == AudioQualityCategory::Positive);
    CHECK(padded.channelNote == "Source channel layout inferred");

    auto const floatState = withChannelConfidence(
      rt::QualityState{.sourceQuality = audio::Quality::BitwisePerfect,
                       .pipelineQuality = audio::Quality::LosslessFloat,
                       .overall = audio::Quality::LosslessFloat,
                       .assessments =
                         {
                           audio::NodeQualityAssessment{
                             .nodeId = "ao-sink",
                             .nodeType = audio::flow::NodeType::Sink,
                             .optFormat = sourceFormat,
                             .findings = {audio::QualityFinding{.kind = audio::QualityFindingKind::LosslessRoundTrip,
                                                                .quality = audio::Quality::LosslessFloat,
                                                                .optFromFormat = floatFormat,
                                                                .optToFormat = sourceFormat}},
                           },
                         }},
      audio::ChannelConfidence::Inferred);

    CHECK(floatState.pipelineQuality == audio::Quality::LosslessFloat);
    CHECK(floatState.overall == audio::Quality::LosslessFloat);
    auto const floated = audioQualityPresentation(floatState);
    CHECK(floated.headline == "Signal preserved");
    CHECK(floated.category == AudioQualityCategory::Positive);
    CHECK(floated.channelNote == "Source channel layout inferred");

    // Verification removes the note without changing the numerical headline.
    auto const preservedPadded =
      audioQualityPresentation(withChannelConfidence(paddedState, audio::ChannelConfidence::Verified));
    CHECK(preservedPadded.headline == "Signal preserved");
    CHECK(preservedPadded.category == AudioQualityCategory::Positive);
    CHECK(preservedPadded.channelNote.empty());

    auto const preservedFloat =
      audioQualityPresentation(withChannelConfidence(floatState, audio::ChannelConfidence::Verified));
    CHECK(preservedFloat.headline == "Signal preserved");
    CHECK(preservedFloat.category == AudioQualityCategory::Positive);
    CHECK(preservedFloat.channelNote.empty());
  }

  TEST_CASE("AudioQualityFormatter - numerical alterations keep precedence alongside layout uncertainty")
  {
    for (auto const kind : {audio::QualityFindingKind::Muted,
                            audio::QualityFindingKind::Truncation,
                            audio::QualityFindingKind::Resampling,
                            audio::QualityFindingKind::SoftwareVolumeModification,
                            audio::QualityFindingKind::UnclassifiedVolumeModification,
                            audio::QualityFindingKind::MixedSources,
                            audio::QualityFindingKind::ChannelMapping,
                            audio::QualityFindingKind::SoftwareAmplification})
    {
      for (auto const confidence : {audio::ChannelConfidence::Unknown,
                                    audio::ChannelConfidence::Unverified,
                                    audio::ChannelConfidence::Inferred,
                                    audio::ChannelConfidence::Verified})
      {
        auto const state = rt::QualityState{
          .sourceQuality = audio::Quality::BitwisePerfect,
          .pipelineQuality = audio::Quality::LinearIntervention,
          .overall = audio::Quality::LinearIntervention,
          .fullyVerified = false,
          .channelConfidence = confidence,
          .assessments = {{.findings = {{.kind = kind, .quality = audio::Quality::LinearIntervention}}}},
        };
        auto const presentation = audioQualityPresentation(state);
        auto const amplification = kind == audio::QualityFindingKind::SoftwareAmplification;
        CHECK(presentation.headline == (amplification ? "Clipping risk" : "Pipeline intervention"));
        CHECK(presentation.category ==
              (amplification ? AudioQualityCategory::Warning : AudioQualityCategory::Diagnostic));
        CHECK(presentation.channelNote.empty() == (confidence == audio::ChannelConfidence::Verified));
      }
    }
  }

  TEST_CASE("AudioQualityFormatter - known channel conversion stays an intervention")
  {
    // The ChannelMapping finding is the conversion authority: it keeps the
    // intervention headline even when the numerical axes disagree.
    auto const mappingState =
      rt::QualityState{.sourceQuality = audio::Quality::BitwisePerfect,
                       .pipelineQuality = audio::Quality::BitwisePerfect,
                       .overall = audio::Quality::BitwisePerfect,
                       .assessments = {
                         audio::NodeQualityAssessment{
                           .nodeId = "ao-engine",
                           .nodeType = audio::flow::NodeType::Engine,
                           .findings = {audio::QualityFinding{.kind = audio::QualityFindingKind::ChannelMapping,
                                                              .quality = audio::Quality::LinearIntervention}},
                         },
                       }};
    auto const mapped = audioQualityPresentation(mappingState);
    CHECK(mapped.headline == "Pipeline intervention");
    CHECK(mapped.category == AudioQualityCategory::Diagnostic);
  }

  TEST_CASE("AudioQualityFormatter - localized copy preserves numeric and external values")
  {
    auto const catalog = ao::test::messageCatalog("de-DE");
    auto const formatter = AudioQualityFormatter{catalog};
    auto const format = audio::SignalFormat{.sampleRate = 44100, .channels = 2, .precisionBits = 16};

    CHECK(formatter.nodeTypeLabel(audio::flow::NodeType::Source) == "[Quelle]");
    CHECK(formatter.formatLabel(format) == "44.1 kHz · 16-bit · 2 Kan.");
    CHECK(formatter.findingLabel(audio::QualityFinding{
            .kind = audio::QualityFindingKind::MixedSources,
            .sharedApps = {"Dvořák", "誰か"},
          }) == "Gemischt mit Dvořák, 誰か");
    CHECK(formatter.presentation(rt::QualityState{.overall = audio::Quality::Unknown}).headline ==
          "Audiokette unbekannt");
  }

  TEST_CASE("AudioQualityFormatter - unclassified volume copy keeps both selector branches in every locale")
  {
    // The catalog compiler validates argument names and kinds, not branch placement. A translation
    // that drops the `yes` branch or moves `{gain}` into `other` still compiles, so assert the shape
    // each maintained locale must produce.
    static constexpr auto kMagnitude = std::string_view{"-6.0 dB"};
    auto const gainFinding =
      audio::QualityFinding{.kind = audio::QualityFindingKind::UnclassifiedVolumeModification, .gain = 0.5F};
    auto const plainFinding = audio::QualityFinding{.kind = audio::QualityFindingKind::UnclassifiedVolumeModification};
    auto const englishGainLabel = englishFormatter().findingLabel(gainFinding);

    for (auto const* const locale : {"de-DE", "zh-CN", "zh-TW", "ja-JP", "es-ES", "fr-FR"})
    {
      INFO("locale: " << locale);
      auto const catalog = ao::test::messageCatalog(locale);
      auto const formatter = AudioQualityFormatter{catalog};
      auto const gainLabel = formatter.findingLabel(gainFinding);
      auto const plainLabel = formatter.findingLabel(plainFinding);

      // A locale that silently fell back to root would pass the shape checks against English copy.
      CHECK(gainLabel != englishGainLabel);
      REQUIRE(gainLabel.contains(kMagnitude));
      CHECK_FALSE(plainLabel.contains("dB"));
      CHECK_FALSE(plainLabel.contains("{"));

      // Erasing the magnitude from the gain form must not reproduce the fallback form. A
      // translation that keeps only an `other` branch renders the gain sentence with an empty
      // slot, which the compiler's name-and-kind signature check cannot see.
      auto strippedGainLabel = gainLabel;
      strippedGainLabel.erase(gainLabel.find(kMagnitude), kMagnitude.size());
      CHECK(strippedGainLabel != plainLabel);
    }
  }

  TEST_CASE("AudioQualityFormatter - localized bit units preserve signal and PCM widths")
  {
    struct ExpectedFormatLabels final
    {
      std::string_view locale;
      std::string_view signal;
      std::string_view pcm;
    };

    static constexpr auto kExpected = std::array{
      ExpectedFormatLabels{
        .locale = "en-US", .signal = "44.1 kHz · 24-bit · Stereo", .pcm = "44.1 kHz · 32-bit · Stereo"},
      ExpectedFormatLabels{
        .locale = "de-DE", .signal = "44.1 kHz · 24-bit · Stereo", .pcm = "44.1 kHz · 32-bit · Stereo"},
      ExpectedFormatLabels{
        .locale = "zh-CN", .signal = "44.1 kHz · 24-bit · 立体声", .pcm = "44.1 kHz · 32-bit · 立体声"},
      ExpectedFormatLabels{
        .locale = "zh-TW", .signal = "44.1 kHz · 24-bit · 立體聲", .pcm = "44.1 kHz · 32-bit · 立體聲"},
      ExpectedFormatLabels{
        .locale = "ja-JP", .signal = "44.1 kHz · 24-bit · ステレオ", .pcm = "44.1 kHz · 32-bit · ステレオ"},
      ExpectedFormatLabels{
        .locale = "es-ES", .signal = "44.1 kHz · 24-bit · Estéreo", .pcm = "44.1 kHz · 32-bit · Estéreo"},
      ExpectedFormatLabels{
        .locale = "fr-FR", .signal = "44.1 kHz · 24-bit · Stéréo", .pcm = "44.1 kHz · 32-bit · Stéréo"},
    };
    auto const signalFormat =
      audio::SignalFormat{.sampleRate = 44100, .channels = 2, .precisionBits = 24, .speakerMask = 0x3};
    auto const pcmFormat = audio::PcmFormat{
      .sampleRate = 44100, .channels = 2, .encoding = audio::SampleEncoding::Signed24In32Le, .speakerMask = 0x3};

    for (auto const& expected : kExpected)
    {
      INFO("locale: " << expected.locale);
      auto const formatter = AudioQualityFormatter{ao::test::messageCatalog(expected.locale)};
      CHECK(formatter.formatLabel(signalFormat) == expected.signal);
      CHECK(formatter.formatLabel(pcmFormat) == expected.pcm);
    }
  }

  TEST_CASE("AudioQualityFormatter - localized unclassified volume copy")
  {
    auto const catalog = ao::test::messageCatalog("de-DE");
    auto const formatter = AudioQualityFormatter{catalog};

    CHECK(formatter.findingLabel(audio::QualityFinding{
            .kind = audio::QualityFindingKind::UnclassifiedVolumeModification,
            .gain = 0.5F,
          }) == "Gerätelautstärke: -6.0 dB (Herkunft ungeklärt)");
    CHECK(formatter.findingLabel(audio::QualityFinding{
            .kind = audio::QualityFindingKind::UnclassifiedVolumeModification,
          }) == "Gerätelautstärke geändert (Herkunft ungeklärt)");
  }

  TEST_CASE("AudioQualityFormatter - localized channel layout copy keeps role tokens external")
  {
    auto const frontPair =
      audio::SignalFormat{.sampleRate = 44100, .channels = 2, .precisionBits = 16, .speakerMask = 0x3};
    auto const sidePair =
      audio::SignalFormat{.sampleRate = 44100, .channels = 2, .precisionBits = 16, .speakerMask = 0x600};
    auto const layoutFinding = audio::QualityFinding{
      .kind = audio::QualityFindingKind::ChannelMapping, .optFromFormat = frontPair, .optToFormat = sidePair};
    auto const inferredState =
      withChannelConfidence(rt::QualityState{.sourceQuality = audio::Quality::BitwisePerfect,
                                             .pipelineQuality = audio::Quality::BitwisePerfect,
                                             .overall = audio::Quality::BitwisePerfect,
                                             .assessments = {{.nodeType = audio::flow::NodeType::Source}}},
                            audio::ChannelConfidence::Inferred);

    CHECK(englishFormatter().formatLabel(sidePair) == "44.1 kHz · 16-bit · 2 ch (SL SR)");

    auto const german = AudioQualityFormatter{ao::test::messageCatalog("de-DE")};
    CHECK(german.formatLabel(sidePair) == "44.1 kHz · 16-bit · 2 Kan. (SL SR)");
    CHECK(german.findingLabel(layoutFinding) == "Kanallayout: Stereo → 2 Kan. (SL SR)");
    CHECK(german.presentation(inferredState).channelNote == "Quellkanallayout abgeleitet");

    auto const simplified = AudioQualityFormatter{ao::test::messageCatalog("zh-CN")};
    CHECK(simplified.formatLabel(sidePair) == "44.1 kHz · 16-bit · 2 声道（SL SR）");
    CHECK(simplified.findingLabel(layoutFinding) == "声道布局：立体声 → 2 声道（SL SR）");
    CHECK(simplified.presentation(inferredState).headline == "Bit-Perfect PCM（已观测的软件链路）");
    CHECK(simplified.presentation(inferredState).channelNote == "源声道布局来自推断");
  }

  TEST_CASE("AudioQualityFormatter - pseudo copy preserves external values")
  {
    auto const catalog = ao::test::messageCatalog("qps-ploc");
    auto const formatter = AudioQualityFormatter{catalog};
    auto const finding = formatter.findingLabel(audio::QualityFinding{
      .kind = audio::QualityFindingKind::MixedSources,
      .sharedApps = {"Dvořák", "誰か"},
    });

    CHECK(finding.starts_with("[!! "));
    CHECK(finding.ends_with(" !!]"));
    CHECK(finding.contains("Dvořák, 誰か"));
  }
} // namespace ao::uimodel::test
