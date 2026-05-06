// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include <ao/audio/QualityAnalyzer.h>

#include <algorithm>
#include <format>
#include <ranges>
#include <set>
#include <unordered_map>
#include <vector>

namespace ao::audio
{
  namespace
  {
    void appendLine(std::string& text, std::string_view line)
    {
      if (line.empty())
      {
        return;
      }

      if (!text.empty())
      {
        text += '\n';
      }

      text += line;
    }

    bool isLosslessBitDepthChange(Format const& src, Format const& dst) noexcept
    {
      if (src.isFloat == dst.isFloat)
      {
        auto const srcBits = (src.validBits != 0) ? src.validBits : src.bitDepth;
        auto const dstBits = (dst.validBits != 0) ? dst.validBits : dst.bitDepth;

        return srcBits <= dstBits;
      }

      if (!src.isFloat && dst.isFloat)
      {
        auto const srcBits = (src.validBits != 0) ? src.validBits : src.bitDepth;

        if (dst.bitDepth == 32)
        {
          return srcBits <= 24;
        }

        if (dst.bitDepth == 64)
        {
          return srcBits <= 32;
        }
      }

      return false;
    }

    auto findPlaybackPath(flow::Graph const& graph, std::string const& startId) -> std::vector<flow::Node const*>
    {
      auto path = std::vector<flow::Node const*>{};
      auto currentId = std::string_view{startId};
      auto visited = std::set<std::string_view>{};

      while (!currentId.empty() && !visited.contains(currentId))
      {
        visited.insert(currentId);

        auto const it = std::ranges::find(graph.nodes, currentId, &flow::Node::id);

        if (it == graph.nodes.end())
        {
          break;
        }

        path.push_back(&(*it));

        auto nextId = std::string_view{};

        for (auto const& link : graph.connections)
        {
          if (link.isActive && link.sourceId == currentId)
          {
            nextId = link.destId;
            break;
          }
        }

        currentId = nextId;
      }

      return path;
    }

    void processInputSources(flow::Node const& node,
                             std::span<flow::Node const* const> path,
                             std::unordered_map<std::string, std::set<std::string>> const& inputSources,
                             flow::Graph const& graph,
                             QualityResult& result)
    {
      if (inputSources.contains(node.id))
      {
        auto const& sources = inputSources.at(node.id);
        auto otherAppNames = std::vector<std::string>{};

        for (auto const& srcId : sources)
        {
          bool const isInternal = std::ranges::contains(path, srcId, &flow::Node::id);

          if (!isInternal)
          {
            auto const it = std::ranges::find(graph.nodes, srcId, &flow::Node::id);

            if (it != graph.nodes.end())
            {
              otherAppNames.push_back(it->name);
            }
          }
        }

        if (!otherAppNames.empty())
        {
          std::ranges::sort(otherAppNames);
          auto const [first, last] = std::ranges::unique(otherAppNames);
          otherAppNames.erase(first, last);
          auto apps = std::string{};

          for (size_t j = 0; j < otherAppNames.size(); ++j)
          {
            apps += otherAppNames[j];

            if (j < otherAppNames.size() - 1)
            {
              apps += ", ";
            }
          }

          appendLine(result.tooltip, std::format("• Mixed: {} shared with {}", node.name, apps));
          result.quality = std::max(result.quality, Quality::LinearIntervention);
        }
      }
    }

    void assessNodeQuality(flow::Node const& node, flow::Node const* nextNode, QualityResult& result)
    {
      if (node.isLossySource)
      {
        appendLine(result.tooltip, std::format("• Source: Lossy format ({})", node.name));
        result.quality = std::max(result.quality, Quality::LossySource);
      }

      if (node.volumeNotUnity)
      {
        appendLine(result.tooltip, std::format("• Volume: Modification at {}", node.name));
        result.quality = std::max(result.quality, Quality::LinearIntervention);
      }

      if (node.isMuted)
      {
        appendLine(result.tooltip, std::format("• Status: {} is MUTED", node.name));
        result.quality = std::max(result.quality, Quality::LinearIntervention);
      }

      if (nextNode != nullptr)
      {
        if (node.optFormat && nextNode->optFormat)
        {
          auto const& f1 = *node.optFormat;
          auto const& f2 = *nextNode->optFormat;

          if (f1.sampleRate != f2.sampleRate)
          {
            appendLine(result.tooltip, std::format("• Resampling: {}Hz → {}Hz", f1.sampleRate, f2.sampleRate));
            result.quality = std::max(result.quality, Quality::LinearIntervention);
          }

          if (f1.channels != f2.channels)
          {
            appendLine(result.tooltip, std::format("• Channels: {}ch → {}ch", f1.channels, f2.channels));
            result.quality = std::max(result.quality, Quality::LinearIntervention);
          }
          else if (f1.bitDepth != f2.bitDepth || f1.isFloat != f2.isFloat)
          {
            if (isLosslessBitDepthChange(f1, f2))
            {
              appendLine(
                result.tooltip, f2.isFloat ? "• Bit-Transparent: Float mapping" : "• Bit-Transparent: Integer padding");
              result.quality = std::max(result.quality, f2.isFloat ? Quality::LosslessFloat : Quality::LosslessPadded);
            }
            else
            {
              appendLine(result.tooltip, std::format("• Precision: Truncated {}b → {}b", f1.bitDepth, f2.bitDepth));
              result.quality = std::max(result.quality, Quality::LinearIntervention);
            }
          }
        }
      }
    }
  } // namespace

  QualityResult analyzeAudioQuality(flow::Graph const& graph)
  {
    auto result = QualityResult{.quality = Quality::BitwisePerfect, .tooltip = {}};

    if (graph.nodes.empty())
    {
      result.quality = Quality::Unknown;
      return result;
    }

    appendLine(result.tooltip, "Audio Routing Analysis:");

    // 1. Build linear path
    auto const path = findPlaybackPath(graph, "ao-decoder");

    // 2. Identify mixing sources
    auto inputSources = std::unordered_map<std::string, std::set<std::string>>{};

    for (auto const& link : graph.connections)
    {
      if (link.isActive)
      {
        inputSources[link.destId].insert(link.sourceId);
      }
    }

    for (size_t i = 0; i < path.size(); ++i)
    {
      auto const* const node = path[i];
      auto const* const nextNode = (i < path.size() - 1) ? path[i + 1] : nullptr;

      assessNodeQuality(*node, nextNode, result);
      processInputSources(*node, path, inputSources, graph, result);
    }

    if (result.quality == Quality::BitwisePerfect)
    {
      appendLine(result.tooltip, "• Signal Path: Byte-perfect from decoder to device");
    }

    switch (result.quality)
    {
      case Quality::BitwisePerfect:
      case Quality::LosslessPadded: appendLine(result.tooltip, "\nConclusion: Bit-perfect output"); break;

      case Quality::LosslessFloat: appendLine(result.tooltip, "\nConclusion: Lossless Conversion"); break;

      case Quality::LinearIntervention:
        appendLine(result.tooltip, "\nConclusion: Linear intervention (Resampled/Mixed/Vol)");
        break;

      case Quality::LossySource: appendLine(result.tooltip, "\nConclusion: Lossy source format"); break;

      case Quality::Clipped: appendLine(result.tooltip, "\nConclusion: Signal clipping detected"); break;

      case Quality::Unknown: break;
    }

    return result;
  }
} // namespace ao::audio
