// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/uimodel/playback/quality/AudioQualityFormatter.h>

#include <ao/Contract.h>
#include <ao/audio/NodeFormat.h>
#include <ao/audio/PcmFormat.h>
#include <ao/audio/Quality.h>
#include <ao/audio/QualityAnalyzer.h>
#include <ao/audio/SampleEncoding.h>
#include <ao/audio/SignalFormat.h>
#include <ao/audio/flow/Graph.h>
#include <ao/i18n/MessageCatalog.h>
#include <ao/rt/PlaybackState.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <format>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace ao::uimodel
{
  namespace
  {
    using i18n::MessageArgument;
    using i18n::MessageCatalog;
    using i18n::MessageId;

    std::string requiredMessage(MessageCatalog const& catalog,
                                MessageId const id,
                                std::span<MessageArgument const> const arguments = {})
    {
      auto result = catalog.format(id, arguments);

      if (!result)
      {
        AO_FATAL("Could not format required audio-quality message: {}", result.error().message);
      }

      return std::move(result->text);
    }

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

    std::string precisionLabel(audio::NodeFormat const& format)
    {
      auto const signal = audio::signalFormat(format);
      return std::format(
        "{}{}", signal.precisionBits, signal.sampleKind == audio::SampleKind::FloatingPoint ? "f" : "b");
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

    std::string gainDecibelLabel(float const gain)
    {
      if (gain <= 0.0F || !std::isfinite(gain))
      {
        return {};
      }

      auto const decibels = 20.0 * std::log10(static_cast<double>(gain));
      return std::format("{:+.1f} dB", decibels);
    }

    std::string gainFindingLabel(MessageCatalog const& catalog, MessageId const id, float const gainValue)
    {
      auto const gain = gainDecibelLabel(gainValue);
      auto const arguments = std::array{
        MessageArgument{"hasGain", gain.empty() ? "no" : "yes"},
        MessageArgument{"gain", gain},
      };
      return requiredMessage(catalog, id, arguments);
    }
  } // namespace

  AudioQualityFormatter::AudioQualityFormatter(MessageCatalog catalog)
    : _catalog{std::move(catalog)}
  {
  }

  std::string AudioQualityFormatter::nodeTypeLabel(audio::flow::NodeType const type) const
  {
    using Type = audio::flow::NodeType;

    auto id = MessageId::AudioNodeUnknown;

    switch (type)
    {
      case Type::Source: id = MessageId::AudioNodeSource; break;
      case Type::Decoder: id = MessageId::AudioNodeDecoder; break;
      case Type::Engine: id = MessageId::AudioNodeEngine; break;
      case Type::Stream: id = MessageId::AudioNodeStream; break;
      case Type::Intermediary: id = MessageId::AudioNodeFilter; break;
      case Type::Sink: id = MessageId::AudioNodeDevice; break;
      case Type::ExternalSource: id = MessageId::AudioNodeOtherSource; break;
    }

    return requiredMessage(_catalog, id);
  }

  std::string AudioQualityFormatter::formatLabel(audio::NodeFormat const& format) const
  {
    constexpr double kKhzMultiplier = 1000.0;
    auto const signal = audio::signalFormat(format);
    auto const channelsText = [&] -> std::string
    {
      if (signal.channels == 1)
      {
        return requiredMessage(_catalog, MessageId::TrackChannelMono);
      }

      if (signal.channels == 2)
      {
        return requiredMessage(_catalog, MessageId::TrackChannelStereo);
      }

      auto const channelCount = std::to_string(signal.channels);
      auto const arguments = std::array{MessageArgument{"count", channelCount}};
      return requiredMessage(_catalog, MessageId::AudioChannelsCompact, arguments);
    }();

    auto bits = signal.precisionBits;

    if (auto const* pcmFormat = std::get_if<audio::PcmFormat>(&format); pcmFormat != nullptr)
    {
      bits = audio::encodingContainerBits(pcmFormat->encoding);
    }

    auto const sampleRate = std::format("{:.1f}", signal.sampleRate / kKhzMultiplier);
    auto const bitsText = std::to_string(bits);
    auto const arguments = std::array{
      MessageArgument{"sampleRate", sampleRate},
      MessageArgument{"bits", bitsText},
      MessageArgument{"channels", channelsText},
    };
    return requiredMessage(_catalog, MessageId::AudioFormat, arguments);
  }

  std::string AudioQualityFormatter::findingLabel(audio::QualityFinding const& finding) const
  {
    switch (finding.kind)
    {
      case audio::QualityFindingKind::BitPerfect: return "";
      case audio::QualityFindingKind::LossySource: return requiredMessage(_catalog, MessageId::AudioFindingLossySource);
      case audio::QualityFindingKind::SoftwareVolumeModification:
        return gainFindingLabel(_catalog, MessageId::AudioFindingSoftwareVolumeAttenuation, finding.gain);
      case audio::QualityFindingKind::SoftwareAmplification:
        return gainFindingLabel(_catalog, MessageId::AudioFindingSoftwareAmplification, finding.gain);
      case audio::QualityFindingKind::HardwareVolumeModification:
        return requiredMessage(_catalog, MessageId::AudioFindingHardwareVolumeModification);
      case audio::QualityFindingKind::UnclassifiedVolumeModification:
        return gainFindingLabel(_catalog, MessageId::AudioFindingUnclassifiedVolumeModification, finding.gain);
      case audio::QualityFindingKind::Muted: return requiredMessage(_catalog, MessageId::AudioFindingMuted);
      case audio::QualityFindingKind::Resampling:
      {
        auto const hasFormats = finding.optFromFormat && finding.optToFormat;
        auto const fromRate = std::to_string(hasFormats ? audio::signalFormat(*finding.optFromFormat).sampleRate : 0);
        auto const toRate = std::to_string(hasFormats ? audio::signalFormat(*finding.optToFormat).sampleRate : 0);
        auto const arguments = std::array{
          MessageArgument{"hasFormats", hasFormats ? "yes" : "no"},
          MessageArgument{"fromRate", fromRate},
          MessageArgument{"toRate", toRate},
        };
        return requiredMessage(_catalog, MessageId::AudioFindingResampling, arguments);
      }
      case audio::QualityFindingKind::ChannelMapping:
      {
        auto const hasFormats = finding.optFromFormat && finding.optToFormat;
        auto const fromChannels = std::to_string(hasFormats ? audio::signalFormat(*finding.optFromFormat).channels : 0);
        auto const toChannels = std::to_string(hasFormats ? audio::signalFormat(*finding.optToFormat).channels : 0);
        auto const arguments = std::array{
          MessageArgument{"hasFormats", hasFormats ? "yes" : "no"},
          MessageArgument{"fromChannels", fromChannels},
          MessageArgument{"toChannels", toChannels},
        };
        return requiredMessage(_catalog, MessageId::AudioFindingChannelMapping, arguments);
      }
      case audio::QualityFindingKind::LosslessPadding:
        return requiredMessage(_catalog, MessageId::AudioFindingLosslessPadding);
      case audio::QualityFindingKind::LosslessFloat:
        return requiredMessage(_catalog, MessageId::AudioFindingLosslessFloat);
      case audio::QualityFindingKind::LosslessRoundTrip:
        return requiredMessage(_catalog, MessageId::AudioFindingLosslessRoundTrip);
      case audio::QualityFindingKind::Truncation:
      {
        auto kind = std::string_view{"other"};
        auto fromPrecision = std::string{};
        auto toPrecision = std::string{};

        if (finding.optFromFormat && finding.optToFormat)
        {
          auto const fromSignal = audio::signalFormat(*finding.optFromFormat);
          auto const toSignal = audio::signalFormat(*finding.optToFormat);
          fromPrecision = precisionLabel(*finding.optFromFormat);
          toPrecision = precisionLabel(*finding.optToFormat);
          kind = "precision";

          if (fromSignal.sampleKind != toSignal.sampleKind)
          {
            kind = fromSignal.sampleKind == audio::SampleKind::FloatingPoint ? "floatToInteger" : "integerToFloat";
          }
        }

        auto const arguments = std::array{
          MessageArgument{"kind", kind},
          MessageArgument{"fromPrecision", fromPrecision},
          MessageArgument{"toPrecision", toPrecision},
        };
        return requiredMessage(_catalog, MessageId::AudioFindingTruncation, arguments);
      }
      case audio::QualityFindingKind::MixedSources:
      {
        auto const apps = joinSharedApps(finding.sharedApps);
        auto const arguments = std::array{
          MessageArgument{"hasApps", apps.empty() ? "no" : "yes"},
          MessageArgument{"apps", apps},
        };
        return requiredMessage(_catalog, MessageId::AudioFindingMixedSources, arguments);
      }
      case audio::QualityFindingKind::Unknown: return "";
    }

    return "";
  }

  std::string AudioQualityFormatter::qualityConclusion(audio::Quality const quality) const
  {
    using Quality = audio::Quality;

    switch (quality)
    {
      case Quality::BitwisePerfect: return requiredMessage(_catalog, MessageId::AudioQualityBitPerfectPlayback);
      case Quality::LosslessPadded:
      case Quality::LosslessFloat: return requiredMessage(_catalog, MessageId::AudioQualitySignalPreserved);
      case Quality::LinearIntervention: return requiredMessage(_catalog, MessageId::AudioQualityPipelineIntervention);
      case Quality::LossySource: return requiredMessage(_catalog, MessageId::AudioFindingLossySource);
      case Quality::Clipped: return requiredMessage(_catalog, MessageId::AudioQualitySignalClippingDetected);
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

  AudioQualityPresentation AudioQualityFormatter::presentation(rt::QualityState const& state) const
  {
    if (state.overall == audio::Quality::Unknown)
    {
      return AudioQualityPresentation{.headline = requiredMessage(_catalog, MessageId::AudioQualityUnknownPipeline),
                                      .category = AudioQualityCategory::Unknown};
    }

    if (firstFinding(state, audio::QualityFindingKind::SoftwareAmplification) != nullptr)
    {
      return AudioQualityPresentation{.headline = requiredMessage(_catalog, MessageId::AudioQualityClippingRisk),
                                      .category = AudioQualityCategory::Warning};
    }

    if (firstFinding(state, audio::QualityFindingKind::Muted) != nullptr ||
        firstFinding(state, audio::QualityFindingKind::Truncation) != nullptr ||
        firstFinding(state, audio::QualityFindingKind::Resampling) != nullptr ||
        firstFinding(state, audio::QualityFindingKind::SoftwareVolumeModification) != nullptr ||
        firstFinding(state, audio::QualityFindingKind::UnclassifiedVolumeModification) != nullptr ||
        firstFinding(state, audio::QualityFindingKind::MixedSources) != nullptr ||
        firstFinding(state, audio::QualityFindingKind::ChannelMapping) != nullptr)
    {
      return AudioQualityPresentation{.headline = qualityConclusion(audio::Quality::LinearIntervention),
                                      .category = AudioQualityCategory::Diagnostic};
    }

    if (!state.fullyVerified)
    {
      return AudioQualityPresentation{
        .headline = requiredMessage(_catalog, MessageId::AudioQualityPartiallyVerifiedPath),
        .category = AudioQualityCategory::Informational};
    }

    if (firstFinding(state, audio::QualityFindingKind::LosslessPadding) != nullptr)
    {
      return AudioQualityPresentation{
        .headline = qualityConclusion(audio::Quality::LosslessFloat), .category = AudioQualityCategory::Positive};
    }

    if (hasFinding(state, audio::QualityFindingKind::LosslessRoundTrip))
    {
      return AudioQualityPresentation{
        .headline = qualityConclusion(audio::Quality::LosslessFloat), .category = AudioQualityCategory::Positive};
    }

    if (hasFinding(state, audio::QualityFindingKind::LosslessFloat))
    {
      return AudioQualityPresentation{
        .headline = qualityConclusion(audio::Quality::LosslessFloat), .category = AudioQualityCategory::Positive};
    }

    if (state.sourceQuality == audio::Quality::LossySource)
    {
      return AudioQualityPresentation{.headline = requiredMessage(_catalog, MessageId::AudioQualityCleanLossyDelivery),
                                      .category = AudioQualityCategory::Informational};
    }

    if (visibleFindings(state).empty() && state.sourceQuality == audio::Quality::BitwisePerfect)
    {
      return AudioQualityPresentation{
        .headline = qualityConclusion(audio::Quality::BitwisePerfect), .category = AudioQualityCategory::Medal};
    }

    return AudioQualityPresentation{.headline = requiredMessage(_catalog, MessageId::AudioQualityCleanDelivery),
                                    .category = AudioQualityCategory::Positive};
  }
} // namespace ao::uimodel
