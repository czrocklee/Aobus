// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/audio/Subscription.h>
#include <ao/audio/backend/detail/AlsaGraphRegistry.h>
#include <ao/audio/backend/detail/AudioBackendVolumeMath.h>
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

      graph.nodes.push_back({.id = "alsa-stream",
                             .type = flow::NodeType::Stream,
                             .name = "ALSA Stream",
                             .optFormat = state.optFormat,
                             .objectPath = ""});

      auto sink = flow::Node{.id = "alsa-sink",
                             .type = flow::NodeType::Sink,
                             .name = state.routeAnchor,
                             .optFormat = state.optFormat,
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
      graph.connections.push_back({.sourceId = "alsa-stream", .destinationId = "alsa-sink", .isActive = true});

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
    mutable std::recursive_mutex callbackMutex;
    std::unordered_map<std::string, AlsaRouteState> states;
    std::vector<Subscriber> subscribers;
    std::uint64_t nextSubId = 1;
    bool shutdown = false;
  };

  AlsaGraphRegistry::AlsaGraphRegistry()
    : _implPtr{std::make_shared<Impl>()}
  {
  }

  AlsaGraphRegistry::~AlsaGraphRegistry()
  {
    auto const impl = _implPtr;
    auto const callbackLock = std::scoped_lock{impl->callbackMutex};
    auto const lock = std::scoped_lock{impl->mutex};
    impl->shutdown = true;
    impl->states.clear();
    impl->subscribers.clear();
  }

  Subscription AlsaGraphRegistry::subscribe(std::string_view routeAnchor, Callback callback)
  {
    if (!callback)
    {
      return {};
    }

    auto const impl = _implPtr;
    auto const anchor = std::string{routeAnchor};
    auto initialGraph = flow::Graph{};
    auto id = std::uint64_t{0};
    // Linearize registration, snapshot capture, and initial delivery with
    // publications while keeping the state mutex out of user callbacks.
    auto const callbackLock = std::scoped_lock{impl->callbackMutex};

    {
      auto const lock = std::scoped_lock{impl->mutex};

      if (impl->shutdown)
      {
        return {};
      }

      id = impl->nextSubId++;
      impl->subscribers.push_back({.id = id, .routeAnchor = anchor, .callback = callback});

      if (auto const it = impl->states.find(anchor); it != impl->states.end())
      {
        initialGraph = buildGraph(it->second);
      }
      else
      {
        initialGraph = buildGraph({.routeAnchor = anchor});
      }
    }

    try
    {
      callback(initialGraph);
    }
    catch (...)
    {
      auto const lock = std::scoped_lock{impl->mutex};
      auto const it = std::ranges::find(impl->subscribers, id, &Impl::Subscriber::id);

      if (it != impl->subscribers.end())
      {
        impl->subscribers.erase(it);
      }

      throw;
    }

    {
      auto const lock = std::scoped_lock{impl->mutex};

      if (impl->shutdown || std::ranges::find(impl->subscribers, id, &Impl::Subscriber::id) == impl->subscribers.end())
      {
        return {};
      }
    }

    return Subscription{[weakImpl = std::weak_ptr{impl}, id]
                        {
                          auto const impl = weakImpl.lock();

                          if (!impl)
                          {
                            return;
                          }

                          auto const callbackLock = std::scoped_lock{impl->callbackMutex};
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
    auto const impl = _implPtr;
    auto const anchor = state.routeAnchor;
    auto const graph = buildGraph(state);
    auto pendingSubscribers = std::vector<Impl::Subscriber>{};

    {
      auto const lock = std::scoped_lock{impl->mutex};

      if (impl->shutdown)
      {
        return;
      }

      impl->states[anchor] = std::move(state);

      for (auto const& sub : impl->subscribers)
      {
        if (sub.routeAnchor == anchor)
        {
          pendingSubscribers.push_back(sub);
        }
      }
    }

    for (auto const& subscriber : pendingSubscribers)
    {
      auto const callbackLock = std::scoped_lock{impl->callbackMutex};

      {
        auto const lock = std::scoped_lock{impl->mutex};

        if (impl->shutdown)
        {
          return;
        }

        if (std::ranges::find(impl->subscribers, subscriber.id, &Impl::Subscriber::id) == impl->subscribers.end())
        {
          continue;
        }
      }

      subscriber.callback(graph);
    }
  }

  void AlsaGraphRegistry::clear(std::string_view routeAnchor)
  {
    auto const impl = _implPtr;
    auto const anchor = std::string{routeAnchor};
    auto const emptyGraph = flow::Graph{};
    auto pendingSubscribers = std::vector<Impl::Subscriber>{};

    {
      auto const lock = std::scoped_lock{impl->mutex};

      if (impl->shutdown)
      {
        return;
      }

      impl->states.erase(anchor);

      for (auto const& sub : impl->subscribers)
      {
        if (sub.routeAnchor == anchor)
        {
          pendingSubscribers.push_back(sub);
        }
      }
    }

    for (auto const& subscriber : pendingSubscribers)
    {
      auto const callbackLock = std::scoped_lock{impl->callbackMutex};

      {
        auto const lock = std::scoped_lock{impl->mutex};

        if (impl->shutdown)
        {
          return;
        }

        if (std::ranges::find(impl->subscribers, subscriber.id, &Impl::Subscriber::id) == impl->subscribers.end())
        {
          continue;
        }
      }

      subscriber.callback(emptyGraph);
    }
  }
} // namespace ao::audio::backend::detail
