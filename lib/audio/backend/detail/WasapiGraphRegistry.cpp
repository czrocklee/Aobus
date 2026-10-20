// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "backend/detail/WasapiGraphRegistry.h"

#include "backend/detail/AudioBackendVolumeMath.h"
#include "backend/detail/BackendGraphRegistry.h"
#include <ao/audio/ChannelDeliveryEvidence.h>
#include <ao/audio/SignalFormat.h>
#include <ao/audio/flow/Graph.h>
#include <ao/utility/ScopedRegistration.h>

#include <bit>
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
      graph.nodes.back().channelDelivery = state.channelDelivery;
      // The retained conversion describes client -> mix, not engine -> client.
      graph.nodes.back().channelDelivery.hasSharedConversion = false;

      auto sink = flow::Node{.id = "wasapi-sink",
                             .type = flow::NodeType::Sink,
                             .name = state.routeAnchor,
                             .optFormat = state.mixObservation.optFormat,
                             .objectPath = state.routeAnchor};

      auto const mixChannels = state.mixObservation.channels;
      auto const mixMask = state.mixObservation.speakerMask;
      auto const hasMixRoles = mixMask != 0U && (mixMask & ~kWaveSpeakerMask) == 0U &&
                               std::cmp_less_equal(std::popcount(mixMask), mixChannels);
      auto const hasMixPositions = isCompleteWaveSpeakerMask(mixMask, mixChannels);

      // GetMixFormat describes the software engine boundary, independently of
      // the accepted client descriptor. Complete observed positions establish
      // this boundary's mapping, not physical endpoint or speaker wiring.
      // Partial roles remain independent constraints without complete mapping.
      sink.channelDelivery.isMapped = hasMixPositions;
      sink.channelDelivery.nativeSpeakerMask = hasMixRoles ? mixMask : 0U;
      sink.channelDelivery.hasSharedConversion = state.channelDelivery.hasSharedConversion;
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

  utility::ScopedRegistration WasapiGraphRegistry::subscribe(std::string_view routeAnchor, Callback callback)
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
