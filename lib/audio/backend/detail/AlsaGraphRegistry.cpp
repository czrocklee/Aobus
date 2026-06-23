// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/audio/Subscription.h>
#include <ao/audio/backend/detail/AlsaGraphRegistry.h>
#include <ao/audio/backend/detail/AudioBackendShared.h>
#include <ao/audio/flow/Graph.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

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

      graph.nodes.push_back(
        {.id = "alsa-stream", .type = flow::NodeType::Stream, .name = "ALSA Stream", .objectPath = ""});

      auto sink = flow::Node{
        .id = "alsa-sink", .type = flow::NodeType::Sink, .name = state.routeAnchor, .objectPath = state.routeAnchor};

      sink.isMuted = state.muted;

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
      graph.connections.push_back({.sourceId = "alsa-stream", .destId = "alsa-sink", .isActive = true});

      return graph;
    }
  } // namespace

  struct AlsaGraphRegistry::Impl final
  {
    struct Subscriber
    {
      std::uint64_t id;
      std::string routeAnchor;
      Callback callback;
    };

    mutable std::mutex mutex;
    std::unordered_map<std::string, AlsaRouteState> states;
    std::vector<Subscriber> subscribers;
    std::uint64_t nextSubId = 1;
  };

  AlsaGraphRegistry::AlsaGraphRegistry()
    : _implPtr{std::make_unique<Impl>()}
  {
  }

  AlsaGraphRegistry::~AlsaGraphRegistry() = default;

  Subscription AlsaGraphRegistry::subscribe(std::string_view routeAnchor, Callback callback)
  {
    if (!callback)
    {
      return {};
    }

    std::uint64_t const id = [this, routeAnchor, &callback]
    {
      auto const lock = std::scoped_lock{_implPtr->mutex};
      auto const subId = _implPtr->nextSubId++;
      _implPtr->subscribers.push_back({.id = subId, .routeAnchor = std::string{routeAnchor}, .callback = callback});
      return subId;
    }();

    // Emit immediate snapshot
    {
      auto const lock = std::scoped_lock{_implPtr->mutex};

      if (auto const it = _implPtr->states.find(std::string{routeAnchor}); it != _implPtr->states.end())
      {
        callback(buildGraph(it->second));
      }
      else
      {
        // Neutral graph if no state yet
        callback(buildGraph({.routeAnchor = std::string{routeAnchor}}));
      }
    }

    return Subscription{[impl = _implPtr.get(), id]
                        {
                          auto const lock = std::scoped_lock{impl->mutex};
                          auto const it = std::ranges::find(impl->subscribers, id, &Impl::Subscriber::id);

                          if (it != impl->subscribers.end())
                          {
                            impl->subscribers.erase(it);
                          }
                        }};
  }

  void AlsaGraphRegistry::publish(AlsaRouteState state)
  {
    auto const anchor = state.routeAnchor;
    auto const graph = buildGraph(state);
    auto pendingCallbacks = std::vector<Callback>{};

    {
      auto const lock = std::scoped_lock{_implPtr->mutex};
      _implPtr->states[anchor] = std::move(state);

      for (auto const& sub : _implPtr->subscribers)
      {
        if (sub.routeAnchor == anchor)
        {
          pendingCallbacks.push_back(sub.callback);
        }
      }
    }

    for (auto const& cb : pendingCallbacks)
    {
      cb(graph);
    }
  }

  void AlsaGraphRegistry::clear(std::string_view routeAnchor)
  {
    auto const anchor = std::string{routeAnchor};
    auto const emptyGraph = flow::Graph{};
    auto pendingCallbacks = std::vector<Callback>{};

    {
      auto const lock = std::scoped_lock{_implPtr->mutex};
      _implPtr->states.erase(anchor);

      for (auto const& sub : _implPtr->subscribers)
      {
        if (sub.routeAnchor == anchor)
        {
          pendingCallbacks.push_back(sub.callback);
        }
      }
    }

    for (auto const& cb : pendingCallbacks)
    {
      cb(emptyGraph);
    }
  }
} // namespace ao::audio::backend::detail
