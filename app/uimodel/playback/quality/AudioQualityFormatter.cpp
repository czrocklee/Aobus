// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/audio/Backend.h>
#include <ao/audio/Format.h>
#include <ao/audio/QualityAnalyzer.h>
#include <ao/audio/flow/Graph.h>
#include <ao/uimodel/playback/quality/AudioQualityFormatter.h>

#include <algorithm>
#include <format>
#include <string>
#include <unordered_set>
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
    auto const bits = (preferValidBits && format.validBits != 0) ? format.validBits : format.bitDepth;

    return std::format("{:.1f} kHz · {}-bit · {}", format.sampleRate / kKhzMultiplier, bits, channelsText);
  }

  std::string audioFindingLabel(audio::QualityFinding const& finding)
  {
    switch (finding.kind)
    {
      case audio::QualityFindingKind::BitPerfect: return "";
      case audio::QualityFindingKind::LossySource: return "Lossy source";
      case audio::QualityFindingKind::SoftwareVolumeModification: return "Software volume attenuation";
      case audio::QualityFindingKind::HardwareVolumeModification: return "Hardware volume control";
      case audio::QualityFindingKind::UnclassifiedVolumeModification: return "Software volume attenuation";
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
      case Quality::LosslessPadded: return "Bit-perfect output";
      case Quality::LosslessFloat: return "Lossless Conversion";
      case Quality::LinearIntervention: return "Linear intervention (Resampled/Mixed/Vol)";
      case Quality::LossySource: return "Lossy source format";
      case Quality::Clipped: return "Signal clipping detected";
      case Quality::Unknown: return "";
    }

    return "";
  }

  audio::Quality qualityForFinding(audio::QualityFinding const& finding) noexcept
  {
    switch (finding.kind)
    {
      case audio::QualityFindingKind::BitPerfect:
      case audio::QualityFindingKind::HardwareVolumeModification: return audio::Quality::BitwisePerfect;
      case audio::QualityFindingKind::LossySource: return audio::Quality::LossySource;
      case audio::QualityFindingKind::SoftwareVolumeModification:
      case audio::QualityFindingKind::UnclassifiedVolumeModification:
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
    auto currentId = std::string{"ao-source"};
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
} // namespace ao::uimodel
