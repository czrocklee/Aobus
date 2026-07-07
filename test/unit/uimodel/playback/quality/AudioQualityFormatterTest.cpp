// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/audio/Backend.h>
#include <ao/audio/Format.h>
#include <ao/audio/QualityAnalyzer.h>
#include <ao/audio/flow/Graph.h>
#include <ao/rt/PlaybackState.h>
#include <ao/uimodel/playback/quality/AudioQualityFormatter.h>

#include <catch2/catch_test_macros.hpp>

namespace ao::uimodel::test
{
  TEST_CASE("AudioQualityFormatter - audioNodeTypeLabel", "[uimodel][unit][playback][quality]")
  {
    CHECK(audioNodeTypeLabel(audio::flow::NodeType::Source) == "[Source]");
    CHECK(audioNodeTypeLabel(audio::flow::NodeType::Decoder) == "[Decoder]");
    CHECK(audioNodeTypeLabel(audio::flow::NodeType::Engine) == "[Engine]");
    CHECK(audioNodeTypeLabel(audio::flow::NodeType::Stream) == "[Stream]");
    CHECK(audioNodeTypeLabel(audio::flow::NodeType::Intermediary) == "[Filter]");
    CHECK(audioNodeTypeLabel(audio::flow::NodeType::Sink) == "[Device]");
    CHECK(audioNodeTypeLabel(audio::flow::NodeType::ExternalSource) == "[Other Source]");
  }

  TEST_CASE("AudioQualityFormatter - audioFormatLabel", "[uimodel][unit][playback][quality]")
  {
    auto format = audio::Format{.sampleRate = 44100, .channels = 2, .bitDepth = 16};
    CHECK(audioFormatLabel(format) == "44.1 kHz · 16-bit · Stereo");

    format.channels = 1;
    CHECK(audioFormatLabel(format) == "44.1 kHz · 16-bit · Mono");

    format.channels = 6;
    format.sampleRate = 48000;
    format.bitDepth = 24;
    CHECK(audioFormatLabel(format) == "48.0 kHz · 24-bit · 6 ch");

    // A low-resolution source padded into a wider container: by default the
    // container width is shown (downstream nodes), but with preferValidBits the
    // true precision is reported (source node).
    format.channels = 2;
    format.sampleRate = 44100;
    format.bitDepth = 32;
    format.validBits = 16;
    CHECK(audioFormatLabel(format) == "44.1 kHz · 32-bit · Stereo");
    CHECK(audioFormatLabel(format, true) == "44.1 kHz · 16-bit · Stereo");

    format.validBits = 24;
    CHECK(audioFormatLabel(format, true) == "44.1 kHz · 24-bit · Stereo");

    // preferValidBits with validBits == 0 falls back to the container width.
    format.validBits = 0;
    CHECK(audioFormatLabel(format, true) == "44.1 kHz · 32-bit · Stereo");
  }

  TEST_CASE("AudioQualityFormatter - quality categories map raw quality to visual tiers",
            "[uimodel][unit][playback][quality]")
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

