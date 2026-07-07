// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/audio/Backend.h>
#include <ao/audio/Format.h>
#include <ao/audio/QualityAnalyzer.h>
#include <ao/audio/flow/Graph.h>
#include <ao/rt/PlaybackState.h>
#include <ao/uimodel/playback/quality/AudioQualityFormatter.h>

#include <algorithm>
#include <cmath>
#include <format>
#include <string>
#include <vector>

namespace ao::uimodel
{
  namespace
  {
    std::string joinSharedApps(std::vector<std::string> const& apps)
    {
      auto text = std::string{};

      for (auto const& app : apps)
      {
        if (!text.empty())
        {
          text += ", ";
        }

        text += app;
      }

      return text;
    }

    std::string precisionLabel(audio::Format const& format)
    {
      return std::format("{}{}", audio::effectiveBits(format), format.isFloat ? "f" : "b");
    }

    std::vector<audio::QualityFinding const*> visibleFindings(rt::QualityState const& state)
    {
      auto findings = std::vector<audio::QualityFinding const*>{};

      for (auto const& assessment : state.assessments)
      {
        for (auto const& finding : assessment.findings)
        {
          if (finding.kind != audio::QualityFindingKind::BitPerfect &&
              finding.kind != audio::QualityFindingKind::HardwareVolumeModification)
          {
            findings.push_back(&finding);
          }
        }
      }

      return findings;
    }

    audio::QualityFinding const* firstFinding(rt::QualityState const& state, audio::QualityFindingKind const kind)
    {
      for (auto const& assessment : state.assessments)
      {
        auto const it = std::ranges::find(assessment.findings, kind, &audio::QualityFinding::kind);

        if (it != assessment.findings.end())
        {
          return &(*it);
        }
      }

      return nullptr;
    }

    bool hasFinding(rt::QualityState const& state, audio::QualityFindingKind const kind)
    {
      return firstFinding(state, kind) != nullptr;
    }

    std::string gainDbLabel(float const gain)
    {
      if (gain <= 0.0F || !std::isfinite(gain))
      {
        return {};
      }

      auto const db = 20.0 * std::log10(static_cast<double>(gain));
      return std::format("{:+.1f} dB", db);
    }

    AudioQualityPresentation diagnosticPresentation(rt::QualityState const& state)
    {
      if (firstFinding(state, audio::QualityFindingKind::SoftwareAmplification) != nullptr)
      {
        return AudioQualityPresentation{.headline = "Clipping risk", .category = AudioQualityCategory::Warning};
      }

      if (firstFinding(state, audio::QualityFindingKind::Muted) != nullptr ||
          firstFinding(state, audio::QualityFindingKind::Truncation) != nullptr ||
          firstFinding(state, audio::QualityFindingKind::Resampling) != nullptr ||
          firstFinding(state, audio::QualityFindingKind::SoftwareVolumeModification) != nullptr ||
          firstFinding(state, audio::QualityFindingKind::UnclassifiedVolumeModification) != nullptr ||
          firstFinding(state, audio::QualityFindingKind::MixedSources) != nullptr ||
          firstFinding(state, audio::QualityFindingKind::ChannelMapping) != nullptr)
      {
        return AudioQualityPresentation{
          .headline = "Pipeline intervention", .category = AudioQualityCategory::Diagnostic};
      }

      return {};
    }
  } // namespace

  std::string audioNodeTypeLabel(audio::flow::NodeType type)
  {
    using Type = audio::flow::NodeType;

    switch (type)
    {
      case Type::Source: return "[Source]";
      case Type::Decoder: return "[Decoder]";
      case Type::Engine: return "[Engine]";
      case Type::Stream: return "[Stream]";
      case Type::Intermediary: return "[Filter]";
      case Type::Sink: return "[Device]";
      case Type::ExternalSource: return "[Other Source]";
    }

    return "[Unknown]";
  }

  std::string audioFormatLabel(audio::Format const& format, bool preferValidBits)
  {
    constexpr double kKhzMultiplier = 1000.0;
    auto const channelsText = [&] -> std::string
    {
      if (format.channels == 1)
      {
        return "Mono";
      }

      if (format.channels == 2)
      {
        return "Stereo";
      }

      return std::format("{} ch", format.channels);
    }();

    // For the source node, report the meaningful precision (valid bits) rather
    // than the storage container width: a 16/24-bit track padded into a 32-bit
    // container has validBits 16/24 but bitDepth 32, and showing the container
    // would misleadingly present a low-resolution source as 32-bit. Downstream
    // nodes keep reporting the transport container width.
    auto const bits = preferValidBits ? audio::effectiveBits(format) : format.bitDepth;

    return std::format("{:.1f} kHz · {}-bit · {}", format.sampleRate / kKhzMultiplier, bits, channelsText);
  }

