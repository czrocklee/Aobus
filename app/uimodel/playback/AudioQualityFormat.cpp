// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/audio/Backend.h>
#include <ao/audio/Format.h>
#include <ao/audio/QualityAnalyzer.h>
#include <ao/audio/flow/Graph.h>
#include <ao/uimodel/playback/AudioQualityFormat.h>

#include <algorithm>
#include <format>
#include <string>
#include <unordered_set>
#include <vector>

namespace ao::uimodel::playback
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
  } // namespace

  std::string audioNodeTypeLabel(audio::flow::NodeType type)
  {
    using Type = audio::flow::NodeType;

    switch (type)
    {
      case Type::Decoder: return "[Source]";
      case Type::Engine: return "[Engine]";
      case Type::Stream: return "[Stream]";
      case Type::Intermediary: return "[Filter]";
      case Type::Sink: return "[Device]";
      case Type::ExternalSource: return "[Other Source]";
    }

    return "[Unknown]";
  }

  std::string audioFormatLabel(audio::Format const& format)
  {
    constexpr auto kKhzMultiplier = 1000.0;
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

    return std::format("{:.1f} kHz · {}-bit · {}", format.sampleRate / kKhzMultiplier, format.bitDepth, channelsText);
  }

  std::string audioFindingLabel(audio::QualityFinding const& finding)
  {
    switch (finding.kind)
    {
      case audio::QualityFindingKind::BitPerfect: return "";
      case audio::QualityFindingKind::LossySource: return "Lossy source";
      case audio::QualityFindingKind::VolumeModification: return "Volume modified";
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
      case audio::QualityFindingKind::Truncation:
        if (finding.optFromFormat && finding.optToFormat)
        {
          return std::format(
            "Precision truncated: {}b → {}b", finding.optFromFormat->bitDepth, finding.optToFormat->bitDepth);
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
      case Quality::BitwisePerfect:
      case Quality::LosslessPadded: return "Conclusion: Bit-perfect output";
      case Quality::LosslessFloat: return "Conclusion: Lossless Conversion";
      case Quality::LinearIntervention: return "Conclusion: Linear intervention (Resampled/Mixed/Vol)";
      case Quality::LossySource: return "Conclusion: Lossy source format";
      case Quality::Clipped: return "Conclusion: Signal clipping detected";
      case Quality::Unknown: return "";
    }

    return "";
  }

  audio::Quality qualityForFinding(audio::QualityFinding const& finding) noexcept
  {
    switch (finding.kind)
    {
      case audio::QualityFindingKind::BitPerfect: return audio::Quality::BitwisePerfect;
      case audio::QualityFindingKind::LossySource: return audio::Quality::LossySource;
      case audio::QualityFindingKind::VolumeModification:
      case audio::QualityFindingKind::Muted:
      case audio::QualityFindingKind::Resampling:
      case audio::QualityFindingKind::ChannelMapping: return audio::Quality::LinearIntervention;
      case audio::QualityFindingKind::LosslessPadding: return audio::Quality::LosslessPadded;
      case audio::QualityFindingKind::LosslessFloat: return audio::Quality::LosslessFloat;
      case audio::QualityFindingKind::Truncation:
      case audio::QualityFindingKind::MixedSources: return audio::Quality::LinearIntervention;
      case audio::QualityFindingKind::Unknown: return audio::Quality::Unknown;
    }

    return audio::Quality::Unknown;
  }

  std::vector<audio::flow::Node const*> playbackPath(audio::flow::Graph const& graph)
  {
    auto path = std::vector<audio::flow::Node const*>{};
    auto currentId = std::string{"ao-decoder"};
    auto visited = std::unordered_set<std::string>{};

    while (!currentId.empty() && !visited.contains(currentId))
    {
      visited.insert(currentId);
      auto const it = std::ranges::find(graph.nodes, currentId, &audio::flow::Node::id);

      if (it == graph.nodes.end())
      {
        break;
      }

      path.push_back(&(*it));

      auto const linkIt = std::ranges::find_if(
        graph.connections, [&](auto const& link) { return link.isActive && link.sourceId == currentId; });
      currentId = (linkIt != graph.connections.end()) ? linkIt->destId : "";
    }

    return path;
  }
} // namespace ao::uimodel::playback
