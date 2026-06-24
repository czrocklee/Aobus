// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include <ao/audio/Backend.h>
#include <ao/audio/Format.h>
#include <ao/audio/QualityAnalyzer.h>
#include <ao/audio/flow/Graph.h>

#include <boost/unordered/unordered_flat_map.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ao::audio
{
  namespace
  {
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

    std::vector<flow::Node const*> findPlaybackPath(flow::Graph const& graph, std::string const& startId)
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
                             boost::unordered_flat_map<std::string, std::set<std::string>> const& inputSources,
                             flow::Graph const& graph,
                             NodeQualityAssessment& targetAssessment)
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

          targetAssessment.findings.push_back(
            QualityFinding{.kind = QualityFindingKind::MixedSources, .sharedApps = std::move(otherAppNames)});
          targetAssessment.worstQuality = worseQuality(targetAssessment.worstQuality, Quality::LinearIntervention);
        }
      }
    }

    void assessNodeSelfProperties(flow::Node const& node, NodeQualityAssessment& assessment)
    {
      if (node.isLossySource)
      {
        assessment.findings.push_back(QualityFinding{.kind = QualityFindingKind::LossySource});
        assessment.worstQuality = worseQuality(assessment.worstQuality, Quality::LossySource);
      }

      if (node.softwareVolumeNotUnity)
      {
        assessment.findings.push_back(QualityFinding{.kind = QualityFindingKind::SoftwareVolumeModification});
        assessment.worstQuality = worseQuality(assessment.worstQuality, Quality::LinearIntervention);
      }

      if (node.hardwareVolumeNotUnity)
      {
        assessment.findings.push_back(QualityFinding{.kind = QualityFindingKind::HardwareVolumeModification});
      }

      if (node.unclassifiedVolumeNotUnity)
      {
        assessment.findings.push_back(QualityFinding{.kind = QualityFindingKind::UnclassifiedVolumeModification});
        assessment.worstQuality = worseQuality(assessment.worstQuality, Quality::LinearIntervention);
      }

      if (node.isMuted)
      {
        assessment.findings.push_back(QualityFinding{.kind = QualityFindingKind::Muted});
        assessment.worstQuality = worseQuality(assessment.worstQuality, Quality::LinearIntervention);
      }
    }

    void assessFormatTransition(flow::Node const& prevNode,
                                flow::Node const& currentNode,
                                NodeQualityAssessment& targetAssessment)
    {
      if (!prevNode.optFormat || !currentNode.optFormat)
      {
        return;
      }

      auto const& f1 = *prevNode.optFormat;
      auto const& f2 = *currentNode.optFormat;

      if (f1.sampleRate != f2.sampleRate)
      {
        targetAssessment.findings.push_back(
          QualityFinding{.kind = QualityFindingKind::Resampling, .optFromFormat = f1, .optToFormat = f2});
        targetAssessment.worstQuality = worseQuality(targetAssessment.worstQuality, Quality::LinearIntervention);
      }

      if (f1.channels != f2.channels)
      {
        targetAssessment.findings.push_back(
          QualityFinding{.kind = QualityFindingKind::ChannelMapping, .optFromFormat = f1, .optToFormat = f2});
        targetAssessment.worstQuality = worseQuality(targetAssessment.worstQuality, Quality::LinearIntervention);
      }
      else if (f1.bitDepth != f2.bitDepth || f1.isFloat != f2.isFloat)
      {
        if (isLosslessBitDepthChange(f1, f2))
        {
          targetAssessment.findings.push_back(
            QualityFinding{.kind = f2.isFloat ? QualityFindingKind::LosslessFloat : QualityFindingKind::LosslessPadding,
                           .optFromFormat = f1,
                           .optToFormat = f2});
          targetAssessment.worstQuality =
            worseQuality(targetAssessment.worstQuality, f2.isFloat ? Quality::LosslessFloat : Quality::LosslessPadded);
        }
        else
        {
          targetAssessment.findings.push_back(
            QualityFinding{.kind = QualityFindingKind::Truncation, .optFromFormat = f1, .optToFormat = f2});
          targetAssessment.worstQuality = worseQuality(targetAssessment.worstQuality, Quality::LinearIntervention);
        }
      }
    }
  } // namespace

  Quality worseQuality(Quality lhs, Quality rhs) noexcept
  {
    return (static_cast<std::uint8_t>(lhs) > static_cast<std::uint8_t>(rhs)) ? lhs : rhs;
  }

  QualityResult analyzeAudioQuality(flow::Graph const& graph)
  {
    auto result = QualityResult{.quality = Quality::BitwisePerfect, .assessments = {}};

    if (graph.nodes.empty())
    {
      result.quality = Quality::Unknown;
      return result;
    }

    auto const path = findPlaybackPath(graph, "ao-source");

    if (path.empty())
    {
      result.quality = Quality::Unknown;
      return result;
    }

    auto inputSources = boost::unordered_flat_map<std::string, std::set<std::string>>{};

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
      auto assessment = NodeQualityAssessment{
        .nodeId = node->id,
        .nodeName = node->name,
        .nodeType = node->type,
        .worstQuality = Quality::BitwisePerfect,
        .findings = {},
      };

      assessNodeSelfProperties(*node, assessment);
      processInputSources(*node, path, inputSources, graph, assessment);

      if (i > 0)
      {
        auto const* const prevNode = path[i - 1];
        assessFormatTransition(*prevNode, *node, assessment);
      }

      if (assessment.findings.empty())
      {
        assessment.findings.push_back(QualityFinding{.kind = QualityFindingKind::BitPerfect});
      }

      result.quality = worseQuality(result.quality, assessment.worstQuality);
      result.assessments.push_back(std::move(assessment));
    }

    return result;
  }
} // namespace ao::audio