  std::string audioFindingLabel(audio::QualityFinding const& finding)
  {
    switch (finding.kind)
    {
      case audio::QualityFindingKind::BitPerfect: return "";
      case audio::QualityFindingKind::LossySource: return "Lossy source";
      case audio::QualityFindingKind::SoftwareVolumeModification:
        if (auto const dbLabel = gainDbLabel(finding.gain); !dbLabel.empty())
        {
          return std::format("Software volume attenuation: {}", dbLabel);
        }

        return "Software volume attenuation";
      case audio::QualityFindingKind::SoftwareAmplification:
        if (auto const dbLabel = gainDbLabel(finding.gain); !dbLabel.empty())
        {
          return std::format("Software amplification: {} gain (clipping risk)", dbLabel);
        }

        return "Software amplification (clipping risk)";
      case audio::QualityFindingKind::HardwareVolumeModification: return "Hardware volume control";
      case audio::QualityFindingKind::UnclassifiedVolumeModification: return "Unclassified volume change";
      case audio::QualityFindingKind::Muted: return "Muted";
      case audio::QualityFindingKind::Resampling:
        if (finding.optFromFormat && finding.optToFormat)
        {
          return std::format(
            "Resampling: {}Hz → {}Hz", finding.optFromFormat->sampleRate, finding.optToFormat->sampleRate);
        }

        return "Resampling";
      case audio::QualityFindingKind::ChannelMapping:
        if (finding.optFromFormat && finding.optToFormat)
        {
          return std::format("Channels: {}ch → {}ch", finding.optFromFormat->channels, finding.optToFormat->channels);
        }

        return "Channel mapping";
      case audio::QualityFindingKind::LosslessPadding: return "Bit-transparent integer padding";
      case audio::QualityFindingKind::LosslessFloat: return "Bit-transparent float mapping";
      case audio::QualityFindingKind::LosslessRoundTrip: return "Bit-transparent round trip (float engine)";
      case audio::QualityFindingKind::Truncation:
        if (finding.optFromFormat && finding.optToFormat)
        {
          if (finding.optFromFormat->isFloat != finding.optToFormat->isFloat)
          {
            char const* const fromDomain = finding.optFromFormat->isFloat ? "Float" : "Integer";
            char const* const toDomain = finding.optToFormat->isFloat ? "float" : "integer";
            return std::format("{} → {} quantization: {} → {}",
                               fromDomain,
                               toDomain,
                               precisionLabel(*finding.optFromFormat),
                               precisionLabel(*finding.optToFormat));
          }

          return std::format("Precision truncated: {} → {}",
                             precisionLabel(*finding.optFromFormat),
                             precisionLabel(*finding.optToFormat));
        }

        return "Precision truncated";
      case audio::QualityFindingKind::MixedSources:
        if (!finding.sharedApps.empty())
        {
          return std::format("Mixed with {}", joinSharedApps(finding.sharedApps));
        }

        return "Mixed sources";
      case audio::QualityFindingKind::Unknown: return "";
    }

    return "";
  }

  std::string audioQualityConclusion(audio::Quality quality)
  {
    using Quality = audio::Quality;

    switch (quality)
    {
      case Quality::BitwisePerfect: return "Bit-perfect playback";
      case Quality::LosslessPadded:
      case Quality::LosslessFloat: return "Signal preserved";
      case Quality::LinearIntervention: return "Pipeline intervention";
      case Quality::LossySource: return "Lossy source";
      case Quality::Clipped: return "Signal clipping detected";
      case Quality::Unknown: return "";
    }

    return "";
  }

  AudioQualityCategory audioFindingCategory(audio::QualityFinding const& finding) noexcept
  {
    if (finding.kind == audio::QualityFindingKind::SoftwareAmplification)
    {
      return AudioQualityCategory::Warning;
    }

    return audioQualityCategory(finding.quality);
  }

  AudioQualityCategory audioQualityCategory(audio::Quality const quality) noexcept
  {
    using Quality = audio::Quality;

    switch (quality)
    {
      case Quality::BitwisePerfect: return AudioQualityCategory::Medal;
      case Quality::LosslessPadded:
      case Quality::LosslessFloat: return AudioQualityCategory::Positive;
      case Quality::LinearIntervention: return AudioQualityCategory::Diagnostic;
      case Quality::LossySource: return AudioQualityCategory::Informational;
      case Quality::Clipped: return AudioQualityCategory::Clipped;
      case Quality::Unknown: return AudioQualityCategory::Unknown;
    }

    return AudioQualityCategory::Unknown;
  }

  AudioQualityPresentation audioQualityPresentation(rt::QualityState const& state)
  {
    if (state.overall == audio::Quality::Unknown)
    {
      return AudioQualityPresentation{.headline = "Unknown pipeline", .category = AudioQualityCategory::Unknown};
    }

    if (auto presentation = diagnosticPresentation(state); !presentation.headline.empty())
    {
      return presentation;
    }

    if (!state.fullyVerified)
    {
      return AudioQualityPresentation{
        .headline = "Partially verified path", .category = AudioQualityCategory::Informational};
    }

    if (firstFinding(state, audio::QualityFindingKind::LosslessPadding) != nullptr)
    {
      return AudioQualityPresentation{.headline = "Signal preserved", .category = AudioQualityCategory::Positive};
    }

    if (hasFinding(state, audio::QualityFindingKind::LosslessRoundTrip))
    {
      return AudioQualityPresentation{.headline = "Signal preserved", .category = AudioQualityCategory::Positive};
    }

    if (hasFinding(state, audio::QualityFindingKind::LosslessFloat))
    {
      return AudioQualityPresentation{.headline = "Signal preserved", .category = AudioQualityCategory::Positive};
    }

    if (state.sourceQuality == audio::Quality::LossySource)
    {
      return AudioQualityPresentation{
        .headline = "Clean lossy delivery", .category = AudioQualityCategory::Informational};
    }

    if (visibleFindings(state).empty() && state.sourceQuality == audio::Quality::BitwisePerfect)
    {
      return AudioQualityPresentation{.headline = "Bit-perfect playback", .category = AudioQualityCategory::Medal};
    }

    return AudioQualityPresentation{.headline = "Clean delivery", .category = AudioQualityCategory::Positive};
  }
} // namespace ao::uimodel
