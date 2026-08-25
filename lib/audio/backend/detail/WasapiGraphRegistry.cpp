// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "backend/detail/WasapiGraphRegistry.h"

#include "backend/detail/AudioBackendVolumeMath.h"
#include "backend/detail/BackendGraphRegistry.h"
#include <ao/audio/Subscription.h>
#include <ao/audio/flow/Graph.h>

#include <cmath>
#include <memory>
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

    flow::Graph buildGraph(WasapiRouteState const& state)
    {
      auto graph = flow::Graph{};

      graph.nodes.push_back({.id = "wasapi-stream",
                             .type = flow::NodeType::Stream,
                             .name = "WASAPI Stream",
                             .optFormat = state.optInputFormat,
                             .objectPath = ""});

      auto sink = flow::Node{.id = "wasapi-sink",
                             .type = flow::NodeType::Sink,
                             .name = state.routeAnchor,
                             .optFormat = state.optMixFormat,
                             .objectPath = state.routeAnchor};

      sink.isMuted = state.muted;

      // Session volume is applied by the Windows audio engine in software.
      sink.maxSoftwareGain = state.volume;
      sink.minSoftwareGain = state.volume;

      if (!isUnity(state.volume))
      {
        sink.softwareVolumeNotUnity = true;
      }

      graph.nodes.push_back(std::move(sink));
      graph.connections.push_back({.sourceId = "wasapi-stream", .destinationId = "wasapi-sink", .isActive = true});

      return graph;
    }
  } // namespace

  struct WasapiGraphRegistry::Impl final
  {
    BackendGraphRegistry registry{};
  };

  WasapiGraphRegistry::WasapiGraphRegistry()
    : _implPtr{std::make_unique<Impl>()}
  {
  }

  WasapiGraphRegistry::~WasapiGraphRegistry() = default;

  Subscription WasapiGraphRegistry::subscribe(std::string_view routeAnchor, Callback callback)
  {
    auto const anchor = std::string{routeAnchor};
    return _implPtr->registry.subscribe(
      anchor, std::move(callback), buildGraph(WasapiRouteState{.routeAnchor = anchor}));
  }

  void WasapiGraphRegistry::publish(WasapiRouteState state)
  {
    auto const anchor = state.routeAnchor;
    _implPtr->registry.publish(anchor, buildGraph(state));
  }

  void WasapiGraphRegistry::clear(std::string_view routeAnchor)
  {
    _implPtr->registry.clear(routeAnchor);
  }

  void WasapiGraphRegistry::shutdown() noexcept
  {
    _implPtr->registry.shutdown();
  }
} // namespace ao::audio::backend::detail
