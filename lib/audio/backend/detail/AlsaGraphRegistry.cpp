// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "backend/detail/AlsaGraphRegistry.h"

#include "backend/detail/AudioBackendVolumeMath.h"
#include "backend/detail/BackendGraphRegistry.h"
#include <ao/audio/NodeFormat.h>
#include <ao/audio/Subscription.h>
#include <ao/audio/flow/Graph.h>

#include <cmath>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace ao::audio::backend::detail
{
  namespace
  {
    bool isUnity(float volume) noexcept
    {
      return std::abs(volume - 1.0F) < kVolumeEpsilon;
    }

    flow::Graph buildGraph(AlsaRouteState const& state)
    {
      auto graph = flow::Graph{};

      auto optStreamFormat = std::optional<NodeFormat>{};
      auto optSinkFormat = std::optional<NodeFormat>{};

      if (state.optMode)
      {
        // The stream carries bytes, so it keeps the concrete client encoding.
        // The sink is hardware, so it reports the precision the endpoint
        // actually resolves rather than the width of the container feeding it.
        optStreamFormat = state.optMode->clientFormat;

        if (state.optMode->optEndpoint)
        {
          optSinkFormat = state.optMode->optEndpoint->signalFormat;
        }
      }

      graph.nodes.push_back({.id = "alsa-stream",
                             .type = flow::NodeType::Stream,
                             .name = "ALSA Stream",
                             .optFormat = optStreamFormat,
                             .objectPath = ""});

      auto sink = flow::Node{.id = "alsa-sink",
                             .type = flow::NodeType::Sink,
                             .name = state.routeAnchor,
                             .optFormat = optSinkFormat,
                             .objectPath = state.routeAnchor};

      sink.isMuted = state.muted;

      if (state.volumeMode == AlsaVolumeControlMode::SoftwareGain)
      {
        sink.maxSoftwareGain = state.volume;
        sink.minSoftwareGain = state.volume;
      }

      if (!isUnity(state.volume))
      {
        if (state.volumeMode == AlsaVolumeControlMode::HardwareMixer)
        {
          sink.hardwareVolumeNotUnity = true;
        }
        else if (state.volumeMode == AlsaVolumeControlMode::SoftwareGain)
        {
          sink.softwareVolumeNotUnity = true;
        }
        else
        {
          sink.unclassifiedVolumeNotUnity = true;
        }
      }

      graph.nodes.push_back(std::move(sink));
      graph.connections.push_back(
        {.sourceId = "alsa-stream", .destinationId = "alsa-sink", .isActive = state.optMode.has_value()});

      return graph;
    }
  } // namespace

  struct AlsaGraphRegistry::Impl final
  {
    BackendGraphRegistry registry{};
  };

  AlsaGraphRegistry::AlsaGraphRegistry()
    : _implPtr{std::make_unique<Impl>()}
  {
  }

  AlsaGraphRegistry::~AlsaGraphRegistry() = default;

  Subscription AlsaGraphRegistry::subscribe(std::string_view routeAnchor, Callback callback)
  {
    auto const anchor = std::string{routeAnchor};
    return _implPtr->registry.subscribe(anchor, std::move(callback), buildGraph(AlsaRouteState{.routeAnchor = anchor}));
  }

  void AlsaGraphRegistry::publish(AlsaRouteState state)
  {
    auto const anchor = state.routeAnchor;
    _implPtr->registry.publish(anchor, buildGraph(state));
  }

  void AlsaGraphRegistry::clear(std::string_view routeAnchor)
  {
    _implPtr->registry.clear(routeAnchor);
  }
} // namespace ao::audio::backend::detail
