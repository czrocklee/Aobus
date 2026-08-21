// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/uimodel/playback/quality/AudioQualityFormatter.h>

#include "test/unit/PresentationTextCatalogTestSupport.h"
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

#include <string>
#include <string_view>

namespace ao::uimodel::test
{
  namespace
  {
    AudioQualityFormatter const& englishFormatter()
    {
      return ao::test::englishPresentationTextCatalog().audioQualityFormatter();
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
  } // namespace

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
    auto format = audio::SignalFormat{.sampleRate = 44100, .channels = 2, .precisionBits = 16};
    CHECK(audioFormatLabel(format) == "44.1 kHz · 16-bit · Stereo");

    format.channels = 1;
    CHECK(audioFormatLabel(format) == "44.1 kHz · 16-bit · Mono");

    format.channels = 0;
    CHECK(audioFormatLabel(format) == "44.1 kHz · 16-bit · 0 ch");

    format.channels = 6;
    format.sampleRate = 48000;
    format.precisionBits = 24;
    CHECK(audioFormatLabel(format) == "48.0 kHz · 24-bit · 6 ch");

    auto const paddedFormat =
      audio::PcmFormat{.sampleRate = 44100, .channels = 2, .encoding = audio::SampleEncoding::Signed32Le};
    CHECK(audioFormatLabel(paddedFormat) == "44.1 kHz · 32-bit · Stereo");
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

  TEST_CASE("AudioQualityFormatter - unclassified volume findings describe device gain magnitude",
            "[uimodel][unit][playback][quality]")
  {
    CHECK(audioFindingLabel(audio::QualityFinding{.kind = audio::QualityFindingKind::UnclassifiedVolumeModification,
                                                  .gain = 0.5F}) == "Device volume: -6.0 dB (source unverified)");
    CHECK(audioFindingLabel(audio::QualityFinding{.kind = audio::QualityFindingKind::UnclassifiedVolumeModification,
                                                  .gain = 1.5F}) == "Device volume: +3.5 dB (source unverified)");
    CHECK(audioFindingLabel(audio::QualityFinding{.kind = audio::QualityFindingKind::UnclassifiedVolumeModification}) ==
          "Device volume change (source unverified)");
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
    auto const sourceFormat = audio::SignalFormat{.sampleRate = 44100, .channels = 2, .precisionBits = 16};
    auto const paddedFormat =
      audio::PcmFormat{.sampleRate = 44100, .channels = 2, .encoding = audio::SampleEncoding::Signed24PackedLe};
    auto const floatFormat =
      audio::PcmFormat{.sampleRate = 44100, .channels = 2, .encoding = audio::SampleEncoding::Float32Le};
    auto const resampledFormat =
      audio::PcmFormat{.sampleRate = 48000, .channels = 2, .encoding = audio::SampleEncoding::Signed16Le};

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

  TEST_CASE("AudioQualityFormatter - localized copy preserves numeric and external values",
            "[uimodel][unit][quality][localization]")
  {
    auto const catalog = ao::test::presentationTextCatalog("de-DE");
    auto const& formatter = catalog.audioQualityFormatter();
    auto const format = audio::SignalFormat{.sampleRate = 44100, .channels = 2, .precisionBits = 16};

    CHECK(formatter.nodeTypeLabel(audio::flow::NodeType::Source) == "[Quelle]");
    CHECK(formatter.formatLabel(format) == "44.1 kHz · 16 Bit · Stereo");
    CHECK(formatter.findingLabel(audio::QualityFinding{
            .kind = audio::QualityFindingKind::MixedSources,
            .sharedApps = {"Dvořák", "誰か"},
          }) == "Gemischt mit Dvořák, 誰か");
    CHECK(formatter.presentation(rt::QualityState{.overall = audio::Quality::Unknown}).headline ==
          "Audiokette unbekannt");
  }

  TEST_CASE("AudioQualityFormatter - unclassified volume copy keeps both selector branches in every locale",
            "[uimodel][unit][quality][localization]")
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
      auto const catalog = ao::test::presentationTextCatalog(locale);
      auto const& formatter = catalog.audioQualityFormatter();
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

  TEST_CASE("AudioQualityFormatter - localized unclassified volume copy", "[uimodel][unit][quality][localization]")
  {
    auto const catalog = ao::test::presentationTextCatalog("de-DE");
    auto const& formatter = catalog.audioQualityFormatter();

    CHECK(formatter.findingLabel(audio::QualityFinding{
            .kind = audio::QualityFindingKind::UnclassifiedVolumeModification,
            .gain = 0.5F,
          }) == "Gerätelautstärke: -6.0 dB (Herkunft ungeklärt)");
    CHECK(formatter.findingLabel(audio::QualityFinding{
            .kind = audio::QualityFindingKind::UnclassifiedVolumeModification,
          }) == "Gerätelautstärke geändert (Herkunft ungeklärt)");
  }

  TEST_CASE("AudioQualityFormatter - pseudo copy preserves external values", "[uimodel][unit][quality][localization]")
  {
    auto const catalog = ao::test::presentationTextCatalog("qps-ploc");
    auto const& formatter = catalog.audioQualityFormatter();
    auto const finding = formatter.findingLabel(audio::QualityFinding{
      .kind = audio::QualityFindingKind::MixedSources,
      .sharedApps = {"Dvořák", "誰か"},
    });

    CHECK(finding.starts_with("[!! "));
    CHECK(finding.ends_with(" !!]"));
    CHECK(finding.contains("Dvořák, 誰か"));
  }
} // namespace ao::uimodel::test
