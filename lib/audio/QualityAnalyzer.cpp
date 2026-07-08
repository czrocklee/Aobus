// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include <ao/audio/Format.h>
#include <ao/audio/Quality.h>
#include <ao/audio/QualityAnalyzer.h>
#include <ao/audio/flow/Graph.h>

#include <boost/unordered/unordered_flat_map.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
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
    constexpr float kGainEpsilon = 1e-4F;

    bool isLosslessBitDepthChange(Format const& sourceFormat, Format const& destinationFormat) noexcept
    {
      if (sourceFormat.isFloat == destinationFormat.isFloat)
      {
        return effectiveBits(sourceFormat) <= effectiveBits(destinationFormat);
      }

      if (!sourceFormat.isFloat && destinationFormat.isFloat)
      {
        if (destinationFormat.bitDepth == 32)
        {
          return effectiveBits(sourceFormat) <= 24;
        }

        if (destinationFormat.bitDepth == 64)
        {
          return effectiveBits(sourceFormat) <= 32;
        }
      }

      return false;
    }

    bool hasKnownEffectiveBitChange(Format const& sourceFormat, Format const& destinationFormat) noexcept
    {
      return sourceFormat.validBits != 0U && destinationFormat.validBits != 0U &&
             effectiveBits(sourceFormat) != effectiveBits(destinationFormat);
    }

    bool hasFormatPrecisionChange(Format const& sourceFormat, Format const& destinationFormat) noexcept
    {
      return sourceFormat.bitDepth != destinationFormat.bitDepth || sourceFormat.isFloat != destinationFormat.isFloat ||
             hasKnownEffectiveBitChange(sourceFormat, destinationFormat);
    }

    void addFinding(NodeQualityAssessment& assessment, QualityFinding finding)
    {
      assessment.worstQuality = worseQuality(assessment.worstQuality, finding.quality);
      assessment.findings.push_back(std::move(finding));
    }

    bool invalidatesProvenPrecision(QualityFinding const& finding) noexcept
    {
      switch (finding.kind)
      {
        case QualityFindingKind::SoftwareVolumeModification:
        case QualityFindingKind::SoftwareAmplification:
        case QualityFindingKind::UnclassifiedVolumeModification:
        case QualityFindingKind::Muted:
        case QualityFindingKind::Resampling:
        case QualityFindingKind::ChannelMapping:
        case QualityFindingKind::Truncation:
        case QualityFindingKind::MixedSources: return true;
        case QualityFindingKind::Unknown:
        case QualityFindingKind::BitPerfect:
        case QualityFindingKind::LossySource:
        case QualityFindingKind::HardwareVolumeModification:
        case QualityFindingKind::LosslessPadding:
        case QualityFindingKind::LosslessFloat:
        case QualityFindingKind::LosslessRoundTrip: return false;
      }

      return true;
    }

    bool hasPrecisionInvalidatingFinding(NodeQualityAssessment const& assessment) noexcept
    {
      return std::ranges::any_of(assessment.findings, invalidatesProvenPrecision);
    }

    bool hasVerifiedOutputEndpoint(std::vector<flow::Node const*> const& path) noexcept
    {
      return !path.empty() && path.back()->type == flow::NodeType::Sink;
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

        auto const linkIt = std::ranges::find_if(
          graph.connections, [currentId](auto const& link) { return link.isActive && link.sourceId == currentId; });

        if (linkIt != graph.connections.end())
        {
          nextId = linkIt->destinationId;
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
        bool hasExternalSource = false;

        for (auto const& sourceId : sources)
        {
          bool const isInternal = std::ranges::contains(path, sourceId, &flow::Node::id);

          if (!isInternal)
          {
            hasExternalSource = true;
            auto const it = std::ranges::find(graph.nodes, sourceId, &flow::Node::id);

            if (it != graph.nodes.end() && !it->name.empty())
            {
              otherAppNames.push_back(it->name);
            }
          }
        }

        if (hasExternalSource)
        {
          std::ranges::sort(otherAppNames);
          auto const [first, last] = std::ranges::unique(otherAppNames);
          otherAppNames.erase(first, last);

          addFinding(targetAssessment,
                     QualityFinding{.kind = QualityFindingKind::MixedSources,
                                    .quality = Quality::LinearIntervention,
                                    .sharedApps = std::move(otherAppNames)});
        }
      }
    }

    void assessNodeSelfProperties(flow::Node const& node, NodeQualityAssessment& assessment)
    {
      if (node.isLossySource)
      {
        addFinding(
          assessment, QualityFinding{.kind = QualityFindingKind::LossySource, .quality = Quality::LossySource});
      }

      if (node.softwareVolumeNotUnity)
      {
        auto const isAmplification = node.maxSoftwareGain > 1.0F + kGainEpsilon;
        auto const attenuationGain = node.minSoftwareGain > 0.0F ? node.minSoftwareGain : node.maxSoftwareGain;
        auto const gain = isAmplification ? node.maxSoftwareGain : attenuationGain;
        addFinding(assessment,
                   QualityFinding{.kind = isAmplification ? QualityFindingKind::SoftwareAmplification
                                                          : QualityFindingKind::SoftwareVolumeModification,
                                  .quality = Quality::LinearIntervention,
                                  .gain = gain});
      }
      else if (node.maxSoftwareGain > 1.0F + kGainEpsilon)
      {
        addFinding(assessment,
                   QualityFinding{.kind = QualityFindingKind::SoftwareAmplification,
                                  .quality = Quality::LinearIntervention,
                                  .gain = node.maxSoftwareGain});
      }

      if (node.hardwareVolumeNotUnity)
      {
        addFinding(
          assessment,
          QualityFinding{.kind = QualityFindingKind::HardwareVolumeModification, .quality = Quality::BitwisePerfect});
      }

      if (node.unclassifiedVolumeNotUnity)
      {
        addFinding(assessment,
                   QualityFinding{.kind = QualityFindingKind::UnclassifiedVolumeModification,
                                  .quality = Quality::LinearIntervention});
      }

      if (node.isMuted)
      {
        addFinding(
          assessment, QualityFinding{.kind = QualityFindingKind::Muted, .quality = Quality::LinearIntervention});
      }
    }

    void assessFormatTransition(flow::Node const& previousNode,
                                flow::Node const& currentNode,
                                NodeQualityAssessment& targetAssessment,
                                std::optional<std::uint8_t> optProvenPrecision)
    {
      if (!previousNode.optFormat || !currentNode.optFormat)
      {
        return;
      }

      auto const& f1 = *previousNode.optFormat;
      auto const& f2 = *currentNode.optFormat;

      if (f1.sampleRate != f2.sampleRate)
      {
        addFinding(targetAssessment,
                   QualityFinding{.kind = QualityFindingKind::Resampling,
                                  .quality = Quality::LinearIntervention,
                                  .optFromFormat = f1,
                                  .optToFormat = f2});
        optProvenPrecision.reset();
      }

      if (f1.channels != f2.channels)
      {
        addFinding(targetAssessment,
                   QualityFinding{.kind = QualityFindingKind::ChannelMapping,
                                  .quality = Quality::LinearIntervention,
                                  .optFromFormat = f1,
                                  .optToFormat = f2});
        optProvenPrecision.reset();
      }

      if (hasFormatPrecisionChange(f1, f2))
      {
        if (f1.isFloat && !f2.isFloat && optProvenPrecision && *optProvenPrecision <= effectiveBits(f2))
        {
          addFinding(targetAssessment,
                     QualityFinding{.kind = QualityFindingKind::LosslessRoundTrip,
                                    .quality = Quality::LosslessFloat,
                                    .optFromFormat = f1,
                                    .optToFormat = f2});
        }
        else if (isLosslessBitDepthChange(f1, f2))
        {
          addFinding(
            targetAssessment,
            QualityFinding{.kind = f2.isFloat ? QualityFindingKind::LosslessFloat : QualityFindingKind::LosslessPadding,
                           .quality = f2.isFloat ? Quality::LosslessFloat : Quality::LosslessPadded,
                           .optFromFormat = f1,
                           .optToFormat = f2});
        }
        else
        {
          addFinding(targetAssessment,
                     QualityFinding{.kind = QualityFindingKind::Truncation,
                                    .quality = Quality::LinearIntervention,
                                    .optFromFormat = f1,
                                    .optToFormat = f2});
        }
      }
    }

    void updateAxes(QualityFinding const& finding, QualityResult& result) noexcept
    {
      if (finding.kind == QualityFindingKind::LossySource)
      {
        result.sourceQuality = worseQuality(result.sourceQuality, finding.quality);
        return;
      }

      result.pipelineQuality = worseQuality(result.pipelineQuality, finding.quality);
    }
  } // namespace

  Quality worseQuality(Quality lhs, Quality rhs) noexcept
  {
    return (static_cast<std::uint8_t>(lhs) > static_cast<std::uint8_t>(rhs)) ? lhs : rhs;
  }

  QualityResult analyzeAudioQuality(flow::Graph const& graph)
  {
    auto result = QualityResult{};

    if (graph.nodes.empty())
    {
      return result;
    }

    auto const path = findPlaybackPath(graph, "ao-source");

    if (path.empty())
    {
      return result;
    }

    result.sourceQuality = path.front()->optFormat ? Quality::BitwisePerfect : Quality::Unknown;
    result.pipelineQuality = Quality::BitwisePerfect;
    result.overall = worseQuality(result.sourceQuality, result.pipelineQuality);
    result.fullyVerified = hasVerifiedOutputEndpoint(path);

    auto inputSources = boost::unordered_flat_map<std::string, std::set<std::string>>{};

    for (auto const& link : graph.connections)
    {
      if (link.isActive)
      {
        inputSources[link.destinationId].insert(link.sourceId);
      }
    }

    auto optProvenPrecision = std::optional<std::uint8_t>{};

    if (auto const& optSourceFormat = path.front()->optFormat; optSourceFormat && !optSourceFormat->isFloat)
    {
      optProvenPrecision = effectiveBits(*optSourceFormat);
    }

    for (size_t i = 0; i < path.size(); ++i)
    {
      auto const* const node = path[i];

      if (!node->optFormat)
      {
        result.fullyVerified = false;
      }

      auto assessment = NodeQualityAssessment{
        .nodeId = node->id,
        .nodeName = node->name,
        .nodeType = node->type,
        .optFormat = node->optFormat,
        .worstQuality = Quality::BitwisePerfect,
        .findings = {},
      };

      assessNodeSelfProperties(*node, assessment);
      processInputSources(*node, path, inputSources, graph, assessment);

      if (i > 0)
      {
        auto const* const previousNode = path[i - 1];
        auto optTransitionPrecision = optProvenPrecision;

        if (!node->optFormat || hasPrecisionInvalidatingFinding(assessment))
        {
          optTransitionPrecision.reset();
        }

        assessFormatTransition(*previousNode, *node, assessment, optTransitionPrecision);
      }

      if (assessment.findings.empty())
      {
        addFinding(
          assessment, QualityFinding{.kind = QualityFindingKind::BitPerfect, .quality = Quality::BitwisePerfect});
      }

      for (auto const& finding : assessment.findings)
      {
        updateAxes(finding, result);
      }

      if (!node->optFormat || hasPrecisionInvalidatingFinding(assessment))
      {
        optProvenPrecision.reset();
      }

      result.overall = worseQuality(result.sourceQuality, result.pipelineQuality);
      result.assessments.push_back(std::move(assessment));
    }

    return result;
  }
} // namespace ao::audio
