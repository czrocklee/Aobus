// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include <ao/audio/QualityAnalyzer.h>

#include <ao/audio/ChannelDeliveryEvidence.h>
#include <ao/audio/ChannelLayoutEvidence.h>
#include <ao/audio/NodeFormat.h>
#include <ao/audio/PcmFormat.h>
#include <ao/audio/Quality.h>
#include <ao/audio/SampleEncoding.h>
#include <ao/audio/SignalFormat.h>
#include <ao/audio/flow/Graph.h>

#include <boost/unordered/unordered_flat_map.hpp>
#include <boost/unordered/unordered_flat_map_fwd.hpp>

#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace ao::audio
{
  namespace
  {
    constexpr float kGainEpsilon = 1e-4F;

    /// Selects the representative reported factor of a volume range: the maximum when it is above
    /// unity, otherwise the positive minimum. Returns zero when the range carries no factor.
    constexpr float representativeGain(float const maxGain, float const minGain) noexcept
    {
      if (maxGain > 1.0F + kGainEpsilon)
      {
        return maxGain;
      }

      return minGain > 0.0F ? minGain : maxGain;
    }

    std::uint8_t representationBits(NodeFormat const& format) noexcept
    {
      if (auto const* pcmFormat = std::get_if<PcmFormat>(&format); pcmFormat != nullptr)
      {
        return encodingContainerBits(pcmFormat->encoding);
      }

      return std::get<SignalFormat>(format).precisionBits;
    }

    bool isLosslessBitDepthChange(NodeFormat const& sourceFormat, NodeFormat const& destinationFormat) noexcept
    {
      auto const sourceSignal = signalFormat(sourceFormat);
      auto const destinationSignal = signalFormat(destinationFormat);

      if (sourceSignal.sampleKind == destinationSignal.sampleKind)
      {
        return sourceSignal.precisionBits <= destinationSignal.precisionBits;
      }

      if (sourceSignal.sampleKind == SampleKind::Integer && destinationSignal.sampleKind == SampleKind::FloatingPoint)
      {
        return representationBits(destinationFormat) == 32U && sourceSignal.precisionBits <= 24U;
      }

      return false;
    }

    bool hasFormatPrecisionChange(NodeFormat const& sourceFormat, NodeFormat const& destinationFormat) noexcept
    {
      auto const sourceSignal = signalFormat(sourceFormat);
      auto const destinationSignal = signalFormat(destinationFormat);
      return sourceSignal.precisionBits != destinationSignal.precisionBits ||
             sourceSignal.sampleKind != destinationSignal.sampleKind ||
             representationBits(sourceFormat) != representationBits(destinationFormat);
    }

    void addFinding(NodeQualityAssessment& assessment, QualityFinding finding)
    {
      assessment.worstQuality = worseQuality(assessment.worstQuality, finding.quality);
      assessment.findings.push_back(std::move(finding));
    }

    bool isPrecisionInvalidating(QualityFinding const& finding) noexcept
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
      return std::ranges::any_of(assessment.findings, isPrecisionInvalidating);
    }

    bool hasVerifiedOutputEndpoint(std::vector<flow::Node const*> const& path) noexcept
    {
      return !path.empty() && path.back()->type == flow::NodeType::Sink;
    }

    bool hasNativeChannelBoundary(flow::Node const& node) noexcept
    {
      return node.type == flow::NodeType::Stream || node.type == flow::NodeType::Intermediary ||
             node.type == flow::NodeType::Sink;
    }

    std::uint32_t channelSpeakerMask(flow::Node const& node) noexcept
    {
      auto const formatMask = node.optFormat ? signalFormat(*node.optFormat).speakerMask : 0U;

      if (!hasNativeChannelBoundary(node))
      {
        return formatMask;
      }

      // Retain every actually known role, including compatible partial reports.
      // This constraint set asserts neither completeness nor native byte order.
      return formatMask | node.channelDelivery.nativeSpeakerMask;
    }

    std::optional<std::uint32_t> channelFindingSpeakerMask(flow::Node const* node, std::uint32_t const mask) noexcept
    {
      if (node == nullptr || mask == 0U || (node->optFormat && signalFormat(*node->optFormat).speakerMask == mask))
      {
        return std::nullopt;
      }

      return mask;
    }

    bool hasCompleteChannelIdentity(flow::Node const& node) noexcept
    {
      if (!node.optFormat)
      {
        return false;
      }

      auto const format = signalFormat(*node.optFormat);
      auto const nativeMask = hasNativeChannelBoundary(node) ? node.channelDelivery.nativeSpeakerMask : 0U;
      return isCompleteWaveSpeakerMask(format.speakerMask, format.channels) ||
             isCompleteWaveSpeakerMask(nativeMask, format.channels);
    }

    struct ChannelIdentityEvidence final
    {
      // Last complete identity, or the first partial/count observation until one exists.
      flow::Node const* node = nullptr;
      std::uint32_t speakerMask = 0;
      std::uint8_t channels = 0;
    };

    ChannelConfidence channelConfidence(std::vector<flow::Node const*> const& path) noexcept
    {
      if (!hasVerifiedOutputEndpoint(path) || path.front()->type != flow::NodeType::Source ||
          !hasCompleteChannelIdentity(*path.front()) ||
          path.front()->sourceChannelLayout.origin == ChannelLayoutOrigin::Unknown)
      {
        return ChannelConfidence::Unknown;
      }

      for (auto const* node : path)
      {
        if (!hasCompleteChannelIdentity(*node))
        {
          return ChannelConfidence::Unverified;
        }

        switch (node->type)
        {
          case flow::NodeType::Source:
          case flow::NodeType::Decoder:
          case flow::NodeType::Engine: break;
          case flow::NodeType::Stream:
          case flow::NodeType::Intermediary:
          case flow::NodeType::Sink:
            if (!node->channelDelivery.isMapped)
            {
              return ChannelConfidence::Unverified;
            }

            break;
          case flow::NodeType::ExternalSource: return ChannelConfidence::Unverified;
        }
      }

      return path.front()->sourceChannelLayout.origin == ChannelLayoutOrigin::Inferred ? ChannelConfidence::Inferred
                                                                                       : ChannelConfidence::Verified;
    }

    bool tryFindPathToSink(flow::Graph const& graph,
                           std::string_view const currentId,
                           std::set<std::string_view>& visitedNodeIds,
                           std::vector<flow::Node const*>& path)
    {
      if (!visitedNodeIds.insert(currentId).second)
      {
        return false;
      }

      auto const nodeIt = std::ranges::find(graph.nodes, currentId, &flow::Node::id);

      if (nodeIt == graph.nodes.end())
      {
        return false;
      }

      path.push_back(&(*nodeIt));

      if (nodeIt->type == flow::NodeType::Sink)
      {
        return true;
      }

      for (auto const& connection : graph.connections)
      {
        if (connection.isActive && connection.sourceId == currentId &&
            tryFindPathToSink(graph, connection.destinationId, visitedNodeIds, path))
        {
          return true;
        }
      }

      path.pop_back();
      return false;
    }

    std::vector<flow::Node const*> findPlaybackPath(flow::Graph const& graph, std::string const& startId)
    {
      auto path = std::vector<flow::Node const*>{};

      if (auto visitedNodeIds = std::set<std::string_view>{}; tryFindPathToSink(graph, startId, visitedNodeIds, path))
      {
        return path;
      }

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

    void assessInputSources(flow::Node const& node,
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
        addFinding(assessment,
                   QualityFinding{.kind = isAmplification ? QualityFindingKind::SoftwareAmplification
                                                          : QualityFindingKind::SoftwareVolumeModification,
                                  .quality = Quality::LinearIntervention,
                                  .gain = representativeGain(node.maxSoftwareGain, node.minSoftwareGain)});
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
        // A factor above unity keeps this kind: the provenance is unknown, so the backend evidence
        // does not support asserting software amplification or its clipping risk.
        addFinding(assessment,
                   QualityFinding{.kind = QualityFindingKind::UnclassifiedVolumeModification,
                                  .quality = Quality::LinearIntervention,
                                  .gain = representativeGain(node.maxUnclassifiedGain, node.minUnclassifiedGain)});
      }

      if (node.isMuted)
      {
        addFinding(
          assessment, QualityFinding{.kind = QualityFindingKind::Muted, .quality = Quality::LinearIntervention});
      }
    }

    QualityFinding channelMappingFinding(flow::Node const* changedFrom,
                                         ChannelIdentityEvidence const& knownIdentity,
                                         flow::Node const& currentNode,
                                         bool const hasCountChange)
    {
      // A node's storage declaration and independent native set can disagree
      // without a transition in its format mask. Explain actual reports, never
      // their impossible union, while keeping endpoint count and storage intact.
      auto const format = currentNode.optFormat ? signalFormat(*currentNode.optFormat) : SignalFormat{};
      auto const nativeMask =
        hasNativeChannelBoundary(currentNode) ? currentNode.channelDelivery.nativeSpeakerMask : 0U;
      bool const hasLocalRoleConflict = format.channels != 0U && format.speakerMask != 0U && nativeMask != 0U &&
                                        std::popcount(format.speakerMask | nativeMask) > format.channels;
      bool const describeLocalSource = hasLocalRoleConflict && !hasCountChange;
      auto const* fromNode = describeLocalSource ? &currentNode : changedFrom;
      auto const fromMask = describeLocalSource ? format.speakerMask : knownIdentity.speakerMask;
      auto const toMask = hasLocalRoleConflict ? nativeMask : channelSpeakerMask(currentNode);
      return {.kind = QualityFindingKind::ChannelMapping,
              .quality = Quality::LinearIntervention,
              .optFromFormat = fromNode != nullptr ? fromNode->optFormat : std::nullopt,
              .optToFormat = currentNode.optFormat,
              .optFromSpeakerMask = channelFindingSpeakerMask(fromNode, fromMask),
              .optToSpeakerMask = channelFindingSpeakerMask(&currentNode, toMask)};
    }

    void assessChannelTransition(flow::Node const* previousNode,
                                 ChannelIdentityEvidence& knownIdentity,
                                 flow::Node const& currentNode,
                                 NodeQualityAssessment& targetAssessment)
    {
      auto const currentMask = channelSpeakerMask(currentNode);
      auto const currentChannels =
        currentNode.optFormat ? signalFormat(*currentNode.optFormat).channels : std::uint8_t{};
      auto const comparisonChannels = currentChannels != 0U ? currentChannels : knownIdentity.channels;
      bool const hasConflictingReports = currentChannels != 0U && std::popcount(currentMask) > currentChannels;
      bool const hasCountChange =
        knownIdentity.channels != 0U && currentChannels != 0U && knownIdentity.channels != currentChannels;
      bool const hasConflictingRoles = knownIdentity.speakerMask != 0U && currentMask != 0U &&
                                       comparisonChannels != 0U &&
                                       std::popcount(knownIdentity.speakerMask | currentMask) > comparisonChannels;
      flow::Node const* changedFrom = nullptr;

      if (previousNode != nullptr && previousNode->optFormat && currentNode.optFormat &&
          signalFormat(*previousNode->optFormat).channels != currentChannels)
      {
        changedFrom = previousNode;
      }
      else if (hasCountChange || hasConflictingRoles)
      {
        changedFrom = knownIdentity.node;
      }

      bool const hasSharedConversion =
        hasNativeChannelBoundary(currentNode) && currentNode.channelDelivery.hasSharedConversion;

      if (hasSharedConversion || hasConflictingReports || changedFrom != nullptr)
      {
        // Missing roles are not a conversion. Known roles contradict only when
        // they cannot coexist within count-owned geometry, or count itself changes.
        addFinding(targetAssessment, channelMappingFinding(changedFrom, knownIdentity, currentNode, hasCountChange));
      }

      if (hasConflictingReports)
      {
        auto const nativeMask =
          hasNativeChannelBoundary(currentNode) ? currentNode.channelDelivery.nativeSpeakerMask : 0U;

        if (isCompleteWaveSpeakerMask(nativeMask, currentChannels))
        {
          // The local disagreement remains a finding. Complete observed output
          // roles, not the impossible union, describe its continuation; an
          // unchanged downstream observation must not repeat the incoming change.
          knownIdentity = {.node = &currentNode, .speakerMask = nativeMask, .channels = currentChannels};
        }
        else if (hasCountChange)
        {
          // Partial or invalid native roles cannot replace prior constraints.
          // A count change still establishes geometry at this receiving node.
          knownIdentity = {.node = &currentNode, .channels = currentChannels};
        }

        return;
      }

      if (hasCountChange || hasConflictingRoles)
      {
        // A formatless conflict changes the role anchor, not the last known count.
        knownIdentity = {.node = &currentNode, .speakerMask = currentMask, .channels = comparisonChannels};
        return;
      }

      // Compatible partial and unknown observations must not replace a complete
      // anchor. Keep their known roles as constraints without claiming completeness.
      knownIdentity.speakerMask |= currentMask;

      if (currentChannels != 0U)
      {
        knownIdentity.channels = currentChannels;
      }

      if (knownIdentity.node == nullptr || hasCompleteChannelIdentity(currentNode))
      {
        knownIdentity.node = &currentNode;
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
      auto const s1 = signalFormat(f1);
      auto const s2 = signalFormat(f2);

      if (s1.sampleRate != s2.sampleRate)
      {
        addFinding(targetAssessment,
                   QualityFinding{.kind = QualityFindingKind::Resampling,
                                  .quality = Quality::LinearIntervention,
                                  .optFromFormat = f1,
                                  .optToFormat = f2});
        optProvenPrecision.reset();
      }

      if (hasFormatPrecisionChange(f1, f2))
      {
        if (s1.sampleKind == SampleKind::FloatingPoint && s2.sampleKind == SampleKind::Integer &&
            optProvenPrecision.value_or(UINT8_MAX) <= s2.precisionBits)
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
            QualityFinding{
              .kind = s2.sampleKind == SampleKind::FloatingPoint ? QualityFindingKind::LosslessFloat
                                                                 : QualityFindingKind::LosslessPadding,
              .quality = s2.sampleKind == SampleKind::FloatingPoint ? Quality::LosslessFloat : Quality::LosslessPadded,
              .optFromFormat = f1,
              .optToFormat = f2});
        }
        else if (s1.sampleKind == SampleKind::Integer && s2.sampleKind == SampleKind::Integer &&
                 optProvenPrecision.value_or(UINT8_MAX) <= s2.precisionBits)
        {
          // Narrowing an integer container back to bits that were only padded
          // on the way in returns the original samples. A 24-bit source widened
          // into a 32-bit container and delivered to a 24-bit endpoint loses
          // nothing, so the padding finding upstream already tells the whole
          // story. Any earlier truncation, gain, or mix clears the proven
          // precision, and this narrowing is then reported as truncation.
          addFinding(targetAssessment,
                     QualityFinding{.kind = QualityFindingKind::LosslessRoundTrip,
                                    .quality = Quality::LosslessPadded,
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

    if (auto const& optSourceFormat = path.front()->optFormat;
        optSourceFormat && signalFormat(*optSourceFormat).sampleKind == SampleKind::Integer)
    {
      optProvenPrecision = signalFormat(*optSourceFormat).precisionBits;
    }

    auto knownIdentity = ChannelIdentityEvidence{};

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
      };

      assessNodeSelfProperties(*node, assessment);
      assessInputSources(*node, path, inputSources, graph, assessment);
      assessChannelTransition(i > 0 ? path[i - 1] : nullptr, knownIdentity, *node, assessment);

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

    result.channelConfidence = channelConfidence(path);
    return result;
  }
} // namespace ao::audio
