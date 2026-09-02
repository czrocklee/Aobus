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
  struct AlsaGraphPublicationState final
  {
    BackendGraphRegistry registry{};
  };

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

  AlsaGraphPublisher::AlsaGraphPublisher(std::shared_ptr<AlsaGraphPublicationState> statePtr)
    : _statePtr{std::move(statePtr)}
  {
  }

  void AlsaGraphPublisher::publish(AlsaRouteState state) const
  {
    if (!_statePtr)
    {
      return;
    }

    auto const anchor = state.routeAnchor;
    _statePtr->registry.publish(anchor, buildGraph(state));
  }

  void AlsaGraphPublisher::clear(std::string_view const routeAnchor) const
  {
    if (_statePtr)
    {
      _statePtr->registry.clear(routeAnchor);
    }
  }

  AlsaGraphRegistry::AlsaGraphRegistry()
    : _statePtr{std::make_shared<AlsaGraphPublicationState>()}
  {
  }

  AlsaGraphRegistry::~AlsaGraphRegistry()
  {
    shutdown();
  }

  Subscription AlsaGraphRegistry::subscribe(std::string_view const routeAnchor, Callback callback)
  {
    auto const statePtr = _statePtr;
    auto const anchor = std::string{routeAnchor};
    return statePtr->registry.subscribe(anchor, std::move(callback), buildGraph(AlsaRouteState{.routeAnchor = anchor}));
  }

  AlsaGraphPublisher AlsaGraphRegistry::publisher() const
  {
    return AlsaGraphPublisher{_statePtr};
  }

  void AlsaGraphRegistry::publish(AlsaRouteState state) const
  {
    publisher().publish(std::move(state));
  }

  void AlsaGraphRegistry::clear(std::string_view const routeAnchor) const
  {
    publisher().clear(routeAnchor);
  }

  void AlsaGraphRegistry::shutdown() noexcept
  {
    auto const statePtr = _statePtr;
    statePtr->registry.shutdown();
  }
} // namespace ao::audio::backend::detail