  TEST_CASE("AudioQualityFormatter - precision findings describe effective domain changes",
            "[uimodel][unit][playback][quality]")
  {
    auto const float32 =
      audio::Format{.sampleRate = 44100, .channels = 2, .bitDepth = 32, .validBits = 32, .isFloat = true};
    auto const int32 = audio::Format{.sampleRate = 44100, .channels = 2, .bitDepth = 32, .validBits = 32};
    auto const int24In32 = audio::Format{.sampleRate = 44100, .channels = 2, .bitDepth = 32, .validBits = 24};

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

  TEST_CASE("AudioQualityFormatter - volume findings describe software gain magnitude",
            "[uimodel][unit][playback][quality]")
  {
    CHECK(audioFindingLabel(audio::QualityFinding{.kind = audio::QualityFindingKind::SoftwareVolumeModification,
                                                  .gain = 0.5F}) == "Software volume attenuation: -6.0 dB");
    CHECK(audioFindingLabel(
            audio::QualityFinding{.kind = audio::QualityFindingKind::SoftwareAmplification, .gain = 1.5F}) ==
          "Software amplification: +3.5 dB gain (clipping risk)");
    CHECK(audioFindingLabel(audio::QualityFinding{.kind = audio::QualityFindingKind::SoftwareAmplification}) ==
          "Software amplification (clipping risk)");
  }

  TEST_CASE("AudioQualityFormatter - signal-preserved quality conclusions share one verdict",
            "[uimodel][unit][playback][quality]")
  {
    CHECK(audioQualityConclusion(audio::Quality::BitwisePerfect) == "Bit-perfect playback");
    CHECK(audioQualityConclusion(audio::Quality::LosslessPadded) == "Signal preserved");
    CHECK(audioQualityConclusion(audio::Quality::LosslessFloat) == "Signal preserved");
  }

  TEST_CASE("AudioQualityFormatter - presentation headlines prioritize pipeline delivery",
            "[uimodel][unit][playback][quality]")
  {
    auto const sourceFormat = audio::Format{.sampleRate = 44100, .channels = 2, .bitDepth = 16, .validBits = 16};
    auto const paddedFormat = audio::Format{.sampleRate = 44100, .channels = 2, .bitDepth = 24, .validBits = 24};
    auto const floatFormat =
      audio::Format{.sampleRate = 44100, .channels = 2, .bitDepth = 32, .validBits = 32, .isFloat = true};
    auto const resampledFormat = audio::Format{.sampleRate = 48000, .channels = 2, .bitDepth = 16, .validBits = 16};

    SECTION("lossless clean path is bit-perfect")
    {
      auto const presentation = audioQualityPresentation(
        rt::QualityState{.sourceQuality = audio::Quality::BitwisePerfect,
                         .pipelineQuality = audio::Quality::BitwisePerfect,
                         .overall = audio::Quality::BitwisePerfect,
                         .assessments = {
                           audio::NodeQualityAssessment{
                             .nodeId = "ao-source",
                             .nodeType = audio::flow::NodeType::Source,
                             .optFormat = sourceFormat,
                             .findings = {audio::QualityFinding{.kind = audio::QualityFindingKind::BitPerfect,
                                                                .quality = audio::Quality::BitwisePerfect}},
                           },
                         }});

      CHECK(presentation.headline == "Bit-perfect playback");
      CHECK(presentation.category == AudioQualityCategory::Medal);
    }

    SECTION("padding preserves the signal")
    {
      auto const presentation = audioQualityPresentation(
        rt::QualityState{.sourceQuality = audio::Quality::BitwisePerfect,
                         .pipelineQuality = audio::Quality::LosslessPadded,
                         .overall = audio::Quality::LosslessPadded,
                         .assessments = {
                           audio::NodeQualityAssessment{
                             .nodeId = "ao-engine",
                             .nodeType = audio::flow::NodeType::Engine,
                             .optFormat = paddedFormat,
                             .findings = {audio::QualityFinding{.kind = audio::QualityFindingKind::LosslessPadding,
                                                                .quality = audio::Quality::LosslessPadded,
                                                                .optFromFormat = sourceFormat,
                                                                .optToFormat = paddedFormat}},
                           },
                         }});

      CHECK(presentation.headline == "Signal preserved");
      CHECK(presentation.category == AudioQualityCategory::Positive);
    }

    SECTION("float round trip preserves the signal")
    {
      auto const presentation = audioQualityPresentation(
        rt::QualityState{.sourceQuality = audio::Quality::BitwisePerfect,
                         .pipelineQuality = audio::Quality::LosslessFloat,
                         .overall = audio::Quality::LosslessFloat,
                         .assessments = {
                           audio::NodeQualityAssessment{
                             .nodeId = "ao-sink",
                             .nodeType = audio::flow::NodeType::Sink,
                             .optFormat = sourceFormat,
                             .findings = {audio::QualityFinding{.kind = audio::QualityFindingKind::LosslessRoundTrip,
                                                                .quality = audio::Quality::LosslessFloat,
                                                                .optFromFormat = floatFormat,
                                                                .optToFormat = sourceFormat}},
                           },
                         }});

      CHECK(presentation.headline == "Signal preserved");
      CHECK(presentation.category == AudioQualityCategory::Positive);
    }

    SECTION("lossy clean source is clean delivery")
    {
      auto const presentation = audioQualityPresentation(
        rt::QualityState{.sourceQuality = audio::Quality::LossySource,
                         .pipelineQuality = audio::Quality::BitwisePerfect,
                         .overall = audio::Quality::LossySource,
                         .assessments = {
                           audio::NodeQualityAssessment{
                             .nodeId = "ao-source",
                             .nodeType = audio::flow::NodeType::Source,
                             .optFormat = sourceFormat,
                             .findings = {audio::QualityFinding{
                               .kind = audio::QualityFindingKind::LossySource, .quality = audio::Quality::LossySource}},
                           },
                         }});

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
      auto const monoFormat = audio::Format{.sampleRate = 44100, .channels = 1, .bitDepth = 16, .validBits = 16};
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
      auto const presentation = audioQualityPresentation(rt::QualityState{
        .sourceQuality = audio::Quality::BitwisePerfect,
        .pipelineQuality = audio::Quality::BitwisePerfect,
        .overall = audio::Quality::BitwisePerfect,
        .assessments = {
          audio::NodeQualityAssessment{
            .nodeId = "ao-sink",
            .nodeType = audio::flow::NodeType::Sink,
            .optFormat = sourceFormat,
            .findings = {audio::QualityFinding{.kind = audio::QualityFindingKind::HardwareVolumeModification,
                                               .quality = audio::Quality::BitwisePerfect}},
          },
        }});

      CHECK(presentation.headline == "Bit-perfect playback");
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
} // namespace ao::uimodel::test
